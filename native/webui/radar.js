'use strict';
// World centimetres: +X to the right, +Y upwards; yaw zero points along +X.
const Radar = (() => {
 const colors = {self:'#f0c75e',mate:'#50ceae',other:'#f08880',unknown:'#a6b9c7'};
 const validTeam = t => Number.isInteger(t) && t >= 0;
 function fresh(p,now) {
  return p && ['x','y','z','timestampMs'].every(k=>Number.isFinite(p[k])) &&
   now-p.timestampMs>=-1000 && now-p.timestampMs<=2000 && ['plausible','warming_up'].includes(p.status);
 }
 function samePerson(p,self) {
  return self && self.actorGuid>0 && p.actorGuid===self.actorGuid;
 }
 function classify(p,self) {
  if(p.kind==='self')return 'self';
  if(!self || !validTeam(self.teamId) || !validTeam(p.teamId) || !p.actorGuid)return 'unknown';
  return p.teamId===self.teamId && !(Number.isInteger(p.camp)&&Number.isInteger(self.camp)&&p.camp!==self.camp)?'mate':'other';
 }
 function project(p,center,range) {
  return {x:500+(p.x-center.x)*440/(range*100),y:500-(p.y-center.y)*440/(range*100),
   rotation:Number.isFinite(p.yaw)?-p.yaw:null};
 }
 function scene(capture,now,follow=true,range=250,connected=true) {
  const battle=capture?.battle||{};
  if(!connected || battle.status!=='live')return {points:[],self:null,center:{x:0,y:0},range,live:false};
  const inFlow=p=>p && p.flow===battle.flow && p.epoch===battle.epoch;
  const self=inFlow(capture.self)&&fresh(capture.self,now)?capture.self:null;
  const rows=(capture.players||[]).filter(p=>inFlow(p)&&fresh(p,now)&&!samePerson(p,self));
  const all=self?[...rows,self]:rows;
  let center={x:0,y:0};
  if(follow&&self)center={x:self.x,y:self.y};
  else if(all.length)center={x:(Math.min(...all.map(p=>p.x))+Math.max(...all.map(p=>p.x)))/2,
   y:(Math.min(...all.map(p=>p.y))+Math.max(...all.map(p=>p.y)))/2};
  const points=all.map(p=>({...p,...project(p,center,range),worldX:p.x,worldY:p.y,relation:classify(p,self)}));
  return {points,self,center,range,live:true};
 }
 const keyOf=p=>p.kind==='self'?'self':`${p.channel}:${p.slot}`;
 const identityOf=p=>`${p.track||''}/${p.actorGuid||0}/${p.playerStateGuid||0}`;
 function between(a,b,t) {
  const dt=b.timestampMs-a.timestampMs;
  // Never bridge a long outage, identity reuse or teleport with invented intermediate positions.
  if(dt<=0||dt>500||Math.hypot(b.x-a.x,b.y-a.y,b.z-a.z)>2500)return {...b};
  const f=Math.max(0,Math.min(1,(t-a.timestampMs)/dt));
  const p={...b};for(const k of ['x','y','z'])p[k]=a[k]+(b[k]-a[k])*f;
  if(Number.isFinite(a.yaw)&&Number.isFinite(b.yaw))p.yaw=a.yaw+(((b.yaw-a.yaw)%360+540)%360-180)*f;
  return p;
 }
 class Timeline {
  constructor(){this.reset(true);}
  reset(resetRevision=false){this.histories=new Map();this.capture={};this.flow='';if(resetRevision)this.revision=-1;}
  ingest(capture,now=Date.now()) {
   if(Number.isFinite(capture.revision)&&capture.revision<this.revision)return false;
   this.revision=capture.revision??this.revision;
   const b=capture.battle||{};
   const flow=b.status==='live'?`${b.flowSession}/${b.flow}/${b.epoch}`:'';
   if(flow!==this.flow||!flow)this.reset();
   this.flow=flow;this.capture=capture;
   if(!flow)return true;
   const active=new Set();
   for(const p of [...(capture.players||[]),...(capture.self?[capture.self]:[])]) {
    const key=keyOf(p);active.add(key);
    if(!fresh(p,now)||p.flow!==b.flow||p.epoch!==b.epoch){this.histories.delete(key);continue;}
    let h=this.histories.get(key);
    if(!h||identityOf(h.at(-1))!==identityOf(p)){h=[];this.histories.set(key,h);}
    if(!h.length||p.timestampMs>h.at(-1).timestampMs)h.push({...p});
    else if(p.timestampMs===h.at(-1).timestampMs)h[h.length-1]={...p};
    while(h.length>32)h.shift();
   }
   for(const key of this.histories.keys())if(!active.has(key))this.histories.delete(key);
   return true;
  }
  frame(now=Date.now(),smooth=true) {
   const sample=p=>{
    const h=this.histories.get(keyOf(p));
    if(!h||!h.length||!fresh(p,now))return p;
    const target=now-(smooth?120:0);let value=h.at(-1);
    if(target<h[0].timestampMs)value=h[0];
    else for(let i=1;i<h.length;i++)if(h[i].timestampMs>=target){value=between(h[i-1],h[i],target);break;}
    // Freshness and identity always describe the latest real sample, not the delayed drawing time.
    return {...p,x:value.x,y:value.y,z:value.z,yaw:value.yaw};
   };
   return {...this.capture,players:(this.capture.players||[]).map(sample),self:this.capture.self?sample(this.capture.self):null};
  }
 }
 const svgNode=(tag,attrs={},text)=>{
  const e=document.createElementNS('http://www.w3.org/2000/svg',tag);
  for(const [k,v] of Object.entries(attrs))e.setAttribute(k,String(v));
  if(text!==undefined)e.textContent=text;
  return e;
 };
 let drawing=null;
 const attr=(e,k,v)=>{v=String(v);if(e.getAttribute(k)!==v)e.setAttribute(k,v);};
 const text=(e,v)=>{if(e.textContent!==v)e.textContent=v;};
 function render(capture,connected,now=Date.now()) {
  const get=id=>document.getElementById(id),range=Number(get('radarRange').value);
  const s=scene(capture,now,get('radarFollow').checked,range,connected);
  const grid=get('radarGrid'),markers=get('radarMarkers');
  if(!drawing||drawing.grid!==grid) {
   drawing={grid,lines:[],markers:new Map(),textAt:0};grid.replaceChildren();markers.replaceChildren();
   for(let i=0;i<20;i++){const line=svgNode('line');grid.append(line);drawing.lines.push(line);}
  }
  const step=range*100/4;
  let lineIndex=0;
  if(s.live)for(const axis of ['x','y']) {
   const first=Math.floor((s.center[axis]-range*100)/step)*step;
   for(let v=first;v<=s.center[axis]+range*100;v+=step){
    const at=project({...s.center,[axis]:v},s.center,range)[axis];
    const line=drawing.lines[lineIndex++];
    for(const [k,v] of Object.entries(axis==='x'?{x1:at,y1:60,x2:at,y2:940}:{x1:60,y1:at,x2:940,y2:at}))attr(line,k,v);
    attr(line,'display','');
   }
  }
  for(let i=lineIndex;i<drawing.lines.length;i++)attr(drawing.lines[i],'display','none');
  let visible=0;const retained=new Set();
  for(const p of s.points) {
   if(p.x<60||p.x>940||p.y<60||p.y>940)continue;
   ++visible;
   const color=colors[p.relation],key=keyOf(p);retained.add(key);
   let m=drawing.markers.get(key);
   if(!m) {
    m={group:svgNode('g',{class:'radar-person'}),title:svgNode('title'),ring:svgNode('circle',{r:23,fill:'none','stroke-opacity':.4,'stroke-width':2}),
       circle:svgNode('circle',{r:12,'stroke-width':2}),arrow:svgNode('path',{d:'M 8 -5 L 24 0 L 8 5 Z'}),label:svgNode('text',{x:0,y:37,'text-anchor':'middle'})};
    for(const name of ['title','ring','circle','arrow','label'])m.group.append(m[name]);
    markers.append(m.group);drawing.markers.set(key,m);
   }
   attr(m.group,'transform',`translate(${p.x.toFixed(3)} ${p.y.toFixed(3)})`);
   const label=p.kind==='self'?'本人':p.name||('槽位 '+p.slot);
   const tags={self:'本人',mate:'同队',other:'其他队伍',unknown:'队伍未知'};
   text(m.title,`${label} · ${tags[p.relation]} · 队伍 ${p.teamId??'—'}\nX ${p.worldX.toFixed(1)} / Y ${p.worldY.toFixed(1)} / Z ${p.z.toFixed(1)}\n朝向 ${Number.isFinite(p.yaw)?p.yaw.toFixed(1)+'°':'未知'}`);
   attr(m.ring,'display',p.kind==='self'?'':'none');attr(m.ring,'stroke',color);
   attr(m.circle,'fill',color);attr(m.circle,'stroke',color);attr(m.circle,'fill-opacity',p.status==='warming_up'?.45:.9);
   attr(m.circle,'stroke-dasharray',p.status==='warming_up'?'4 3':'none');
   attr(m.arrow,'display',p.rotation===null?'none':'');attr(m.arrow,'fill',color);
   if(p.rotation!==null)attr(m.arrow,'transform',`rotate(${p.rotation})`);
   attr(m.label,'display',get('radarNames').checked?'':'none');attr(m.label,'fill',color);text(m.label,label);
  }
  for(const [key,m] of drawing.markers)if(!retained.has(key)){m.group.remove();drawing.markers.delete(key);}
  text(get('radarScale'),`每格 ${range/4} m · 中心到边缘 ${range} m`);
  text(get('radarCount'),`${visible} / ${s.points.length} 个标记`);
  const status=!connected?'连接中断，雷达已清空':!s.live?'等待活跃战斗流':!s.points.length?'等待有效坐标':
   !s.self?'本人坐标暂无或已过期 · 当前按全体居中':get('radarFollow').checked?'跟随本人 · 上行坐标':'全体居中';
  text(get('radarStatus'),status);
  const rates=capture?.rates||{};
  text(get('radarRates'),`真实数据：下行 ${rates.coordinatePacketsPerSecond??0} Hz · 本人上行 ${rates.selfSamplesPerSecond??0} Hz`);
  const self=s.self;
  if(now-drawing.textAt>=200||!self){
   drawing.textAt=now;
   text(get('selfPose'),self?`${self.name||'本人'} · 队伍 ${self.teamId??'未知'} · X ${self.x.toFixed(1)} / Y ${self.y.toFixed(1)} / Z ${self.z.toFixed(1)} · 朝向 ${self.yaw.toFixed(1)}° / 俯仰 ${self.pitch.toFixed(1)}° · ${Math.max(0,now-self.timestampMs)} ms 前`:'本人：等待上行坐标');
  }
 }
 return {fresh,samePerson,classify,project,scene,between,Timeline,render};
})();
if(typeof module!=='undefined')module.exports=Radar;
