"""Read selected recovered code objects without importing/executing them."""
import dis
import importlib.util
import marshal
from pathlib import Path
import sys
import types

root = Path(__file__).resolve().parents[2]
module = sys.argv[1]
raw = (root / 'runtime/vendor_pyc' / (module.replace('.', '/') + '.pyc')).read_bytes()
assert raw[:4] == importlib.util.MAGIC_NUMBER

def walk(code):
    yield code
    for value in code.co_consts:
        if isinstance(value, types.CodeType):
            yield from walk(value)

for code in walk(marshal.loads(raw[16:])):
    if len(sys.argv) == 2:
        print(code.co_qualname, code.co_varnames[:code.co_argcount + code.co_kwonlyargcount])
        continue
    if code.co_qualname not in sys.argv[2:]:
        continue
    print('FUNCTION', code.co_qualname, 'CONSTANTS', repr(code.co_consts))
    previous, row = None, []
    for instruction in dis.get_instructions(code):
        line = instruction.positions.lineno
        if line != previous and row:
            print(previous, ' | '.join(row))
            row = []
        previous = line
        if instruction.opname in ('RESUME', 'NOT_TAKEN', 'PUSH_NULL'):
            continue
        op = instruction.opname.replace('LOAD_FAST_BORROW', 'get').replace('LOAD_FAST', 'get').replace('LOAD_CONST', 'const').replace('LOAD_SMALL_INT', 'int').replace('LOAD_ATTR', 'attr').replace('LOAD_GLOBAL', 'global').replace('STORE_FAST', 'set').replace('BINARY_OP', 'op').replace('TO_BOOL', 'bool')
        arg = instruction.argrepr or (str(instruction.arg) if instruction.arg is not None else '')
        row.append(f'{instruction.offset}:{op} {arg}')
    if row:
        print(previous, ' | '.join(row))
