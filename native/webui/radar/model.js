'use strict';
// Shared by the standalone renderer and deterministic geometry/lifecycle tests.
const Tactical = (() => {
 // Alternate strong hues; explicit team labels disambiguate palette reuse.
 const teamColors=['#ff525f','#ffd43b','#b980ff','#ff9234','#ff64cf','#c5ed39','#7187ff','#fff0ce','#d553ff','#ffbd85','#ff83a3','#b2a4ff'];
 const finite=Number.isFinite;
 const TARGET_FPS=120,HOLD_MS=5000,UNKNOWN_MS=300000,STALE_MS=500;
 const fresh=(p,now)=>p&&['x','y','z','timestampMs'].every(k=>finite(p[k]))&&
  now-p.timestampMs>=-1000&&now-p.timestampMs<=2000&&['plausible','warming_up'].includes(p.status);
 const key=p=>p.kind==='self'?'self':`${p.channel}:${p.slot}`;
 const identity=p=>`${p.track}/${p.actorGuid}/${p.playerStateGuid}`;
 const samePlayer=(a,b)=>a.identityKey&&b.identityKey?a.identityKey===b.identityKey:
  !!((a.actorGuid&&a.actorGuid===b.actorGuid)||(a.playerStateGuid&&a.playerStateGuid===b.playerStateGuid));
 const wrap=a=>((a+180)%360+360)%360-180;
 const angle=(a,b,t)=>a+wrap(b-a)*t;
 function relation(p,self){
  if(p.kind==='self')return 'self';
  if(!self||!p.actorGuid||!Number.isInteger(p.teamId)||!Number.isInteger(self.teamId)||p.teamId<0||self.teamId<0)return 'unknown';
  return p.teamId===self.teamId&&!(Number.isInteger(p.camp)&&Number.isInteger(self.camp)&&p.camp!==self.camp)?'mate':'other';
 }
 function color(p,self){const r=relation(p,self);return r==='self'?'#50f0a0':r==='mate'?'#48dfff':r==='unknown'?'#a9b6c6':teamColors[Math.abs(p.teamId)%teamColors.length];}
 function heading(p,now){const m=p.motion||{};return finite(m.viewYaw)&&finite(m.viewAtMs)&&now-m.viewAtMs>=-1000?m.viewYaw:finite(p.yaw)?p.yaw:null;}
 function speed(p,now){const m=p.motion||{};return Array.isArray(m.velocity)&&m.velocity.length===3&&m.velocity.every(finite)&&now-m.velocityAtMs>=-1000&&now-m.velocityAtMs<=1000?Math.hypot(...m.velocity)/100:null;}
 function lifeState(p){return p.vitals?.life?.state||({1:'alive',2:'dead',3:'downed'}[p.attributes?.liveStatus])||'unknown';}
 function life(p){return {alive:'存活',dead:'死亡',downed:'倒地',unknown:'状态未知'}[lifeState(p)]||'状态未知';}
 function hp(p){const value=p.vitals?.health?.Health?.value;return finite(value)&&value>=0?value:null;}
 function hpRing(p){
  const value=hp(p),maximum=p.vitals?.health?.MaxHealth?.value;
  const confirmed=finite(maximum)&&maximum>0;
  return {value,maximum:confirmed?maximum:100,assumed:!confirmed,ratio:value===null?null:Math.max(0,Math.min(1,value/(confirmed?maximum:100)))};
 }
 function gear(p){
  const e=p.equipment||{},w=e.weapon;
  const weapon=w?.status==='empty'?'空手':w?.status==='resolved'?(w.name||`武器 ${w.itemId}`):'未知';
  const level=slot=>Number.isInteger(slot?.level)&&slot.level>0?`${slot.level}级`:'未知';
  return {weapon,helmet:level(e.helmet),armor:level(e.armor)};
 }
 class Operators {
  constructor(){this.entries=new Map();}
  load(data){
   this.entries.clear();
   for(const [id,v] of Object.entries(data?.operators||{})){
    if(!/^[1-9][0-9]{0,14}$/.test(id)||!Number.isSafeInteger(Number(id))||typeof v?.name!=='string'||!v.name.trim())continue;
    const portrait=`/radar/portraits/${id}.png`;
    this.entries.set(id,{id,name:v.name.trim().slice(0,40),portrait:v.portrait===portrait?portrait:null});
   }
  }
  get(p){
   const raw=p.attributes?.heroId,id=typeof raw==='number'&&Number.isSafeInteger(raw)&&raw>0?String(raw):typeof raw==='string'&&/^[1-9][0-9]{0,14}$/.test(raw)?raw:null;
   return this.entries.get(id)||{id,name:'未知干员',portrait:null};
  }
 }
 const rarityColors=['#a9b6c6','#ffffff','#49df80','#42a5ff','#ba74ff','#ffd34a','#ff4d58'];
 const containerTypes=[
  ['boss_death','首领死亡盒','#ffd34a'],['player_death','角色死亡盒','#6ebcff'],
  ['ai_death','人机死亡盒','#65d7b4'],['ordinary','普通容器','#d1b99b'],
  ['death_unknown','死亡盒（类型未知）','#a9b6c6']
 ];
 class Items {
  constructor(){
   this.entries=new Map();
   this.containers=new Map(containerTypes.map(([type,name,color])=>[type,{id:`container:${type}`,name,color,grade:0,category:'containers',container:true}]));
   this.categories=this.baseCategories();
  }
  baseCategories(){return new Map([['containers',{name:'容器与死亡盒',group:'容器'}],['unknown',{name:'未知物品',group:'其他'}]]);}
  load(data){
   if(!data?.items||!data?.categories)throw Error('物资目录格式错误');
   this.entries.clear();this.categories=this.baseCategories();
   for(const [key,c] of Object.entries(data.categories))if(typeof c?.name==='string'&&typeof c.group==='string')this.categories.set(key,{name:c.name.slice(0,40),group:c.group.slice(0,40)});
   for(const [id,v] of Object.entries(data.items))if(/^[0-9]{1,16}$/.test(id)&&typeof v?.name==='string'){
    const grade=Number.isInteger(v.grade)&&v.grade>=1&&v.grade<=6?v.grade:0;
    this.entries.set(id,{id,name:v.name.slice(0,80),grade,color:rarityColors[grade],category:this.categories.has(v.category)?v.category:'unknown'});
   }
  }
  get(id){return this.entries.get(String(id))||{id:String(id),name:`未知物品 ${id}`,grade:0,color:rarityColors[0],category:'unknown'};}
  rows(c,pref){
   if(pref.loot===false||c.running===false||c.battle?.status!=='live')return [];
   const b=c.battle,rows=[];
   for(const p of Object.values(c.loot||{})){
    if(p.present!==true||p.flow!==b.flow||p.epoch!==b.epoch||!['x','y','z'].every(k=>finite(p[k])&&Math.abs(p[k])<=2097152))continue;
    const item=p.kind==='container'?this.containers.get(p.containerType):p.itemId?this.get(p.itemId):null;
    if(!item)continue;
    const group=this.categories.get(item.category)?.group||'其他';
    if(pref.lootGroups?.[group]===false||pref.lootCategories?.[item.category]===false||!item.container&&pref.lootGrades?.[item.grade]===false||pref.lootItems?.[item.id]===false)continue;
    rows.push({...p,item});
   }
   return rows.sort((a,b)=>b.item.grade-a.item.grade||a.actorGuid-b.actorGuid);
  }
 }
 function selfSeparation(p,s,now){
  return p?.kind==='self'&&s?.kind==='self_server'&&fresh(p,now)&&fresh(s,now)&&
   p.flow===s.flow&&p.epoch===s.epoch&&p.actorGuid>0&&p.actorGuid===s.actorGuid?
   Math.hypot(s.x-p.x,s.y-p.y,s.z-p.z)/100:null;
 }
 function worldToPlane(x,y,m){
  if(!m)return {lat:y/100,lng:x/100};
  const b=m.bj||128,dx=(m.centerX-x)/(m.width/b),dy=(m.centerY+y)/(m.height/b);
  return m.rotate===90?{lng:b-dy,lat:-b+dx}:m.rotate===-90?{lng:b+dy,lat:-b-dx}:{lng:b-dx,lat:-b-dy};
 }
 function screenAngle(p,yaw,m){
  if(!finite(yaw))return null;
  const r=yaw*Math.PI/180,a=worldToPlane(p.x,p.y,m),b=worldToPlane(p.x+Math.cos(r)*100,p.y+Math.sin(r)*100,m);
  return Math.atan2(-(b.lat-a.lat),b.lng-a.lng);
 }
 function visible(c,now){
  const b=c.battle||{};if(b.status!=='live')return [];
  const valid=p=>fresh(p,now)&&p.flow===b.flow&&p.epoch===b.epoch;
  const self=valid(c.self)?c.self:null;
  return [...(c.players||[]).filter(p=>valid(p)&&!(self&&self.actorGuid>0&&self.actorGuid===p.actorGuid)),...(self?[self]:[])];
 }
 class Presenter {
  constructor(){this.reset();}
  reset(){this.capture={};this.lastValid=new Map();this.unavailable=new Set();this.histories=new Map();this.drawn=new Map();this.revision=-1;this.session='';this.lastFrame=null;this.lastPoll=null;this.missingSince=new Map();}
  forget(k){this.lastValid.delete(k);this.unavailable.delete(k);this.histories.delete(k);this.drawn.delete(k);this.missingSince.delete(k);}
  ingest(c,now){
   const b=c.battle||{},session=`${b.flowSession}/${b.flow}/${b.epoch}`;
   if(session===this.session&&finite(c.revision)&&c.revision<this.revision)return false;
   if(session!==this.session||b.status!=='live'||c.running===false)this.reset();
   this.session=session;this.capture=c;this.revision=c.revision??this.revision;this.lastPoll=now;
   if(b.status!=='live'||c.running===false)return true;
   // Position, view and life have separate clocks. Silence in a delta field is not removal.
   this.unavailable=new Set(this.lastValid.keys());
   players: for(let p of [...(c.players||[]),...(c.self?[c.self]:[])]){
    if(p.flow!==b.flow||p.epoch!==b.epoch)continue;
    const k=key(p),previous=this.lastValid.get(k);
    if(previous&&finite(p.timestampMs)&&p.timestampMs<previous.timestampMs)continue;
    // The backend retired this account's old incarnation; do not hold its ghost
    // for the normal missing-row grace period or interpolate across incarnations.
    if(p.kind!=='self')for(const [oldKey,old] of this.lastValid){
     if(oldKey===k||old.kind==='self'||!samePlayer(old,p))continue;
     if(p.timestampMs<old.timestampMs)continue players;
     this.forget(oldKey);
    }
    // A reused slot cannot inherit the old actor even when its first pose is rejected.
    if(previous&&identity(previous)!==identity(p))this.forget(k);
    if(p.lifecycle?.present===false){this.forget(k);continue;}
    this.missingSince.delete(k);
    const cached=this.lastValid.get(k);
    const good=['x','y','z','timestampMs'].every(k=>finite(p[k]))&&now-p.timestampMs>=-1000&&['plausible','warming_up'].includes(p.status);
    if(!good){
     if(cached)this.lastValid.set(k,{...cached,vitals:p.vitals||cached.vitals,equipment:p.equipment||cached.equipment,attributes:p.attributes||cached.attributes,lifecycle:p.lifecycle||cached.lifecycle,
      motion:(p.motion?.updatedAtMs??0)>=(cached.motion?.updatedAtMs??0)?p.motion:cached.motion});
     continue;
    }
    this.lastValid.set(k,p);this.unavailable.delete(k);let h=this.histories.get(k);
    if(!h){h=[];this.histories.set(k,h);}
    if(!h.length||p.timestampMs>h.at(-1).timestampMs)h.push(p);
    else if(p.timestampMs===h.at(-1).timestampMs)h[h.length-1]=p;
    while(h.length>24)h.shift();
   }
   const supplied=new Set([...(c.players||[]),...(c.self?[c.self]:[])].filter(p=>p.flow===b.flow&&p.epoch===b.epoch).map(key));
   for(const k of this.lastValid.keys())if(!supplied.has(k)&&!this.missingSince.has(k))this.missingSince.set(k,now);
   this.getRows(now);
   return true;
  }
  getRows(now,offline=false){
   const rows=[];
   for(const [k,p] of this.lastValid){
    if(now-this.lastPoll>HOLD_MS||(this.missingSince.has(k)&&now-this.missingSince.get(k)>HOLD_MS)||
       (lifeState(p)==='unknown'&&now-p.timestampMs>UNKNOWN_MS)||now-p.timestampMs< -1000){this.forget(k);continue;}
    if(lifeState(p)==='dead')continue;
    rows.push({...p,held:offline||p.lifecycle?.reason==='out_of_relevancy'||this.unavailable.has(k)||now-p.timestampMs>STALE_MS});
   }
   const self=rows.find(p=>p.kind==='self');
   return rows.filter(p=>p.kind==='self'||!(self&&self.actorGuid>0&&self.actorGuid===p.actorGuid));
  }
  frame(now,mode='original',predict=false,offline=false){
   const dt=this.lastFrame===null?1/TARGET_FPS:Math.max(0,Math.min(.25,(now-this.lastFrame)/1000));this.lastFrame=now;
   const a=1-Math.exp(-dt/.12),out=[];
   for(const p of this.getRows(now,offline)){
    const k=key(p);
    let q={...p},target=now-120,h=this.histories.get(k)||[];
    if(mode==='interpolate'&&h.length){
     const after=h.findIndex(v=>v.timestampMs>=target);
     if(after===0)q={...p,x:h[0].x,y:h[0].y,z:h[0].z,yaw:h[0].yaw};
     else if(after>0){const x=h[after-1],y=h[after],gap=y.timestampMs-x.timestampMs;
      if(gap>0&&gap<=500&&Math.hypot(x.x-y.x,x.y-y.y,x.z-y.z)<=3000){const t=(target-x.timestampMs)/gap;
       for(const axis of ['x','y','z'])q[axis]=x[axis]+(y[axis]-x[axis])*t;
       if(finite(x.yaw)&&finite(y.yaw))q.yaw=angle(x.yaw,y.yaw,t);
      }
     }
    }
    if(mode==='original'){
     if(predict&&!p.held&&speed(p,now)!==null&&now-p.timestampMs<=150&&p.decodeCompleteness==='complete'){
      const t=Math.max(0,Math.min(.08,(now-p.timestampMs)/1000));
      if(Math.hypot(...p.motion.velocity)*t<=500)for(const [i,axis] of ['x','y','z'].entries())q[axis]+=p.motion.velocity[i]*t;
     }
     const prev=this.drawn.get(k);
     if(prev&&p.timestampMs-prev.sourceAt<=500&&Math.hypot(prev.x-q.x,prev.y-q.y,prev.z-q.z)<=3000)
      for(const axis of ['x','y','z'])q[axis]=prev[axis]+(q[axis]-prev[axis])*a;
    }
    this.drawn.set(k,{...q,sourceAt:p.timestampMs});out.push(q);
   }
   return out;
  }
 }
 class FrameGate {
  constructor(){this.next=null;}
  ready(t,visible=true){if(!visible){this.next=null;return false;}const step=1000/TARGET_FPS;if(this.next!==null&&t+.5<this.next)return false;
   this.next=this.next===null?t+step:this.next+Math.max(1,Math.floor((t-this.next+.5)/step)+1)*step;return true;}
 }
 class TileLoadState {
  constructor(){this.tiles=new Map();}
  update(tile,state){if(state==='unload')this.tiles.delete(tile);else this.tiles.set(tile,state);}
  failed(visible=()=>true){return [...this.tiles].some(([tile,state])=>state==='error'&&visible(tile));}
 }
 return {TARGET_FPS,HOLD_MS,UNKNOWN_MS,STALE_MS,fresh,key,identity,relation,color,heading,speed,lifeState,life,hp,hpRing,gear,Operators,Items,rarityColors,selfSeparation,worldToPlane,screenAngle,visible,Presenter,FrameGate,TileLoadState};
})();
if(typeof module!=='undefined')module.exports=Tactical;
