from __future__ import annotations

from hashlib import sha256
from pathlib import Path
import shutil
import zlib

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "brand_host_login" / "original_nva.exe"
BACKUP = ROOT / "brand_host_login" / "original_nva.before_title.exe"
PARSER_OFFSET = 9_959_176
DOCK_JS_OFFSET = 2_574_922
SLOT_SIZE = 2_200
OLD = "女娃免费本地解析".encode()
NEW = "佳佳Radaer".encode() + (b" " * 12)

source = SOURCE.read_bytes()
if not BACKUP.exists():
    shutil.copy2(SOURCE, BACKUP)
parser = bytearray(source[PARSER_OFFSET:PARSER_OFFSET + 31_624_196])
block = zlib.decompress(parser[DOCK_JS_OFFSET:DOCK_JS_OFFSET + SLOT_SIZE])
if block.count(OLD) != 1:
    raise SystemExit(f"expected one old title, found {block.count(OLD)}")
changed = block.replace(OLD, NEW)
compressed = zlib.compress(changed, 9)
if len(compressed) > SLOT_SIZE:
    raise SystemExit("compressed dock.js no longer fits its fixed slot")
parser[DOCK_JS_OFFSET:DOCK_JS_OFFSET + SLOT_SIZE] = compressed.ljust(SLOT_SIZE, b"\0")
patched = bytearray(source)
patched[PARSER_OFFSET:PARSER_OFFSET + len(parser)] = parser
SOURCE.write_bytes(patched)
print(f"SOURCE={SOURCE}")
print(f"OLD_PRESENT={OLD in zlib.decompress(parser[DOCK_JS_OFFSET:DOCK_JS_OFFSET + SLOT_SIZE])}")
print(f"NEW_PRESENT={NEW.rstrip() in zlib.decompress(parser[DOCK_JS_OFFSET:DOCK_JS_OFFSET + SLOT_SIZE])}")
print(f"SHA256={sha256(patched).hexdigest().upper()}")
