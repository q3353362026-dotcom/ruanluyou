"""Native equipment decoding: exact frames, removals, current identity, reference parity."""
import json, os, struct, subprocess, sys, tempfile
from pathlib import Path
from player_fixtures import W, player_records, packet, bunch, event
from player_integration import run, write, lines
from health_integration import exports

GEAR='SOLCharacterEquipComponent';WEAPON='WeaponReplicateComponent';HEALTH='GPAttributeSetHealth'
def block(w,profile,component=15,time=1000):return dict(hex=w.raw().hex(),bits=len(w.b),profile=profile,component=component,time=time)
def gear(helmet=11010002004,armor=11050001002):
    return W().bit(0).packed(3).bits(2,16).packed(1).bits(helmet,64).packed(10).bits(armor,64).packed(0).packed(0)
def held(identity=123):return W().bit(0).packed(5).bits(1,16).packed(3).bits(identity,32).packed(6).bits(0,8).packed(0).packed(0)
def weapon_extra(identity=123,weapon=18010000001):
    # A real-shaped part: name omitted, no GUID. The two uint32 flags have distinct meanings.
    return W().bits(identity,32).bits(weapon,64).bit(1).packed(0).packed(0).bits(0,8).bits(1,32).bits(weapon,64).bits(weapon,64).bits(0,8).bits(0,32).bits(1,32).bits(0,32).bits(0xfffffffe,32).bits(0,3).bit(1).bits(1,32).bits(0,8)
def extra_field(w,identity=123,weapon=18010000001):
    f=W().bit(0).bits(1,32).bits(0,32).bits(0,32).bits(1,32).bits(10,32).join(weapon_extra(identity,weapon))
    return w.integer(17,24).packed(len(f.b)).join(f)
def sub(w,guid):return W().bit(1).bit(0).packed(guid).bit(1).packed(len(w.b)).join(w)
def main():
    exe,probe=map(Path,sys.argv[1:]);checks=0
    def inspect(blocks,ok=True):
        nonlocal checks
        q={'mode':'equipment','hex':'','blocks':blocks}
        result=json.loads(subprocess.check_output([str(probe)],input=json.dumps(q).encode()))
        assert result['ok']==ok,result;checks+=1
        return result.get('result')
    g=gear();w=extra_field(held());mx=W().bit(0).packed(4).bits(struct.unpack('<I',struct.pack('<f',95))[0],32).packed(0)
    result=inspect([block(g,GEAR),block(w,WEAPON,17),block(mx,HEALTH,19)])
    assert result['equipment']['helmet']['level']==2 and result['equipment']['armor']['level']==1,result
    assert result['equipment']['weapon']['name']=='M4A1' and result['maxHealth']['value']==95,result
    result=inspect([block(w,WEAPON),block(held(456),WEAPON,time=2000)])
    assert result['equipment']['weapon']['status']=='unresolved','do not label carried weapon as current'
    empty=W().bit(0).packed(5).bits(0,16).packed(0).packed(0)
    assert inspect([block(w,WEAPON),block(empty,WEAPON,time=2000)])['equipment']['weapon']['status']=='empty'
    clear=W().bit(0).packed(3).bits(0,16).packed(0).packed(0)
    assert 'helmet' not in inspect([block(g,GEAR),block(clear,GEAR,time=2000)])['equipment']
    assert inspect([block(w,WEAPON,time=2000),block(empty,WEAPON,time=1000)])['equipment']['weapon']['status']=='resolved'
    for source,profile in ((g,GEAR),(w,WEAPON),(mx,HEALTH)):
        for bits in (0,1,len(source.b)//2,len(source.b)-1):
            malformed=block(source,profile);malformed['bits']=bits;inspect([malformed],False)
    root=Path(__file__).resolve().parents[2]
    os.environ['ER_ROOT']=str(root/'runtime/ErCore');sys.path.insert(0,str(root/'runtime/vendor_pyc'))
    reference=sys.version_info[:2]==(3,14) and (root/'runtime/vendor_pyc/weapon_state.pyc').exists()
    if reference:
        import weapon_state
        ref=weapon_state.WeaponTracker();ref.apply_block(4,1,w.raw(),{'payload_start':0,'payload_end':len(w.b)})
        original=ref.current(4);assert original['status']=='resolved' and original['weapon_id']==18010000001,original
    else:
        print('SKIP reference parity only: requires recovered Python 3.14 bytecode; native checks still run')
    with tempfile.TemporaryDirectory(prefix='radaer-equipment-') as folder:
        folder=Path(folder);records=player_records(3)
        body=exports([(15,4,'CharacterEquip'),(17,4,'WeaponReplicate'),(19,4,'AttributeCenter')]).join(sub(g,15)).join(sub(w,17)).join(sub(mx,19))
        records.append(event(packet([bunch(body,channel=3,seq=2,exports=True)],8),8))
        write(folder/'in.jsonl',records)
        summary=json.loads(run(exe,'decode-players','--in',folder/'in.jsonl','--output-directory',folder/'out'))
        assert summary['errors']==summary['warnings']==summary['exceptions']==0,summary
        states=[e['data'] for e in lines(folder/'out/player-state.jsonl') if e['data'].get('equipment',{}).get('weapon',{}).get('status')=='resolved']
        assert states and all(s['actorGuid']==4 for s in states)
        assert states[-1]['equipment']['helmet']['level']==2 and states[-1]['vitals']['health']['MaxHealth']['value']==95
    print(f'Equipment: {checks} framing/state checks and actor-bound state-event integration passed; reference parity={reference}')
if __name__=='__main__':main()
