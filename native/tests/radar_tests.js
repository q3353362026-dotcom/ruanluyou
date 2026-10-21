'use strict';
const assert=require('node:assert/strict');
const Radar=require('../webui/radar.js');
const self={x:10000,y:20000,z:300,yaw:0,pitch:0,kind:'self',actorGuid:4,teamId:7,flow:'a>b',epoch:1,timestampMs:1000,status:'plausible'};
const capture={battle:{status:'live',flow:'a>b',epoch:1},self,players:[{...self,kind:'player',slot:17},
 {...self,actorGuid:6,kind:'player',x:20000,y:30000,yaw:90}]};
let s=Radar.scene(capture,1100,true,100);
assert.equal(s.points.length,2);assert.equal(s.points[0].relation,'mate');
assert.equal(s.points[0].x,940);assert.equal(s.points[0].y,60);assert.equal(s.points[0].rotation,-90);
assert.equal(s.points[1].x,500);assert.equal(s.points[1].y,500);assert.ok(s.points[1].rotation===0);
assert.equal(Radar.classify({...self,kind:'player',teamId:null},self),'unknown');
assert.equal(Radar.classify({...self,kind:'player',teamId:8},self),'other');
assert.equal(Radar.classify({...self,kind:'player'},null),'unknown');
assert.equal(Radar.project({...self,yaw:null},self,100).rotation,null);
assert.equal(Radar.scene(capture,3001).points.length,0);
assert.equal(Radar.scene({...capture,battle:{...capture.battle,status:'idle'}},1100).points.length,0);
assert.equal(Radar.scene(capture,1100,true,100,false).points.length,0);
assert.equal(Radar.scene({...capture,battle:{...capture.battle,epoch:2}},1100).points.length,0);
assert.equal(Radar.scene({...capture,players:[],self:{...self,status:'rejected'}},1100).points.length,0);
assert.equal(Radar.scene(capture,1100,false,100).center.x,15000);
// Minimal DOM exercises the real SVG renderer, including hostile player names as text.
class Element {
 constructor(tag='div'){this.tag=tag;this.children=[];this.attrs={};this.textContent='';}
 append(e){this.children.push(e);e.parent=this;}replaceChildren(){this.children=[];}setAttribute(k,v){this.attrs[k]=v;}
 getAttribute(k){return this.attrs[k];}remove(){this.parent.children=this.parent.children.filter(e=>e!==this);}
}
const elements={};global.document={getElementById:id=>elements[id]??=new Element(),createElementNS:(_,tag)=>new Element(tag)};
document.getElementById('radarRange').value=100;document.getElementById('radarFollow').checked=true;document.getElementById('radarNames').checked=true;
capture.self.timestampMs=Date.now();capture.players=capture.players.map(p=>({...p,timestampMs:Date.now(),name:'<script>unsafe</script>'}));
Radar.render(capture,true);
assert.equal(elements.radarMarkers.children.length,2);
assert.equal(elements.radarMarkers.children[0].children.at(-1).textContent,'<script>unsafe</script>');
assert.equal(elements.radarMarkers.children[0].children.find(e=>e.tag==='path').attrs.transform,'rotate(-90)');
const original=elements.radarMarkers.children[0];Radar.render(capture,true);
assert.equal(elements.radarMarkers.children[0],original,'animation must reuse marker DOM');
Radar.render(capture,false);assert.equal(elements.radarMarkers.children.length,0);
const timeline=new Radar.Timeline(),base={...self,timestampMs:1000,x:0,y:0,yaw:359};
const feed=(p,revision=1)=>timeline.ingest({...capture,revision,players:[],self:p},p.timestampMs);
feed(base);feed({...base,timestampMs:1100,x:100,yaw:1},2);
let frame=timeline.frame(1170).self;
assert.equal(frame.x,50);assert.equal(frame.yaw,360);assert.equal(frame.timestampMs,1100);
assert.equal(timeline.frame(1170,false).self.x,100);
assert.equal(timeline.frame(2100).self.x,100,'never extrapolate past last real sample');
assert.equal(feed({...base,timestampMs:1050,x:999},1),false,'late HTTP response cannot rewind state');
feed({...base,timestampMs:1200,x:800,track:'reused'},3);
assert.equal(timeline.frame(1230).self.x,800,'identity reuse does not interpolate');
feed({...base,timestampMs:1300,x:900,status:'rejected'},4);
assert.equal(timeline.histories.size,0);
timeline.ingest({...capture,revision:5,battle:{...capture.battle,epoch:2},self:null,players:[]},1400);
assert.equal(timeline.histories.size,0);assert.equal(timeline.frame(1400).self,null);
assert.equal(Radar.between(base,{...base,timestampMs:2000,x:100},1500).x,100,'long gap snaps');
assert.equal(Radar.between(base,{...base,timestampMs:1100,x:10000},1050).x,10000,'teleport snaps');
console.log('Radar geometry, heading, team classification, stale/reset and SVG rendering passed');
