"""Extract the analyzed .NET bundle without executing its entry point.

Uses only Python's standard library. Outputs the bundle manifest and the
application/non-framework assemblies needed for offline inspection.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path

SIGNATURE = bytes.fromhex('8b1202b96a612038727b930214d7a03213f5b9e6efae3318ee3b2dce24b36aae')

def main() -> None:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', type=Path, default=root/'runtime/brand_host_login/original_nva.exe')
    parser.add_argument('--output', type=Path, default=root/'reverse/host_crypto')
    args = parser.parse_args()
    b = args.host.read_bytes()
    marker = b.find(SIGNATURE)
    if marker < 8:
        raise ValueError('No .NET single-file bundle marker')
    header = struct.unpack_from('<Q', b, marker-8)[0]
    pos = header
    def unpack(fmt: str):
        nonlocal pos
        values = struct.unpack_from(fmt, b, pos)
        pos += struct.calcsize(fmt)
        return values
    def string() -> str:
        nonlocal pos
        size = shift = 0
        while True:
            c = b[pos]
            pos += 1
            size |= (c & 127) << shift
            if c < 128:
                break
            shift += 7
            if shift >= 35:
                raise ValueError('Invalid 7-bit string length')
        end = pos + size
        if end > len(b):
            raise ValueError('String exceeds bundle')
        text = b[pos:end].decode('utf-8')
        pos = end
        return text
    major, minor, count = unpack('<IIi')
    if major not in (1, 2, 6) or count < 0 or count > 100000:
        raise ValueError(f'Unsupported bundle version/count: {major}.{minor}/{count}')
    bundle_id = string()
    extra = unpack('<qqqqQ') if major >= 2 else ()
    entries = []
    args.output.mkdir(parents=True, exist_ok=True)
    destination = (args.output/'bundle').resolve()
    for _ in range(count):
        offset, size = unpack('<qq')
        compressed = unpack('<q')[0] if major >= 6 else 0
        kind = unpack('<B')[0]
        name = string()
        stored_size = compressed or size
        if min(offset, size, compressed) < 0 or offset+stored_size > header:
            raise ValueError(f'Invalid entry bounds: {name}')
        entry = dict(name=name, offset=offset, size=size, compressed_size=compressed, type=kind)
        entries.append(entry)
        if kind in (3, 4) or (kind == 1 and not name.startswith(('System.', 'Microsoft.'))):
            data = b[offset:offset+stored_size]
            if compressed:
                data = zlib.decompress(data, -15)
            if len(data) != size:
                raise ValueError(f'Extracted size mismatch: {name}')
            target = (destination/name).resolve()
            if not target.is_relative_to(destination):
                raise ValueError(f'Unsafe relative path: {name}')
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists() or target.read_bytes() != data:
                target.write_bytes(data)
            entry['extracted_sha256'] = hashlib.sha256(data).hexdigest()
    manifest = dict(source=str(args.host.resolve()), source_sha256=hashlib.sha256(b).hexdigest(),
                    header_offset=header, version=[major, minor], file_count=count,
                    bundle_id=bundle_id, header_extra=extra, entries=entries)
    (args.output/'bundle_manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(json.dumps(dict(version=[major,minor], entries=count,
                         extracted=sum('extracted_sha256' in e for e in entries),
                         host_sha256=manifest['source_sha256'])))

if __name__ == '__main__':
    main()
