#!/usr/bin/env python3
"""CI: rebake all three anim.png colour variants and require byte-identical results.

  Beta     packs/beta-base/anim.png       bake_anim_fx.sh, no ramp (Beta colours)
  E&A      assets/anim.png == artwork/cc0-pack/earth-and-ash-complete/anim.png
           build_full_pack.beta_motion(full_effects sheet) with E&A ramps (own ice)
  Isabella packs/isabella-ii/anim.png      artwork/isabella-ii-extras/make_anim.py
Committed ramps must also equal what artwork/fx_ramps.py generates. Needs bash, cc,
Python 3 + Pillow (same as bake_anim_fx.sh).
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def main():
    bash = shutil.which("bash") or "bash"
    ramp_dir = REPO / "artwork/fx-ramps"
    before = {p: p.read_bytes() for p in sorted(ramp_dir.rglob("*.ramp"))}
    subprocess.run([sys.executable, str(REPO / "artwork/fx_ramps.py")], check=True, stdout=subprocess.DEVNULL)
    after = {p: p.read_bytes() for p in sorted(ramp_dir.rglob("*.ramp"))}
    assert before == after and len(after) == 10, "committed ramps differ from artwork/fx_ramps.py"
    failures = []
    with tempfile.TemporaryDirectory(prefix="cw-pack-bakes-") as tmp:
        tmp = Path(tmp)
        beta = tmp / "beta.png"
        subprocess.run([bash, str(REPO / "tools/gen_anim_fx/bake_anim_fx.sh"), str(beta),
                        str(REPO / "packs/beta-base/anim.png")], check=True, stdout=subprocess.DEVNULL)
        if beta.read_bytes() != (REPO / "packs/beta-base/anim.png").read_bytes():
            failures.append("packs/beta-base/anim.png")
        sys.path.insert(0, str(REPO / "artwork/cc0-pack"))
        import build_full_pack
        from full_effects import build as effects
        ea = tmp / "ea.png"
        build_full_pack.beta_motion(effects()["anim.png"].convert("RGBA")).save(ea)
        # build_full_pack saves with Image.save defaults; compare the raw PNG bytes.
        for committed in ("artwork/cc0-pack/earth-and-ash-complete/anim.png", "assets/anim.png"):
            if ea.read_bytes() != (REPO / committed).read_bytes():
                failures.append(committed)
        isa = tmp / "isa.png"
        subprocess.run([sys.executable, str(REPO / "artwork/isabella-ii-extras/make_anim.py"), str(isa)],
                       check=True, stdout=subprocess.DEVNULL)
        if isa.read_bytes() != (REPO / "packs/isabella-ii/anim.png").read_bytes():
            failures.append("packs/isabella-ii/anim.png")
    if failures:
        print("FAIL: rebake differs for " + ", ".join(failures))
        return 1
    print("PASS: Beta, Earth & Ash and Isabella II anim.png rebake identically from their generators")
    return 0


if __name__ == "__main__":
    sys.exit(main())
