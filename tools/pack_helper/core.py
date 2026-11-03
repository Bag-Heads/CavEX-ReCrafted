"""Safe, transactional core for the CavEX Asset Importer.

The GUI is deliberately a thin caller of the three functions exported here.
Pack input is data: it is never imported as Python, launched, or extracted by
an operating-system archive helper.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import stat
import struct
import sys
import tempfile
import threading
import uuid
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any, Callable, Iterator, Optional

from . import nether_scene


SCHEMA_VERSION = 1
MANIFEST_NAME = ".cavewii-pack.json"
DEFAULT_LIMITS = {
    "files": 8192,
    "file_bytes": 32 * 1024 * 1024,
    "total_bytes": 256 * 1024 * 1024,
    "path_bytes": 240,
    "ratio": 200,
}
MODERN_MARKERS = ("assets/minecraft/", "textures/block/", "textures/item/",
                  "textures/blocks/", "textures/items/")
# One plain sentence for every "this is a newer Minecraft" refusal. A modern
# jar used to die on "archive has too many files" (the file-count limit ran
# before the format check), which told the player nothing (2026-10-30 audit).
MODERN_MESSAGE = ("This is not Minecraft Beta 1.7.3 -- it is a newer version. Pick b1.7.3.jar "
                  "(in the Minecraft Launcher's .minecraft/versions/b1.7.3 folder), or a "
                  "Beta-era texture pack.")
IMAGE_EXTENSIONS = {".png"}
AUDIO_EXTENSIONS = {".ogg", ".wav"}
_IMPORTER_LOCK = threading.Lock()


class PackError(Exception):
    def __init__(self, code: str, message: str, path: Optional[str] = None):
        super().__init__(message)
        self.code = code
        self.path = path


def _error(exc: Exception) -> dict:
    if isinstance(exc, PackError):
        out = {"code": exc.code, "message": str(exc)}
        if exc.path:
            out["path"] = exc.path
        return out
    return {"code": "io_error", "message": str(exc)}


def _limits(overrides: Optional[dict]) -> dict:
    result = dict(DEFAULT_LIMITS)
    if overrides:
        for key, value in overrides.items():
            if key not in result or not isinstance(value, int) or value <= 0:
                raise PackError("invalid_limits", "invalid positive limit: {0}".format(key))
            result[key] = value
    return result


def _safe_rel(raw: str, limits: dict) -> str:
    if not raw or "\x00" in raw or "\\" in raw:
        raise PackError("unsafe_path", "empty, NUL, or backslash path", raw)
    if len(raw.encode("utf-8")) > limits["path_bytes"]:
        raise PackError("path_too_long", "archive path is too long", raw)
    if raw.startswith("/") or re.match(r"^[A-Za-z]:", raw):
        raise PackError("absolute_path", "absolute paths are not allowed", raw)
    # Validate before PurePosixPath gets a chance to normalize empty or '.'.
    raw_parts = raw.split("/")
    if any(part in ("", ".", "..") for part in raw_parts):
        raise PackError("path_traversal", "ambiguous or parent path is not allowed", raw)
    parts = PurePosixPath(raw).parts
    if any(":" in part or any(ord(ch) < 32 for ch in part) for part in parts):
        raise PackError("unsafe_path", "colon or control character in path", raw)
    reserved = {"CON", "PRN", "AUX", "NUL"} | {
        prefix + str(number) for prefix in ("COM", "LPT") for number in range(1, 10)
    }
    for part in parts:
        if part.endswith((".", " ")) or part.split(".", 1)[0].upper() in reserved:
            raise PackError("windows_ambiguous_path", "Windows-reserved path is not allowed", raw)
    return "/".join(parts)


def _check_prefix_collisions(files: list[dict]):
    # Every path whose PARENT chain contains another entry collides -- a file
    # and a directory cannot share a name on the target filesystem. Comparing
    # only sorted-adjacent pairs missed non-adjacent ones: "a", "a-x", "a/b"
    # sorts with "a-x" between them ('-' < '/'), so the real "a" vs "a/b"
    # collision slipped through (2026-09-17 review, finding L7). Walking each
    # path's own parents covers ALL pairs that can collide, at O(n * depth).
    folded = {}
    for row in files:
        folded.setdefault(row["path"].casefold(), row["path"])
    for key in folded:
        parts = key.split("/")
        for depth in range(1, len(parts)):
            parent = "/".join(parts[:depth])
            if parent in folded:
                raise PackError("file_directory_collision",
                                "a file path is also used as a directory", folded[parent])


def _category(path: str) -> str:
    suffix = PurePosixPath(path).suffix.lower()
    if suffix in IMAGE_EXTENSIONS:
        return "image"
    if suffix in AUDIO_EXTENSIONS:
        return "audio"
    return "ignored"


def _modern_marker(paths) -> Optional[str]:
    paths = [path.casefold() for path in paths]
    marker = next((path for path in paths if path.startswith(MODERN_MARKERS)), None)
    if marker or "pack.mcmeta" not in paths:
        return marker
    # Launchers also put pack.mcmeta and icons/pe artwork in the legacy
    # resources tree. Only its audio layout earns this metadata exception;
    # the usual archive/path checks and native audio validation still run.
    audio_roots = tuple(kind + "/" for kind in _discovery().AUDIO_DIRS)
    legacy_audio = any(path.startswith(audio_roots) and _category(path) == "audio"
                       for path in paths)
    incompatible = any(path.endswith(".class")
                       or path.startswith(("assets/", "textures/", "models/"))
                       or (_category(path) == "image" and not path.startswith(("icons/", "pe/")))
                       for path in paths)
    return None if legacy_audio and not incompatible else "pack.mcmeta"


def _zip_inventory(source: Path, limits: dict) -> list[dict]:
    if not zipfile.is_zipfile(source):
        raise PackError("invalid_archive", "source is not a valid ZIP archive")
    files: list[dict] = []
    seen: dict[str, str] = {}
    total = 0
    try:
        with zipfile.ZipFile(source) as archive, source.open("rb") as raw_archive:
            # The format check runs BEFORE the size/count limits: a 1.21 client
            # jar is far over the file limit, and "too many files" is not the
            # reason it cannot be used.
            modern = _modern_marker(archive.namelist())
            if modern:
                raise PackError("modern_format", MODERN_MESSAGE, modern)
            for info in archive.infolist():
                # On Windows zipfile normalizes a stored backslash to '/'
                # while constructing ZipInfo. Inspect the local header too so
                # an ambiguous name cannot become acceptable by normalization.
                raw_archive.seek(info.header_offset)
                header = raw_archive.read(30)
                if len(header) != 30 or header[:4] != b"PK\x03\x04":
                    raise PackError("invalid_archive", "invalid ZIP local header", info.filename)
                name_len, _extra_len = struct.unpack_from("<HH", header, 26)
                raw_name = raw_archive.read(name_len)
                if b"\\" in raw_name or b"\x00" in raw_name:
                    raise PackError("unsafe_path", "backslash or NUL archive path", info.filename)
                archive_name = info.filename[:-1] if info.is_dir() and info.filename.endswith("/") else info.filename
                path = _safe_rel(archive_name, limits)
                mode = (info.external_attr >> 16) & 0o170000
                if mode == stat.S_IFLNK:
                    raise PackError("symlink", "symbolic links are not allowed", path)
                if info.is_dir():
                    continue
                folded = path.casefold()
                if folded in seen:
                    raise PackError("case_collision", "paths collide on a case-insensitive filesystem", path)
                seen[folded] = path
                if info.flag_bits & 1:
                    raise PackError("encrypted", "encrypted archive entries are not supported", path)
                if info.file_size > limits["file_bytes"]:
                    raise PackError("file_too_large", "entry exceeds the decoded size limit", path)
                total += info.file_size
                if total > limits["total_bytes"]:
                    raise PackError("archive_too_large", "archive exceeds the decoded size limit")
                if len(files) >= limits["files"]:
                    raise PackError("too_many_files", "archive has too many files")
                if info.compress_size == 0:
                    if info.file_size:
                        raise PackError("compression_bomb", "non-empty entry has no compressed payload", path)
                elif info.file_size > info.compress_size * limits["ratio"]:
                    raise PackError("compression_bomb", "entry compression ratio exceeds the limit", path)
                files.append({"path": path, "size": info.file_size, "category": _category(path)})
            _check_prefix_collisions(files)
            corrupt = archive.testzip()
            if corrupt is not None:
                raise PackError("corrupt_archive", "archive CRC verification failed", corrupt)
    except (zipfile.BadZipFile, OSError) as exc:
        raise PackError("invalid_archive", "cannot read archive: {0}".format(exc)) from exc
    return files


def is_client_jar(kind: str, files: list[dict]) -> bool:
    """A Minecraft CLIENT jar (code + art), as opposed to a texture pack.

    A texture pack may legitimately replace only some files; a client jar
    ships every texture the game draws, so a jar missing one is damaged or
    modded and must not import as "OK" with silent bundled fallbacks
    (2026-10-30 audit F2)."""
    return any(f["path"].endswith(".class") for f in files)


def client_jar_textures(conversion_map: dict) -> list[str]:
    """Every pack texture a real b1.7.3.jar supplies: the conversion map's
    entries minus what CavEX composes (gui_2, anim) or supplies itself."""
    return sorted(dest for dest, spec in conversion_map.get("assets", {}).items()
                  if spec.get("strategy") not in ("composite_only", "bundled_only",
                                                  "animation_sheet"))


SIZE_HINT = ("CavEX takes standard 16x textures and HD packs at 32x, 64x or 128x "
             "(2x, 4x or 8x); 256x and odd sizes are not supported.")


def _folder_inventory(source: Path, limits: dict) -> list[dict]:
    files: list[dict] = []
    seen: dict[str, str] = {}
    total = 0
    walk_errors = []
    for root, dirnames, filenames in os.walk(source, topdown=True, followlinks=False,
                                             onerror=walk_errors.append):
        root_path = Path(root)
        for name in list(dirnames):
            candidate = root_path / name
            st = candidate.lstat()
            reparse = bool(getattr(st, "st_file_attributes", 0)
                           & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0))
            if candidate.is_symlink() or reparse:
                raise PackError("symlink", "symbolic links are not allowed", candidate.relative_to(source).as_posix())
        for name in filenames:
            candidate = root_path / name
            rel = _safe_rel(candidate.relative_to(source).as_posix(), limits)
            st = candidate.lstat()
            reparse = bool(getattr(st, "st_file_attributes", 0)
                           & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0))
            if stat.S_ISLNK(st.st_mode) or reparse:
                raise PackError("symlink", "symbolic links are not allowed", rel)
            if not stat.S_ISREG(st.st_mode):
                raise PackError("special_file", "only regular files are allowed", rel)
            folded = rel.casefold()
            if folded in seen:
                raise PackError("case_collision", "paths collide on a case-insensitive filesystem", rel)
            seen[folded] = rel
            if st.st_size > limits["file_bytes"]:
                raise PackError("file_too_large", "file exceeds the size limit", rel)
            total += st.st_size
            if total > limits["total_bytes"]:
                raise PackError("archive_too_large", "folder exceeds the size limit")
            if len(files) >= limits["files"]:
                raise PackError("too_many_files", "folder has too many files")
            files.append({"path": rel, "size": st.st_size, "category": _category(rel)})
    if walk_errors:
        raise PackError("folder_unreadable", "cannot inspect folder: {0}".format(walk_errors[0]))
    return files


def _source_info(source: os.PathLike | str, limits: dict) -> tuple[Path, str, list[dict]]:
    path = Path(source).expanduser()
    if not path.exists():
        raise PackError("source_missing", "source does not exist", str(path))
    if path.is_symlink():
        raise PackError("symlink", "source may not be a symbolic link", str(path))
    if path.is_dir():
        return path.resolve(), "folder", _folder_inventory(path, limits)
    if path.is_file():
        return path.resolve(), "zip", _zip_inventory(path, limits)
    raise PackError("invalid_source", "source must be a ZIP file or folder", str(path))


def _strip_wrapper(files: list[dict]) -> tuple[Optional[str], list[dict]]:
    useful = [f for f in files if f["category"] != "ignored" or f["path"].lower() == "pack.txt"]
    if not useful:
        return None, files
    first_parts = [f["path"].split("/", 1) for f in useful]
    asset_roots = {"gui", "mob", "armor", "item", "environment", "misc", "art", "font",
                   "sound", "newsound", "music", "newmusic", "streaming", "records",
                   "assets", "textures"}
    if (first_parts[0][0].casefold() not in asset_roots and
            all(len(parts) == 2 and parts[0].casefold() == first_parts[0][0].casefold()
                for parts in first_parts)):
        prefix = first_parts[0][0] + "/"
        adjusted = [dict(f, logical_path=f["path"][len(prefix):] if f["path"].startswith(prefix) else f["path"]) for f in files]
        return prefix, adjusted
    return None, [dict(f, logical_path=f["path"]) for f in files]


def inspect(source, *, resources_path=None, limits=None) -> dict:
    """Inspect a legacy pack without extracting or modifying it."""
    result = {"ok": False, "source": None, "format": None, "files": [],
              "warnings": [], "errors": [], "totals": {}}
    try:
        lim = _limits(limits)
        path, kind, files = _source_info(source, lim)
        wrapper, files = _strip_wrapper(files)
        logical = [f.get("logical_path", f["path"]).casefold() for f in files]
        modern = _modern_marker(logical)
        if modern:
            raise PackError("modern_format", MODERN_MESSAGE, modern)
        images = sum(f["category"] == "image" for f in files)
        audio = sum(f["category"] == "audio" for f in files)
        if images == 0 and audio == 0:
            raise PackError("no_legacy_assets", "no legacy textures or audio were found")
        result.update({
            "ok": True,
            "source": {"path": str(path), "kind": kind, "name": path.stem if kind == "zip" else path.name},
            "format": "legacy-java",
            "files": files,
            "totals": {"files": len(files), "bytes": sum(f["size"] for f in files),
                       "images": images, "audio": audio},
        })
        if wrapper:
            result["source"]["wrapper"] = wrapper
        if resources_path is not None:
            rpath, rkind, rfiles = _source_info(resources_path, lim)
            if rkind != "folder":
                raise PackError("resources_not_folder", "audio resources must be a folder")
            result["resources"] = {"path": str(rpath), "files": len(rfiles),
                                   "audio": sum(f["category"] == "audio" for f in rfiles)}
    except Exception as exc:
        result["ok"] = False
        result["errors"].append(_error(exc))
    return result


def _read_source(path: Path, kind: str, physical_path: str, limit: int) -> bytes:
    if kind == "folder":
        candidate = path / Path(*PurePosixPath(physical_path).parts)
        current = path
        for part in PurePosixPath(physical_path).parts:
            current = current / part
            st = current.lstat()
            reparse = bool(getattr(st, "st_file_attributes", 0)
                           & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0))
            if stat.S_ISLNK(st.st_mode) or reparse:
                raise PackError("symlink", "source changed to a link during preparation", physical_path)
        if not candidate.resolve().is_relative_to(path.resolve()):
            raise PackError("path_traversal", "source path escaped its root", physical_path)
        with candidate.open("rb") as stream:
            data = stream.read(limit + 1)
    else:
        with zipfile.ZipFile(path) as archive:
            with archive.open(physical_path) as stream:
                data = stream.read(limit + 1)
    if len(data) > limit:
        raise PackError("file_too_large", "decoded file exceeds the size limit", physical_path)
    return data


def _bundle_root() -> Path:
    return Path(__file__).resolve().parent / "resources"


def _beta_base() -> Path:
    """Beta-pack base: the bundle's beta-base/ when frozen or built, else the
    source checkout's packs/beta-base (EAD-005)."""
    bundled = _bundle_root() / "beta-base"
    if getattr(sys, "frozen", False) or bundled.is_dir():
        return bundled
    return Path(__file__).resolve().parents[2] / "packs" / "beta-base"


def _bundled_assets() -> Path:
    """CavEX's own art the HUD composite builds on: the bundle's assets/ when
    frozen or built, else the source checkout's assets/. Without this fallback
    a source-checkout run silently skipped the gui_2.png HUD composite."""
    bundled = _bundle_root() / "assets"
    if getattr(sys, "frozen", False) or bundled.is_dir():
        return bundled
    return Path(__file__).resolve().parents[2] / "assets"


def _default_config(name: str) -> Optional[Path]:
    """The shipped default settings (config_wii.json / config_pc.json): the
    bundle's defaults/ when frozen or built, else the source checkout's."""
    bundled = _bundle_root() / "defaults" / name
    if bundled.is_file():
        return bundled
    if getattr(sys, "frozen", False):
        return None
    repo = Path(__file__).resolve().parents[2] / name
    return repo if repo.is_file() else None


def _load_import_map(path: Optional[os.PathLike | str]) -> dict:
    map_path = Path(path) if path else _bundle_root() / "mapping.json"
    if not map_path.is_file():
        raise PackError("mapping_missing", "packaged importer mapping is missing: {0}".format(map_path))
    try:
        data = json.loads(map_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise PackError("mapping_invalid", "cannot read importer mapping: {0}".format(exc)) from exc
    if data.get("schema_version") != 1:
        raise PackError("mapping_version", "unsupported importer mapping version")
    return data


def _load_conversion(map_path):
    try:
        from . import conversion
    except ImportError as exc:
        raise PackError("converter_missing", "image converter is not installed") from exc
    try:
        mapping = conversion.load_map(map_path)
    except Exception as exc:
        raise PackError("mapping_invalid", str(exc)) from exc
    return conversion, mapping


def _emit(progress, phase: str, current: int, total: int, path: Optional[str] = None):
    if progress:
        progress({"phase": phase, "current": current, "total": total, "path": path})


# What a Beta client jar's pack is called, whatever the jar file was named
# (minecraft.jar used to show as "minecraft" in the Texture Packs list, and
# every import added another "b1.7.3-<hash>" folder; 2026-10-30 Wii run).
# The FOLDER has no space (a space is risky for the Wii rig, the stager and
# game paths; 2026-10-30) and matches the in-game importer's folder; the name
# players see, "Beta 1.7.3", is the manifest's display_name.
BETA_PACK_NAME = "Beta-1.7.3"
BETA_DISPLAY_NAME = "Beta 1.7.3"
# Same rules as the in-game importer (source/platform/beta_import.c, Coder A
# 2026-10-30): the manifest "name" is what the Texture Packs list shows
# (texturepacks_display_name). The previous import in the SAME folder under
# any spelling ("beta-1.7.3" is that folder on FAT) is replaced whole, never
# merged; one without the helper/importer manifest is kept, renamed
# "Beta-1.7.3-old" ("-old2", ...). Earlier imports under other names
# ("Beta 1.7.3", "minecraft", "b1.7.3", "b1.7.3-<hash>") are removed only when
# they carry the manifest, i.e. the helper wrote them.
BETA_REPLACE_RE = re.compile(r"^(beta 1\.7\.3|minecraft|b1\.7\.3(-[0-9a-f]{10})?)$",
                             re.IGNORECASE)
# Other leftovers of earlier helper versions (with the helper's manifest):
# listed, removed only on request.
OLD_BETA_PACK_RE = re.compile(r"^(b1\.7\.3-copy|minecraft)(-[0-9a-f]{10})?$", re.IGNORECASE)


def _pack_folder_name(name: str) -> str:
    if name in (BETA_PACK_NAME, BETA_DISPLAY_NAME):
        return BETA_PACK_NAME
    return _unique_name(name)


def _unique_name(name: str) -> str:
    clean = re.sub(r"[^A-Za-z0-9._-]+", "-", name).strip(".-") or "imported-pack"
    return clean[:48]


def _write_file(root: Path, rel: str, data: bytes) -> dict:
    target = root / Path(*PurePosixPath(rel).parts)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    return {"path": rel, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def _find_transcoder(explicit) -> Optional[Path]:
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    candidates.extend((_bundle_root() / "sound_transcode.exe", _bundle_root() / "sound_transcode"))
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    return None


def _prepare_audio(source_path: Path, source_kind: str, source_files: list[dict],
                   resources_path, staging: Path, import_map: dict, transcoder,
                   limits, warnings, written):
    """Adapt validated inputs to the established importer implementation."""
    accepted_roots = ("sound/", "newsound/", "music/", "newmusic/", "streaming/")
    inputs = [(source_path, source_kind, source_files)]
    if resources_path is not None:
        resource_root, resource_kind, resource_files = _source_info(resources_path, limits)
        if resource_kind != "folder":
            raise PackError("resources_invalid", "legacy audio resources must be a folder")
        inputs.append((resource_root, resource_kind, resource_files))
    with tempfile.TemporaryDirectory(prefix="cavewii-resources-") as td:
        provider = Path(td)
        staged_any = False
        for input_root, input_kind, entries in inputs:
            for entry in entries:
                logical = entry.get("logical_path", entry["path"])
                if entry["category"] != "audio" or not logical.casefold().startswith(accepted_roots):
                    continue
                data = _read_source(input_root, input_kind, entry["path"], limits["file_bytes"])
                target = provider / Path(*PurePosixPath(logical).parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.exists():
                    if target.read_bytes() != data:
                        warnings.append("An external audio resource replaced an embedded file at {0}.".format(logical))
                    else:
                        continue
                target.write_bytes(data)
                staged_any = True
        if not staged_any:
            warnings.append("No compatible owned audio was found; existing/default audio remains effective.")
            return

        native = _find_transcoder(transcoder)
        if native is None:
            warnings.append("Sound effects were not imported because the bundled native audio converter is unavailable; streamed music and records can still be imported.")
        from . import owned_importer as importer
        before = {p.relative_to(staging).as_posix() for p in staging.rglob("*") if p.is_file()}
        with _IMPORTER_LOCK:
            previous_mapping = importer.FROZEN_MAPPING
            importer.FROZEN_MAPPING = import_map
            try:
                results = []
                if native is not None:
                    results.extend(importer.process_sounds(provider, staging, False, native,
                                                          strict_transcode=True))
                results.extend(importer.process_music(provider, staging, False))
                results.extend(importer.process_records(provider, staging, False))
            finally:
                importer.FROZEN_MAPPING = previous_mapping
        statuses = []
        for row in results:
            statuses.append(row.get("status"))
            statuses.extend(variant.get("status") for variant in row.get("variants", ()))
        skipped = [status for status in statuses
                   if status in ("skipped_variant_overflow", "skipped_transcode_failed")]
        if skipped:
            warnings.append("Some sound effects were skipped because conversion failed or the engine variant limit was reached.")
        if "skipped_duplicate_name" in statuses:
            warnings.append("A duplicate music filename was skipped according to the legacy importer precedence rule.")
        for path in sorted(p for p in staging.rglob("*") if p.is_file()):
            rel = path.relative_to(staging).as_posix()
            if rel in before:
                continue
            data = path.read_bytes()
            written.append({"path": rel, "size": len(data),
                            "sha256": hashlib.sha256(data).hexdigest()})


NO_SOUNDS_MESSAGE = (
    "No Beta 1.7.3 sounds or music were found in {0}. Pick the \"resources\" folder "
    "inside your .minecraft folder (it holds sound, newsound, music and streaming). "
    "If it is missing, start Beta 1.7.3 once in the Minecraft Launcher, which "
    "creates it.")


def _materialize_legacy_index(index: Path, objects: Path, into: Path) -> int:
    """Copy the Beta audio a launcher asset index lists (pre-1.6: sound/,
    newsound/, music/, newmusic/, streaming/) from assets/objects into a
    resources-shaped folder, verifying each object's sha1 and size. The
    launcher does the same copy into .minecraft/resources when Beta starts."""
    try:
        entries = json.loads(index.read_text(encoding="utf-8")).get("objects", {})
    except (OSError, ValueError) as exc:
        raise PackError("resources_invalid", "cannot read launcher asset index {0}: {1}".format(index, exc))
    count = 0
    for rel, info in sorted(entries.items()):
        if not isinstance(info, dict) or _category(rel) != "audio":
            continue
        if not rel.casefold().startswith(tuple(kind + "/" for kind in _discovery().AUDIO_DIRS)):
            continue
        digest = str(info.get("hash", ""))
        if not re.fullmatch(r"[0-9a-f]{40}", digest):
            continue
        source = objects / digest[:2] / digest
        if not source.is_file() or source.is_symlink():
            continue
        data = source.read_bytes()
        if hashlib.sha1(data).hexdigest() != digest or len(data) != info.get("size", len(data)):
            continue
        target = into / Path(*PurePosixPath(_safe_rel(rel, DEFAULT_LIMITS)).parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        count += 1
    return count


def _discovery():
    from . import discovery
    return discovery


def _resolve_resources(picked, scratch: Path):
    """Where the Beta sounds really are for what the player picked: the
    folder itself, the 'resources' folder inside/next to a picked .minecraft
    (discovery.find_resources_folder), or the launcher's asset objects when
    resources/ was never created. None when there are none. Picking
    .minecraft used to die on "launcher_log.txt exceeds the size limit"."""
    discovery = _discovery()
    picked = Path(picked).expanduser()
    if not picked.exists():
        raise PackError("source_missing", "the sounds folder does not exist", str(picked))
    found = discovery.find_resources_folder(picked)
    if found is not None:
        return Path(found), None
    legacy = discovery.find_legacy_index(picked)
    if legacy is not None:
        target = scratch / "resources"
        target.mkdir(parents=True, exist_ok=True)
        if _materialize_legacy_index(legacy[0], legacy[1], target):
            return target, "Sounds were read from the Minecraft Launcher's asset store ({0}).".format(legacy[0])
    return None, None


# Tests set this so a synthetic jar never picks up the developer's own
# .minecraft through the launcher-location fallback below.
NO_LAUNCHER_SEARCH_ENV = "CAVEWII_PACK_HELPER_NO_LAUNCHER_SEARCH"


def _auto_resources(jar: Path, scratch: Path):
    """Beta sounds for a jar when the player chose no sounds folder: first
    beside the jar (.minecraft/versions/b1.7.3/b1.7.3.jar -> .minecraft/
    resources, or the launcher's asset store there), then the standard
    launcher location on this computer (a jar copied to the Desktop)."""
    discovery = _discovery()
    found, note = _resolve_resources(jar, scratch / "near")
    if found is not None:
        return found, note or "Sounds and music were found in {0}.".format(found)
    if os.environ.get(NO_LAUNCHER_SEARCH_ENV):
        return None, None
    for root in discovery.minecraft_roots():
        found, note = _resolve_resources(root, scratch / "launcher")
        if found is not None:
            return found, note or "Sounds and music were found in {0}.".format(found)
    return None, None


def prepare(source, destination, look="crisp", progress=None, *, resources_path=None,
            transcoder=None, mapping_path=None, import_mapping_path=None, limits=None) -> dict:
    """Convert *source* into a new, immutable prepared-pack directory."""
    with tempfile.TemporaryDirectory(prefix="cavewii-sounds-") as scratch:
        notes = []
        try:
            picked_source = Path(source).expanduser()
            # Audio-only: the player picked a sounds location as the source.
            texture_markers = ("terrain.png", "particles.png", "pack.txt", "gui", "mob", "item",
                               "armor", "environment", "misc", "art", "font", "terrain", "title")
            if (picked_source.is_dir() and not _discovery().has_audio_dirs(picked_source)
                    and not any((picked_source / name).exists() for name in texture_markers)):
                found, note = _resolve_resources(picked_source, Path(scratch) / "src")
                launcher_root = any((picked_source / name).exists() for name in
                                    ("versions", "assets", "resources", "launcher_profiles.json"))
                if found is None and launcher_root:
                    raise PackError("no_sounds_found", NO_SOUNDS_MESSAGE.format(picked_source),
                                    str(picked_source))
                if found is not None:
                    source = found
                    if note:
                        notes.append(note)
            if resources_path is None and picked_source.is_file():
                # No sounds folder chosen: look where the launcher keeps Beta's
                # sounds for this jar (2026-10-30 real-Wii run: the real
                # .minecraft/versions/b1.7.3/b1.7.3.jar imported NO audio
                # because nothing searched .minecraft/resources).
                found, note = _auto_resources(picked_source, Path(scratch) / "auto")
                if found is not None:
                    resources_path = found
                    notes.append(note)
            if resources_path is not None:
                found, note = _resolve_resources(resources_path, Path(scratch) / "res")
                if found is None:
                    # Textures still import; the player is told plainly.
                    notes.append(NO_SOUNDS_MESSAGE.format(Path(resources_path).expanduser())
                                 + " Your textures were imported; sounds stay as they were.")
                    resources_path = None
                else:
                    resources_path = found
                    if note:
                        notes.append(note)
        except PackError as exc:
            return {"ok": False, "path": None, "name": None, "files": [], "warnings": [],
                    "errors": [_error(exc)], "hashes": {}, "manifest": None}
        result = _prepare(source, destination, look, progress, resources_path=resources_path,
                          transcoder=transcoder, mapping_path=mapping_path,
                          import_mapping_path=import_mapping_path, limits=limits)
        result["warnings"] = notes + result["warnings"]
        return result


def _prepare(source, destination, look="crisp", progress=None, *, resources_path=None,
             transcoder=None, mapping_path=None, import_mapping_path=None, limits=None) -> dict:
    result = {"ok": False, "path": None, "name": None, "files": [], "warnings": [],
              "errors": [], "hashes": {}, "manifest": None}
    staging = None
    try:
        if look not in ("crisp", "smooth"):
            raise PackError("invalid_look", "look must be 'crisp' or 'smooth'")
        lim = _limits(limits)
        scan = inspect(source, resources_path=resources_path, limits=lim)
        if not scan["ok"]:
            first = scan["errors"][0]
            raise PackError(first["code"], first["message"], first.get("path"))
        destination = Path(destination).expanduser()
        _reject_reparse(destination, "preparation destination")
        source_path = Path(scan["source"]["path"])
        if destination.resolve() == source_path or (source_path.is_dir() and destination.resolve().is_relative_to(source_path)):
            raise PackError("destination_inside_source", "destination must be separate from the source")
        destination.mkdir(parents=True, exist_ok=True)
        if not destination.is_dir():
            raise PackError("destination_invalid", "preparation destination is not a folder")
        if getattr(sys, "frozen", False):
            for required in ("mapping.json", "conversion_map.json", "assets/gui_2.png",
                             "defaults/config_wii.json", "defaults/config_pc.json",
                             "beta-base/anim.png", "beta-base/pointer.png"):
                if not (_bundle_root() / required).is_file():
                    raise PackError("bundle_incomplete", "Reinstall the CavEX Asset Importer: missing bundled " + required)
        conversion, conversion_map = _load_conversion(mapping_path)
        assets = conversion_map.get("assets", {})
        by_source = {}
        reserved_sources = set()
        for dest, spec in assets.items():
            if spec.get("strategy") in ("composite_only", "bundled_only"):
                reserved_sources.update(name.casefold() for name in spec.get("source_names", [dest]))
                continue
            for source_name in spec.get("source_names", [dest]):
                by_source[source_name.casefold()] = dest
        parent = destination.resolve()
        staging = Path(tempfile.mkdtemp(prefix=".cavewii-prepare-", dir=parent))
        written = []
        report_files = []
        selected = []
        for entry in scan["files"]:
            logical = entry.get("logical_path", entry["path"])
            dest = by_source.get(logical.casefold())
            if dest:
                selected.append((entry, logical, dest))
        audio_only = not selected and scan["totals"]["audio"] > 0
        if not selected and not audio_only:
            raise PackError("no_supported_assets", "no supported legacy texture paths were found")
        # A client jar ships every texture the game draws. One that lacks any
        # is damaged or modded: refuse it by name instead of importing "OK"
        # with bundled art in the gaps (2026-10-30 audit F2).
        if is_client_jar(scan["source"]["kind"], scan["files"]):
            present = {dest for _entry, _logical, dest in selected}
            missing = [dest for dest in client_jar_textures(conversion_map) if dest not in present]
            if missing:
                shown = ", ".join(missing[:6]) + (" and {0} more".format(len(missing) - 6)
                                                  if len(missing) > 6 else "")
                raise PackError(
                    "incomplete_jar",
                    "This Minecraft jar is missing {0} texture file(s) the game needs: {1}. "
                    "It is damaged or modded. Pick an unmodified b1.7.3.jar (the Minecraft "
                    "Launcher re-downloads it if you start Beta 1.7.3 once). Nothing was "
                    "installed.".format(len(missing), shown), missing[0])
        # Every image the pack DOES supply must convert. A corrupt, odd-sized
        # or unsupported-scale (e.g. 256x) file used to be swapped for CavEX's
        # own art while the import reported success, so the player saw the
        # wrong textures with no explanation (2026-10-30 audit F5/F8). It now
        # stops the import and names the file and the reason.
        for index, (entry, logical, dest) in enumerate(selected, 1):
            _emit(progress, "convert", index, len(selected), logical)
            data = _read_source(source_path, scan["source"]["kind"], entry["path"], lim["file_bytes"])
            try:
                converted = conversion.convert_image(data, dest, look,
                                                     conversion_map=conversion_map)
            except Exception as exc:
                code = getattr(exc, "code", "conversion_failed")
                hint = (" " + SIZE_HINT) if code in ("unsupported_dimensions",
                                                     "decoded_too_large") else ""
                if code == "invalid_png":
                    hint = " The file is damaged or is not a real PNG image."
                raise PackError(code, "{0} cannot be used: {1}.{2} Nothing was installed.".format(
                    logical, str(exc).rstrip("."), hint), logical) from exc
            record = _write_file(staging, dest, converted)
            record.update({"source": logical, "category": "texture", "status": "converted"})
            written.append(record)
            report_files.append(record)
        # Optional pack text belongs to the player's jar, with no decoding or
        # newline conversion. Including it in the manifest makes install copy it.
        for entry in scan["files"]:
            logical = entry.get("logical_path", entry["path"])
            if not audio_only and logical.casefold() == "title/splashes.txt":
                data = _read_source(source_path, scan["source"]["kind"],
                                    entry["path"], lim["file_bytes"])
                record = _write_file(staging, "title/splashes.txt", data)
                record.update({"source": logical, "category": "text", "status": "copied"})
                written.append(record)
                report_files.append(record)
        # Images the game never reads (title screens, achievement art, mod
        # sprites...) are listed per file but no longer raise a warning: the
        # real b1.7.3.jar always produced "22 unsupported layouts", which read
        # like a fault on every successful import (2026-10-30 audit).
        for entry in scan["files"]:
            if entry["category"] != "image":
                continue
            logical = entry.get("logical_path", entry["path"])
            if logical.casefold() in by_source:
                continue
            status = "reserved" if logical.casefold() in reserved_sources else "unused"
            report_files.append({"path": logical, "category": "texture", "status": status})
        selected_destinations = {dest for _entry, _logical, dest in selected}
        for dest, spec in assets.items():
            if audio_only:
                continue
            if dest in selected_destinations or spec.get("strategy") == "composite_only":
                continue
            status = "missing_required" if spec.get("required") else "fallback"
            report_files.append({"path": dest, "category": "texture", "status": status,
                                 "fallback": spec.get("fallback", "bundled")})
            if spec.get("required"):
                raise PackError("missing_required", "required legacy texture is missing", dest)

        # These two sheets preserve CavEX-owned controller/animation pixels
        # and overlay only the documented regions from the player's artwork.
        converted_by_path = {row["path"]: staging / Path(*PurePosixPath(row["path"]).parts)
                             for row in written}
        bundled_assets = _bundled_assets()
        if all(key in converted_by_path for key in ("gui/gui.png", "gui/icons.png")):
            base = bundled_assets / "gui_2.png"
            if not base.is_file():
                raise PackError("bundle_incomplete",
                                "missing CavEX base gui_2.png for the HUD -- reinstall the "
                                "CavEX Asset Importer (from source: python "
                                "tools/pack_helper/build_resources.py --out "
                                "tools/pack_helper/resources)")
            if base.is_file():
                data = conversion.compose_gui2(base.read_bytes(),
                                               converted_by_path["gui/gui.png"].read_bytes(),
                                               converted_by_path["gui/icons.png"].read_bytes())
                row = _write_file(staging, "gui_2.png", data)
                row.update({"category": "texture", "status": "composited",
                            "source": "gui/gui.png + gui/icons.png"})
                written.append(row)
                report_files.append(row)
        # Beta-format packs take Beta's own animated-texture colours and the
        # Homebrew pointer from beta-base/ (packs/beta-base in the repo), never
        # from assets/, which is the Earth & Ash default since 2026-10-11.
        beta_base = _beta_base()
        for required in ("anim.png", "pointer.png"):
            if not (beta_base / required).is_file():
                raise PackError("bundle_incomplete",
                                "missing Beta pack base " + required + " -- a Beta pack would "
                                "inherit Earth & Ash art; reinstall the CavEX Asset Importer")
        if "terrain.png" in converted_by_path and "anim.png" not in converted_by_path:
            base = beta_base / "anim.png"
            if base.is_file():
                data = conversion.compose_anim(base.read_bytes(), converted_by_path["terrain.png"].read_bytes())
                row = _write_file(staging, "anim.png", data)
                row.update({"category": "texture", "status": "composited", "source": "terrain.png"})
                written.append(row)
                report_files.append(row)
        # An audio-only pack keeps the installed texture selection (no texture rows).
        if "terrain.png" in converted_by_path:
            scene_rows = []
            _emit(progress, "convert", 0, 1, "Nether loading scene")
            try:
                scene = nether_scene.compose((staging / "terrain.png").read_bytes(),
                                             (staging / "anim.png").read_bytes())
                for rel, data in zip(nether_scene.FILES, scene):
                    row = _write_file(staging, rel, data)
                    row.update({"category": "texture" if rel.endswith(".png") else "text",
                                "status": "composited", "source": "terrain.png + anim.png"})
                    scene_rows.append(row)
            except Exception as exc:
                # This optional illustration must never make a usable pack fail.
                # Remove both outputs, including a partially written file, before
                # the normal pack transaction creates its manifest.
                for rel in nether_scene.FILES:
                    (staging / rel).unlink(missing_ok=True)
                result["warnings"].append("Nether loading scene skipped: {0}. "
                                          "All other assets will still be imported; "
                                          "the game can use tiled netherrack.".format(exc))
            else:
                written.extend(scene_rows)
                report_files.extend(scene_rows)
        if not audio_only and (beta_base / "pointer.png").is_file():
            row = _write_file(staging, "pointer.png", (beta_base / "pointer.png").read_bytes())
            row.update({"category": "texture", "status": "beta_base", "source": "beta-base/pointer.png"})
            written.append(row)
            report_files.append(row)
        if transcoder is not None:
            transcoder_path = Path(transcoder).expanduser().resolve()
            forbidden_roots = [source_path]
            if resources_path is not None:
                forbidden_roots.append(Path(resources_path).expanduser().resolve())
            if any(transcoder_path == root or transcoder_path.is_relative_to(root)
                   for root in forbidden_roots):
                raise PackError("unsafe_transcoder", "the native converter may not come from pack input")
        embedded_audio = any(entry["category"] == "audio" for entry in scan["files"])
        default_import_map = _bundle_root() / "mapping.json"
        needs_import_map = resources_path is not None or embedded_audio
        if import_mapping_path is not None or default_import_map.is_file() or needs_import_map:
            import_map = _load_import_map(import_mapping_path)
        else:
            import_map = {"sound_keys": [], "sound_max_variants": 8,
                          "required_textures": []}
        missing_required = [path for path in import_map.get("required_textures", ())
                            if not audio_only
                            if path not in selected_destinations]
        if missing_required:
            raise PackError("missing_required", "required legacy texture is missing",
                            missing_required[0])
        before_audio = len(written)
        _emit(progress, "audio", 0, 0)
        _prepare_audio(source_path, scan["source"]["kind"], scan["files"], resources_path,
                       staging, import_map, transcoder, lim, result["warnings"], written)
        for row in written[before_audio:]:
            row.update({"category": "audio", "status": "transcoded"})
            report_files.append(row)
        if not written:
            raise PackError("no_supported_assets", "No compatible textures or audio could be imported.")
        name = (BETA_PACK_NAME if is_client_jar(scan["source"]["kind"], scan["files"])
                else _unique_name(scan["source"]["name"]))
        manifest = {
            "schema_version": SCHEMA_VERSION,
            "name": BETA_DISPLAY_NAME if name == BETA_PACK_NAME else name,
            "look": look,
            "source": {"name": scan["source"]["name"], "kind": scan["source"]["kind"]},
            "files": [{k: row[k] for k in ("path", "size", "sha256")} for row in written],
        }
        manifest_bytes = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
        (staging / MANIFEST_NAME).write_bytes(manifest_bytes)
        final = parent / (_unique_name(name) + "-" + uuid.uuid4().hex[:10])
        os.replace(staging, final)
        staging = None
        result.update({"ok": True, "path": str(final), "name": name, "files": report_files,
                       "hashes": {r["path"]: r["sha256"] for r in written},
                       "manifest": manifest})
        _emit(progress, "prepared", len(written), len(written), str(final))
    except Exception as exc:
        result["errors"].append(_error(exc))
    finally:
        if staging is not None:
            shutil.rmtree(staging, ignore_errors=True)
    return result


def _read_manifest(prepared: Path) -> dict:
    try:
        manifest = json.loads((prepared / MANIFEST_NAME).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise PackError("prepared_invalid", "prepared pack manifest is missing or invalid") from exc
    if manifest.get("schema_version") != SCHEMA_VERSION or not isinstance(manifest.get("files"), list):
        raise PackError("prepared_invalid", "prepared pack manifest version is unsupported")
    return manifest


def _verify_prepared(prepared: Path, manifest: dict):
    for entry in prepared.rglob("*"):
        _reject_reparse(entry, "prepared file or folder")
    expected = set()
    total = 0
    if len(manifest["files"]) > DEFAULT_LIMITS["files"]:
        raise PackError("prepared_invalid", "prepared pack has too many files")
    for row in manifest["files"]:
        rel = _safe_rel(row.get("path", ""), DEFAULT_LIMITS)
        if rel.casefold() in expected:
            raise PackError("prepared_invalid", "prepared manifest contains duplicate paths", rel)
        expected.add(rel.casefold())
        path = prepared / Path(*PurePosixPath(rel).parts)
        if not path.is_file() or path.is_symlink():
            raise PackError("prepared_changed", "prepared file is missing", rel)
        declared_size = row.get("size")
        if not isinstance(declared_size, int) or declared_size < 0 or declared_size > DEFAULT_LIMITS["file_bytes"]:
            raise PackError("prepared_invalid", "prepared file size is invalid", rel)
        total += declared_size
        if total > DEFAULT_LIMITS["total_bytes"]:
            raise PackError("prepared_invalid", "prepared pack exceeds the size limit")
        with path.open("rb") as stream:
            data = stream.read(declared_size + 1)
        if len(data) != declared_size:
            raise PackError("prepared_changed", "prepared file size does not match", rel)
        digest = hashlib.sha256(data).hexdigest()
        if digest != row.get("sha256"):
            raise PackError("prepared_changed", "prepared file hash does not match", rel)
    entries = list(prepared.rglob("*"))
    if any(p.is_symlink() for p in entries):
        raise PackError("prepared_changed", "prepared directory contains a symbolic link")
    actual = {p.relative_to(prepared).as_posix().casefold() for p in entries if p.is_file()}
    if actual != expected | {MANIFEST_NAME.casefold()}:
        raise PackError("prepared_changed", "prepared directory contains unverified files")


def _boot_path(sd_root: Path) -> Optional[Path]:
    for relative in ("apps/CavEX/boot.dol", "CavEX-ReCrafted.exe", "CavEX-ReCrafted",
                     "cavewii_v2.exe", "cavewii_v2"):
        candidate = sd_root / relative
        if candidate.is_file() and not candidate.is_symlink():
            return candidate
    return None


def validate_destination(destination, select=True) -> Path:
    """Read-only preflight shared by the desktop flow and final installation."""
    root_input = Path(destination).expanduser()
    _reject_reparse(root_input, "CavEX location")
    if not root_input.is_dir():
        raise PackError("sd_root_missing", "Choose an existing SD card or CavEX folder.")
    root = root_input.resolve()
    boot = _boot_path(root)
    if boot is None:
        raise PackError("game_not_installed",
                        "Put CavEX on the SD card first: copy the release zip's files to the "
                        "card's top folder (so sd:/apps/CavEX/boot.dol exists), then choose "
                        "that SD card here. For the PC game, choose the folder with "
                        "CavEX-ReCrafted.exe and config.json.")
    if select:
        with boot.open("rb") as game:
            audio_capable = b"paths.audiopack\x00" in game.read(32 * 1024 * 1024)
        if not audio_capable:
            raise PackError("game_update_required",
                            "Update CavEX before activating imported assets. This build cannot "
                            "keep textures and audio separate. You can still import without activating in Import options.")
    return root


def _reject_reparse(path: Path, label: str):
    if not path.exists() and not path.is_symlink():
        return
    st = path.lstat()
    reparse = bool(getattr(st, "st_file_attributes", 0)
                   & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0))
    if path.is_symlink() or reparse:
        raise PackError("unsafe_destination", label + " may not be a link or reparse point", str(path))


def _decode_config(raw: bytes) -> dict:
    value = json.loads(raw.decode("utf-8-sig"))
    if not isinstance(value, dict):
        raise ValueError("root must be an object")
    return value


# The helper's OWN backup name. `config.json.bak` is a generic name anyone --
# the player, another tool, an editor -- may already be using; the helper used
# to overwrite it on every install and destroy whatever was in it (2026-09-17
# review, finding M5). The helper now only ever writes a file it named itself,
# and still READS the generic one when recovering an unparseable config.
HELPER_BACKUP_SUFFIX = ".pack-helper.bak"
LEGACY_BACKUP_SUFFIX = ".bak"
# Where an unparseable config.json is kept before a backup replaces it.
CORRUPT_COPY_SUFFIX = ".pack-helper.corrupt"


def _helper_backup_path(path: Path) -> Path:
    return path.with_name(path.name + HELPER_BACKUP_SUFFIX)


def _usable_backup(value: object) -> bool:
    """Is this decoded backup plausibly a CavEX config, not just any JSON?

    Adopting "an object" was too weak: an unrelated .bak that happens to be a
    JSON object would silently replace the player's settings (2026-09-17 DELTA
    review, MEDIUM). A CavEX config carries a `paths` object; require it.
    """
    return isinstance(value, dict) and isinstance(value.get("paths"), dict)


def _load_config(path: Path) -> tuple[dict, Optional[bytes], bool]:
    _reject_reparse(path, "config.json")
    if not path.exists():
        return {}, None, False
    raw = path.read_bytes()
    try:
        value = _decode_config(raw)
    except (UnicodeError, ValueError) as exc:
        # The original is about to be superseded. Keep it FIRST: it is the only
        # copy of whatever the player had, and the recovery path used to
        # destroy it outright. Failing to keep it aborts the recovery.
        corrupt_copy = path.with_name(path.name + CORRUPT_COPY_SUFFIX)
        _reject_reparse(corrupt_copy, "corrupt configuration copy")
        try:
            corrupt_copy.write_bytes(raw)
        except OSError as write_exc:
            raise PackError(
                "config_invalid",
                "config.json is invalid and a copy of it could not be kept; pack installed but selection was not changed",
            ) from write_exc
        # Recovery reads (never writes) both names, the helper's own first.
        for suffix in (HELPER_BACKUP_SUFFIX, LEGACY_BACKUP_SUFFIX):
            backup = path.with_name(path.name + suffix)
            _reject_reparse(backup, "configuration backup")
            if backup.is_file():
                try:
                    recovered = _decode_config(backup.read_bytes())
                except (OSError, UnicodeError, ValueError):
                    continue
                if _usable_backup(recovered):
                    return recovered, raw, True
        raise PackError("config_invalid", "config.json is invalid and no valid backup is available; pack installed but selection was not changed") from exc
    return value, raw, False


def config_targets(root: Path) -> list[tuple[Path, str]]:
    """Every settings file the game can load at this location, with the
    shipped default each one falls back to (main.c: config_load_user_or_default
    reads "config.json" from the working folder, else config_wii.json /
    config_pc.json beside it).

    A Homebrew Channel launch passes argv[0] = sd:/apps/CavEX/boot.dol,
    and libfat's fatInitDefault then makes the APP folder the working folder;
    a launcher without arguments leaves the card root. The release therefore
    ships config.json in both places (docs/release-install.md section 2), and
    the helper must switch the pack in both: writing only sd:/config.json left
    every HBC launch on the default art while the helper said "done"
    (2026-10-30 audit F1). The app-folder file comes first: it is the one an
    HBC launch reads. Pack paths stay "texturepacks/<name>": on the Wii
    config_read_path resolves a relative path from sd:/ (util.c
    path_from_sd_root), whichever folder the config was read from."""
    app_boot = root / "apps" / "CavEX" / "boot.dol"
    if app_boot.is_file() and not app_boot.is_symlink():
        return [(root / "apps" / "CavEX" / "config.json", "config_wii.json"),
                (root / "config.json", "config_wii.json")]
    return [(root / "config.json", "config_pc.json")]


def _template_config(config_path: Path, default_name: str) -> tuple[dict, str]:
    """Starting point for a missing config.json: never a file holding only the
    pack path (that drops every control mapping -- audit F9). Prefer the
    default shipped beside it, then the bundled copy of the same default."""
    for candidate in (config_path.with_name(default_name), _default_config(default_name)):
        if candidate is None or not candidate.is_file() or candidate.is_symlink():
            continue
        try:
            value = _decode_config(candidate.read_bytes())
        except (OSError, UnicodeError, ValueError):
            continue
        return value, str(candidate)
    raise PackError("default_config_missing",
                    "CavEX's settings file {0} is missing and no default settings file "
                    "({1}) was found to start it from. Copy the game's files onto the card "
                    "again, then run the import again.".format(config_path, default_name),
                    str(config_path))


def _read_only(path: Path) -> bool:
    try:
        st = path.stat()
    except FileNotFoundError:
        return False
    return not (st.st_mode & stat.S_IWRITE) or not os.access(path, os.W_OK)


def _read_only_error(path: Path) -> PackError:
    return PackError(
        "config_read_only",
        "CavEX's settings file is read-only, so the new pack could not be switched on: {0}. "
        "On your PC, right-click that config.json, choose Properties, untick Read-only, "
        "then run the import again. If it is on the SD card, also check the card's lock "
        "switch.".format(path), str(path))


def _plan_select(config_path: Path, default_name: str, pack_value: str, audio_select: bool,
                 texture_select=True) -> dict:
    _reject_reparse(config_path, "config.json")
    if config_path.exists() and _read_only(config_path):
        raise _read_only_error(config_path)
    if config_path.exists():
        config, original, recovered = _load_config(config_path)
        template = None
    else:
        config, template = _template_config(config_path, default_name)
        original, recovered = None, False
    paths = config.get("paths")
    if paths is None:
        paths = {}
        config["paths"] = paths
    if not isinstance(paths, dict):
        raise PackError("config_invalid", "config.json paths must be an object")
    previous_texture = paths.get("texturepack", "assets")
    if not isinstance(previous_texture, str):
        previous_texture = "assets"
    if texture_select and not isinstance(paths.get("audiopack"), str):
        paths["audiopack"] = previous_texture
    if texture_select:
        paths["texturepack"] = pack_value
    if audio_select:
        paths["audiopack"] = pack_value
    payload = (json.dumps(config, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    backup = _helper_backup_path(config_path)
    _reject_reparse(backup, "configuration backup")
    if backup.exists() and not backup.is_file():
        raise PackError("config_backup_blocked",
                        "the helper's backup name is not a regular file; settings were not changed",
                        str(backup))
    return {"path": config_path, "payload": payload, "original": original,
            "recovered": recovered, "template": template, "backup": backup,
            "previous_texture": previous_texture, "paths": paths}


def _commit_select(plan: dict) -> dict:
    config_path, original, backup = plan["path"], plan["original"], plan["backup"]
    config_path.parent.mkdir(parents=True, exist_ok=True)
    temp = config_path.with_name(config_path.name + ".pack-helper-" + uuid.uuid4().hex + ".tmp")
    try:
        with temp.open("xb") as stream:
            stream.write(plan["payload"])
            stream.flush()
            os.fsync(stream.fileno())
        current = config_path.read_bytes() if config_path.exists() else None
        if current != original:
            raise PackError("config_changed", "config.json changed during installation; selection was not changed")
        if original is not None and not plan["recovered"]:
            backup_temp = backup.with_name(backup.name + ".tmp-" + uuid.uuid4().hex)
            backup_temp.write_bytes(original)
            os.replace(backup_temp, backup)
        try:
            os.replace(temp, config_path)
        except PermissionError as exc:
            raise _read_only_error(config_path) from exc
    finally:
        if temp.exists():
            temp.unlink()
    paths, previous = plan["paths"], plan["previous_texture"]
    return {"path": str(config_path), "backup": str(backup) if original is not None else None,
            "created_from": plan["template"],
            "previous_texturepack": previous, "texturepack": paths.get("texturepack", "assets"),
            "audiopack": paths.get("audiopack", previous), "recovered_from_backup": plan["recovered"],
            "corrupt_copy": str(config_path.with_name(config_path.name + CORRUPT_COPY_SUFFIX))
            if plan["recovered"] else None}


def _atomic_select(config_path: Path, pack_value: str, audio_select: bool, texture_select=True,
                   default_name: str = "config_wii.json") -> dict:
    return _commit_select(_plan_select(config_path, default_name, pack_value, audio_select,
                                       texture_select))


def _select_everywhere(root: Path, pack_value: str, audio_select: bool, texture_select: bool) -> list:
    """Plan every config first (read, parse, read-only check), so one bad
    file changes none of them; then write each atomically."""
    plans = [_plan_select(path, default, pack_value, audio_select, texture_select)
             for path, default in config_targets(root)]
    return [_commit_select(plan) for plan in plans]


def old_beta_imports(sd_root, keep: Optional[str] = None) -> list[str]:
    """Earlier helper versions' Beta jar imports (texturepacks/b1.7.3-<hash>,
    texturepacks/minecraft, ...): only folders carrying the helper's own
    manifest, never the current one (*keep*) and never a player's folder."""
    packs = Path(sd_root) / "texturepacks"
    if not packs.is_dir():
        return []
    found = []
    for entry in sorted(packs.iterdir()):
        if entry.name == keep or entry.is_symlink() or not entry.is_dir():
            continue
        if OLD_BETA_PACK_RE.match(entry.name) and (entry / MANIFEST_NAME).is_file():
            found.append(entry.name)
    return found


def remove_old_imports(sd_root, names) -> list[str]:
    """Delete the named old helper imports (as listed by old_beta_imports);
    anything else is refused. Returns what was removed."""
    allowed = set(old_beta_imports(sd_root))
    removed = []
    for name in names:
        if name not in allowed:
            raise PackError("not_an_old_import", "not an earlier helper import; left alone", name)
        target = Path(sd_root) / "texturepacks" / name
        _reject_reparse(target, "old pack folder")
        shutil.rmtree(target)
        removed.append(name)
    return removed


def install(prepared, sd_root, select=True, *, progress=None, audio_select=False,
            remove_old=False) -> dict:
    """Publish a prepared pack, then optionally select it in SD-root config."""
    result = {"ok": False, "path": None, "pack_name": None, "selected": False,
              "instructions": [], "warnings": [], "errors": [], "config": None}
    staging = None
    try:
        if isinstance(prepared, dict):
            if not prepared.get("ok") or not prepared.get("path"):
                raise PackError("prepared_invalid", "prepare result is not successful")
            prepared = prepared["path"]
        prepared_input = Path(prepared).expanduser()
        if not prepared_input.is_dir() or prepared_input.is_symlink():
            raise PackError("prepared_invalid", "prepared pack directory does not exist")
        prepared_path = prepared_input.resolve()
        manifest = _read_manifest(prepared_path)
        _verify_prepared(prepared_path, manifest)
        has_audio = any(row.get("path", "").startswith(("sound/", "music/", "records/"))
                        for row in manifest["files"])
        has_textures = any(row.get("path", "").endswith(".png") for row in manifest["files"])
        audio_select_effective = bool(audio_select and has_audio)
        if audio_select and not has_audio:
            result["warnings"].append("This prepared pack has no imported audio; the previous effective audio selection was kept.")
        try:
            root = validate_destination(sd_root, select)
        except PackError as exc:
            result["instructions"] = [str(exc)]
            raise
        packs = root / "texturepacks"
        _reject_reparse(packs, "texturepacks directory")
        packs.mkdir(exist_ok=True)
        base = _pack_folder_name(manifest.get("name", prepared_path.name))
        # One folder per pack name: importing the same jar again REPLACES the
        # helper's earlier copy instead of piling up b1.7.3-<random> folders
        # (2026-10-30 audit). A same-named folder the helper did not write (no
        # helper manifest) is never touched; that import gets a unique name.
        target = packs / base
        _reject_reparse(target, "existing pack folder")
        replace_existing = False
        beta_replaced = []
        if base == BETA_PACK_NAME:
            for entry in sorted(packs.iterdir()):
                same = entry.name.casefold() == BETA_PACK_NAME.casefold()
                if not (same or BETA_REPLACE_RE.match(entry.name)):
                    continue
                _reject_reparse(entry, "existing pack folder")
                if not entry.is_dir():
                    continue
                if (entry / MANIFEST_NAME).is_file():
                    beta_replaced.append(entry)
                elif same:
                    # Not ours: keep the player's files, out of the way.
                    for index in range(1, 1000):
                        aside = packs / (BETA_PACK_NAME + ("-old" if index == 1 else "-old%d" % index))
                        if not aside.exists():
                            break
                    os.replace(entry, aside)
                    result["kept_as"] = aside.name
        elif target.exists():
            if target.is_dir() and (target / MANIFEST_NAME).is_file():
                replace_existing = True
            else:
                target = packs / (base + "-" + uuid.uuid4().hex[:10])
        for config_path, _default in config_targets(root):
            _reject_reparse(config_path, "config.json")
        remnants = list(root.glob(".cavewii-install-*"))
        if remnants:
            result["warnings"].append("A previous private install staging directory remains; it is not visible to CavEX and was left untouched.")
        # The staging directory lives beside texturepacks, not inside it, so
        # the game's directory scanner cannot expose an interrupted copy.
        staging = Path(tempfile.mkdtemp(prefix=".cavewii-install-", dir=root))
        rows = manifest["files"]
        for index, row in enumerate(rows, 1):
            _emit(progress, "install", index, len(rows), row["path"])
            rel_path = Path(*PurePosixPath(row["path"]).parts)
            source_file = prepared_path / rel_path
            target_file = staging / rel_path
            target_file.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source_file, target_file)
            with target_file.open("rb+") as copied:
                os.fsync(copied.fileno())
            if hashlib.sha256(target_file.read_bytes()).hexdigest() != row["sha256"]:
                raise PackError("copy_verify_failed", "installed file hash does not match", row["path"])
        shutil.copyfile(prepared_path / MANIFEST_NAME, staging / MANIFEST_NAME)
        if beta_replaced:
            # Every earlier Beta import goes aside first (one of them may BE
            # the target on a case-insensitive card), then the new pack is
            # published, then the old copies are deleted. On failure the old
            # folders come back.
            retired = []
            try:
                for entry in beta_replaced:
                    aside = root / (".cavewii-replaced-" + uuid.uuid4().hex[:10])
                    os.replace(entry, aside)
                    retired.append((aside, entry))
                os.replace(staging, target)
            except OSError:
                for aside, entry in reversed(retired):
                    os.replace(aside, entry)
                raise
            for aside, _entry in retired:
                shutil.rmtree(aside, ignore_errors=True)
            result["replaced"] = [entry.name for entry in beta_replaced]
        elif replace_existing:
            # Move the old copy aside (same volume, so atomic), publish the new
            # one, then delete the old: the game never sees a half-written pack.
            retired = root / (".cavewii-replaced-" + uuid.uuid4().hex[:10])
            os.replace(target, retired)
            try:
                os.replace(staging, target)
            except OSError:
                os.replace(retired, target)
                raise
            shutil.rmtree(retired, ignore_errors=True)
            result["replaced"] = True
        else:
            os.replace(staging, target)
        staging = None
        result.update({"path": str(target), "pack_name": target.name})
        if select and (has_textures or audio_select_effective):
            pack_value = "texturepacks/" + target.name
            result["configs"] = _select_everywhere(root, pack_value, audio_select_effective,
                                                   has_textures)
            # "config" stays the card-root (or PC) file for older callers,
            # unless one of the files had to be rebuilt from a backup.
            recovered = [row for row in result["configs"] if row.get("recovered_from_backup")]
            result["config"] = recovered[0] if recovered else result["configs"][-1]
            result["selected"] = True
            if result["config"].get("recovered_from_backup"):
                # Never silent: the player's settings file was unreadable and
                # has been replaced by a backup's contents. Both callers show
                # this (2026-09-17 DELTA review, MEDIUM).
                result["warnings"].append(
                    "CavEX's settings file could not be read, so it was rebuilt from an earlier backup; "
                    "settings you changed since then may be gone. The unreadable file was kept as {0}.".format(
                        result["config"].get("corrupt_copy")))
        else:
            result["instructions"].append("Select {0} from CavEX's texture-pack menu.".format(target.name))
        old = old_beta_imports(root, keep=target.name) if base == BETA_PACK_NAME else []
        old = [name for name in old if not BETA_REPLACE_RE.match(name)]
        if old and remove_old:
            result["removed_old"] = remove_old_imports(root, old)
        elif old:
            result["old_imports"] = old
            result["warnings"].append(
                "Older imports of Beta 1.7.3 are still on the card: {0}. They are no longer "
                "needed; the importer can remove them for you.".format(", ".join(old)))
        result["ok"] = True
        _emit(progress, "installed", len(rows), len(rows), str(target))
    except Exception as exc:
        result["errors"].append(_error(exc))
        if result["path"]:
            result["warnings"].append("The pack was installed, but automatic selection did not complete.")
    finally:
        if staging is not None:
            shutil.rmtree(staging, ignore_errors=True)
    return result
