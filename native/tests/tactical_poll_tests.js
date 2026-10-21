'use strict';
// Exercise the actual page polling/render loop without a browser or network.
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
class Element {
 constructor(){this.children=[];this.handlers={};this.value='';this.hidden=false;this.style={};this.classList={toggle(){}};this.textContent='';}
 addEventListener(name,fn){this.handlers[name]=fn;}
 append(...children){this.children.push(...children);}
 replaceChildren(...children){this.children=children;}
 setAttribute(){}
 getContext(){return context;}
}
const elements=new Map(),el=id=>{if(!elements.has(id))elements.set(id,new Element());return elements.get(id);};
const images=[],draws=[],paintLabels=[],paintArcs=[];let rotation=0;const transforms=[];
class FakeImage {
 constructor(){this.naturalWidth=127;this.naturalHeight=169;}
 set src(value){this.url=value;images.push(value);queueMicrotask(()=>value.endsWith('028.png')?this.onerror():this.onload());}
}
const context=new Proxy({globalAlpha:1}, {get:(o,k)=>k in o?o[k]:k==='measureText'?s=>({width:s.length*6}):k==='createLinearGradient'?()=>({addColorStop(){}}):
 k==='save'?()=>transforms.push({rotation,alpha:o.globalAlpha}):k==='restore'?()=>{const saved=transforms.pop();rotation=saved.rotation;o.globalAlpha=saved.alpha;}:k==='rotate'?a=>{rotation+=a;}:
 k==='drawImage'?(img,...args)=>{if(img.url)draws.push({url:img.url,rotation,args,alpha:o.globalAlpha});}:k==='fillText'?s=>paintLabels.push(s):k==='arc'?(_x,_y,r)=>paintArcs.push(r):()=>{}});
el('overlay').getContext=()=>context;
let now=1000,reply,replayReply,raf=[],timers=new Map(),timerId=0,savedPref=null,liveRequests=0;
class Events {
 constructor(){this.events=new Map();}
 on(names,fn){for(const name of names.split(' ')){if(!this.events.has(name))this.events.set(name,new Set());this.events.get(name).add(fn);}return this;}
 off(names,fn){for(const name of names.split(' '))this.events.get(name)?.delete(fn);return this;}
 fire(name,data){for(const fn of this.events.get(name)||[])fn(data);}
}
const tileLayers=[],mapCalls=[];
const map=Object.assign(new Events(),{setView(){return this;},invalidateSize(){},setMinZoom(){},setZoom(){},panTo(){},fitBounds(){mapCalls.push('fit');},removeLayer(){},zoomIn(){},zoomOut(){},
 getContainer:()=>({getBoundingClientRect:()=>({left:0,top:0,right:390,bottom:844})}),
 latLngToContainerPoint:([lat,lng])=>({x:lng+600,y:400-lat}),containerPointToLatLng:([x,y])=>({lng:x-600,lat:400-y})});
const mapCatalog=JSON.parse(fs.readFileSync(require.resolve('../webui/radar/maps.json'),'utf8'));
const own={kind:'self',actorGuid:1,track:'self',x:100,y:200,z:0,yaw:0,teamId:1,timestampMs:1000,status:'plausible',flow:'a>b',epoch:1,attributes:{heroId:88000000025}};
const other={...own,kind:'player',actorGuid:2,track:'other',channel:15,slot:1,x:200,yaw:90,attributes:{heroId:88000000027}};
const scene={revision:1,serverTimeMs:1000,running:true,battle:{status:'live',flow:'a>b',epoch:1,flowSession:'s'},players:[other],self:own};
reply=scene;
const sandbox={console,AbortController,Image:FakeImage,HTMLInputElement:Element,HTMLSelectElement:Element,
 Date:class extends Date{static now(){return now;}},performance:{now:()=>now},
 window:{innerWidth:1300,innerHeight:900,devicePixelRatio:1,addEventListener(){}},
 document:{getElementById:el,createElement:()=>new Element(),visibilityState:'visible',addEventListener(){},documentElement:new Element()},
 localStorage:{getItem:()=>null,setItem:(key,value)=>{savedPref=JSON.parse(value);}},
 L:{CRS:{Simple:{}},map:()=>map,tileLayer:(url,options)=>{
  const layer=Object.assign(new Events(),{url,options,addTo(){mapCalls.push('add');return this;}});tileLayers.push(layer);return layer;
 }},
 fetch:async path=>{if(path==='/api/bootstrap')return {ok:true,json:async()=>({token:'fixture'})};if(path==='/api/replay/load')return {ok:true,json:async()=>replayReply};if(path==='/api/live')liveRequests++;
  if(path.endsWith('maps.json'))return {ok:true,json:async()=>mapCatalog};if(path.endsWith('items.json'))return {ok:true,json:async()=>JSON.parse(fs.readFileSync(require.resolve('../webui/radar/items.json'),'utf8'))};if(path.endsWith('operators.json'))return {ok:true,json:async()=>JSON.parse(fs.readFileSync(require.resolve('../webui/radar/operators.json'),'utf8'))};if(reply instanceof Error)throw reply;return {ok:true,json:async()=>reply};},
 setTimeout:(fn,ms)=>{timers.set(++timerId,{fn,ms});return timerId;},clearTimeout:id=>timers.delete(id),requestAnimationFrame:fn=>raf.push(fn)};
vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(require.resolve('../webui/radar/model.js'),'utf8'),sandbox);
vm.runInContext(fs.readFileSync(require.resolve('../webui/radar/replay.js'),'utf8'),sandbox);
vm.runInContext(fs.readFileSync(require.resolve('../webui/radar/view.js'),'utf8'),sandbox);
const flush=()=>new Promise(resolve=>setImmediate(resolve));
function paint(at){now=at;assert.ok(raf.length);raf.shift()(now);}
async function poll(next,at){now=at;reply=next;const timer=[...timers].find(([,v])=>v.ms===20||v.ms===1000);assert.ok(timer,'next request scheduled');timers.delete(timer[0]);timer[1].fn();await flush();}
(async()=>{
 await flush();paint(1200);
 const chooseMap=name=>el('mapTabs').children.find(b=>b.textContent===name).handlers.click();
 chooseMap('长弓溪谷');
 const layer=tileLayers.at(-1);
 assert.equal(layer.options.minNativeZoom,2,'mobile overview rescales existing level 2 tiles');
 assert.deepEqual(mapCalls.slice(-2),['fit','add'],'settle new map view before requesting tiles');
 let rect={left:20,top:20,right:120,bottom:120,width:100,height:100};
 const tile={isConnected:true,getBoundingClientRect:()=>rect};
 layer.fire('tileerror',{tile});assert.equal(el('mapWarning').hidden,false,'visible failed tile shows warning');
 layer.fire('tileload',{tile:{isConnected:true,getBoundingClientRect:()=>rect}});
 assert.equal(el('mapWarning').hidden,false,'unrelated successful tile cannot hide real failure');
 rect={...rect,left:400,right:500};map.fire('moveend');assert.equal(el('mapWarning').hidden,true,'offscreen tile cannot leave warning behind');
 rect={...rect,left:20,right:120};map.fire('zoomend');assert.equal(el('mapWarning').hidden,false);
 layer.fire('tileload',{tile});assert.equal(el('mapWarning').hidden,true,'recovered tile clears warning');
 layer.fire('tileerror',{tile});layer.fire('tileunload',{tile});assert.equal(el('mapWarning').hidden,true,'unloaded failed tile clears warning');
 const lateError=[...layer.events.get('tileerror')][0];
 chooseMap('航天基地');assert.equal(tileLayers.at(-1).options.minNativeZoom,2);
 lateError({tile});assert.equal(el('mapWarning').hidden,true,'late previous-map error is ignored');
 assert.equal(layer.events.get('tileerror').size,0,'retired layer listeners removed');
 assert.equal(map.events.get('moveend').size,1,'only current layer observes viewport changes');
 chooseMap('巴克什');assert.equal(tileLayers.at(-1).options.minNativeZoom,2);
 chooseMap('坐标网格');assert.equal(map.events.get('moveend').size,0,'grid releases tile listeners');
 await flush();paint(1220);
 assert.ok(draws.some(x=>x.url.endsWith('027.png')),'known operator portrait is drawn');
 assert.ok(draws.every(x=>x.rotation===0),'portrait stays upright while direction arrow rotates');
 assert.ok(paintLabels.includes('本人 · 1 · 威龙')&&paintLabels.includes('未知玩家 · 1 · 蜂医'),'player, team and operator share one map label');
 assert.equal(images.length,2,'each distinct operator image loads once');
 assert.equal(el('rosterCount').textContent,'2');assert.equal(el('emptyState').hidden,true);
 el('rosterRows').children[0].handlers.click();assert.equal(el('detail').hidden,false);
 const beforeHold=draws.length;
 await poll(new Error('temporary request failure'),1300);paint(1450);
 assert.ok(draws.length>beforeHold,'retained portraits are rendered during outage');
 assert.ok(draws.slice(beforeHold).every(x=>x.alpha===1),'retained portraits stay fully opaque');
 assert.equal(el('rosterCount').textContent,'2','request failure must not clear the roster');
 assert.equal(el('linkState').textContent,'重连中 · 保留位置');
 assert.equal(el('detail').hidden,false,'selected detail survives a short outage');
 assert.equal(el('emptyState').hidden,true);
 await poll({...scene,revision:2,serverTimeMs:1500,players:[{...other,status:'rejected',timestampMs:1500}],self:null},1500);paint(1700);
 assert.equal(el('rosterCount').textContent,'2','missing self and rejected remote retain last valid rows');
 await poll({...scene,revision:3,serverTimeMs:1800,self:{...own,timestampMs:1800},players:[{...other,timestampMs:1800}]},1800);paint(1950);
 assert.equal(el('linkState').textContent,'已连接');assert.equal(el('rosterCount').textContent,'2');
 assert.equal(images.length,2,'polling and roster rebuilds reuse decoded images');
 await poll(new Error('long outage'),2000);paint(6900);
 assert.equal(el('rosterCount').textContent,'0','long outage removes expired positions');assert.equal(el('emptyState').hidden,false);
 await poll({...scene,revision:4,serverTimeMs:7000,self:{...own,timestampMs:7000},players:[{...other,timestampMs:7000}]},7000);paint(7200);
 assert.equal(el('rosterCount').textContent,'2');
 await poll({...scene,revision:5,serverTimeMs:7250,players:[{...other,timestampMs:7250,attributes:{heroId:88000000028}}]},7250);paint(7260);await flush();paint(7500);
 assert.equal(el('rosterCount').textContent,'2','image load failure cannot remove a player');
 assert.ok(paintLabels.some(s=>s.endsWith(' · 露娜')),'name survives portrait failure');
 assert.equal(images.filter(p=>p.endsWith('028.png')).length,1,'failed image is not retried every frame');
 await poll({...scene,revision:6,serverTimeMs:7600,running:false,battle:{...scene.battle,status:'stopped'}},7600);paint(7850);
 assert.equal(el('rosterCount').textContent,'0','explicit stop must clear immediately');
 const loot={actorGuid:100,itemId:'15080050142',present:true,x:0,y:0,z:0,flow:'a>b',epoch:1,count:2};
 await poll({...scene,revision:7,serverTimeMs:8000,self:null,players:[],loot:{100:loot}},8000);paint(8250);
 assert.equal(el('lootCount').textContent,'1');assert.ok(paintLabels.includes('海洋之泪 ×2'),'item name and stack rendered');
 assert.equal(el('emptyState').hidden,true,'items can display before player positions arrive');
 paintLabels.length=0;paint(8270);assert.ok(!paintLabels.includes('海洋之泪 ×2'),'unchanged static labels use cached canvas');
 el('lootNone').handlers.click();paint(8290);assert.equal(el('lootCount').textContent,'0','category none hides items');
 el('lootAll').handlers.click();paint(8310);assert.equal(el('lootCount').textContent,'1');
 const red=el('lootGrades').children[6].children[0];red.checked=false;red.handlers.change();paint(8330);assert.equal(el('lootCount').textContent,'0');
 red.checked=true;red.handlers.change();paint(8350);assert.equal(el('lootCount').textContent,'1');
 el('loot').checked=false;el('loot').handlers.change();assert.equal(el('lootCount').textContent,'0');
 el('loot').checked=true;el('loot').handlers.change();assert.equal(el('lootCount').textContent,'1');
 assert.equal(el('lootCategories').children[0].children[0].children[0].textContent,'容器','container group is pinned first');
 assert.equal(el('lootItemRows').children.length,5,'container choices are the initial category');
 const collection=el('lootCategories').children.find(section=>section.children[0].children[0].textContent==='收集品');
 collection.children.find(row=>row.children[0].textContent==='工艺藏品').children[0].handlers.click();
 el('lootSearch').value='15080050142';el('lootSearch').handlers.input();
 const pearlCard=el('lootItemRows').children[0],pearlToggle=pearlCard.children[0];
 assert.equal(el('lootItemRows').children.length,1,'search by item ID');assert.equal(pearlCard.style.backgroundColor,'#ff4d5826','red rarity card background');
 pearlToggle.checked=false;pearlToggle.handlers.change();assert.equal(el('lootCount').textContent,'0');assert.equal(savedPref.lootItems['15080050142'],false);
 el('lootCategoryEnabled').checked=false;el('lootCategoryEnabled').handlers.change();
 el('lootCategoryEnabled').checked=true;el('lootCategoryEnabled').handlers.change();assert.equal(el('lootCount').textContent,'0','category toggle preserves individual exclusions');
 el('lootItemsAll').handlers.click();assert.equal(el('lootCount').textContent,'1');assert.equal(savedPref.lootItems['15080050142'],true);
 const groupSwitch=()=>el('lootCategories').children.find(section=>section.children[0].children[0].textContent==='收集品').children[0].children[1];
 groupSwitch().checked=false;groupSwitch().handlers.change();assert.equal(el('lootCount').textContent,'0');assert.equal(savedPref.lootGroups['收集品'],false);
 groupSwitch().checked=true;groupSwitch().handlers.change();assert.equal(el('lootCount').textContent,'1');
 const assets=el('lootCategories').children.find(section=>section.children[0].children[0].textContent==='外观');
 assets.children.find(row=>row.children[0].textContent==='枪械').children[0].handlers.click();
 assert.equal(el('lootItemRows').children.length,60,'large category is paged');assert.equal(el('lootNext').disabled,false);
 const firstName=el('lootItemRows').children[0].children[1].children[0].textContent;
 el('lootNext').handlers.click();assert.notEqual(el('lootItemRows').children[0].children[1].children[0].textContent,firstName,'next page changes item cards');
 await poll({...scene,revision:8,serverTimeMs:8400,loot:{}},8400);paint(8650);assert.equal(el('lootCount').textContent,'0','server removals clear cached item layer');
 const box={...loot,itemId:null,kind:'container',containerType:'boss_death',empty:true};
 await poll({...scene,revision:9,serverTimeMs:8700,loot:{100:box}},8700);paintLabels.length=0;paint(8750);
 assert.ok(paintLabels.includes('首领死亡盒 · 已搜空'),'box label replaces count and keeps explicit empty state');
 el('lootCategories').children[0].children[1].children[0].handlers.click();
 const boxToggle=el('lootItemRows').children[0].children[0];boxToggle.checked=false;boxToggle.handlers.change();
 assert.equal(el('lootCount').textContent,'0');assert.equal(savedPref.lootItems['container:boss_death'],false);
 const replayPlayer={...other,name:'回放玩家',teamId:2,timestampMs:9000,equipment:{weapon:{status:'resolved',name:'测试武器'},armor:{level:4}}};
 replayReply={format:'radaer-replay-v1',name:'fixture',startMs:9000,endMs:20000,frames:[
  {t:9000,battle:scene.battle,players:{'15:1':replayPlayer},self:{...own,timestampMs:9000}},
  {t:10000,players:{'15:1':{...replayPlayer,x:350,timestampMs:10000}}},
  {t:20000,battle:{...scene.battle,status:'stopped'},players:{'15:1':null},self:null}]};
 el('replayRecord').value='fixture';await el('replayLoad').handlers.click();
 paintLabels.length=0;paintArcs.length=0;paint(9000);assert.ok(paintLabels.includes('回放玩家 · 2 · 蜂医'));
 assert.ok(paintLabels.some(s=>s.includes('测试武器')));
 assert.ok(paintArcs.some(r=>Math.abs(r-10.22)<.001));
 const requests=liveRequests;await poll(scene,9500);assert.equal(liveRequests,requests,'replay does not poll or mix live data');
 for(const id of ['names','operatorNames','teamIds','weapons','armorLevels','distances']){el(id).checked=false;el(id).handlers.change();}
 paintLabels.length=0;paintArcs.length=0;paint(70000);
 assert.ok(!paintLabels.includes('回放玩家')&&!paintLabels.includes('蜂医')&&!paintLabels.includes('2'));
 assert.ok(!paintLabels.some(s=>s.includes('测试武器'))&&!paintArcs.some(r=>Math.abs(r-10.22)<.001));
 assert.equal(el('rosterCount').textContent,'2','paused replay survives wall-clock timeouts');
 assert.equal(el('replayTime').textContent,'00:00 / 00:11');
 for(const id of ['names','operatorNames','teamIds','weapons','armorLevels','distances']){el(id).checked=true;el(id).handlers.change();}
 paintLabels.length=0;paint(70200);assert.ok(paintLabels.includes('回放玩家 · 2 · 蜂医'),'paused labels can be restored');
 el('replaySeek').value=1000;el('replaySeek').handlers.input();assert.equal(el('replayTime').textContent,'00:01 / 00:11');
 el('replayPlay').handlers.click();paint(70700);assert.equal(el('replayPlay').textContent,'暂停');
 el('replayLive').handlers.click();await poll({...scene,revision:9,serverTimeMs:71000},71000);paint(71200);
 assert.equal(el('replayBar').hidden,true);assert.equal(el('modeLabel').textContent,'实时数据');
 console.log('Radar page polling: short errors/rejected samples retain positions and selection; recovery, expiration and explicit stop passed');
})().catch(e=>{console.error(e);process.exitCode=1;});
