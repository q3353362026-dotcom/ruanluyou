"""Wire-to-identity and self lifecycle regressions; synthetic and independent of the driver."""
import copy,json,sys,tempfile
from pathlib import Path
from player_fixtures import *
from player_integration import run,write,lines

exe=Path(sys.argv[1])
with tempfile.TemporaryDirectory(prefix='radaer-radar-') as t:
    root=Path(t)
    def decode(label,records):
        write(root/(label+'.jsonl'),records)
        summary=json.loads(run(exe,'decode-players','--in',root/(label+'.jsonl'),'--output-directory',root/label))
        return summary,root/label
    records=player_records()
    # Exact 64-bit account IDs survive Character/PlayerState recreation. Names
    # alone are not identifiers, and zero/missing Uin must never collapse players.
    account=18446744073709551000
    identified=event(packet([bunch(content(W().bit(0).packed(28).bits(account,64).packed(0)),channel=4,seq=2),
        bunch(movement_payload(),seq=20)],24),24)
    reborn_ps=W().bit(0).packed(27).string('测试玩家').packed(28).bits(account,64).packed(62).bits(7,32).packed(0)
    reborn_char=W().bit(0).packed(18).packed(10).packed(60).packed(18).packed(0)
    reborn=event(packet([
        bunch(actor_prefix(10,7,'BP_SOLPlayerState_C').join(content(reborn_ps)),channel=5,opened=True,exports=True),
        bunch(actor_prefix(8,5,'BP_DFMCharacter_C',True,True).join(content(reborn_char)),channel=6,opened=True,exports=True),
        bunch(movement_payload(slots=(18,)),seq=21)],25),25)
    zero=event(packet([bunch(content(W().bit(0).packed(28).bits(0,64).packed(0)),channel=5,seq=2),
        bunch(movement_payload(slots=(18,)),seq=22)],26),26)
    _,identity_out=decode('account-migration',records+[identified,reborn,zero])
    account_rows=[e['data'] for e in lines(identity_out/'players.jsonl')]
    original=next(p for p in account_rows if p.get('identityKey') and p['actorGuid']==4)
    recreated=next(p for p in account_rows if p.get('identityKey') and p['actorGuid']==8)
    assert len(original['identityKey'])==64 and original['identityKey']==recreated['identityKey']
    assert 'identityKey' not in account_rows[-1] and 'identityKey' not in account_rows[0]
    assert str(account) not in original['identityKey'], 'view identity must be opaque and JavaScript-safe'
    destroyed=event(packet([bunch(W().packed(4),channel=3,seq=2,opened=True,closed=True),
        bunch(movement_payload(),seq=20)],25),25)
    summary,destroy_out=decode('guid-destroy',records+[destroyed])
    assert summary['errors']==0 and lines(destroy_out/'players.jsonl')[-1]['data']['teamId'] is None,summary
    malformed=event(packet([bunch(W().packed(4),channel=3,seq=2,opened=True)],25),25)
    summary,_=decode('incomplete-spawn',records+[malformed])
    assert summary['errors']==1,'missing NewActor fields without a close flag must still fail'
    own=[self_event(i,ts=100+i*.01,full=i%2==0) for i in range(30,35)]
    summary,out=decode('self',records+own)
    assert summary['selfSamples']==5 and summary['samples']==36 and summary['errors']==0,summary
    poses=[e['data'] for e in lines(out/'self.jsonl')]
    assert all(p['teamId']==7 and p['slot']==17 and p['actorGuid']==4 and p['playerStateGuid']==6 for p in poses),poses
    assert all(p['name']=='测试玩家' and p['x']==10000.1 and p['yaw']==90 for p in poses)
    quality=lines(out/'self-quality.jsonl')
    assert [e['data']['status'] for e in quality]==['warming_up','warming_up','plausible','plausible','plausible'],quality
    assert all(e['type']=='self_validation' for e in quality)
    dual=event(packet([bunch(self_payload(ts=200,yaw=30),channel=3,reliable=False),
        bunch(self_payload(ts=201,yaw=60,full=False),channel=3,reliable=False)],40),40)
    dual['data']['direction']='up'
    next_pose=self_event(41,ts=202)
    next_pose['timestampMs']=next_pose['data']['timestampMs']=dual['timestampMs']
    summary,out=decode('coalesced',records+[dual,next_pose])
    poses=lines(out/'self.jsonl');quality=lines(out/'self-quality.jsonl')
    assert summary['selfSamples']==2 and poses[0]['data']['yaw']==60.0018310546875,poses
    assert quality[1]['data']['status']=='warming_up' and quality[1]['data']['intervalSource'].startswith('upstream_game_clock'),quality
    # Only a unique PlayerState sharing the Controller owner may resolve missing Character.PlayerState.
    ps=W().bit(0).packed(14).packed(100).packed(27).string('Owner bridge').packed(62).bits(8,32).packed(0)
    char=W().bit(0).packed(14).packed(100).packed(60).packed(17).packed(0)
    bridge=copy.deepcopy(records[:4])
    bridge[1]=event(packet([bunch(actor_prefix(6,7,'BP_SOLPlayerState_C').join(content(ps)),channel=4,opened=True,exports=True)],2),2)
    bridge[2]=event(packet([bunch(actor_prefix(4,5,'BP_DFMCharacter_C',True,True).join(content(char)),channel=3,opened=True,exports=True)],3),3)
    _,out=decode('bridge',bridge+records[4:])
    named=[e['data'] for e in lines(out/'players.jsonl') if e['data']['slot']==17]
    assert all(p['teamId']==8 and p['playerStateSource']=='shared_Controller_Owner' for p in named),named
    ambiguous=event(packet([bunch(actor_prefix(10,7,'BP_SOLPlayerState_C').join(content(ps)),channel=5,opened=True,exports=True),
        bunch(movement_payload(),seq=20)],24),24)
    _,out=decode('ambiguous-owner',bridge+records[4:]+[ambiguous])
    assert lines(out/'players.jsonl')[-1]['data']['teamId'] is None
    # Latest explicit assignment transfers a reused slot; release never revives its old owner.
    newps=W().bit(0).packed(27).string('Replacement').packed(62).bits(9,32).packed(0)
    newchar=W().bit(0).packed(18).packed(10).packed(60).packed(17).packed(0)
    reuse=event(packet([
        bunch(actor_prefix(10,7,'BP_SOLPlayerState_C').join(content(newps)),channel=5,opened=True,exports=True),
        bunch(actor_prefix(8,5,'BP_DFMCharacter_C',True,True).join(content(newchar)),channel=6,opened=True,exports=True),
        bunch(movement_payload(),seq=20)],24),24)
    release=event(packet([bunch(content(W().bit(0).packed(60).packed(0xffffffff).packed(0)),channel=6,seq=2),
        bunch(movement_payload(),seq=21)],25),25)
    _,out=decode('reuse',records+[reuse,release])
    tail=[e['data'] for e in lines(out/'players.jsonl')[-4:] if e['data']['slot']==17]
    assert tail[0]['teamId']==9 and tail[0]['actorGuid']==8 and tail[1]['teamId'] is None,tail
    collision=event(packet([
        bunch(content(W().bit(0).packed(60).packed(17).packed(0)),channel=3,seq=2),
        bunch(actor_prefix(8,5,'BP_DFMCharacter_C',True,True).join(content(newchar)),channel=6,opened=True,exports=True),
        bunch(movement_payload(),seq=20)],24),24)
    _,out=decode('collision',records+[collision])
    tail=lines(out/'players.jsonl')[-1]['data']
    assert tail['teamId'] is None and tail['identitySource']=='conflicting_explicit_claims',tail
    # Preserve a complete actor block before malformed later subobject data; keep the warning visible.
    trailing=copy.deepcopy(records)
    trailing[2]=event(packet([bunch(actor_prefix(4,5,'BP_DFMCharacter_C',True,True).join(content(
        W().bit(0).packed(18).packed(6).packed(60).packed(17).packed(0))).bits(1,1),
        channel=3,opened=True,exports=True)],3),3)
    summary,out=decode('tail',trailing)
    assert summary['warnings']==1 and summary['errors']==0,summary
    assert all(e['data']['teamId']==7 for e in lines(out/'players.jsonl') if e['data']['slot']==17)
    # Upstream partial chain, duplicate packet, unrelated ch3 messages and reset all remain isolated.
    w=self_payload();a=W();a.b=w.b[:101];b=W();b.b=w.b[101:]
    split=[]
    for i,(body,parts) in enumerate(((a,(True,False)),(b,(False,True))),40):
        e=event(packet([bunch(body,channel=3,seq=i,partial=parts)],i),i);e['data']['direction']='up';split.append(e)
    gap={'type':'capture_gap','session':'ue-test','timestampMs':1700000000500,'data':{}}
    post=self_event(60)
    summary,out=decode('partial',records+split+[split[-1],gap,post])
    poses=lines(out/'self.jsonl')
    assert len(poses)==2 and poses[0]['data']['teamId']==7 and poses[1]['data']['teamId'] is None,poses
    assert poses[0]['data']['handshakeOk'] and not poses[1]['data']['handshakeOk']
    assert summary['errors']==0,summary
print('Self full/lite, validation, partial reassembly, identity/teams, slot reuse and reset passed')
