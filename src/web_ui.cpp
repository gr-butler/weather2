#include "web_ui.h"

#include <Arduino.h>

// Only the visible tab polls, and polling stops while the browser tab is in the
// background, so an idle page costs the device nothing.
const char kWebUiHtml[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Weather Station</title>
<style>
:root{color-scheme:dark;--bg:#0b0f14;--panel:#131a22;--line:#243140;--text:#d7dde5;--dim:#8b97a5;--accent:#4aa8ff;--ok:#3fb950;--bad:#f85149;--track:#202b38}
*{box-sizing:border-box}
[hidden]{display:none!important}
html,body{height:100%}
body{margin:0;display:flex;flex-direction:column;font-family:system-ui,sans-serif;background:var(--bg);color:var(--text)}
header{flex:none;background:var(--panel);border-bottom:1px solid var(--line)}
.bar{display:flex;gap:.75rem;align-items:baseline;flex-wrap:wrap;padding:.6rem .9rem .2rem}
h1{font-size:1.05rem;margin:0;font-weight:600}
#conn{font-size:.8rem;color:var(--dim)}
#conn.bad{color:var(--bad)}
nav{display:flex;gap:.25rem;padding:0 .5rem}
nav a{padding:.5rem .9rem;color:var(--dim);text-decoration:none;font-size:.9rem;border-bottom:2px solid transparent}
nav a:hover{color:var(--text)}
nav a[aria-selected=true]{color:var(--text);border-bottom-color:var(--accent)}
main{flex:1;min-height:0;overflow:auto}
section{padding:.9rem}
a{color:var(--accent)}
.grid{display:flex;flex-wrap:wrap;justify-content:center;gap:.8rem;max-width:44rem;margin:0 auto}
.grid .card{flex:1 1 8.5rem;max-width:10.4rem;text-align:center}
.stack{display:grid;gap:.8rem;max-width:40rem;margin:0 auto}
.card{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:.7rem .8rem;transition:opacity .3s}
.card h2{margin:0 0 .4rem;font-size:.78rem;font-weight:600;color:var(--dim);text-transform:uppercase;letter-spacing:.05em}
.grid svg{display:block;width:100%;max-width:13rem;margin:0 auto}
.card.off,.stale .grid .card{opacity:.45}
.sub{min-height:1.2em;font-size:.82rem;color:var(--dim)}
.trk,.val,.ring{fill:none}
.trk,.val{stroke-width:9;stroke-linecap:round}
.trk{stroke:var(--track)}
.val{stroke:var(--accent);transition:stroke-dasharray .6s,stroke .6s}
.ring{stroke:var(--track);stroke-width:3}
.tick{stroke:var(--dim);stroke-width:1}
.mark{stroke:var(--text);stroke-width:2.5;stroke-linecap:round;transform-origin:60px 60px;transition:transform .6s}
#vane{fill:var(--accent);transform-origin:60px 60px;transition:transform .8s}
.num{fill:var(--text);font-size:22px;font-weight:600;text-anchor:middle}
#c-dir .num{font-size:16px}
.unit,.lim{fill:var(--dim);text-anchor:middle}
.unit{font-size:9px}
.lim{font-size:8px}
.chips{display:flex;flex-wrap:wrap;gap:.4rem;justify-content:center;margin:1rem auto 0;max-width:44rem}
.chip{font-size:.78rem;padding:.2rem .6rem;border:1px solid var(--line);border-radius:999px;color:var(--dim)}
.chip::before{margin-right:.35rem}
.chip.ok::before{content:"\25CF";color:var(--ok)}
.chip.bad{color:var(--text);border-color:var(--bad)}
.chip.bad::before{content:"\2715";color:var(--bad)}
.foot{text-align:center;font-size:.8rem;color:var(--dim)}
#p-logs{display:flex;flex-direction:column;height:100%;padding:0}
.tools{flex:none;display:flex;gap:.6rem;align-items:center;flex-wrap:wrap;padding:.5rem .9rem;border-bottom:1px solid var(--line)}
.tools input[type=text]{flex:1;min-width:8rem;max-width:20rem}
.tools label{font-size:.85rem;display:inline-flex;gap:.3rem;align-items:center}
button,input[type=text],input[type=number],.btn{background:#1c2530;color:var(--text);border:1px solid #33404f;border-radius:6px;padding:.35rem .7rem;font:inherit;font-size:.85rem}
button:hover,.btn:hover{background:#263143;cursor:pointer}
button:disabled{opacity:.5;cursor:default}
.btn{display:inline-block;text-decoration:none}
.row{display:flex;gap:.6rem;align-items:center;flex-wrap:wrap}
.row input[type=range]{flex:1;min-width:8rem}
.row input[type=number]{width:5.5rem}
.msg{min-height:1.2em;color:var(--dim)}
.msg.bad{color:var(--bad)}
#log{flex:1;min-height:0;overflow:auto;padding:.4rem .9rem;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:.82rem;line-height:1.4;white-space:pre-wrap;word-break:break-word}
#log div{border-bottom:1px solid #10161d;padding:1px 0}
table{width:100%;border-collapse:collapse;font-size:.88rem}
td{padding:.3rem 0;border-top:1px solid var(--line);vertical-align:top}
td:first-child{color:var(--dim);width:9rem;padding-right:.8rem}
.stack p{margin:.2rem 0 .7rem;font-size:.88rem}
</style>
</head>
<body>
<header>
  <div class="bar"><h1>Weather Station</h1><span id="conn">connecting…</span></div>
  <nav role="tablist">
    <a id="t-live" role="tab" href="#live">Live</a>
    <a id="t-logs" role="tab" href="#logs">Logs</a>
    <a id="t-settings" role="tab" href="#settings">Settings</a>
  </nav>
</header>
<main>
<section id="p-live" role="tabpanel" aria-labelledby="t-live">
  <div class="grid" id="gauges"></div>
  <div class="chips" id="health"></div>
  <p class="foot" id="stime"></p>
</section>
<section id="p-logs" role="tabpanel" aria-labelledby="t-logs" hidden>
  <div class="tools">
    <input id="filter" type="text" placeholder="filter…" autocomplete="off" aria-label="Filter log lines">
    <label><input id="auto" type="checkbox" checked> autoscroll</label>
    <button id="pause">Pause</button>
    <button id="clear">Clear</button>
  </div>
  <div id="log"></div>
</section>
<section id="p-settings" role="tabpanel" aria-labelledby="t-settings" hidden>
  <div class="stack">
    <div class="card">
      <h2>Firmware</h2>
      <p id="fw">–</p>
      <a class="btn" href="/update">Firmware update (OTA)</a>
    </div>
    <div class="card">
      <h2>Device</h2>
      <table id="sys"></table>
    </div>
    <div class="card">
      <h2>Wind direction correction</h2>
      <p>Use this if the wind vane is no longer lined up with north. The offset is added to every direction reading before it is shown here or sent to MQTT, Prometheus and the Met Office. Positive turns the reading clockwise.</p>
      <div class="row">
        <input id="off-r" type="range" min="-180" max="180" step="1" value="0" aria-label="Wind direction offset slider">
        <input id="off-n" type="number" step="1" value="0" aria-label="Wind direction offset in degrees">
        <span>°</span>
        <button id="off-save" disabled>Save</button>
      </div>
      <p id="off-prev">–</p>
      <p class="msg" id="off-msg"></p>
    </div>
    <div class="card">
      <h2>Data endpoints</h2>
      <p><a href="/data.json">/data.json</a> &middot; current readings (JSON)</p>
      <p><a href="/metrics">/metrics</a> &middot; Prometheus metrics</p>
    </div>
  </div>
</section>
</main>
<script>
const $=i=>document.getElementById(i);
const TABS=['live','logs','settings'];
const C=2*Math.PI*42,ARC=.75*C;

// Gauge ranges and colour bands. z = [from value, colour], last match wins.
const G={
  temp:{t:'Temperature',u:'°C',min:-10,max:40,d:1,z:[[-99,'#3274d9'],[0,'#6ed0e0'],[10,'#3fb950'],[20,'#eab839'],[25,'#ff9830'],[28,'#f2495c']]},
  wind:{t:'Wind speed',u:'mph',min:0,max:60,d:1,z:[[0,'#3fb950'],[20,'#eab839'],[30,'#ff9830'],[40,'#f2495c']]},
  rain:{t:'Rain',u:'mm in last hour',min:0,max:20,d:1,z:[[0,'#3fb950'],[1,'#6ed0e0'],[2,'#3274d9'],[4,'#eab839'],[8,'#ff9830'],[16,'#f2495c']]},
  river:{t:'River level',u:'m',min:0,max:3,d:2,z:[[0,'#f2495c'],[.3,'#eab839'],[.5,'#3fb950'],[2.4,'#eab839']]},
  hum:{t:'Humidity',u:'% RH',min:0,max:100,d:0,z:[[0,'#4aa8ff']]},
  pres:{t:'Pressure',u:'hPa',min:950,max:1050,d:1,z:[[0,'#b877d9']]}
};
const HEALTH=[['temperature','Temperature'],['pressure_humidity','Pressure / humidity'],['wind','Wind'],['rain','Rain'],['river','River']];
const POINTS=['N','NNE','NE','ENE','E','ESE','SE','SSE','S','SSW','SW','WSW','W','WNW','NW','NNW'];

function gaugeHtml(k){
  const g=G[k];
  return `<div class="card" id="c-${k}"><h2>${g.t}</h2><svg viewBox="0 0 120 104" role="img" aria-label="${g.t}">`+
    `<circle class="trk" cx="60" cy="60" r="42" stroke-dasharray="${ARC} ${C}" transform="rotate(135 60 60)"/>`+
    `<circle class="val" cx="60" cy="60" r="42" stroke-dasharray="0 ${C}" style="visibility:hidden" transform="rotate(135 60 60)"/>`+
    `<line class="mark" x1="60" y1="10" x2="60" y2="26" hidden/>`+
    `<text class="num" x="60" y="64">–</text><text class="unit" x="60" y="79">${g.u}</text>`+
    `<text class="lim" x="27" y="101">${g.min}</text><text class="lim" x="93" y="101">${g.max}</text>`+
    `</svg><div class="sub"></div></div>`;
}

function compassHtml(){
  let t='';
  for(let i=0;i<16;i++)t+=`<line class="tick" x1="60" y1="16" x2="60" y2="${i%4?19:21}" transform="rotate(${i*22.5} 60 60)"/>`;
  return `<div class="card" id="c-dir"><h2>Wind direction</h2><svg viewBox="0 0 120 104" role="img" aria-label="Wind direction">`+
    `<g transform="translate(0 -8)"><circle class="ring" cx="60" cy="60" r="44"/>${t}`+
    `<text class="lim" x="60" y="36">N</text><text class="lim" x="87" y="63">E</text>`+
    `<text class="lim" x="60" y="90">S</text><text class="lim" x="33" y="63">W</text>`+
    `<path id="vane" d="M60 27 L54 17 L66 17 Z"/><text class="num" x="60" y="66">–</text></g>`+
    `</svg><div class="sub"></div></div>`;
}

function frac(k,v){const g=G[k];return Math.min(1,Math.max(0,(v-g.min)/(g.max-g.min)))}

// ok = the reading is usable; stale = show it, but dimmed.
function setGauge(k,v,ok,sub,stale){
  const g=G[k],c=$('c-'+k),a=c.querySelector('.val');
  ok=ok&&typeof v==='number';
  c.classList.toggle('off',!ok||!!stale);
  const f=ok?frac(k,v):0;
  a.style.strokeDasharray=f*ARC+' '+C;
  a.style.visibility=f>0?'':'hidden';
  let col=g.z[0][1];
  if(ok)for(const z of g.z)if(v>=z[0])col=z[1];
  a.style.stroke=col;
  c.querySelector('.num').textContent=ok?v.toFixed(g.d):'–';
  c.querySelector('.sub').textContent=sub||'';
}

let vaneAng=0;
function render(d){
  const s=d.status||{};
  setGauge('temp',d.hiResTemp_C,s.temperature);
  setGauge('wind',d.wind_speed,s.wind,s.wind?'Gust '+d.wind_gust.toFixed(1)+' mph':'');
  const m=$('c-wind').querySelector('.mark');
  m.toggleAttribute('hidden',!s.wind);
  if(s.wind)m.style.transform=`rotate(${270*frac('wind',d.wind_gust)-135}deg)`;

  const c=$('c-dir');
  c.classList.toggle('off',!s.wind);
  $('vane').toggleAttribute('hidden',!s.wind);
  if(s.wind){
    // Turn the short way round so 350 -> 10 does not swing through south.
    vaneAng+=((d.wind_dir-vaneAng)%360+540)%360-180;
    $('vane').style.transform=`rotate(${vaneAng}deg)`;
    c.querySelector('.num').textContent=POINTS[Math.round(d.wind_dir/22.5)%16];
    c.querySelector('.sub').textContent='From '+Math.round(d.wind_dir)%360+'°';
  }else{
    c.querySelector('.num').textContent='–';
    c.querySelector('.sub').textContent='';
  }

  setGauge('rain',d.rain_mm_hr,s.rain,s.rain?'Today '+d.rain_day.toFixed(1)+' mm':'');
  setGauge('river',d.river_level,true,s.river?'':'Last update failed',!s.river);
  setGauge('hum',d.humidity_RH,s.pressure_humidity);
  setGauge('pres',d.pressure_hPa,s.pressure_humidity);
  for(const h of HEALTH){
    const e=$('h-'+h[0]);
    e.className='chip '+(s[h[0]]?'ok':'bad');
    e.title=h[1]+(s[h[0]]?': working':': not working');
  }
  $('stime').textContent='Station time '+d.time;
}

const logEl=$('log');
let after=0,paused=false,filter='';
const filtered=t=>!!filter&&!t.toLowerCase().includes(filter);
$('filter').oninput=e=>{
  filter=e.target.value.toLowerCase();
  for(const d of logEl.children)d.hidden=filtered(d.textContent);
};
$('clear').onclick=()=>{logEl.textContent=''};
$('pause').onclick=e=>{
  paused=!paused;
  e.target.textContent=paused?'Resume':'Pause';
  if(paused)status('paused');else tick();
};

// Fetches the next page of buffered lines. Returns true while the device still
// holds lines we have not seen, so the caller can fetch the next page at once.
async function pollLogs(){
  const d=await getJson('/logs.json?limit=50&after='+after);
  if(d.seq<after){after=0;return true}   // device rebooted: refetch from the start
  const n=d.lines?d.lines.length:0;
  if(!n){after=d.seq;return false}
  const f=document.createDocumentFragment();
  for(const l of d.lines){
    const v=document.createElement('div');
    v.textContent=l.t;
    v.hidden=filtered(l.t);
    f.appendChild(v);
  }
  logEl.appendChild(f);
  while(logEl.children.length>1000)logEl.firstChild.remove();
  if($('auto').checked)logEl.scrollTop=logEl.scrollHeight;
  after=d.lines[n-1].n;
  return after<d.seq;
}

function row(t,k,v){const r=t.insertRow();r.insertCell().textContent=k;r.insertCell().textContent=v}
function uptime(s){
  const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);
  return (d?d+'d ':'')+h+'h '+m+'m';
}
const kb=b=>Math.round(b/1024)+' KB';
function renderSys(d){
  $('fw').textContent='Version '+d.version+' · build '+d.build+' · '+d.git;
  const t=$('sys');
  t.textContent='';
  row(t,'Uptime',uptime(d.uptime_s));
  row(t,'Network',d.iface+' · '+d.ip+(d.iface==='wifi'?' · '+d.rssi+' dBm':''));
  row(t,'MQTT',d.mqtt?'connected':'disconnected');
  row(t,'Mode',d.recovery?'recovery (sensors suspended)':'normal');
  row(t,'Heap',kb(d.heap_free)+' free · '+kb(d.heap_min)+' lowest · '+kb(d.heap_largest)+' largest block');
  row(t,'Last boot',d.boot_reason);
  row(t,'Reboot history',d.reboots||'–');
}

// Wind direction offset. `saved` is what the device holds; the inputs may be
// ahead of it while the user is trying a value (dirty), and polling must not
// overwrite what they are typing.
const offR=$('off-r'),offN=$('off-n'),offSave=$('off-save');
let saved=null,rawDir=null,windOk=false,dirty=false;
const turn=d=>((d%360)+360)%360;
const point=d=>POINTS[Math.round(turn(d)/22.5)%16];
const bearing=d=>Math.round(turn(d))%360+'° ('+point(d)+')';
function normOff(v){let d=v%360;if(d>180)d-=360;else if(d<=-180)d+=360;return d}
function offValue(){
  const v=Number(offN.value);
  return offN.value.trim()!==''&&Number.isInteger(v)&&v>=-360&&v<=360?v:null;
}
function offMsg(text,bad){const m=$('off-msg');m.textContent=text;m.className=bad?'msg bad':'msg'}
function preview(){
  const v=offValue();
  offSave.disabled=v===null||saved===null||normOff(v)===saved;
  let t;
  if(v===null)t='Enter a whole number of degrees from -360 to 360.';
  else if(rawDir===null)t='–';
  else if(!windOk)t='Wind sensor offline, so there is no live reading to preview.';
  else t='Sensor reads '+bearing(rawDir)+' → reported as '+bearing(rawDir+v);
  $('off-prev').textContent=t;
}
function renderSet(d){
  saved=d.wind_dir_offset;rawDir=d.wind_dir_raw;windOk=d.wind_online;
  if(!dirty){offN.value=saved;offR.value=saved}
  preview();
}
offR.oninput=()=>{dirty=true;offN.value=offR.value;offMsg('');preview()};
offN.oninput=()=>{
  dirty=true;
  const v=offValue();
  if(v!==null)offR.value=normOff(v);
  offMsg('');preview();
};
offSave.onclick=async()=>{
  const v=offValue();
  if(v===null)return;
  offSave.disabled=true;
  offMsg('Saving…');
  try{
    const r=await fetch('/settings.json',{method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded','X-Requested-With':'weather-ui'},
      body:'wind_dir_offset='+v});
    if(!r.ok)throw new Error(await r.text()||r.status);
    dirty=false;
    renderSet(await r.json());
    offMsg('Saved. All outputs now use an offset of '+saved+'°.');
  }catch(e){
    offMsg('Not saved: '+e.message,true);
    preview();
  }
};

async function getJson(url){
  const c=new AbortController(),t=setTimeout(()=>c.abort(),4000);
  try{
    const r=await fetch(url,{signal:c.signal,cache:'no-store'});
    if(!r.ok)throw new Error(r.status);
    return await r.json();
  }finally{clearTimeout(t)}
}

function status(text,bad){
  const c=$('conn');
  c.className=bad?'bad':'';
  c.textContent=text;
  document.body.classList.toggle('stale',!!bad);
}
const live=()=>status('live · '+new Date().toLocaleTimeString());

// One request at a time, and only for the tab on screen.
let tab='',timer=0,busy=false;
async function tick(){
  clearTimeout(timer);
  if(busy||document.hidden)return;
  busy=true;
  const t=tab;
  let wait=3000;
  try{
    if(t==='logs'){
      wait=1000;
      if(!paused){if(await pollLogs())wait=150;live()}
    }else if(t==='settings'){
      wait=5000;
      renderSys(await getJson('/system.json'));
      renderSet(await getJson('/settings.json'));live();
    }else{
      render(await getJson('/data.json'));live();
    }
  }catch(e){status('no response — retrying…',true)}
  busy=false;
  timer=setTimeout(tick,t===tab?wait:0);
}

function show(h){
  tab=TABS.includes(h)?h:'live';
  for(const n of TABS){
    $('p-'+n).hidden=n!==tab;
    $('t-'+n).setAttribute('aria-selected',n===tab);
  }
  tick();
}

$('gauges').innerHTML=gaugeHtml('temp')+gaugeHtml('wind')+compassHtml()+gaugeHtml('rain')+gaugeHtml('river')+gaugeHtml('hum')+gaugeHtml('pres');
$('health').innerHTML=HEALTH.map(h=>`<span class="chip" id="h-${h[0]}">${h[1]}</span>`).join('');
addEventListener('hashchange',()=>show(location.hash.slice(1)));
document.addEventListener('visibilitychange',()=>{if(!document.hidden)tick()});
show(location.hash.slice(1));
</script>
</body>
</html>)HTML";

const size_t kWebUiHtmlLen = sizeof(kWebUiHtml) - 1;
