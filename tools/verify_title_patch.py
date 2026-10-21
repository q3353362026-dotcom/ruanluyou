from __future__ import annotations

from hashlib import sha256
from pathlib import Path
import sys
import zlib


PARSER_OFFSET = 9_959_176
PARSER_LENGTH = 31_624_196
DOCK_JS_OFFSET_IN_PARSER = 2_574_922
DOCK_JS_COMPRESSED_LENGTH = 2_200


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: verify_title_patch.py HOST EXPECTED_TITLE")
        return 2
    host_path = Path(sys.argv[1])
    expected = sys.argv[2]
    host = host_path.read_bytes()
    parser = host[PARSER_OFFSET:PARSER_OFFSET + PARSER_LENGTH]
    start = DOCK_JS_OFFSET_IN_PARSER
    dock_js = zlib.decompress(
        parser[start:start + DOCK_JS_COMPRESSED_LENGTH]
    ).decode("utf-8")
    present = expected in dock_js
    old_present = "女娃免费本地解析" in dock_js
    new_present = "佳佳Radaer" in dock_js
    print(f"INPUT={host_path.resolve()}")
    print(f"EXPECTED={expected}")
    print(f"EXPECTED_PRESENT={present}")
    print(f"OLD_PRESENT={old_present}")
    print(f"NEW_PRESENT={new_present}")
    print(f"SHA256={sha256(host).hexdigest().upper()}")
    print(f"RESULT={'PASS' if present else 'FAIL'}")
    return 0 if present else 1


if __name__ == "__main__":
    raise SystemExit(main())
