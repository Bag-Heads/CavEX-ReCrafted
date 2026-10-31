"""Combine an existing game release ZIP and a target-platform helper, without publishing."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import zipfile


def _safe_member(name):
    path = PurePosixPath(name.rstrip("/"))
    if (not name or not path.parts or path.is_absolute() or ".." in path.parts
            or "\\" in name or ":" in name):
        raise ValueError("Unsafe release member: " + name)
    return path.as_posix()


def _sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _new_info(name, mode):
    info = zipfile.ZipInfo(name)
    info.create_system = 3
    info.external_attr = mode << 16
    if stat.S_ISDIR(mode):
        info.external_attr |= 0x10
    return info


def _is_link(info):
    return info.create_system == 3 and stat.S_ISLNK(info.external_attr >> 16)


def assemble(game_zip, helper_dir, output, platform):
    if platform not in ("Windows", "macOS", "Linux"):
        raise ValueError("Choose Windows, macOS or Linux")
    game_zip, output, helper_dir = Path(game_zip), Path(output), Path(helper_dir)
    if not helper_dir.is_dir() or helper_dir.is_symlink():
        raise ValueError("Built helper directory missing or linked")

    files = {}
    infos = {}
    order = []
    folded = {}
    directories = set()
    links = {}

    def reserve(name, data, info):
        key = _safe_member(name).casefold()
        previous = folded.get(key)
        if previous is not None:
            raise ValueError("Case-insensitive release member collision: {0} / {1}".format(previous, name))
        folded[key] = name
        order.append(name)
        infos[name] = info
        if info.is_dir():
            directories.add(name)
        else:
            files[name] = data

    with zipfile.ZipFile(game_zip) as source:
        for item in source.infolist():
            data = b"" if item.is_dir() else source.read(item)
            reserve(item.filename, data, copy.copy(item))
            if not item.is_dir() and _is_link(item):
                try:
                    links[item.filename] = data.decode("utf-8")
                except UnicodeDecodeError as exc:
                    raise ValueError("Game release symlink target is not UTF-8") from exc

    if "apps/CavEX/boot.dol" not in files or "provenance.json" not in files:
        raise ValueError("Use the complete game ZIP produced by make_release.sh")
    if b"paths.audiopack\x00" not in files["apps/CavEX/boot.dol"]:
        raise ValueError("The game release predates independent audio selection; build the matching game before bundling this helper")
    original_sums = files.get("SHA256SUMS", b"")
    try:
        sums = original_sums.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ValueError("Game release checksum inventory is not UTF-8") from exc
    covered = set()
    for line in sums.splitlines():
        if not line.strip():
            continue
        try:
            digest, name = line.split(maxsplit=1)
        except ValueError as exc:
            raise ValueError("Malformed game release checksum row") from exc
        name = name.lstrip(" *")
        if name.startswith("./"):
            name = name[2:]
        if name not in files or hashlib.sha256(files[name]).hexdigest() != digest:
            raise ValueError("Game release checksum mismatch: " + name)
        if name in covered:
            raise ValueError("Duplicate game release checksum row: " + name)
        covered.add(name)
    if covered != set(files) - {"SHA256SUMS"}:
        raise ValueError("Game release checksum inventory incomplete")

    # The helper converts nothing without its generated resources (2026-10-30:
    # an export without them failed "bundle_incomplete: missing gui_2.png").
    # package_app.py always builds them; refuse a helper folder that lacks them.
    required = ("mapping.json", "conversion_map.json", "assets/gui_2.png",
                "beta-base/anim.png", "beta-base/pointer.png", "defaults/config_wii.json",
                "defaults/config_pc.json")
    bundles = [Path(top) for top, _dirs, names in os.walk(helper_dir)
               if "resource_hashes.json" in names and Path(top).name == "resources"
               and Path(top).parent.name == "pack_helper"]
    if not bundles or any(not (bundles[0] / name).is_file() for name in required):
        missing = [name for name in required if not bundles or not (bundles[0] / name).is_file()]
        raise ValueError("Built helper is missing pack_helper/resources ({0}); build it with "
                         "tools/pack_helper/package_app.py".format(", ".join(missing)))
    helper_root = helper_dir.resolve(strict=True)
    for path in sorted(helper_dir.rglob("*"), key=lambda candidate: candidate.as_posix().casefold()):
        relative = path.relative_to(helper_dir).as_posix()
        name = "Pack Helper/" + relative
        if path.is_symlink():
            target = os.readlink(path)
            if os.path.isabs(target) or "\\" in target:
                raise ValueError("Helper symlinks must use portable relative targets")
            try:
                resolved = path.resolve(strict=True)
            except OSError as exc:
                raise ValueError("Helper symlink target is missing: " + relative) from exc
            if not resolved.is_relative_to(helper_root):
                raise ValueError("Helper symlink escapes its bundle: " + relative)
            links[name] = target
            reserve(name, target.encode("utf-8"), _new_info(name, stat.S_IFLNK | 0o777))
        elif path.is_dir():
            directory_name = name.rstrip("/") + "/"
            reserve(directory_name, b"", _new_info(directory_name, stat.S_IFDIR | 0o755))
        elif path.is_file():
            mode = stat.S_IFREG | (0o755 if path.stat().st_mode & 0o111 else 0o644)
            reserve(name, path.read_bytes(), _new_info(name, mode))

    provenance = files["provenance.json"]
    release_name = "pack-helper-release.json"
    release_data = (json.dumps({
        "schema_version": 1,
        "platform": platform,
        "game_release_sha256": _sha256_file(game_zip),
        "game_checksum_inventory_sha256": hashlib.sha256(original_sums).hexdigest(),
        "game_provenance": "provenance.json",
        "game_provenance_sha256": hashlib.sha256(provenance).hexdigest(),
        "helper_acceptance": "pending clean-machine verification",
    }, indent=2, sort_keys=True) + "\n").encode("utf-8")
    reserve(release_name, release_data, _new_info(release_name, stat.S_IFREG | 0o644))

    link_manifest_name = "pack-helper-symlinks.json"
    link_manifest = (json.dumps({
        "schema_version": 1,
        "links": [{"path": name, "target": target} for name, target in sorted(links.items())],
    }, indent=2, sort_keys=True) + "\n").encode("utf-8")
    reserve(link_manifest_name, link_manifest,
            _new_info(link_manifest_name, stat.S_IFREG | 0o644))

    # File hashes deliberately exclude directories, symbolic links, and the
    # inventory itself. After extraction, ordinary sha256sum follows a link to
    # its target bytes, which cannot equal the ZIP's stored link-target text.
    files["SHA256SUMS"] = "".join(
        hashlib.sha256(files[name]).hexdigest() + "  " + name + "\n"
        for name in sorted(files)
        if name != "SHA256SUMS" and name not in links
    ).encode("utf-8")

    # Exclusive creation protects previous release artifacts. Original game
    # members retain their ZipInfo metadata; only SHA256SUMS content changes.
    with output.open("xb") as stream, zipfile.ZipFile(stream, "w") as archive:
        for name in order:
            info = infos[name]
            data = b"" if name in directories else files[name]
            archive.writestr(info, data)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-zip", required=True, type=Path)
    parser.add_argument("--helper-dir", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--platform", required=True, choices=("Windows", "macOS", "Linux"))
    args = parser.parse_args()
    assemble(args.game_zip, args.helper_dir, args.out, args.platform)
