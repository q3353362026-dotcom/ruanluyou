"""Pickup wire/reference parity, transactional failures and dormant actor destruction."""
import json, os, subprocess, sys, tempfile
from pathlib import Path
from player_fixtures import W, actor_prefix, content, bunch, packet, event, player_records
from player_integration import run, write, lines

def props(item='15080050142', count=1):
    return W().bit(0).packed(50).bits(12345,64).packed(51).bits(0,3).packed(52).bit(0).string(item).bits(0,32).packed(53).bits(count,32).packed(0)

def pickup_open(actor=100,channel=25,sequence=1,body=None):
    # Pickup NewActor uses the quantization flag, but no Character extra byte.
    prefix=actor_prefix(actor,101,'BP_InventoryPickup_C',character=True,transforms=True);prefix.b=prefix.b[:-8]
    return bunch(prefix.join(content(body or props())),channel=channel,seq=sequence,opened=True,exports=True)

def native_item(item=15080050142):
    w=W().bits(item//10000,32).bits(item%10000,32).bits(999,64)
    w.bits(0,32+4*8+32+8).bits(0,32+8+64+32+64+8+64+32+32).bits(0,64)
    w.bits(0,7).bits(0,64).bits(0,3).bits(0,32).bit(0).bits(0,33).bits(0,2).bits(0,32).bits(0,4)
    return W().bit(0).packed(67).join(w).packed(0)

def main():
    exe,probe=map(Path,sys.argv[1:]);root=Path(__file__).resolve().parents[2]
    os.environ['ER_ROOT']=str(root/'runtime/ErCore');sys.path.insert(0,str(root/'runtime/vendor_pyc'))
    reference=sys.version_info[:2]==(3,14) and (root/'runtime/vendor_pyc/pickup_state.pyc').exists()
    if reference:
        import pickup_state,repdec6
        ref=pickup_state.PickupTracker(set(),{},{})
    else:
        print('SKIP reference parity only: requires recovered Python 3.14 bytecode; native checks still run')
    checks=0
    for body in (props(),props(count=0),native_item()):
        result=json.loads(subprocess.check_output([str(probe)],input=json.dumps(dict(mode='properties',profile='pickup',hex=body.raw().hex(),bits=len(body.b))).encode()))['result']
        assert result['status']=='exact',result
        if reference:
            events,pos,status=repdec6.decode_block(body.raw(),{'payload_start':0,'payload_end':len(body.b)},ref.catalog,ref.meta,value_reader=pickup_state.pickup_value)
            assert status==result['status'],(status,result)
        assert result['fields'].get('InventoryIdName',{}).get('text')=='15080050142' or result['fields']['PickupItemInfo']['itemId']=='15080050142'
        for length in (0,1,len(body.b)//2,len(body.b)-1):
            r=json.loads(subprocess.check_output([str(probe)],input=json.dumps(dict(mode='properties',profile='pickup',hex=body.raw().hex(),bits=length)).encode()))['result']
            assert r['status']=='error' and not r['fields'],r
        checks+=1
    with tempfile.TemporaryDirectory(prefix='radaer-loot-') as folder:
        folder=Path(folder);records=player_records(3)
        records.append(event(packet([pickup_open()],20),20))
        # Dormancy keeps the actor's last position and item identity.
        records.append(event(packet([bunch(W(),channel=25,seq=2,closed=True,close_reason=1)],21),21))
        # A new channel may destroy a dormant actor by GUID only.
        records.append(event(packet([bunch(W().packed(100),channel=26,seq=1,opened=True,closed=True)],22),22))
        records.append(event(packet([pickup_open(102,27)],23),23))
        records.append(event(packet([bunch(content(props(count=0)),channel=27,seq=2)],24),24))
        # Native-only identity, plus malformed deltas must not mutate the retained state.
        records.append(event(packet([pickup_open(104,28,body=native_item())],25),25))
        malformed=props('99999');malformed.b=malformed.b[:-1]
        records.append(event(packet([bunch(content(malformed),channel=28,seq=2)],26),26))
        records.append(event(packet([bunch(W(),channel=28,seq=3,closed=True,close_reason=3)],27),27))
        write(folder/'in.jsonl',records)
        summary=json.loads(run(exe,'decode-players','--in',folder/'in.jsonl','--output-directory',folder/'out'))
        assert summary['errors']==summary['warnings']==0,summary
        assert summary['lootRejectedBlocks']==1,summary
        rows=[e['data'] for e in lines(folder/'out/loot.jsonl')]
        dormant=next(r for r in rows if r['actorGuid']==100 and r['reason']=='dormant')
        assert dormant['present'] and dormant['itemId']=='15080050142' and dormant['x']==5,dormant
        assert any(r['actorGuid']==100 and not r['present'] for r in rows)
        assert any(r['actorGuid']==102 and not r['present'] and r['reason']=='item_unavailable' for r in rows)
        last=next(r for r in reversed(rows) if r['actorGuid']==104)
        assert last['present'] and last['itemId']=='15080050142' and last['reason']=='out_of_relevancy',last
    print(f'Loot: {checks} native fixtures, truncated blocks, spawn, dormancy, GUID destruction, count-zero and native-only IDs passed; reference parity={reference}')

if __name__=='__main__':main()
