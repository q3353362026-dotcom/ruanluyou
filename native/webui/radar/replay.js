'use strict';
// Immutable row deltas + checkpoints; virtual time is independent of wall time and view preferences.
const RadarReplay=(()=>{
 const cloneState=s=>({...s,players:{...s.players},loot:{...s.loot}});
 const empty=()=>({battle:{status:'waiting'},players:{},loot:{},self:null,serverSelf:null});
 const remember=p=>({capture:p.capture,lastValid:new Map(p.lastValid),unavailable:new Set(p.unavailable),
  histories:new Map([...p.histories].map(([k,v])=>[k,[...v]])),revision:p.revision,session:p.session,
  lastPoll:p.lastPoll,missingSince:new Map(p.missingSince)});
 class Timeline {
  constructor(data,Presenter){
   if(data.format!=='radaer-replay-v1'||!Array.isArray(data.frames)||!data.frames.length||
      !Number.isFinite(data.startMs)||!Number.isFinite(data.endMs)||data.endMs<data.startMs)throw Error('回放格式无效');
   this.data=data;this.frames=data.frames;this.start=data.startMs;this.end=data.endMs;this.time=this.start;
   this.playing=false;this.speed=1;this.index=-1;this.state=empty();this.presenter=new Presenter();this.checkpoints=[];
   let last=this.start;
   for(let i=0;i<this.frames.length;i++){
    const f=this.frames[i];if(!Number.isFinite(f.t)||f.t<last||f.t>this.end)throw Error('回放时间顺序无效');last=f.t;
    this.apply(i);
    if(i%200===0)this.checkpoints.push({index:i,state:cloneState(this.state),presenter:remember(this.presenter)});
   }
   this.seek(this.start);
  }
  snapshot(time=this.time){return {replay:true,revision:this.index+1,serverTimeMs:time,running:this.state.battle.status!=='stopped',
   battle:this.state.battle,players:Object.values(this.state.players),loot:this.state.loot,self:this.state.self,serverSelf:this.state.serverSelf,rates:{}};}
  apply(i){
   const f=this.frames[i];this.index=i;
   for(const key of ['battle','self','serverSelf'])if(Object.hasOwn(f,key))this.state[key]=f[key];
   for(const key of ['players','loot'])for(const [id,row] of Object.entries(f[key]||{})){
    if(row===null){delete this.state[key][id];continue;}
    if(key==='players'&&this.data.rowPatches){const value={...(row.replace?{}:this.state.players[id]),...row.set};
     for(const field of row.remove||[])delete value[field];this.state.players[id]=value;
    }else this.state[key][id]=row;
   }
   // A delta stream still represents a continuously available recording, including long quiet intervals.
   this.presenter.lastPoll=f.t;
   this.presenter.ingest(this.snapshot(f.t),f.t);
  }
  seek(time){
   this.time=Math.max(this.start,Math.min(this.end,Number(time)||this.start));
   let lo=0,hi=this.frames.length;
   while(lo<hi){const mid=(lo+hi)>>1;if(this.frames[mid].t<=this.time)lo=mid+1;else hi=mid;}
   const target=lo-1,checkpoint=target<0?null:this.checkpoints[Math.floor(target/200)];
   this.presenter.reset();this.index=-1;this.state=empty();
   if(checkpoint){this.index=checkpoint.index;this.state=cloneState(checkpoint.state);
    const saved=checkpoint.presenter;Object.assign(this.presenter,{...saved,lastValid:new Map(saved.lastValid),unavailable:new Set(saved.unavailable),
     histories:new Map([...saved.histories].map(([k,v])=>[k,[...v]])),missingSince:new Map(saved.missingSince)});}
   while(this.index<target)this.apply(this.index+1);
   this.presenter.lastPoll=this.time;return this.snapshot();
  }
  advance(elapsed){
   if(this.playing){this.time=Math.min(this.end,this.time+Math.max(0,elapsed)*this.speed);if(this.time>=this.end)this.playing=false;}
   while(this.index+1<this.frames.length&&this.frames[this.index+1].t<=this.time)this.apply(this.index+1);
   this.presenter.lastPoll=this.time;return this.snapshot();
  }
 }
 return {Timeline};
})();
if(typeof module!=='undefined')module.exports=RadarReplay;
