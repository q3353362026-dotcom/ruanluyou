'use strict';
const assert=require('node:assert/strict'),T=require('../webui/radar/model.js'),{Timeline}=require('../webui/radar/replay.js');
const battle={status:'live',flow:'a>b',epoch:1,flowSession:'s'};
const p={channel:14,slot:1,track:'p',actorGuid:2,flow:'a>b',epoch:1,x:100,y:200,z:0,yaw:0,status:'plausible',timestampMs:1000,
 name:'测试玩家',teamId:2,attributes:{heroId:88000000027},equipment:{armor:{level:3}},vitals:{life:{state:'alive'}}};
const frames=[{t:1000,battle,players:{'14:1':p}},...Array.from({length:450},(_,i)=>({t:1100+i*50,players:{'14:1':{...p,x:100+i,timestampMs:1100+i*50}}})),
 {t:24000,players:{'14:1':{...p,status:'rejected',timestampMs:24000,equipment:{armor:{level:4}}}}},
 {t:25000,battle:{...battle,status:'idle'},players:{'14:1':null}},{t:26000,battle,players:{'14:1':{...p,timestampMs:26000,vitals:{life:{state:'dead'}}}}}];
const timeline=new Timeline({format:'radaer-replay-v1',startMs:1000,endMs:27000,frames},T.Presenter);
assert.equal(timeline.presenter.getRows(1000)[0].name,'测试玩家');
timeline.advance(100000);assert.equal(timeline.time,1000,'paused time never advances');
timeline.playing=true;timeline.speed=2;timeline.advance(500);assert.equal(timeline.time,2000);
const sequential=new Timeline(timeline.data,T.Presenter);sequential.playing=true;sequential.advance(23000);
timeline.seek(24000);assert.deepEqual(timeline.presenter.getRows(24000),sequential.presenter.getRows(24000),'seek preserves last valid position and equipment after rejected sample');
assert.equal(timeline.presenter.getRows(24000)[0].equipment.armor.level,4);
const patched=new Timeline({format:'radaer-replay-v1',rowPatches:true,startMs:1000,endMs:2000,frames:[
 {t:1000,battle,players:{'14:1':{replace:true,set:p}}},
 {t:1500,players:{'14:1':{set:{x:250,timestampMs:1500},remove:['equipment']}}}]},T.Presenter);
patched.seek(1500);assert.equal(patched.snapshot().players[0].name,p.name);assert.equal(patched.snapshot().players[0].x,250);
assert.equal(patched.snapshot().players[0].equipment,undefined,'explicitly removed fields do not persist');
patched.seek(1000);assert.equal(patched.snapshot().players[0].equipment.armor.level,3);
timeline.seek(1000);assert.equal(timeline.presenter.getRows(1000)[0].equipment.armor.level,3,'backward seek cannot leak future equipment');
timeline.seek(25000);assert.equal(timeline.presenter.getRows(25000).length,0);
timeline.seek(26000);assert.equal(timeline.presenter.getRows(26000).length,0,'death still hides a player');
timeline.seek(1000);timeline.playing=true;timeline.advance(100000);assert.equal(timeline.time,27000);assert.equal(timeline.playing,false);
console.log('Replay virtual clock, pause/speed, checkpoint seeking, rejected pose retention, idle/death and end passed');
