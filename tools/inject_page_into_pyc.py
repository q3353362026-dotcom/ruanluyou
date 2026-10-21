from __future__ import annotations

from pathlib import Path
import marshal
import types


ROOT = Path(__file__).resolve().parent
TARGET = ROOT / "vendor_pyc" / "er_radar.pyc"
PAGE = ROOT / "launcher_ui.html"
BACKUP = ROOT / "vendor_pyc" / "er_radar.pyc.pre_launcher"


def replace_page(code: types.CodeType, page: str) -> tuple[types.CodeType, int]:
    changed = 0
    constants = []
    for item in code.co_consts:
        if isinstance(item, str) and item.startswith("<!DOCTYPE html>"):
            constants.append(page)
            changed += 1
        elif isinstance(item, types.CodeType):
            item, nested = replace_page(item, page)
            constants.append(item)
            changed += nested
        else:
            constants.append(item)
    return code.replace(co_consts=tuple(constants)), changed


def main() -> None:
    page = PAGE.read_text(encoding="utf-8")
    raw = TARGET.read_bytes()
    if not BACKUP.exists():
        BACKUP.write_bytes(raw)
    code = marshal.loads(raw[16:])
    patched, count = replace_page(code, page)
    if count != 1:
        raise RuntimeError(f"expected one embedded HTML page, found {count}")
    TARGET.write_bytes(raw[:16] + marshal.dumps(patched))
    print(f"UPDATED={TARGET}")
    print(f"PAGE_BYTES={len(page.encode('utf-8'))}")
    print(f"PYC_BYTES={TARGET.stat().st_size}")


if __name__ == "__main__":
    main()
