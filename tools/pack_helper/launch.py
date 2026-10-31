"""PyInstaller entry point; also runnable directly during development."""
import sys
from pathlib import Path

if not getattr(sys, "frozen", False):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from pack_helper.gui import main

if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--self-test":
        from pack_helper.selftest import run
        raise SystemExit(run(sys.argv[2]))
    elif len(sys.argv) > 1:
        from pack_helper.__main__ import main as cli_main
        raise SystemExit(cli_main())
    else:
        main()
