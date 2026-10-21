from __future__ import annotations

from pathlib import Path
import marshal
import shutil
import types

from PyInstaller.archive.readers import CArchiveReader
from PyInstaller.archive.writers import ZlibArchiveWriter


ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "original_parser_runtime" / "NvaParser.exe"
OUTPUT = ROOT / "brand_parser_login" / "NvaParser.exe"
PAGE_PYC = ROOT / "vendor_pyc" / "er_radar.pyc"
PYZ_NAME = "PYZ.pyz"


def page_code() -> types.CodeType:
    code = marshal.loads(PAGE_PYC.read_bytes()[16:])
    for item in code.co_consts:
        if isinstance(item, str) and item.startswith("<!DOCTYPE html>"):
            return code
    raise RuntimeError("patched er_radar.pyc does not contain the HTML page")


def main() -> None:
    archive = CArchiveReader(str(SOURCE))
    pyz_pos, pyz_len, _uncompressed, _compressed, typecode = archive.toc[PYZ_NAME]
    if typecode != "z":
        raise RuntimeError(f"unexpected PYZ typecode: {typecode}")
    pyz = archive.open_embedded_archive(PYZ_NAME)
    entries = []
    code_dict: dict[str, types.CodeType] = {}
    patched = page_code()
    for name, (item_type, _pos, _length) in pyz.toc.items():
        if item_type == 3:
            entries.append((name, "-", "PYMODULE-2"))
            continue
        code = patched if name == "er_radar" else pyz.extract(name)
        if not isinstance(code, types.CodeType):
            raise RuntimeError(f"missing code object for {name}")
        code_dict[name] = code
        if item_type == 1:
            src_path = f"{name.replace('.', '/')}/__init__.py"
            entries.append((name, src_path, "PYMODULE-1"))
        else:
            src_path = f"{name.replace('.', '/')}.py"
            entries.append((name, src_path, "PYMODULE"))

    temp_pyz = OUTPUT.parent / "PYZ.login.pyz"
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    ZlibArchiveWriter(str(temp_pyz), entries, code_dict)
    new_pyz = temp_pyz.read_bytes()
    if len(new_pyz) > pyz_len:
        raise RuntimeError(f"patched PYZ is too large: {len(new_pyz)} > {pyz_len}")

    original = SOURCE.read_bytes()
    abs_start = archive._start_offset + pyz_pos
    changed = bytearray(original)
    changed[abs_start:abs_start + pyz_len] = new_pyz.ljust(pyz_len, b"\0")
    OUTPUT.write_bytes(changed)
    temp_pyz.unlink(missing_ok=True)
    print(f"SOURCE={SOURCE}")
    print(f"OUTPUT={OUTPUT}")
    print(f"OLD_PYZ_BYTES={pyz_len}")
    print(f"NEW_PYZ_BYTES={len(new_pyz)}")
    print(f"PARSER_BYTES={OUTPUT.stat().st_size}")


if __name__ == "__main__":
    main()
