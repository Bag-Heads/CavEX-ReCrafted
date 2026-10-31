"""JSON operations for automation; no subcommand opens the desktop interface."""
import argparse
import json
import sys
from . import core


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json-output", type=argparse.FileType("w", encoding="utf-8"),
                        help="Write JSON-lines here (required for the windowed packaged app)")
    sub = parser.add_subparsers(dest="operation", required=True)
    inspect = sub.add_parser("inspect")
    inspect.add_argument("source")
    inspect.add_argument("--resources")
    prepare = sub.add_parser("prepare")
    prepare.add_argument("source")
    prepare.add_argument("destination")
    prepare.add_argument("--look", choices=("crisp", "smooth"), default="crisp")
    prepare.add_argument("--resources")
    install = sub.add_parser("install")
    install.add_argument("prepared")
    install.add_argument("sd_root")
    install.add_argument("--no-select", action="store_true")
    install.add_argument("--remove-old", action="store_true",
                         help="delete earlier helper imports of Beta 1.7.3 (b1.7.3-<hash>, minecraft)")
    args = parser.parse_args()
    output = args.json_output or sys.stdout
    if output is None:
        raise ValueError("The windowed helper requires --json-output FILE for command-line operations")
    progress = lambda event: print(json.dumps({"type": "progress", **event}), file=output, flush=True)
    if args.operation == "inspect":
        result = core.inspect(args.source, resources_path=args.resources)
    elif args.operation == "prepare":
        result = core.prepare(args.source, args.destination, args.look, progress, resources_path=args.resources)
    else:
        # Same as the desktop app: imported sounds are switched on too. The
        # command line used to pin audio to the previous pack even when the
        # prepared pack carried sounds (2026-10-30 audit F11); install() only
        # moves the audio selection when the pack really has audio.
        result = core.install(args.prepared, args.sd_root, not args.no_select, progress=progress,
                              audio_select=True, remove_old=args.remove_old)
    print(json.dumps({"type": "result", **result}), file=output, flush=True)
    # Warnings go to stderr as well as into the JSON: with --json-output the
    # JSON lands in a file nobody reads, and a settings file rebuilt from a
    # backup must not pass unnoticed (2026-09-17 DELTA review, MEDIUM).
    for warning in result.get("warnings", []):
        print("warning: {0}".format(warning), file=sys.stderr, flush=True)
    if args.json_output:
        args.json_output.close()
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
