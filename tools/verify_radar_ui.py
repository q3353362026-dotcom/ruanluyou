from __future__ import annotations

from pathlib import Path
import sys

path = Path(sys.argv[1])
mode = sys.argv[2] if len(sys.argv) > 2 else "modified"
text = path.read_text(encoding="utf-8")
controls = "radaer-desktop-controls" in text
brand = "佳佳Radaer" in text
old = "显示与元素" in text
if mode == "baseline":
    ok = controls and old
else:
    ok = (not controls) and brand
print(f"INPUT={path.resolve()}")
print(f"CONTROLS_PRESENT={controls}")
print(f"BRAND_PRESENT={brand}")
print(f"OLD_SUBTITLE_PRESENT={old}")
print(f"RESULT={'PASS' if ok else 'FAIL'}")
raise SystemExit(0 if ok else 1)
