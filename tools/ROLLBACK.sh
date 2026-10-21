#!/usr/bin/env bash
set -euo pipefail

ROOT='C:/Users/Administrator/Documents/ChatGPT/1/full_rebuild'

if [[ "${1:-}" == "--radar-html" ]]; then
  TARGET="${2:-$ROOT/rollback_test/original_radar_page.html}"
  py -3.14 - "$ROOT/original_radar_page.before_radar_controls.html" "$TARGET" <<'PY'
from hashlib import sha256
from pathlib import Path
import shutil
import sys

source = Path(sys.argv[1])
target = Path(sys.argv[2])
target.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(source, target)
source_hash = sha256(source.read_bytes()).hexdigest().upper()
target_hash = sha256(target.read_bytes()).hexdigest().upper()
print(f"INPUT={target.resolve()}")
print(f"RESTORED_FROM={source.resolve()}")
print(f"RESTORED_SHA256={target_hash}")
print(f"RESULT={'PASS' if source_hash == target_hash else 'FAIL'}")
raise SystemExit(0 if source_hash == target_hash else 1)
PY
  exit $?
fi

TARGET="${1:-$ROOT/brand_host/original_nva.exe}"

py -3.14 - "$ROOT/original_nva.exe" "$TARGET" <<'PY'
from hashlib import sha256
from pathlib import Path
import shutil
import sys

source = Path(sys.argv[1])
target = Path(sys.argv[2])
target.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(source, target)
source_hash = sha256(source.read_bytes()).hexdigest().upper()
target_hash = sha256(target.read_bytes()).hexdigest().upper()
print(f"INPUT={target.resolve()}")
print(f"RESTORED_FROM={source.resolve()}")
print(f"RESTORED_SHA256={target_hash}")
print(f"RESULT={'PASS' if source_hash == target_hash else 'FAIL'}")
raise SystemExit(0 if source_hash == target_hash else 1)
PY
