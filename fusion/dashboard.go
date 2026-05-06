// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 CEMAXECUTER LLC
//
// fusion/dashboard.go: embedded HTML dashboard.
//
// Same slate-palette / sister-project style as the per-sniffer
// dashboard, but multi-station-aware. Each tab will grow over a few
// commits; this one ships the skeleton, the Sensors tab (add/remove
// via /api/sensors), and the Config tab (command fan-out via
// /api/fanout/*). Live/Activity/Topology are stubs that render
// placeholder text until their dedicated commits land.

package main

const dashboardHTML = `<!doctype html>
<html><head><meta charset="utf-8">
<title>meshtastic-fusion</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:system-ui,-apple-system,sans-serif;background:#0f172a;color:#e2e8f0;
  height:100vh;display:flex;flex-direction:column;font-size:14px}
#bar{height:44px;flex-shrink:0;background:#1e293b;display:flex;align-items:center;
  padding:0 16px;gap:20px;border-bottom:1px solid #334155}
#bar .title{font-weight:600;color:#f8fafc;letter-spacing:0.5px}
#bar .stat{color:#94a3b8;font-size:13px}
#bar .val{color:#38bdf8;font-weight:600;font-variant-numeric:tabular-nums;margin-left:6px}
#bar #status{margin-left:auto;color:#64748b;font-size:12px}
#tabs{display:flex;align-items:center;background:#1e293b;border-bottom:1px solid #334155;
  flex-shrink:0}
#tabs button{background:none;color:#64748b;border:none;padding:8px 16px;cursor:pointer;
  font:inherit;text-transform:uppercase;font-size:12px;letter-spacing:1px;font-weight:600;
  border-bottom:2px solid transparent}
#tabs button:hover{color:#94a3b8}
#tabs button.active{color:#38bdf8;border-bottom-color:#38bdf8}
.tab{flex:1;display:none;overflow:auto;padding:18px}
.tab.active{display:block}
h2{font-size:12px;color:#38bdf8;text-transform:uppercase;letter-spacing:1px;font-weight:600;
  border-bottom:1px solid #334155;padding-bottom:5px;margin-bottom:10px}
table{width:100%;border-collapse:collapse;font-size:12px;font-variant-numeric:tabular-nums}
th,td{text-align:left;padding:5px 8px;border-bottom:1px solid #1e293b}
th{color:#64748b;font-weight:600;font-size:11px;text-transform:uppercase;letter-spacing:0.5px}
tr:hover td{background:#1e293b}
.muted{color:#64748b}
.status-ok{color:#4ade80}
.status-err{color:#f87171}
.status-stale{color:#fbbf24}
.row{margin-bottom:14px;display:flex;gap:8px;flex-wrap:wrap;align-items:center}
input[type=text],textarea{background:#0f172a;color:#e2e8f0;border:1px solid #334155;
  border-radius:3px;padding:6px 10px;font:inherit;font-size:13px;flex:1;min-width:200px}
input[type=text]:focus,textarea:focus{outline:none;border-color:#38bdf8}
textarea{font-family:'SF Mono',Consolas,monospace;min-height:80px}
button.primary{background:#0c4a6e;color:#bae6fd;border:1px solid #0284c7;border-radius:3px;
  padding:6px 14px;cursor:pointer;font-size:13px;font-weight:500}
button.primary:hover{background:#075985}
button.danger{background:#7f1d1d;color:#fecaca;border:1px solid #b91c1c;border-radius:3px;
  padding:3px 9px;cursor:pointer;font-size:11px}
button.danger:hover{background:#991b1b}
.hint{color:#64748b;font-size:11px;margin-top:4px;display:block}
.placeholder{color:#64748b;font-style:italic;padding:30px 0;text-align:center}
.fanout-result{font-family:'SF Mono',Consolas,monospace;font-size:11px;color:#94a3b8;
  background:#1e293b;border-radius:3px;padding:8px 10px;margin-top:6px;white-space:pre-wrap;
  max-height:200px;overflow:auto}
</style></head><body>
<div id="bar">
  <span class="title">meshtastic-fusion</span>
  <span class="stat">Sensors <span id="st-sensors" class="val">0</span></span>
  <span class="stat">Events <span id="st-events" class="val">0</span></span>
  <span class="stat">Transmissions <span id="st-tx" class="val">0</span></span>
  <span id="status">connecting...</span>
</div>
<div id="tabs">
  <button id="tab-live" class="active" onclick="showTab('live')">Live</button>
  <button id="tab-activity" onclick="showTab('activity')">Activity</button>
  <button id="tab-topology" onclick="showTab('topology')">Topology</button>
  <button id="tab-sensors" onclick="showTab('sensors')">Sensors</button>
  <button id="tab-config" onclick="showTab('config')">Config</button>
</div>

<div id="live" class="tab active">
  <div class="placeholder">Live multi-station map landing in next commit.<br>
    For now, see the Sensors tab for registry management and Config for command fan-out.</div>
</div>

<div id="activity" class="tab">
  <div class="placeholder">Activity (per-preset, aggregated across stations) coming soon.</div>
</div>

<div id="topology" class="tab">
  <div class="placeholder">Topology (multi-station-observed mesh graph) coming soon.</div>
</div>

<div id="sensors" class="tab">
  <h2>Add sensor</h2>
  <div class="row">
    <input id="s-name" type="text" placeholder="name (e.g. basement-rx)">
    <input id="s-zmq" type="text" placeholder="zmq endpoint (e.g. tcp://10.0.0.5:7008)">
  </div>
  <div class="row">
    <input id="s-api" type="text" placeholder="api endpoint (e.g. http://10.0.0.5:8888) -- optional but required for C2 fan-out">
    <input id="s-token" type="text" placeholder="api bearer token (optional, --api-token on the sensor)">
  </div>
  <div class="row">
    <button class="primary" onclick="addSensor()">Add</button>
    <span id="add-status" class="hint"></span>
  </div>
  <h2>Registered sensors</h2>
  <table id="sensors-tbl">
    <thead><tr><th>Name</th><th>ZMQ</th><th>API</th><th>Auth</th><th></th></tr></thead>
    <tbody></tbody>
  </table>
</div>

<div id="config" class="tab">
  <h2>Add key (fan-out to all sensors)</h2>
  <div class="row">
    <textarea id="key-input" placeholder="ChannelName=SPEC, one per line. SPEC is default | simpleN | hex:HHHH... | base64:..."></textarea>
  </div>
  <div class="row">
    <button class="primary" onclick="fanout('keys','key-input')">Add to all sensors</button>
    <span class="hint">Same wire format as a per-sensor /api/keys add. Each sensor reports back independently.</span>
  </div>
  <div id="key-result" class="fanout-result" style="display:none"></div>

  <h2>Channel-share URL (fan-out)</h2>
  <div class="row">
    <input id="share-input" type="text" placeholder="https://meshtastic.org/e/#...">
    <button class="primary" onclick="fanout('share-url','share-input')">Add channel</button>
  </div>
  <div id="share-result" class="fanout-result" style="display:none"></div>

  <h2>Add extra frequency (fan-out)</h2>
  <div class="row">
    <input id="freq-input" type="text" placeholder="HZ:bw=BW:sf=SF:cr=CR (e.g. 915183000:bw=125000:sf=12:cr=8)">
    <button class="primary" onclick="fanout('extra-freq','freq-input')">Add slot</button>
  </div>
  <div id="freq-result" class="fanout-result" style="display:none"></div>

  <h2>CoT multicast (fan-out)</h2>
  <div class="row">
    <input id="cot-input" type="text" placeholder="239.2.3.1:6969 (empty = disable on every sensor)">
    <button class="primary" onclick="fanout('cot-multicast','cot-input')">Apply</button>
  </div>
  <div id="cot-result" class="fanout-result" style="display:none"></div>
</div>

<script>
function showTab(name){
  for(const t of ['live','activity','topology','sensors','config']){
    document.getElementById(t).classList.toggle('active',t===name);
    document.getElementById('tab-'+t).classList.toggle('active',t===name);
  }
  if(name==='sensors') refreshSensors();
}
function setStat(id,v){const el=document.getElementById(id);if(el)el.textContent=v;}
let nEvents=0,nTx=0;
const es=new EventSource('/events');
es.onopen=()=>{const s=document.getElementById('status');s.textContent='connected';s.style.color=''};
es.onerror=()=>{const s=document.getElementById('status');s.textContent='disconnected';s.style.color='#f87171'};
es.onmessage=(e)=>{
  let p; try{p=JSON.parse(e.data);}catch(_){return;}
  nEvents++; setStat('st-events',nEvents);
  if(p.event==='TX'){ nTx++; setStat('st-tx',nTx); }
};

async function refreshSensors(){
  try{
    const r=await fetch('/api/sensors');
    const list=await r.json();
    setStat('st-sensors', list.length);
    const tb=document.querySelector('#sensors-tbl tbody');
    tb.innerHTML='';
    for(const s of list){
      const tr=document.createElement('tr');
      tr.innerHTML='<td>'+escHtml(s.name)+'</td>'+
        '<td><code>'+escHtml(s.zmq)+'</code></td>'+
        '<td>'+(s.api?'<code>'+escHtml(s.api)+'</code>':'<span class=muted>--</span>')+'</td>'+
        '<td>'+(s.api_token?'<span class=status-ok>token set</span>':'<span class=muted>none</span>')+'</td>'+
        '<td><button class=danger onclick="removeSensor(\''+escHtml(s.name)+'\')">remove</button></td>';
      tb.appendChild(tr);
    }
    if(list.length===0){
      const tr=document.createElement('tr');
      tr.innerHTML='<td colspan=5 class="muted" style="text-align:center;padding:20px">no sensors registered. add one above, or pass tcp://... on the fusion command line.</td>';
      tb.appendChild(tr);
    }
  }catch(err){
    console.error('refreshSensors',err);
  }
}
async function addSensor(){
  const name=document.getElementById('s-name').value.trim();
  const zmq =document.getElementById('s-zmq').value.trim();
  const api =document.getElementById('s-api').value.trim();
  const tok =document.getElementById('s-token').value.trim();
  if(!name || !zmq){
    setStatus('add-status','name and zmq endpoint are required',false);
    return;
  }
  const body={name:name,zmq:zmq};
  if(api) body.api=api;
  if(tok) body.api_token=tok;
  try{
    const r=await fetch('/api/sensors',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    const j=await r.json();
    if(r.ok){
      setStatus('add-status','added '+j.added,true);
      document.getElementById('s-name').value='';
      document.getElementById('s-zmq').value='';
      document.getElementById('s-api').value='';
      document.getElementById('s-token').value='';
      refreshSensors();
    }else{
      setStatus('add-status',j.error||'error',false);
    }
  }catch(err){
    setStatus('add-status',String(err),false);
  }
}
async function removeSensor(name){
  try{
    const r=await fetch('/api/sensors/'+encodeURIComponent(name),{method:'DELETE'});
    if(r.ok) refreshSensors();
  }catch(_){}
}
async function fanout(endpoint,inputId){
  const body=document.getElementById(inputId).value.trim().replace(/\n/g,',');
  const out=document.getElementById(inputId.replace('-input','-result'));
  out.style.display='block';
  out.textContent='sending...';
  try{
    const r=await fetch('/api/fanout/'+endpoint,{method:'POST',body:body});
    const j=await r.json();
    out.textContent=JSON.stringify(j,null,2);
  }catch(err){
    out.textContent='error: '+String(err);
  }
}
function setStatus(id,msg,ok){
  const el=document.getElementById(id);
  el.textContent=msg;
  el.className='hint '+(ok?'status-ok':'status-err');
}
function escHtml(s){return String(s).replace(/[&<>"']/g, c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}

// Initial state
refreshSensors();
</script>
</body></html>`
