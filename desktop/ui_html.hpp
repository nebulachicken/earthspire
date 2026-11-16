// The control-panel page, embedded so the program is a single executable.
#pragma once

namespace sv {

static const char kIndexHtml[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Sentinel Viewer</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<style>
:root{color-scheme:dark;--bg:#12151a;--panel:#1b2028;--line:#2d3542;--txt:#e6e9ee;--dim:#98a2b3;--acc:#ffb000;--bad:#ff6b6b;--ok:#6bd48a}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--txt);font:14px/1.45 system-ui,Segoe UI,Roboto,sans-serif}
header{display:flex;align-items:center;gap:16px;padding:10px 18px;border-bottom:1px solid var(--line);background:var(--panel)}
header h1{font-size:16px;margin:0;font-weight:650}
#status{flex:1;color:var(--dim);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#status.err{color:var(--bad)}
main{display:grid;grid-template-columns:minmax(340px,5fr) minmax(420px,6fr);gap:16px;padding:16px;max-width:1280px;margin:0 auto}
@media(max-width:900px){main{grid-template-columns:1fr}}
section{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:14px 16px}
h2{font-size:13px;text-transform:uppercase;letter-spacing:.06em;color:var(--dim);margin:14px 0 8px}
h2:first-child{margin-top:0}
h2 small{text-transform:none;letter-spacing:0;font-weight:400}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:6px 0}
input[type=text],input[type=number]{background:#10131a;color:var(--txt);border:1px solid var(--line);border-radius:6px;padding:6px 8px;font:inherit}
input#q{flex:1;min-width:140px}
input.num{width:96px}
button{background:#2a3240;color:var(--txt);border:1px solid var(--line);border-radius:6px;padding:7px 12px;font:inherit;cursor:pointer}
button:hover{background:#344057}
button.primary{background:var(--acc);color:#1a1300;border-color:var(--acc);font-weight:650}
button.primary:hover{filter:brightness(1.08)}
button:disabled{opacity:.5;cursor:default}
#map{height:290px;border-radius:8px;border:1px solid var(--line);margin:8px 0;background:#10131a}
.hint{color:var(--dim);font-size:12.5px}
#results{display:flex;flex-direction:column;gap:2px;margin:4px 0}
#results a{color:var(--txt);cursor:pointer;padding:4px 8px;border-radius:5px;background:#10131a;font-size:13px}
#results a:hover{background:#252d3a}
#scenes{margin-top:8px;max-height:230px;overflow:auto;border:1px solid var(--line);border-radius:8px}
#scenes:empty{display:none}
.scene{display:flex;gap:10px;padding:6px 10px;border-bottom:1px solid var(--line);cursor:pointer;align-items:center}
.scene:last-child{border-bottom:0}
.scene:hover{background:#252d3a}
.scene.sel{background:#2f3a4d;outline:1px solid var(--acc);outline-offset:-1px}
.scene .d{width:90px;font-variant-numeric:tabular-nums}
.scene .c{flex:1;color:var(--dim)}
.scene .c.good{color:var(--ok)} .scene .c.bad{color:var(--bad)}
#pv{position:relative;background:#000;border-radius:8px;overflow:hidden;aspect-ratio:4/3;display:flex;align-items:center;justify-content:center}
#preview{width:100%;height:100%;image-rendering:pixelated;display:none}
#placeholder{color:var(--dim);padding:20px;text-align:center}
#stats{margin:8px 0;color:var(--dim);font-size:12.5px;min-height:18px}
#stats b{color:var(--txt);font-weight:600} #stats .warn{color:var(--acc)}
.sl{display:grid;grid-template-columns:132px 1fr 76px;gap:8px;align-items:center;margin:3px 0}
.sl label{color:var(--txt)} .sl output{text-align:right;color:var(--dim);font-variant-numeric:tabular-nums}
.sl input[type=range]{width:100%;accent-color:var(--acc)}
.sl.off label{color:var(--acc)}
label.chk{display:flex;gap:8px;align-items:center;margin:8px 0;color:var(--txt)}
#saved{margin-top:8px;font-size:13px} #saved.ok{color:var(--ok)} #saved.err{color:var(--bad)}
</style></head>
<body>
<header><h1>Sentinel Viewer</h1><span id="status">Starting&hellip;</span><button id="quit" title="Stop the program">Quit</button></header>
<main>
<section>
  <h2>1 &middot; Location</h2>
  <div class="row"><input type="text" id="q" placeholder="Search a place, or type &ldquo;lat, lon&rdquo;"><button id="go">Search</button></div>
  <div id="results"></div>
  <div class="row"><label>Lat <input class="num" type="text" id="lat"></label><label>Lon <input class="num" type="text" id="lon"></label></div>
  <div id="map"></div>
  <div class="hint">Click the map or drag the pin. The orange box is the 3.2 &times; 2.4 km area the calculator will show.</div>
  <h2>2 &middot; Scene</h2>
  <div class="row"><label>Look back <input class="num" style="width:70px" type="number" id="days" value="120" min="1" max="3650"> days</label>
  <label>Max cloud <input class="num" style="width:62px" type="number" id="maxcloud" value="10" min="0" max="100"> %</label></div>
  <button id="find" class="primary">Find scenes</button>
  <div id="scenes"></div>
</section>
<section>
  <h2>3 &middot; Preview <small>(the same conversion the calculator uses)</small></h2>
  <div id="pv"><img id="preview" alt="Preview"><div id="placeholder">Pick a location, then press &ldquo;Find scenes&rdquo;.</div></div>
  <div id="stats"></div>
  <h2>4 &middot; Color</h2>
  <div id="sliders"></div>
  <label class="chk"><input type="checkbox" id="clouds"> Show cloud overlay in the preview</label>
  <div class="row"><button id="reset">Reset colors</button><button id="usedef" title="Use the offset stated in the scene's metadata">Use scene offset</button></div>
  <h2>5 &middot; Save</h2>
  <label class="chk"><input type="checkbox" id="raw"> Save without color adjustments (raw data)</label>
  <button id="save" class="primary">Save file for the calculator</button>
  <div id="saved"></div>
</section>
</main>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script>
(function(){
'use strict';
var $=function(id){return document.getElementById(id);};
var SL=[
 {k:'ev', label:'Exposure', min:-3,max:3,step:0.05,def:0,fmt:function(v){return (v>0?'+':'')+v.toFixed(2)+' EV';}},
 {k:'mid',label:'Midtones', min:-100,max:100,step:1,def:0},
 {k:'sh', label:'Shadows (lift)', min:-100,max:100,step:1,def:50},
 {k:'hi', label:'Highlight recovery', min:0,max:100,step:1,def:60},
 {k:'con',label:'Contrast', min:-100,max:100,step:1,def:0},
 {k:'sat',label:'Saturation', min:0,max:200,step:1,def:100,fmt:function(v){return v+'%';}},
 {k:'tmp',label:'Temperature', min:-100,max:100,step:1,def:0},
 {k:'off',label:'Black level / offset', min:-0.2,max:0.1,step:0.005,def:0,off:true,fmt:function(v){return v.toFixed(3);}}
];
var loaded=null, scenes=[], offsetDefault=0, busy=false;

function setStatus(t,err){var s=$('status');s.textContent=t;s.className=err?'err':'';}
function sleep(ms){return new Promise(function(r){setTimeout(r,ms);});}
function post(url,params){return fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:params.toString()});}
async function getStatus(){var r=await fetch('/api/status');return r.json();}

/* ---------- sliders ---------- */
function buildSliders(){
  var host=$('sliders');
  SL.forEach(function(s){
    var d=document.createElement('div');d.className='sl'+(s.off?' off':'');
    var l=document.createElement('label');l.textContent=s.label;l.htmlFor='s_'+s.k;
    var i=document.createElement('input');i.type='range';i.id='s_'+s.k;i.min=s.min;i.max=s.max;i.step=s.step;i.value=s.def;
    var o=document.createElement('output');o.id='o_'+s.k;
    i.addEventListener('input',function(){showVal(s);schedulePreview();});
    if(s.off){i.disabled=true;}
    d.appendChild(l);d.appendChild(i);d.appendChild(o);host.appendChild(d);
    showVal(s);
  });
}
function val(s){return parseFloat($('s_'+s.k).value);}
function showVal(s){var v=val(s);$('o_'+s.k).textContent=s.fmt?s.fmt(v):String(v);}
function resetColors(){SL.forEach(function(s){if(s.off)return;$('s_'+s.k).value=s.def;showVal(s);});schedulePreview();}
function useSceneOffset(){if(!loaded)return;var s=SL[SL.length-1];$('s_off').value=offsetDefault;showVal(s);schedulePreview();}

/* ---------- preview ---------- */
var pvTimer=null,pvCtl=null,pvUrl=null;
function schedulePreview(){clearTimeout(pvTimer);pvTimer=setTimeout(updatePreview,120);}
function gradeParams(){
  var p=new URLSearchParams();
  SL.forEach(function(s){if(s.off&&!loaded)return;p.set(s.k,String(val(s)));});
  p.set('clouds',$('clouds').checked?'1':'0');
  return p;
}
async function updatePreview(){
  if(!loaded)return;
  if(pvCtl)pvCtl.abort();
  pvCtl=new AbortController();
  try{
    var r=await fetch('/api/preview.bmp?'+gradeParams().toString(),{signal:pvCtl.signal});
    if(!r.ok)throw new Error('Preview failed ('+r.status+')');
    var st=null;try{st=JSON.parse(r.headers.get('X-Stats'));}catch(e){}
    var blob=await r.blob();var url=URL.createObjectURL(blob);
    var img=$('preview');img.src=url;img.style.display='block';$('placeholder').style.display='none';
    if(pvUrl)URL.revokeObjectURL(pvUrl);pvUrl=url;
    showStats(st);
  }catch(e){if(e.name!=='AbortError')setStatus(e.message,true);}
}
function showStats(st){
  var h='';
  if(loaded){
    h+='<b>'+loaded.date+'</b> &middot; '+loaded.cloud.toFixed(1)+'% cloud over this area &middot; offset in file: '+loaded.offset.toFixed(3);
    h+='<br>Raw data (red/green/blue): 1% '+loaded.rawP1.join('/')+' &middot; median '+loaded.rawP50.join('/')+' &middot; 99% '+loaded.rawP99.join('/');
  }
  if(st){
    h+='<br>Pixels clipped to black by the offset: <b'+(st.clippedBlack>20?' class="warn"':'')+'>'+st.clippedBlack.toFixed(1)+'%</b> &middot; near white after grading: <b>'+st.clippedWhite.toFixed(1)+'%</b>';
    if(st.clippedBlack>20)h+='<br><span class="warn">Lots of solid black? Try moving &ldquo;Black level / offset&rdquo; toward 0.</span>';
  }
  $('stats').innerHTML=h;
}

/* ---------- scenes & jobs ---------- */
function renderScenes(list,sel){
  var maxc=parseFloat($('maxcloud').value)||10;
  var host=$('scenes');host.innerHTML='';
  list.forEach(function(s){
    var d=document.createElement('div');d.className='scene'+(s.i===sel?' sel':'');
    var cls=s.nodata>2?'bad':(s.cloud<=maxc?'good':'');
    d.innerHTML='<span class="d">'+s.date+'</span><span class="c '+cls+'">'+s.cloud.toFixed(1)+'% cloud'+(s.nodata>2?' &middot; partial coverage':'')+'</span>';
    d.addEventListener('click',function(){loadScene(s.i);});
    host.appendChild(d);
  });
}
function recommend(list){
  var maxc=parseFloat($('maxcloud').value)||10,best=-1;
  for(var i=0;i<list.length;i++){var s=list[i];if(s.nodata>2)continue;if(s.cloud<=maxc)return s.i;if(best<0||s.cloud<list[best].cloud)best=i;}
  return best;
}
async function waitIdle(){
  for(;;){
    var s=await getStatus();
    setStatus(s.message,s.state==='error');
    scenes=s.scenes;renderScenes(scenes,s.loaded?s.loaded.index:-1);
    if(s.state!=='searching'&&s.state!=='loading'){
      if(s.state==='error')setStatus((s.message?s.message+' ':'')+s.error,true);
      return s;
    }
    await sleep(600);
  }
}
function setBusy(b){busy=b;$('find').disabled=b;}
function applyLoaded(s){
  loaded=s.loaded;
  if(!loaded)return;
  offsetDefault=loaded.offset;
  var os=SL[SL.length-1];var el=$('s_off');el.disabled=false;el.value=offsetDefault;showVal(os);
  renderScenes(s.scenes,loaded.index);
  updatePreview();
}
async function loadScene(i){
  if(busy)return;setBusy(true);
  try{
    var r=await post('/api/load',new URLSearchParams({index:String(i)}));
    if(!r.ok){var j=await r.json();setStatus(j.error||'Load failed',true);return;}
    var s=await waitIdle();
    if(s.state!=='error')applyLoaded(s);
  }catch(e){setStatus(e.message,true);}finally{setBusy(false);}
}
async function findScenes(){
  if(busy)return;
  var lat=parseFloat($('lat').value),lon=parseFloat($('lon').value);
  if(!isFinite(lat)||!isFinite(lon)){setStatus('Enter a valid latitude and longitude.',true);return;}
  setBusy(true);
  try{
    var r=await post('/api/search',new URLSearchParams({lat:String(lat),lon:String(lon),days:$('days').value,maxscreen:'12'}));
    if(!r.ok){var j=await r.json();setStatus(j.error||'Search failed',true);return;}
    var s=await waitIdle();
    if(s.state==='error')return;
    var pick=recommend(s.scenes);
    if(pick<0){setStatus('No usable scenes. Raise max cloud, or look back further.',true);return;}
    setBusy(false);
    await loadScene(pick);
  }catch(e){setStatus(e.message,true);}finally{setBusy(false);}
}

/* ---------- save / quit ---------- */
async function save(){
  if(!loaded){$('saved').className='err';$('saved').textContent='Find and load a scene first.';return;}
  var p=gradeParams();p.set('raw',$('raw').checked?'1':'0');
  try{
    var r=await post('/api/save',p);var j=await r.json();
    if(!r.ok)throw new Error(j.error||'Save failed');
    $('saved').className='ok';
    $('saved').textContent='Saved '+(j.bytes/1024).toFixed(0)+' KB to '+j.path+'. Copy it to the calculator\u2019s My Documents (checksums verified).';
  }catch(e){$('saved').className='err';$('saved').textContent=e.message;}
}

/* ---------- map & place search ---------- */
var map=null,marker=null,box=null;
function bboxFor(lat,lon){var c=Math.max(Math.cos(lat*Math.PI/180),0.05);var hl=1.2/111.2,hn=1.6/(111.32*c);return [[lat-hl,lon-hn],[lat+hl,lon+hn]];}
function setLocation(lat,lon,pan){
  lat=Math.max(-80,Math.min(80,lat));lon=Math.max(-180,Math.min(180,lon));
  $('lat').value=lat.toFixed(5);$('lon').value=lon.toFixed(5);
  if(map){marker.setLatLng([lat,lon]);box.setBounds(bboxFor(lat,lon));if(pan)map.setView([lat,lon],Math.max(map.getZoom(),13));}
}
function initMap(lat,lon){
  if(!window.L){$('map').innerHTML='<div class="hint" style="padding:12px">The map could not load (no internet?). You can still type coordinates above.</div>';return;}
  map=L.map('map').setView([lat,lon],12);
  var osm=L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'&copy; OpenStreetMap contributors'});
  var sat=L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',{maxZoom:18,attribution:'Imagery &copy; Esri'});
  osm.addTo(map);L.control.layers({'Map':osm,'Satellite':sat}).addTo(map);
  marker=L.marker([lat,lon],{draggable:true}).addTo(map);
  box=L.rectangle(bboxFor(lat,lon),{color:'#ffb000',weight:2,fill:false}).addTo(map);
  map.on('click',function(e){setLocation(e.latlng.lat,e.latlng.lng,false);});
  marker.on('dragend',function(){var p=marker.getLatLng();setLocation(p.lat,p.lng,false);});
}
async function placeSearch(){
  var q=$('q').value.trim();if(!q)return;
  var m=q.match(/^\s*(-?\d+(?:\.\d+)?)\s*[, ]\s*(-?\d+(?:\.\d+)?)\s*$/);
  if(m){setLocation(parseFloat(m[1]),parseFloat(m[2]),true);return;}
  var host=$('results');host.textContent='Searching\u2026';
  try{
    var r=await fetch('https://nominatim.openstreetmap.org/search?format=jsonv2&limit=6&q='+encodeURIComponent(q));
    var j=await r.json();host.innerHTML='';
    if(!j.length){host.textContent='No places found.';return;}
    j.forEach(function(p){
      var a=document.createElement('a');a.textContent=p.display_name;
      a.addEventListener('click',function(){setLocation(parseFloat(p.lat),parseFloat(p.lon),true);host.innerHTML='';});
      host.appendChild(a);
    });
  }catch(e){host.textContent='Place search is unavailable. Click the map or type coordinates.';}
}

/* ---------- start-up ---------- */
async function main(){
  buildSliders();
  $('go').addEventListener('click',placeSearch);
  $('q').addEventListener('keydown',function(e){if(e.key==='Enter')placeSearch();});
  ['lat','lon'].forEach(function(id){$(id).addEventListener('change',function(){
    var a=parseFloat($('lat').value),b=parseFloat($('lon').value);if(isFinite(a)&&isFinite(b))setLocation(a,b,true);});});
  $('find').addEventListener('click',findScenes);
  $('reset').addEventListener('click',resetColors);
  $('usedef').addEventListener('click',useSceneOffset);
  $('clouds').addEventListener('change',schedulePreview);
  $('save').addEventListener('click',save);
  $('quit').addEventListener('click',async function(){
    try{await post('/api/quit',new URLSearchParams());}catch(e){}
    setStatus('Stopped. You can close this tab.');
    document.querySelectorAll('button').forEach(function(b){b.disabled=true;});
  });
  var s=await getStatus();
  $('lat').value=s.defaultLat.toFixed(5);$('lon').value=s.defaultLon.toFixed(5);
  initMap(s.defaultLat,s.defaultLon);
  if(s.loaded){scenes=s.scenes;applyLoaded(s);}
  setStatus(s.loaded?s.message:'Ready. Pick a location, then press Find scenes.');
}
main().catch(function(e){setStatus('Could not reach the program: '+e.message,true);});
})();
</script>
</body></html>
)HTML";

}  // namespace sv
