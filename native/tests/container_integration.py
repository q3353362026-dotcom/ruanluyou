"""Class-specific container wire fixtures, classification and lifetime regression tests."""
import json, subprocess, sys, tempfile
from pathlib import Path
from player_fixtures import W, actor_prefix, content, bunch, packet, event, player_records, pv
from player_integration import run, write, lines


def box_props(mesh=None, ai=None, taker=None, empty=None, hidden=None, maker=None):
    w = W().bit(0)
    if hidden is not None: w.packed(1).bit(hidden)
    if empty is not None: w.packed(113).bit(empty)
    if maker is not None: w.packed(maker).bit(1)
    if taker is not None: w.packed(taker).bit(1)
    if ai is not None: w.packed(175).bit(ai)
    if mesh is not None: w.packed(185).bits(mesh, 4)
    return w.packed(0)


def opened(actor, channel, body=None, cls='BP_Inventory_DeadBody_C', transforms=True):
    prefix = actor_prefix(actor, 301, cls, character=True, transforms=transforms)
    prefix.b = prefix.b[:-8]  # Containers use quantized NewActor transforms, without the Character extra byte.
    if body is not None: prefix.join(content(body))
    return bunch(prefix, channel=channel, seq=1, opened=True, exports=True)


def probe(exe, body, profile='death_box'):
    return json.loads(subprocess.check_output([str(exe)], input=json.dumps(
        dict(mode='properties', profile=profile, hex=body.raw().hex(), bits=len(body.b))).encode()))['result']


def main():
    exe, reader = map(Path, sys.argv[1:])
    # Actual class-specific enum is four raw bits, not SerializeInt(11).
    for mesh in range(11):
        assert probe(reader, box_props(mesh=mesh))['fields']['DeadBodyBoxInfo.DeadBodySkeletalMeshType'] == mesh
    for mesh in range(11, 16): assert probe(reader, box_props(mesh=mesh))['status'] == 'error'
    # Legacy class omits three top-level handles before these fields.
    legacy = W().bit(0).packed(182).bits(7, 4).packed(0)
    assert probe(reader, legacy, 'death_box_legacy')['fields']['DeadBodyBoxInfo.DeadBodySkeletalMeshType'] == 7
    # Geometry and localized text must be consumed correctly before classification fields.
    body = W().bit(0).packed(103).bits(1,32).bits(0,64).bits(1,32).bits(0,5*32+16)
    body.packed(144).bits(0,32).bits(255,8).bits(1,32).string('fixture')
    body.packed(185).bits(6,4).packed(0)
    assert probe(reader,body)['status']=='exact'
    for length in range(len(body.b)):
        truncated=W();truncated.b=body.b[:length]
        result=probe(reader,truncated)
        assert result['status']=='error' and not result['fields'],(length,result)
    # Physics RepMovement consumes its angular velocity at the established scale.
    movement=W().bit(0).packed(7).bits(2,2);pv(movement);movement.bits(0,3);pv(movement);pv(movement);movement.packed(0)
    result=probe(reader,movement)
    assert result['status']=='exact' and result['fields']['ReplicatedMovement']['location']==[0,0,0],result
    with tempfile.TemporaryDirectory(prefix='radaer-containers-') as path:
        path=Path(path);records=player_records(3)
        def add(b):
            n=len(records)+20;records.append(event(packet([b],n),n))
        fixtures=[(200,box_props(mesh=6),'boss_death'),(202,box_props(mesh=1),'player_death'),
                  (204,box_props(mesh=4),'ai_death'),(206,box_props(),'death_unknown'),
                  (208,box_props(taker=140),'boss_death'),(210,box_props(taker=137),'player_death'),
                  (212,box_props(taker=139),'ai_death'),(214,box_props(taker=138),'ai_death'),
                  (216,box_props(maker=135),'death_unknown'),(218,box_props(ai=True),'ai_death'),
                  (220,box_props(ai=False),'player_death')]
        for i,(actor,body,_) in enumerate(fixtures):add(opened(actor,30+i,body))
        add(opened(222,50,cls='Default__BP_InteractorContainer_Safe_C'))
        add(opened(224,51,cls='BP_LootContainer_Crate_C'))
        add(opened(226,52,cls='BP_NotAContainer_Box_C'))
        add(opened(228,53,box_props(mesh=7),transforms=False))
        # Invalid change must not overwrite the boss classification; dormancy retains it.
        bad=box_props(mesh=1);bad.b=bad.b[:-1]
        add(bunch(content(bad),channel=30,seq=2))
        add(bunch(W(),channel=30,seq=3,closed=True,close_reason=1))
        add(bunch(W().packed(200),channel=60,seq=1,opened=True,closed=True))
        add(bunch(content(box_props(empty=True)),channel=31,seq=2))
        add(bunch(W(),channel=50,seq=2,closed=True,close_reason=3))
        add(bunch(content(box_props(hidden=True)),channel=32,seq=2))
        write(path/'in.jsonl',records)
        summary=json.loads(run(exe,'decode-players','--in',path/'in.jsonl','--output-directory',path/'out'))
        assert summary['errors']==summary['warnings']==0,summary
        assert summary['lootRejectedBlocks']==1,summary
        rows=[e['data'] for e in lines(path/'out/loot.jsonl')]
        final={r['actorGuid']:r for r in rows}
        for actor,_,kind in fixtures:
            assert final[actor]['containerType']==kind,(actor,final[actor])
            assert final[actor]['x']==5 and 'itemId' not in final[actor] and final[actor]['contentsKnown'] is False
        assert any(r['actorGuid']==200 and r['reason']=='dormant' and r['present'] and r['containerType']=='boss_death' for r in rows)
        assert not final[200]['present'] and not final[204]['present']
        assert final[202]['present'] and final[202]['empty'] is True,'empty boxes remain visible with an explicit label'
        assert final[222]['containerType']==final[224]['containerType']=='ordinary'
        assert final[222]['present'] and final[222]['reason']=='out_of_relevancy'
        assert 226 not in final and 'x' not in final[228],'unknown classes/absent transforms cannot invent locations'
        # If the profile export has not arrived, only spawn identity/location is safe to publish.
        write(path/'unknown.jsonl',[event(packet([opened(300,70,box_props(mesh=6))],1),1)])
        run(exe,'decode-players','--in',path/'unknown.jsonl','--output-directory',path/'unknown')
        unknown=lines(path/'unknown/loot.jsonl')
        assert all(e['data']['containerType']=='death_unknown' for e in unknown) and unknown
    print('Containers: current/legacy wire, all-bit truncations, native geometry/text, physics, explicit deceased types, lifecycle and safe fallbacks passed')


if __name__=='__main__':main()
