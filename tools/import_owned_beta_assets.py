#!/usr/bin/env python3
"""import_owned_beta_assets.py -- command-line entry point for importing YOUR
OWN Minecraft Beta 1.7.3 jar + resources into a CaveWii_v2 texture pack.

The implementation lives in tools/pack_helper/owned_importer.py (moved
2026-10-30) so the Pack Helper package is self-contained: exporting
tools/pack_helper alone, or freezing it into the exe, carries the importer.
This shim keeps the documented command and the `tools.import_owned_beta_assets`
import name working, and exposes the same module attributes.
"""
import sys
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from pack_helper import owned_importer as _impl
else:
    from .pack_helper import owned_importer as _impl

globals().update({key: value for key, value in vars(_impl).items()
                  if not key.startswith("__")})
main = _impl.main

if __name__ == "__main__":
    sys.exit(main())
