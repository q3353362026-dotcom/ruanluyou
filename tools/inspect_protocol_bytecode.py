"""Static protocol-bytecode inventory; never import or execute target modules."""
import dis, hashlib, importlib.util, json, marshal, pathlib, types
root = pathlib.Path(__file__).resolve().parents[1]
base = root / 'runtime/vendor_pyc'
out = root / 'reverse/protocol_trace'
out.mkdir(parents=True, exist_ok=True)
modules = ('flowlive liveview flowload handshake c2s_self codec.packet codec.engine_packet codec.engine_stream codec.engine_bunch codec.bitstream blocks sna repmove4 repmove3 state_track repdec6 repdec4 playerstate_protocol replication_header property_health death_state pickup_state death_box_state death_box_protocol box_contents container_state weapon_state weapon_native er_live field13 inventory_native').split()
def walk(c):
    yield c
    for v in c.co_consts:
        if isinstance(v, types.CodeType): yield from walk(v)
results = []
for name in modules:
    path = base / (name.replace('.', '/') + '.pyc')
    raw = path.read_bytes()
    if raw[:4] != importlib.util.MAGIC_NUMBER: raise RuntimeError('Python version mismatch: '+name)
    code = marshal.loads(raw[16:])
    result = dict(module=name,sha256=hashlib.sha256(raw).hexdigest(),imports=[],functions=[])
    rows = ['MODULE '+name,'SHA256 '+result['sha256'],'STATIC ONLY: target code was not executed. Operands over 400 characters are abbreviated.','']
    for c in walk(code):
        if c.co_name == '__annotate__': continue
        rows += ['FUNCTION '+c.co_qualname+' | original line '+str(c.co_firstlineno),'PARAMETERS '+repr(c.co_varnames[:c.co_argcount+c.co_kwonlyargcount]),'NAMES '+repr(c.co_names)]
        if c.co_consts and isinstance(c.co_consts[0],str): rows.append('DOC '+c.co_consts[0][:4000])
        for i in dis.get_instructions(c):
            if i.opname == 'IMPORT_NAME': result['imports'].append(i.argval)
            arg = i.argrepr or (str(i.arg) if i.arg is not None else '')
            arg = arg if len(arg)<=400 else arg[:400]+' ... [abbreviated]'
            rows.append(f'{i.offset:6} L{i.positions.lineno or "-":>5} {i.opname:28} {arg}')
        rows += ['EXCEPTIONS '+repr(list(dis.Bytecode(c).exception_entries)),'']
        result['functions'].append(dict(name=c.co_qualname,original_line=c.co_firstlineno,names=list(c.co_names)))
    text='\n'.join(rows)+'\n'
    (out/(name+'.dis.txt')).write_text(text,encoding='utf-8')
    anchors=[n for n,line in enumerate(text.splitlines(),1) if line.startswith('FUNCTION ')]
    for f,n in zip(result['functions'],anchors): f['trace_line']=n
    result['imports']=sorted(set(result['imports']))
    results.append(result)
(out/'index.json').write_text(json.dumps(results,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(json.dumps(dict(modules=len(results),functions=sum(len(m['functions']) for m in results),output=str(out))))
