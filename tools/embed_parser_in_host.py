from __future__ import annotations

from hashlib import sha256
from pathlib import Path


ROOT = Path(__file__).resolve().parent
SOURCE_HOST = ROOT / "original_nva.exe"
SOURCE_PARSER = ROOT / "brand_parser_login" / "NvaParser.exe"
OUTPUT_HOST = ROOT / "brand_host_login" / "original_nva.exe"
PARSER_OFFSET = 9_959_176


def main() -> None:
    host = SOURCE_HOST.read_bytes()
    parser = SOURCE_PARSER.read_bytes()
    end = PARSER_OFFSET + len(parser)
    if end > len(host):
        raise RuntimeError("parser does not fit in host")
    if len(parser) != 31_624_196:
        raise RuntimeError(f"unexpected parser size: {len(parser)}")
    changed = bytearray(host)
    changed[PARSER_OFFSET:end] = parser
    OUTPUT_HOST.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT_HOST.write_bytes(changed)
    print(f"OUTPUT={OUTPUT_HOST}")
    print(f"HOST_BYTES={OUTPUT_HOST.stat().st_size}")
    print(f"SOURCE_SHA256={sha256(host).hexdigest().upper()}")
    print(f"OUTPUT_SHA256={sha256(changed).hexdigest().upper()}")


if __name__ == "__main__":
    main()
