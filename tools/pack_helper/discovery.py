"""Bounded discovery in standard launcher locations; never downloads assets."""
import os
import sys
from pathlib import Path


def minecraft_roots():
    home = Path.home()
    if sys.platform == "win32":
        roots = [Path(os.environ.get("APPDATA", home)) / ".minecraft"]
    elif sys.platform == "darwin":
        roots = [home / "Library/Application Support/minecraft"]
    else:
        roots = [home / ".minecraft"]
    return [p for p in roots if p.is_dir() and not p.is_symlink()]


def discover_packs():
    """Offer candidates, leaving format validation to inspect()."""
    found = []
    for root in minecraft_roots():
        for folder in (root / "texturepacks", root / "resourcepacks", root / "versions"):
            if not folder.is_dir() or folder.is_symlink():
                continue
            try:
                with os.scandir(folder) as entries:
                    for index, entry in enumerate(entries):
                        if index >= 512:
                            break
                        if entry.is_symlink():
                            continue
                        path = Path(entry.path)
                        if folder.name == "versions":
                            path = path / (path.name + ".jar")
                        if path.is_file() and path.suffix.lower() in (".jar", ".zip"):
                            found.append(str(path))
                        elif folder.name != "versions" and path.is_dir():
                            found.append(str(path))
            except OSError:
                continue
    return sorted(set(found))


def discover_resources(source):
    path = Path(source).expanduser()
    parent = path if path.is_dir() else path.parent
    candidates = [parent / "resources", parent.parent / "resources"]
    if parent.parent.name == "versions":
        candidates.insert(0, parent.parent.parent / "resources")
    candidates.extend(root / "resources" for root in minecraft_roots())
    for candidate in candidates:
        if candidate.is_dir() and not candidate.is_symlink() and any(
                (candidate / kind).is_dir() for kind in ("sound", "newsound", "music", "newmusic", "streaming")):
            return str(candidate)
    return ""


AUDIO_DIRS = ("sound", "newsound", "music", "newmusic", "streaming")
# The launcher's asset index for Beta-era versions (b1.7.3.json "assets":
# "pre-1.6", "map_to_resources": true): the launcher copies these objects into
# .minecraft/resources when Beta is started. "legacy" is the older name.
LEGACY_INDEXES = ("pre-1.6.json", "legacy.json")


def has_audio_dirs(path):
    path = Path(path)
    return path.is_dir() and any((path / kind).is_dir() and not (path / kind).is_symlink()
                                 for kind in AUDIO_DIRS)


def find_resources_folder(picked):
    """The Beta sounds folder for what the player picked: the folder itself,
    or -- when they picked .minecraft, a version folder or the jar's folder --
    the 'resources' folder discover_resources finds next to it. Only folders
    inside or beside the pick are accepted, never an unrelated install."""
    picked = Path(picked).expanduser()
    if has_audio_dirs(picked):
        return picked
    found = discover_resources(picked)
    if not found:
        return None
    found = Path(found)
    base = picked if picked.is_dir() else picked.parent
    allowed = {base.resolve(), base.parent.resolve(), base.parent.parent.resolve()}
    return found if found.parent.resolve() in allowed else None


def find_legacy_index(picked):
    """(index.json, objects folder) of a launcher install at or above the pick
    whose Beta sounds were never copied to resources/, else None."""
    picked = Path(picked).expanduser()
    base = picked if picked.is_dir() else picked.parent
    for root in (base, base.parent, base.parent.parent):
        objects = root / "assets" / "objects"
        if not objects.is_dir() or objects.is_symlink():
            continue
        for name in LEGACY_INDEXES:
            index = root / "assets" / "indexes" / name
            if index.is_file() and not index.is_symlink():
                return index, objects
    return None
