'use strict';
(() => {
 const $=id=>document.getElementById(id),T=Tactical,gate=new T.FrameGate();
 let presenter=new T.Presenter(),replay=null,replayWall=null,sourceVersion=0,loadVersion=0;
 const defaults={loot:true,lootCategories:{},lootGrades:{},lootGroups:{},lootItems:{},lootCategory:null,map:'daba',names:true,operatorNames:true,teamIds:true,weapons:true,armorLevels:true,distances:true,trails:true,cones:true,mates:true,others:true,smoothing:'original',prediction:false,markerSize:100,follow:true,sort:'distance'};
 let pref={...defaults};try{
  const saved=JSON.parse(localStorage.getItem('radaer.tactical.v1')||'{}');
  pref={...defaults,...saved};
  if(typeof saved.operatorNames!=='boolean'&&typeof saved.names==='boolean')pref.operatorNames=saved.names;
  for(const key of ['weapons','armorLevels'])if(typeof saved[key]!=='boolean'&&typeof saved.equipment==='boolean')pref[key]=saved.equipment;
  delete pref.equipment;
 }catch{}
 for(const k of ['loot','names','operatorNames','teamIds','weapons','armorLevels','distances','trails','cones','mates','others','prediction','follow'])pref[k]=typeof pref[k]==='boolean'?pref[k]:defaults[k];
 if(!['original','interpolate','raw'].includes(pref.smoothing))pref.smoothing='original';
 pref.markerSize=Number.isFinite(Number(pref.markerSize))?Math.max(70,Math.min(160,Number(pref.markerSize))):100;
 for(const key of ['lootCategories','lootGrades','lootGroups','lootItems']){const v=pref[key];pref[key]=v&&typeof v==='object'&&!Array.isArray(v)?Object.fromEntries(Object.entries(v).filter(([,value])=>typeof value==='boolean')):{};}
 const save=()=>{try{localStorage.setItem('radaer.tactical.v1',JSON.stringify(pref));}catch{}};
 const setText=(id,s)=>{if($(id).textContent!==String(s))$(id).textContent=String(s);};
 const node=(tag,cls,text)=>{const n=document.createElement(tag);if(cls)n.className=cls;if(text!==undefined)n.textContent=text;return n;};
 const fmt=(v,n=1)=>Number.isFinite(v)?v.toFixed(n):'—';
 const teamLabel=p=>Number.isInteger(p.teamId)&&p.teamId>=0?String(p.teamId):'';
 const now=()=>replay?replay.time:Date.now()+clockOffset;
 let clockOffset=0,connected=false,lastReceived=0,capture={},maps=[],mapInfo=null,tile=null,selected='',hits=[];
 let frameCount=0,fpsAt=performance.now(),sidebarAt=0,firstCenter=true,lastSession='',rosterSignature='',lastPaintRows=[];
 const operators=new T.Operators(),portraits=new Map(),items=new T.Items();
 let lootRows=[],lootSignature='',lootDirty=true,lootPage=0;
 const lootPageSize=60;
 const lootCanvas=document.createElement('canvas'),lootCtx=lootCanvas.getContext('2d');
 function portrait(p){
  const info=operators.get(p);if(!info.portrait)return null;
  if(!portraits.has(info.id)){
   const entry={image:new Image(),ready:false};portraits.set(info.id,entry);
   entry.image.onload=()=>{entry.ready=entry.image.naturalWidth>0&&entry.image.naturalHeight>0;rosterSignature='';};
   entry.image.onerror=()=>{entry.ready=false;rosterSignature='';};
   entry.image.src=info.portrait;
  }
  const entry=portraits.get(info.id);return entry.ready?entry.image:null;
 }
 function drawPortrait(context,image,x,y,diameter){
  const side=Math.min(image.naturalWidth,image.naturalHeight),sx=(image.naturalWidth-side)/2,sy=(image.naturalHeight-side)/2;
  context.drawImage(image,sx,sy,side,side,x,y,diameter,diameter);
 }
 function drawHealthRing(context,p,color,x,y,r,lineWidth){
  const health=T.hpRing(p);context.save();context.lineWidth=lineWidth;context.lineCap='butt';context.setLineDash([]);
  context.beginPath();context.arc(x,y,r,0,Math.PI*2);context.strokeStyle='#33404b';context.stroke();
  if(health.ratio===null){context.setLineDash([2,3]);context.strokeStyle='#a9b6c6';context.stroke();}
  else if(health.ratio>0){context.beginPath();context.arc(x,y,r,-Math.PI/2,-Math.PI/2+health.ratio*Math.PI*2);context.strokeStyle=color;context.stroke();}
  context.restore();
 }
 const tierColors=['#ffffff','#49df80','#42a5ff','#ba74ff','#ffd34a','#ff4d58'];
 function drawEquipmentArcs(context,p,x,y,r){
  if(!pref.armorLevels)return;
  context.save();context.lineCap='round';
  const radius=r*.73,lineWidth=Math.max(1.4,r*.16);
  for(const [slot,start,end] of [['helmet',Math.PI*.68,Math.PI*1.32],['armor',-Math.PI*.32,Math.PI*.32]]){
   const level=p.equipment?.[slot]?.level,known=Number.isInteger(level)&&level>=1&&level<=6;
   context.setLineDash([]);context.beginPath();context.arc(x,y,radius,start,end);
   context.lineWidth=lineWidth+2;context.strokeStyle='#07121de6';context.stroke();
   context.lineWidth=lineWidth;context.strokeStyle=known?tierColors[level-1]:'#64717f';
   if(!known)context.setLineDash([1.5,3]);context.stroke();
  }
  context.restore();
 }
 async function loadOperators(){try{
  const response=await fetch('/radar/operators.json',{cache:'no-store'});if(!response.ok)throw Error();
  operators.load(await response.json());rosterSignature='';
 }catch{/* Unknown characters keep their circle and name without a portrait. */}}
 const history=new Map(),canvas=$('overlay'),ctx=canvas.getContext('2d');let width=0,height=0;
 const map=L.map('map',{crs:L.CRS.Simple,minZoom:-3,maxZoom:7,zoomControl:false,attributionControl:false,zoomSnap:.25,wheelPxPerZoomLevel:100,zoomAnimation:false,fadeAnimation:false}).setView([0,0],0);
 const isCompact=()=>window.innerWidth<=1300||window.innerHeight<=600;
 let compactLayout=isCompact();
 function resize(){
  width=window.innerWidth;height=window.innerHeight;lootDirty=true;
  const nextCompact=isCompact();if(nextCompact&&!compactLayout){$('tools').hidden=true;$('roster').hidden=true;$('lootPanel').hidden=true;}compactLayout=nextCompact;
  const ratio=Math.min(2,window.devicePixelRatio||1);canvas.width=width*ratio;canvas.height=height*ratio;ctx.setTransform(ratio,0,0,ratio,0,0);map.invalidateSize();
 }
 window.addEventListener('resize',resize);resize();
 const plane=p=>T.worldToPlane(p.x,p.y,mapInfo);
 const project=p=>{const ll=plane(p);return map.latLngToContainerPoint([ll.lat,ll.lng]);};
 const selfOf=rows=>rows.find(p=>p.kind==='self')||null;
 function follow(on){pref.follow=on;$('follow').classList.toggle('active',on);save();}
 map.on('dragstart',()=>follow(false));
 map.on('move zoom viewreset',()=>{lootDirty=true;});
 const mapButtons=new Map();let clearTileWarnings=()=>{};
 function renderMapButtons(){
  mapButtons.clear();$('mapTabs').replaceChildren();$('mapSelect').replaceChildren();
  for(const m of [{key:'grid',name:'坐标网格'},...maps]){
   const button=node('button','map-choice',m.name);button.type='button';
   button.title=m.key==='grid'?'离线坐标网格':`切换到${m.name}`;
   button.addEventListener('click',()=>mapChange(m.key));
   mapButtons.set(m.key,button);$('mapTabs').append(button);
   const option=node('option','',m.name);option.value=m.key;$('mapSelect').append(option);
  }
 }
 $('mapSelect').addEventListener('change',()=>mapChange($('mapSelect').value));
 function mapChange(k){
  const next=maps.find(m=>m.key===k)||null;mapInfo=next;pref.map=next?next.key:'grid';save();
  $('mapSelect').value=pref.map;
  for(const [key,button] of mapButtons){const active=key===pref.map;button.classList.toggle('active',active);button.setAttribute('aria-pressed',String(active));}
  clearTileWarnings();
  if(tile){map.removeLayer(tile);tile=null;}
  $('mapWarning').hidden=true;setText('mapTitle',next?next.name:'坐标网格');$('map').classList.toggle('grid',!next);
  setText('mapNote',next?'使用原版地图标定。请选与当前对局一致的底图；图片需要网络连接。':'世界坐标网格 · +X 向右，+Y 向上。');
  setText('northLabel',next?'地图方向':'+Y');
  if(next){
   map.setMinZoom(1);map.fitBounds(next.bounds,{padding:[40,95]});
   const layer=L.tileLayer(next.tileUrl,{tileSize:next.tileSize||256,minNativeZoom:next.minNativeZoom||1,maxNativeZoom:next.maxNativeZoom||4,bounds:next.bounds,keepBuffer:2,noWrap:true});
   tile=layer;
   const state=new T.TileLoadState(),refresh=()=>{
    if(tile!==layer)return;
    const viewport=map.getContainer().getBoundingClientRect();
    $('mapWarning').hidden=!state.failed(image=>{
     const r=image.getBoundingClientRect();return image.isConnected&&r.width>0&&r.height>0&&r.right>viewport.left&&r.left<viewport.right&&r.bottom>viewport.top&&r.top<viewport.bottom;
    });
   };
   const events={};
   for(const [event,status] of [['tileloadstart','loading'],['tileload','loaded'],['tileerror','error'],['tileunload','unload']]){
    events[event]=e=>{state.update(e.tile,status);refresh();};layer.on(event,events[event]);
   }
   layer.on('load',refresh);map.on('moveend zoomend',refresh);
   clearTileWarnings=()=>{for(const [event,handler] of Object.entries(events))layer.off(event,handler);layer.off('load',refresh);map.off('moveend zoomend',refresh);};
   layer.addTo(map);
  }else{map.setMinZoom(-3);map.setView([0,0],0);}
  history.clear();firstCenter=true;
 }
 async function loadMaps(){try{
  const response=await fetch('/radar/maps.json',{cache:'no-store'});if(!response.ok)throw Error();const data=await response.json();
  maps=(data.maps||[]).filter(m=>[m.width,m.height,m.centerX,m.centerY].every(Number.isFinite)&&m.width>0&&m.height>0&&/^https:\/\/game\.gtimg\.cn\//.test(m.tileUrl));
  renderMapButtons();
  mapChange(pref.map);
 }catch{maps=[];renderMapButtons();mapChange('grid');setText('mapNote','地图目录未加载，使用离线坐标网格。');}}
 function displayRows(rows){const self=selfOf(rows);return rows.filter(p=>p.kind==='self'||(T.relation(p,self)==='mate'?pref.mates:pref.others));}
 function centerOn(p){const ll=plane(p);map.panTo([ll.lat,ll.lng],{animate:false});}
 function fitAll(){
  const rows=displayRows(presenter.getRows(now(),!connected));if(!rows.length)return;
  follow(false);const bounds=L.latLngBounds(rows.map(p=>{const ll=plane(p);return [ll.lat,ll.lng];}));
  if(rows.length===1){centerOn(rows[0]);map.setZoom(mapInfo?5:1);}
  else map.fitBounds(bounds.pad(.25),{paddingTopLeft:[$('tools').hidden?35:270,190],paddingBottomRight:[$('roster').hidden?35:320,140],maxZoom:mapInfo?6:2});
 }
 function resetDisplay(){presenter.reset();history.clear();hits=[];lastPaintRows=[];selected='';rosterSignature='';}
 function updateHistory(){
  const active=new Set();
  for(const p of presenter.getRows(now())){const k=T.key(p);active.add(k);let h=history.get(k);
   if(!h||h.identity!==T.identity(p)){h={identity:T.identity(p),points:[]};history.set(k,h);}
   const prev=h.points.at(-1);if(prev&&Math.hypot(p.x-prev.x,p.y-prev.y,p.z-prev.z)>3000)h.points=[];
   if(!prev||p.timestampMs>prev.timestampMs)h.points.push({x:p.x,y:p.y,z:p.z,timestampMs:p.timestampMs});
   while(h.points.length>60||h.points.length&&now()-h.points[0].timestampMs>4000)h.points.shift();
  }
  for(const k of history.keys())if(!active.has(k))history.delete(k);
 }
 const replayTime=ms=>{const s=Math.floor(Math.max(0,ms)/1000);return `${Math.floor(s/60).toString().padStart(2,'0')}:${(s%60).toString().padStart(2,'0')}`;};
 function replayControls(){if(!replay)return;
  setText('replayPlay',replay.playing?'暂停':replay.time>=replay.end?'重播':'播放');
  setText('replayTime',`${replayTime(replay.time-replay.start)} / ${replayTime(replay.end-replay.start)}`);
  $('replaySeek').value=replay.time-replay.start;
 }
 async function recordings(){
  $('replayRefresh').disabled=true;
  try{const response=await fetch('/api/replays',{cache:'no-store'});if(!response.ok)throw Error('读取历史记录失败');
   const records=await response.json(),previous=$('replayRecord').value;$('replayRecord').replaceChildren();
   for(const record of records){const option=node('option','',`${record.id} · ${(record.bytes/1048576).toFixed(1)} MB${record.active?' · 采集中':''}`);option.value=record.id;option.disabled=record.active;$('replayRecord').append(option);}
   if(records.some(r=>r.id===previous&&!r.active))$('replayRecord').value=previous;
   else $('replayRecord').value=records.find(r=>!r.active)?.id||'';
   if(!records.length)$('replayRecord').append(node('option','','暂无历史采集记录'));
   setText('replayMessage',records.length?'选择一条已结束的采集记录。回放仅影响当前页面。':'采集记录保存在主机 captures/webui 目录，结束采集后可在此回放。');
  }catch(error){setText('replayMessage',error.message);}finally{$('replayRefresh').disabled=false;}
 }
 $('replayRefresh').addEventListener('click',recordings);
 $('toggleReplay').addEventListener('click',()=>{$('tools').hidden=false;$('replaySettings').open=true;$('tools').scrollTop=0;$('lootPanel').hidden=true;if(isCompact())$('roster').hidden=true;recordings();});
 $('replayLoad').addEventListener('click',async()=>{
  const id=$('replayRecord').value;if(!id){setText('replayMessage','请先刷新并选择一条记录。');return;}
  const version=++loadVersion,controller=new AbortController(),timeout=setTimeout(()=>controller.abort(),120000);
  $('replayLoad').disabled=true;setText('replayMessage','正在重建回放数据…');
  try{const bootstrap=await fetch('/api/bootstrap',{cache:'no-store',signal:controller.signal});if(!bootstrap.ok)throw Error('读取服务信息失败');const auth=await bootstrap.json();
   const response=await fetch('/api/replay/load',{method:'POST',headers:{'Content-Type':'application/json','X-Radaer-Token':auth.token},body:JSON.stringify({id}),signal:controller.signal});
   const data=await response.json();if(!response.ok)throw Error(data.error||'读取回放失败');if(version!==loadVersion)return;
   const timeline=new RadarReplay.Timeline(data,T.Presenter);sourceVersion++;resetDisplay();replay=timeline;presenter=replay.presenter;
   capture=replay.snapshot();connected=true;lastReceived=performance.now();firstCenter=true;replayWall=null;lastSession='';
   $('replayBar').hidden=false;$('replaySeek').max=Math.max(1,replay.end-replay.start);$('replaySpeed').value='1';
   refreshLoot(true);updateHistory();replayControls();sidebar(now(),true);
   setText('replayMessage',`${data.name} · 已载入${data.skippedLines?` · 跳过 ${data.skippedLines} 条损坏记录`:''}。播放或暂停时均可调整显示开关。`);
  }catch(error){if(version===loadVersion)setText('replayMessage',error.name==='AbortError'?'载入超时，请选择较短的记录。':error.message);}
  finally{clearTimeout(timeout);if(version===loadVersion)$('replayLoad').disabled=false;}
 });
 $('replayPlay').addEventListener('click',()=>{if(!replay)return;if(replay.time>=replay.end){replay.seek(replay.start);history.clear();}
  replay.playing=!replay.playing;replayWall=performance.now();replayControls();});
 $('replaySpeed').addEventListener('change',()=>{if(replay)replay.speed=Number($('replaySpeed').value)||1;replayWall=performance.now();});
 $('replaySeek').addEventListener('input',()=>{if(!replay)return;capture=replay.seek(replay.start+Number($('replaySeek').value));
  history.clear();rosterSignature='';replayWall=performance.now();refreshLoot(true);updateHistory();replayControls();sidebar(now(),true);});
 $('replayLive').addEventListener('click',()=>{loadVersion++;sourceVersion++;replay=null;replayWall=null;presenter=new T.Presenter();resetDisplay();
  capture={};connected=false;lastReceived=0;lastSession='';firstCenter=true;clockOffset=0;$('replayBar').hidden=true;$('replayLoad').disabled=false;refreshLoot(true);
  setText('replayMessage','已返回实时数据。');});
 async function poll(){
  if(replay){setTimeout(poll,20);return;}
  const version=sourceVersion;
  const controller=new AbortController(),timeout=setTimeout(()=>controller.abort(),1500);
  try{
   const response=await fetch('/api/live',{cache:'no-store',signal:controller.signal});if(!response.ok)throw Error('HTTP '+response.status);
   const data=await response.json();if(version!==sourceVersion||replay)return;clockOffset=Number.isFinite(data.serverTimeMs)?data.serverTimeMs-Date.now():0;
   const b=data.battle||{},session=`${b.flowSession}/${b.flow}/${b.epoch}`;
   if(session!==lastSession||b.status!=='live'||data.running===false){resetDisplay();firstCenter=true;lastSession=session;}
   if(!presenter.ingest(data,now()))return;
   capture=data;refreshLoot();connected=true;lastReceived=performance.now();
   updateHistory();
  }catch{if(version===sourceVersion&&!replay)connected=false;}
  finally{clearTimeout(timeout);setTimeout(poll,document.visibilityState==='visible'?20:1000);}
 }
 function drawGrid(){
  if(mapInfo)return;const a=map.containerPointToLatLng([0,0]),b=map.containerPointToLatLng([width,height]);
  const span=Math.abs(b.lng-a.lng),step=span>4000?500:span>1500?200:span>600?100:span>250?50:span>100?20:10;
  ctx.strokeStyle='#94b4d312';ctx.lineWidth=1;ctx.font='10px Segoe UI';ctx.fillStyle='#7590a750';
  for(let x=Math.floor(a.lng/step)*step;x<b.lng+step;x+=step){const px=map.latLngToContainerPoint([0,x]).x;ctx.beginPath();ctx.moveTo(px,0);ctx.lineTo(px,height);ctx.stroke();}
  for(let y=Math.floor(b.lat/step)*step;y<a.lat+step;y+=step){const py=map.latLngToContainerPoint([y,0]).y;ctx.beginPath();ctx.moveTo(0,py);ctx.lineTo(width,py);ctx.stroke();}
  ctx.fillText(`每格 ${step} m`,width/2-32,height-132);
 }
 function refreshLoot(force=false){
  const signature=JSON.stringify([capture.battle,capture.running,capture.loot]);
  if(!force&&signature===lootSignature)return;
  lootSignature=signature;lootRows=items.rows(capture,pref);lootDirty=true;setText('lootCount',lootRows.length);
  if(pref.lootCategory==='unknown'&&!$('lootPanel').hidden)renderLootItems();
 }
 function categoryItems(key){
  if(key==='containers')return [...items.containers.values()];
  const result=[...items.entries.values()].filter(item=>item.category===key);
  if(key==='unknown'){
   const ids=new Set(result.map(item=>item.id));
   for(const p of Object.values(capture.loot||{}))if(p.itemId&&!items.entries.has(String(p.itemId))&&!ids.has(String(p.itemId))){ids.add(String(p.itemId));result.push(items.get(p.itemId));}
  }
  return result.sort((a,b)=>b.grade-a.grade||a.name.localeCompare(b.name,'zh-CN')||a.id.localeCompare(b.id));
 }
 function selectLootCategory(key){pref.lootCategory=key;lootPage=0;$('lootSearch').value='';save();renderLootFilters();}
 function renderLootFilters(){
  if(!items.categories.has(pref.lootCategory))pref.lootCategory='containers';
  const query=$('lootCategorySearch').value.trim().toLowerCase(),groups=new Map(),counts=new Map();
  for(const item of items.entries.values())counts.set(item.category,(counts.get(item.category)||0)+1);
  counts.set('unknown',categoryItems('unknown').length);
  counts.set('containers',items.containers.size);
  for(const [key,c] of items.categories){
   if(query&&!`${c.group} ${c.name}`.toLowerCase().includes(query))continue;
   if(!groups.has(c.group))groups.set(c.group,[]);groups.get(c.group).push([key,c]);
  }
  $('lootCategories').replaceChildren();
  for(const [group,entries] of groups){
   const section=node('section','loot-group'),head=node('label','toggle loot-group-head'),master=node('input');master.type='checkbox';master.checked=pref.lootGroups[group]!==false;
   master.setAttribute('aria-label',`${group}总开关`);master.addEventListener('change',()=>{pref.lootGroups[group]=master.checked;save();refreshLoot(true);renderLootFilters();});
   head.append(node('strong','',group),master);section.append(head);
   for(const [key,c] of entries){
    const row=node('div','loot-category'+(pref.lootCategory===key?' selected':'')),button=node('button','loot-category-select',c.name),input=node('input');
    button.type='button';button.setAttribute('aria-pressed',String(pref.lootCategory===key));button.append(node('small','',counts.get(key)||0));button.addEventListener('click',()=>selectLootCategory(key));
    const label=node('label','toggle');input.type='checkbox';input.checked=pref.lootCategories[key]!==false;input.setAttribute('aria-label',`${c.name}总开关`);
    input.addEventListener('change',()=>{pref.lootCategories[key]=input.checked;save();refreshLoot(true);renderLootItems();});label.append(input);row.append(button,label);section.append(row);
   }
   $('lootCategories').append(section);
  }
  if(!groups.size)$('lootCategories').append(node('p','empty-small','没有匹配的类别'));
  renderLootItems();
 }
 function renderLootItems(){
  const category=items.categories.get(pref.lootCategory);if(!category)return;
  const all=categoryItems(pref.lootCategory),query=$('lootSearch').value.trim().toLowerCase(),filtered=all.filter(item=>!query||`${item.name} ${item.id}`.toLowerCase().includes(query));
  const selected=all.filter(item=>pref.lootItems[item.id]!==false).length;
  setText('lootCategoryTitle',`${category.group} / ${category.name}`);setText('lootCategoryCount',`${selected} / ${all.length} 项已选${query?` · 搜索匹配 ${filtered.length} 项`:''}`);
  $('lootCategoryEnabled').checked=pref.lootCategories[pref.lootCategory]!==false;
  setText('lootFilterHint',!pref.loot?'物资显示已关闭，单项选择仍会保存。':pref.lootGroups[category.group]===false?'大类总开关已关闭，单项选择仍会保存。':pref.lootCategories[pref.lootCategory]===false?'该类总开关已关闭，单项选择仍会保存。':pref.lootCategory==='containers'?'容器不受稀有度筛选影响；只显示已收到的位置，箱内物资未知。':'所选物资还需通过上方稀有度筛选；只显示已收到位置的数据。');
  const pages=Math.max(1,Math.ceil(filtered.length/lootPageSize));lootPage=Math.min(lootPage,pages-1);
  $('lootItemRows').replaceChildren();
  for(const item of filtered.slice(lootPage*lootPageSize,(lootPage+1)*lootPageSize)){
   const card=node('label','loot-item-card'),input=node('input'),info=node('span','loot-item-info');input.type='checkbox';input.checked=pref.lootItems[item.id]!==false;
   input.setAttribute('aria-label',`显示 ${item.name}${item.container?'':` · ${item.id}`}`);card.style.backgroundColor=item.color+'26';card.style.borderColor=item.color+'70';
   const name=node('strong','',item.name),meta=node('small','',item.container?'容器位置 · 独立开关':`${['未知','白色','绿色','蓝色','紫色','金色','红色'][item.grade]} · ${item.id}`);info.append(name,meta);
   const state=node('span','loot-item-state',input.checked?'已选':'未选');
   input.addEventListener('change',()=>{pref.lootItems[item.id]=input.checked;state.textContent=input.checked?'已选':'未选';save();refreshLoot(true);const selectedNow=all.filter(item=>pref.lootItems[item.id]!==false).length;setText('lootCategoryCount',`${selectedNow} / ${all.length} 项已选${query?` · 搜索匹配 ${filtered.length} 项`:''}`);});
   card.append(input,info,state);$('lootItemRows').append(card);
  }
  if(!filtered.length)$('lootItemRows').append(node('p','empty-small',all.length?'没有匹配的物资':pref.lootCategory==='unknown'?'尚未收到目录外物品。':'该类别暂无物资。'));
  $('lootPrev').disabled=lootPage===0;$('lootNext').disabled=lootPage>=pages-1;setText('lootPage',`${lootPage+1} / ${pages}`);
 }
 async function loadItems(){try{
  const response=await fetch('/radar/items.json',{cache:'no-store'});if(!response.ok)throw Error();items.load(await response.json());
  setText('lootCatalogStatus',`${items.entries.size} 种物品 · ${items.containers.size} 类容器 · 设置自动保存`);
 }catch{setText('lootCatalogStatus','物资目录加载失败，暂以物品 ID 显示。');}
  renderLootFilters();refreshLoot(true);
 }
 for(let grade=0;grade<=6;grade++){
  const label=node('label','loot-grade'),input=node('input');input.type='checkbox';input.checked=pref.lootGrades[grade]!==false;
  label.style.color=T.rarityColors[grade];label.append(input,node('span','',['未知','白','绿','蓝','紫','金','红'][grade]));
  input.addEventListener('change',()=>{pref.lootGrades[grade]=input.checked;save();refreshLoot(true);renderLootItems();});$('lootGrades').append(label);
 }
 $('lootCategorySearch').addEventListener('input',renderLootFilters);
 $('lootSearch').addEventListener('input',()=>{lootPage=0;renderLootItems();});
 $('lootCategoryEnabled').addEventListener('change',()=>{pref.lootCategories[pref.lootCategory]=$('lootCategoryEnabled').checked;save();refreshLoot(true);renderLootFilters();});
 for(const [id,step] of [['lootPrev',-1],['lootNext',1]])$(id).addEventListener('click',()=>{lootPage=Math.max(0,lootPage+step);renderLootItems();$('lootItemRows').scrollTop=0;});
 for(const [id,value] of [['lootItemsAll',true],['lootItemsNone',false]])$(id).addEventListener('click',()=>{for(const item of categoryItems(pref.lootCategory))pref.lootItems[item.id]=value;save();refreshLoot(true);renderLootItems();});
 for(const [id,value] of [['lootAll',true],['lootNone',false]])$(id).addEventListener('click',()=>{
  pref.lootGroups=Object.fromEntries([...items.categories.values()].map(c=>[c.group,value]));pref.lootCategories=Object.fromEntries([...items.categories.keys()].map(key=>[key,value]));save();renderLootFilters();refreshLoot(true);
 });
 function drawLoot(time){
  if(!pref.loot||!lootRows.length||!replay&&performance.now()-lastReceived>5000)return;
  if(lootDirty){
   lootDirty=false;const ratio=Math.min(2,window.devicePixelRatio||1);
   lootCanvas.width=width*ratio;lootCanvas.height=height*ratio;lootCtx.setTransform(ratio,0,0,ratio,0,0);
   lootCtx.font='11px "Segoe UI","Microsoft YaHei",sans-serif';lootCtx.textAlign='left';lootCtx.lineJoin='round';
   for(const p of lootRows){const q=project(p);if(q.x<0||q.y<0||q.x>width||q.y>height)continue;
    const label=p.item.name+(p.item.container?(p.empty===true?' · 已搜空':''):(Number.isInteger(p.count)&&p.count>1?` ×${p.count}`:''));
    lootCtx.fillStyle=p.item.color;
    if(p.item.container){
     lootCtx.fillRect(q.x-4,q.y-4,8,8);lootCtx.strokeStyle='#08121eef';lootCtx.lineWidth=1.5;lootCtx.strokeRect(q.x-4,q.y-4,8,8);
     lootCtx.beginPath();lootCtx.moveTo(q.x-4,q.y-1);lootCtx.lineTo(q.x+4,q.y-1);lootCtx.stroke();
    }else{lootCtx.beginPath();lootCtx.arc(q.x,q.y,3,0,Math.PI*2);lootCtx.fill();}
    lootCtx.lineWidth=3;lootCtx.strokeStyle='#08121eef';lootCtx.strokeText(label,q.x+8,q.y+4);lootCtx.fillText(label,q.x+8,q.y+4);
   }
  }
  ctx.drawImage(lootCanvas,0,0,width,height);
 }
 function draw(rows,time){
  ctx.clearRect(0,0,width,height);drawGrid();drawLoot(time);hits=[];const self=selfOf(rows),size=pref.markerSize/100;
  for(const p of rows){
   const color=T.color(p,self),k=T.key(p),q=project(p),r=14*size,heading=T.screenAngle(p,T.heading(p,time),mapInfo),operator=operators.get(p),face=portrait(p);
   if(pref.trails){const h=history.get(k);if(h?.points.length>1){ctx.beginPath();h.points.forEach((p,i)=>{const v=project(p);i?ctx.lineTo(v.x,v.y):ctx.moveTo(v.x,v.y);});ctx.strokeStyle=color+'42';ctx.lineWidth=1.5;ctx.stroke();}}
   if(q.x<-80||q.y<-80||q.x>width+80||q.y>height+80)continue;
   hits.push({k,x:q.x,y:q.y});ctx.save();ctx.translate(q.x,q.y);ctx.globalAlpha=1;
   if(k===selected||p.kind==='self'){ctx.beginPath();ctx.arc(0,0,r+7,0,Math.PI*2);ctx.strokeStyle=color+'66';ctx.lineWidth=1;ctx.stroke();}
   if(heading!==null){ctx.save();ctx.rotate(heading);
    if(pref.cones){const grad=ctx.createLinearGradient(0,0,75*size,0);grad.addColorStop(0,color+'40');grad.addColorStop(1,color+'00');ctx.fillStyle=grad;ctx.beginPath();ctx.moveTo(0,0);ctx.arc(0,0,75*size,-.23,.23);ctx.closePath();ctx.fill();}
    ctx.beginPath();ctx.moveTo(r-1,-4*size);ctx.lineTo(r+12*size,0);ctx.lineTo(r-1,4*size);ctx.closePath();ctx.fillStyle=color;ctx.fill();ctx.restore();}
   ctx.beginPath();ctx.arc(0,0,r,0,Math.PI*2);ctx.fillStyle='#101b28';ctx.fill();
   if(face){ctx.save();ctx.beginPath();ctx.arc(0,0,r-1,0,Math.PI*2);ctx.clip();drawPortrait(ctx,face,-r,-r,2*r);ctx.restore();}
   else{ctx.beginPath();ctx.arc(0,0,r-3,0,Math.PI*2);ctx.fillStyle=color;ctx.globalAlpha=p.status==='warming_up'?.35:.85;ctx.fill();ctx.globalAlpha=1;}
   ctx.beginPath();ctx.arc(0,0,r,0,Math.PI*2);ctx.strokeStyle='#07121d';ctx.lineWidth=5;ctx.stroke();
   if(T.lifeState(p)==='downed'){
    // Downed markers keep the portrait with a fixed, legible cross only.
    ctx.beginPath();ctx.moveTo(-6*size,0);ctx.lineTo(6*size,0);ctx.moveTo(0,-6*size);ctx.lineTo(0,6*size);
    ctx.lineCap='round';ctx.strokeStyle='#07121d';ctx.lineWidth=7*size;ctx.stroke();
    ctx.strokeStyle='#ffda78';ctx.lineWidth=3*size;ctx.stroke();
    ctx.restore();continue;
   }
   drawHealthRing(ctx,p,color,0,0,r,3);
   drawEquipmentArcs(ctx,p,0,0,r);
   if(p.held){ctx.beginPath();ctx.arc(0,0,r+3,0,Math.PI*2);ctx.strokeStyle=color;ctx.lineWidth=1;ctx.setLineDash([2,4]);ctx.stroke();ctx.setLineDash([]);}
   // Keep labels in world-marker space: portraits rotate neither text nor status.
   ctx.textAlign='center';ctx.font='11px "Segoe UI","Microsoft YaHei",sans-serif';
   ctx.lineJoin='round';ctx.lineWidth=2.5;ctx.strokeStyle='#07121dd9';
   const label=(text,y,ink)=>{ctx.fillStyle=ink;ctx.strokeText(text,0,y);ctx.fillText(text,0,y);};
   const above=[];
   if(pref.names){const name=Array.from(p.name||(p.kind==='self'?'本人':'未知玩家')),limit=isCompact()?8:18;above.push(name.slice(0,limit).join('')+(name.length>limit?'…':''));}
   if(pref.teamIds&&teamLabel(p))above.push(teamLabel(p));
   if(pref.operatorNames)above.push(operator.name);
   if(above.length)label(above.join(' · '),-r-12*size-5,color);
   const below=[];
   if(pref.weapons)below.push(T.gear(p).weapon);
   if(pref.distances&&self&&p.kind!=='self')below.push(`${Math.round(Math.hypot(p.x-self.x,p.y-self.y)/100)} m`);
   if(below.length)label(below.join(' · '),r+18,'#e3eaf1');
   ctx.restore();
  }
 }
 function details(p,time){
  $('detail').hidden=!p;if(!p)return;setText('detailName',p.kind==='self'?'本人 · '+(p.name||'未关联姓名'):p.name||`槽位 ${p.slot}`);
  const m=p.motion||{},a=p.attributes||{},self=selfOf(presenter.getRows(time,!connected));
  const gear=T.gear(p),health=T.hpRing(p);
  const items=[['干员',operators.get(p).name],['队伍',p.teamId??'未知'],['状态',T.life(p)],['血量',fmt(T.hp(p),1)],
   ['血量上限',health.assumed?'未下发 · 环形按 100':fmt(health.maximum,0)],['手持武器',gear.weapon],['头盔等级',gear.helmet],['护甲等级',gear.armor],
   ['是否存活',T.lifeState(p)==='unknown'?'未知':T.lifeState(p)==='dead'?'否':'是'],['是否倒地',T.lifeState(p)==='unknown'?'未知':T.lifeState(p)==='downed'?'是':'否'],
   ['速度',fmt(T.speed(p,time))+' m/s'],['视角',fmt(T.heading(p,time))+'°'],
   ['俯仰',time-m.viewAtMs<=1000?fmt(m.viewPitch)+'°':'—'],['英雄 ID',a.heroId??'—'],['移动时间',fmt(m.timeSeconds,3)+' s']];
  for(const [field,label] of [['ShieldHealth','护盾'],['ArmorShieldHealth','护甲护盾'],['ArmorHealth','护甲耐久'],['HelmetArmorHealth','头盔耐久']]){
   const value=p.vitals?.health?.[field]?.value;if(Number.isFinite(value))items.push([label,fmt(value)]);
  }
  if(p.vitals?.health?.Health)items.push(['血量更新时间',fmt(Math.max(0,time-p.vitals.health.Health.timestampMs)/1000)+' s 前']);
  if(Number.isFinite(a.rescueSeconds))items.push(['救援倒计时',fmt(a.rescueSeconds,0)+' s']);
  const separation=T.selfSeparation(p,capture.serverSelf,time);
  if(separation!==null){
   items.push(['上下行位置差',fmt(separation,2)+' m']);
  }else if(self&&p.kind!=='self')items.push(['距离',fmt(Math.hypot(p.x-self.x,p.y-self.y)/100)+' m']);
  $('detailFields').replaceChildren(...items.map(([k,v])=>{const d=node('div');d.append(node('span','',k),node('b','',v));return d;}));
 }
 function sidebar(time,alive){
  const rows=displayRows(presenter.getRows(time,!alive)),self=selfOf(rows),b=capture.battle||{},r=capture.rates||{};
  setText('linkState',replay?(replay.playing?'回放中':'回放暂停'):alive?'已连接':rows.length?'重连中 · 保留位置':'连接中断');$('linkState').className='badge '+(alive?'connected':'error');
  setText('battleStatus',!alive?(rows.length?'正在重连':'等待连接'):({live:'战斗流已连接',waiting:'等待对局',recovery_required:'丢包后等待重连',idle:'战斗流静默',stopped:'采集已停止'}[b.status]||'等待采集'));
  setText('modeLabel',capture.replay?'离线回放':'实时数据');setText('totalCount',rows.filter(p=>p.kind!=='self').length);
  setText('mateCount',rows.filter(p=>T.relation(p,self)==='mate').length);setText('otherCount',rows.filter(p=>T.relation(p,self)==='other').length);
  const distances=self?rows.filter(p=>T.relation(p,self)==='other').map(p=>Math.hypot(p.x-self.x,p.y-self.y)/100):[];
  setText('nearest',distances.length?Math.min(...distances).toFixed(0):'—');
  setText('decryptHz',alive&&!replay?r.udpDecryptPacketsPerSecond??0:'—');setText('coordinateHz',alive&&!replay?r.coordinatePacketsPerSecond??0:'—');setText('selfSpeed',self?fmt(T.speed(self,time)):'—');
  setText('selfSource',self?`本人坐标 · ${fmt(self.x/100)} / ${fmt(self.y/100)} m`:'等待本人上行坐标');
  $('emptyState').hidden=rows.length>0||(alive&&lootRows.length>0);
  setText('emptyTitle',!alive?'等待数据连接':b.status==='recovery_required'?'采集丢包，请重新进入对局':b.status==='idle'?'战斗数据已静默':b.status==='stopped'?'采集已停止':'等待战斗数据');
  setText('emptyText',replay?'当前时间没有可显示的人物，可拖动回放进度。':!alive?'正在重新连接采集服务。':b.status==='recovery_required'?'采集仍在运行，重新确认战斗连接后自动恢复。':b.status==='live'?'战斗连接已确认，等待有效坐标。':'在采集控制台开始任务，进入对局后自动显示。');
  if(selected&&!rows.some(p=>T.key(p)===selected))selected='';
  const query=$('search').value.trim().toLowerCase(),list=rows.filter(p=>(`${p.name||''} ${operators.get(p).name} ${p.teamId??''} ${p.slot??''}`).toLowerCase().includes(query));
  const distance=p=>self?Math.hypot(p.x-self.x,p.y-self.y)/100:Infinity;
  list.sort((a,b)=>a.kind==='self'?-1:b.kind==='self'?1:pref.sort==='team'?(a.teamId??9999)-(b.teamId??9999)||distance(a)-distance(b):distance(a)-distance(b));
  setText('rosterCount',list.length);
  const signature=JSON.stringify(list.map(p=>[T.key(p),p.name,p.teamId,operators.get(p).id,operators.get(p).name,!!portrait(p),T.life(p),T.hpRing(p),p.equipment,p.held,Math.round(distance(p)),fmt(T.speed(p,time)),selected===T.key(p)]));
  if(signature!==rosterSignature){rosterSignature=signature;
   $('rosterRows').replaceChildren(...list.map(p=>{
    const k=T.key(p),operator=operators.get(p),face=portrait(p),button=node('button','person-row'+(selected===k?' selected':''));button.setAttribute('aria-label',`选择 ${p.name||'槽位 '+p.slot} · ${operator.name}`);
    const av=node('span','avatar health-avatar');av.style.color=T.color(p,self);av.title=operator.name+' · HP '+fmt(T.hp(p),0);
    const image=node('canvas','portrait');image.width=64;image.height=64;image.setAttribute('aria-label',av.title);
    const ac=image.getContext('2d');ac.save();ac.beginPath();ac.arc(32,32,26,0,Math.PI*2);ac.clip();
    if(face)drawPortrait(ac,face,6,6,52);else{ac.fillStyle=T.color(p,self);ac.fillRect(6,6,52,52);}ac.restore();
    drawHealthRing(ac,p,T.color(p,self),32,32,29,4);drawEquipmentArcs(ac,p,32,32,29);av.append(image);
    const info=node('div','row-info');
    const title=node('div','row-title');if(pref.names)title.append(node('span','row-name',p.kind==='self'?'本人 · '+(p.name||'未关联'):p.name||'槽位 '+p.slot));
    if(pref.teamIds&&teamLabel(p)){const tag=node('span','row-team',teamLabel(p));tag.style.color=T.color(p,self);title.append(tag);}
    if(pref.operatorNames)title.append(node('span','row-operator',operator.name));
    if(pref.names||pref.teamIds||pref.operatorNames)info.append(title);
    info.append(node('div','row-sub',`${{self:'本人',mate:'同队',other:'其他队伍',unknown:'队伍未知'}[T.relation(p,self)]} · ${T.life(p)} · HP ${fmt(T.hp(p),0)}`));
    if(pref.weapons)info.append(node('div','row-equipment',`手持 ${T.gear(p).weapon}`));
    const dist=node('div','row-distance',p.kind==='self'?'SELF':Number.isFinite(distance(p))?distance(p).toFixed(0)+' m':'—');dist.append(node('small','',fmt(T.speed(p,time))+' m/s'));
    button.append(av,info,dist);button.addEventListener('click',()=>{selected=selected===k?'':k;rosterSignature='';sidebar(now(),connected);});return button;
   }));if(!list.length)$('rosterRows').append(node('p','empty-small',query?'没有匹配的人物':'等待人物数据'));
  }
  details(rows.find(p=>T.key(p)===selected),time);
 }
 function animate(t){
  requestAnimationFrame(animate);if(!gate.ready(t,document.visibilityState==='visible'))return;
  if(replay){const previous=capture.revision;capture=replay.advance(replayWall===null?0:t-replayWall);replayWall=t;
   if(previous!==capture.revision){refreshLoot();updateHistory();}replayControls();}
  const alive=!!replay||connected&&t-lastReceived<1500,time=now();const rows=displayRows(presenter.frame(time,pref.smoothing,pref.prediction,!alive));
  const self=selfOf(rows);
  if(firstCenter&&(rows.length||lootRows.length)){firstCenter=false;centerOn(self||rows[0]||lootRows[0]);if(!mapInfo)map.setZoom(0);}
  if(pref.follow&&self){const q=project(self);if(Math.hypot(q.x-width/2,q.y-height/2)>1)centerOn(self);}
  lastPaintRows=rows;draw(rows,time);frameCount++;
  if(t-fpsAt>=1000){setText('fps',Math.round(frameCount*1000/(t-fpsAt)));frameCount=0;fpsAt=t;}
  if(t-sidebarAt>=200){sidebarAt=t;sidebar(time,alive);}
 }
 for(const k of ['loot','names','operatorNames','teamIds','weapons','armorLevels','distances','trails','cones','mates','others','prediction']){
  $(k).checked=pref[k];$(k).addEventListener('change',()=>{pref[k]=$(k).checked;save();rosterSignature='';if(k==='loot'){refreshLoot(true);renderLootItems();}});
 }
 $('smoothing').value=pref.smoothing;
 function syncPrediction(){$('prediction').disabled=pref.smoothing!=='original';}
 $('smoothing').addEventListener('change',()=>{pref.smoothing=$('smoothing').value;pref.prediction=false;$('prediction').checked=false;presenter.drawn.clear();syncPrediction();save();});syncPrediction();
 $('markerSize').value=pref.markerSize;setText('sizeValue',pref.markerSize+'%');$('markerSize').addEventListener('input',()=>{pref.markerSize=Number($('markerSize').value);setText('sizeValue',pref.markerSize+'%');save();});
 $('sort').value=pref.sort;$('sort').addEventListener('change',()=>{pref.sort=$('sort').value;save();rosterSignature='';});
 $('search').addEventListener('input',()=>{rosterSignature='';sidebar(now(),connected);});
 $('follow').addEventListener('click',()=>follow(!pref.follow));follow(pref.follow);
 $('fit').addEventListener('click',fitAll);$('zoomIn').addEventListener('click',()=>map.zoomIn());$('zoomOut').addEventListener('click',()=>map.zoomOut());
 const togglePanel=id=>{$(id).hidden=!$(id).hidden;if(!$(id).hidden)for(const other of ['tools','roster','lootPanel'])if(other!==id&&(isCompact()||id==='lootPanel'||other==='lootPanel'||other!=='roster'&&id!=='roster'))$(other).hidden=true;};
 $('toggleLoot').addEventListener('click',()=>{togglePanel('lootPanel');if(!$('lootPanel').hidden)renderLootFilters();});$('closeLoot').addEventListener('click',()=>{$('lootPanel').hidden=true;});
 $('toggleTools').addEventListener('click',()=>togglePanel('tools'));$('closeTools').addEventListener('click',()=>{$('tools').hidden=true;});
 $('toggleRoster').addEventListener('click',()=>togglePanel('roster'));$('closeRoster').addEventListener('click',()=>{$('roster').hidden=true;});
 $('tools').hidden=true;$('roster').hidden=true;
 $('focusSelected').addEventListener('click',()=>{const p=lastPaintRows.find(p=>T.key(p)===selected);if(p){follow(p.kind==='self');centerOn(p);}});
 $('fullscreen').addEventListener('click',async()=>{try{if(document.fullscreenElement)await document.exitFullscreen();else await document.documentElement.requestFullscreen();}catch{}});
 map.on('click',e=>{const hit=hits.find(h=>Math.hypot(h.x-e.containerPoint.x,h.y-e.containerPoint.y)<19*pref.markerSize/100);if(hit){selected=hit.k;$('roster').hidden=false;if(isCompact()){$('tools').hidden=true;$('lootPanel').hidden=true;}rosterSignature='';sidebar(now(),connected);}});
 window.addEventListener('keydown',e=>{if(e.key==='Escape'){selected='';rosterSignature='';}if(e.target instanceof HTMLInputElement||e.target instanceof HTMLSelectElement)return;if(e.key.toLowerCase()==='f')follow(!pref.follow);});
 document.addEventListener('visibilitychange',()=>{fpsAt=performance.now();frameCount=0;presenter.lastFrame=null;replayWall=null;});
 loadMaps();loadOperators();loadItems();poll();requestAnimationFrame(animate);
})();
