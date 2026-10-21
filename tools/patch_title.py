from __future__ import annotations

from hashlib import sha256
from pathlib import Path
import zlib


ROOT = Path(__file__).resolve().parent
ORIGINAL_HOST = ROOT / "original_nva.exe"
MODIFIED_HOST = ROOT / "brand_host" / "original_nva.exe"
ORIGINAL_PARSER = ROOT / "original_parser_runtime" / "NvaParser.exe"

PARSER_OFFSET = 9_959_176
DOCK_JS_OFFSET_IN_PARSER = 2_574_922
DOCK_JS_COMPRESSED_LENGTH = 2_200
OLD_TEXT = "女娃免费本地解析".encode()
NEW_TEXT = "佳佳Radaer".encode() + (b" " * 12)


def digest(data: bytes) -> str:
    return sha256(data).hexdigest().upper()


def main() -> None:
    original_host = ORIGINAL_HOST.read_bytes()
    original_parser = ORIGINAL_PARSER.read_bytes()
    parser_end = PARSER_OFFSET + len(original_parser)
    if original_host[PARSER_OFFSET:parser_end] != original_parser:
        raise RuntimeError("The embedded parser does not match the preserved original")

    block_start = DOCK_JS_OFFSET_IN_PARSER
    block_end = block_start + DOCK_JS_COMPRESSED_LENGTH
    dock_js = zlib.decompress(original_parser[block_start:block_end])
    if dock_js.count(OLD_TEXT) != 1:
        raise RuntimeError("Expected exactly one original subtitle in dock.js")

    changed_dock_js = dock_js.replace(OLD_TEXT, NEW_TEXT)
    if len(changed_dock_js) != len(dock_js):
        raise RuntimeError("The fixed-width subtitle replacement changed dock.js length")
    compressed = zlib.compress(changed_dock_js, 9)
    if len(compressed) > DOCK_JS_COMPRESSED_LENGTH:
        raise RuntimeError("The modified dock.js no longer fits its CArchive slot")
    padded = compressed.ljust(DOCK_JS_COMPRESSED_LENGTH, b"\0")

    changed_parser = bytearray(original_parser)
    changed_parser[block_start:block_end] = padded
    if len(changed_parser) != len(original_parser):
        raise RuntimeError("Parser size changed")

    changed_host = bytearray(original_host)
    changed_host[PARSER_OFFSET:parser_end] = changed_parser
    if len(changed_host) != len(original_host):
        raise RuntimeError("Host size changed")

    MODIFIED_HOST.parent.mkdir(parents=True, exist_ok=True)
    MODIFIED_HOST.write_bytes(changed_host)
    print(f"ORIGINAL_SHA256={digest(original_host)}")
    print(f"MODIFIED_SHA256={digest(changed_host)}")
    print(f"MODIFIED_FILE={MODIFIED_HOST}")
    print(f"DOCK_JS_TEXT={changed_dock_js.decode('utf-8').split('class=\"brand-sub\"')[0][-80:] if b'brand-sub' in changed_dock_js else '佳佳Radaer'}")


if __name__ == "__main__":
    main()
