#pragma once

// Shared doorbell TX web UI. ESP32 IDF and ESP8266 Arduino include this file.
// Keep the markup identical so preferences, routes, and config JSON stay in lockstep.
// Must be a real C++ string (not a #define): GCC 10's preprocessor cannot host a
// multiline raw string inside a macro, which breaks the ESP8266 Arduino build.
#ifdef ARDUINO_ARCH_ESP8266
#include <pgmspace.h>
#define LUMOS_DBTX_PROGMEM PROGMEM
#else
#define LUMOS_DBTX_PROGMEM
#endif

static const char LUMOS_DOORBELL_TX_INDEX_HTML[] LUMOS_DBTX_PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8"/><meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Doorbell TX</title>
<style>
:root{--bg:#0e1116;--card:#171c24;--text:#e8edf5;--muted:#8b95a8;--accent:#6cb6ff;--line:#2a3340}
*{box-sizing:border-box}body{margin:0;font:15px/1.45 system-ui,sans-serif;background:var(--bg);color:var(--text)}
header{padding:1.25rem}h1{margin:0;font-size:1.3rem}
p,label{color:var(--muted)}main{padding:0 1.25rem 2rem;max-width:520px}
section{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:1rem;margin-bottom:1rem}
label{display:block;margin:.5rem 0 .25rem;font-size:.85rem}
input,select,button{width:100%;padding:.65rem .75rem;border-radius:8px;border:1px solid var(--line);background:#0f141b;color:var(--text)}
button{background:var(--accent);color:#041018;border:none;font-weight:600;margin-top:.75rem;cursor:pointer}
button.secondary{background:transparent;color:var(--accent);border:1px solid var(--accent)}
.check{display:flex;align-items:center;gap:.5rem;color:var(--text);margin:.6rem 0}
.check input{width:auto}
.hint{font-size:.8rem;color:var(--muted)}
.kv{display:grid;grid-template-columns:7.5rem 1fr;gap:.2rem .75rem;margin:0 0 .9rem;font-size:.9rem}
.kv dt{color:var(--muted);margin:0}
.kv dd{margin:0;word-break:break-all;color:var(--text)}
.row{display:grid;grid-template-columns:1fr auto;gap:.75rem;align-items:end}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:.75rem}
pre{white-space:pre-wrap;background:#0f141b;padding:.75rem;border-radius:8px;font-size:.8rem}
.pair{display:flex;gap:.75rem;align-items:flex-start;padding:.9rem;border-radius:10px;border:1px solid var(--line);margin:0 0 .75rem}
.pair .dot{width:.7rem;height:.7rem;border-radius:50%;margin-top:.28rem;flex:0 0 auto}
.pair h3{margin:0;font-size:1.02rem;color:var(--text)}
.pair p{margin:.25rem 0 0;font-size:.85rem}
.pair.ok{border-color:#2f6f52;background:#102018}
.pair.ok .dot{background:#3ecf8e}
.pair.no{border-color:#7a5a28;background:#1c160e}
.pair.no .dot{background:#e0a04a}
.pair.wait{border-color:#2a5a7a;background:#101820}
.pair.wait .dot{background:#6cb6ff}
#staticFields{display:none}#staticFields.show{display:block}
</style></head>
<body>
<header><h1>Doorbell transmitter</h1><p>ESP-NOW · join home Wi-Fi like LumosOS</p></header>
<main>
<section>
<h2>Status</h2>
<div id="pairCard" class="pair no"><div class="dot"></div><div><h3>LED board</h3><p>Loading pairing status…</p></div></div>
<pre id="status">Loading…</pre>
</section>
<section>
<h2>Wi-Fi</h2>
<dl id="wifiNow" class="kv">
  <dt>SSID</dt><dd id="wifiSsid">—</dd>
  <dt>Status</dt><dd id="wifiStatus">—</dd>
  <dt>Mode</dt><dd id="wifiMode">—</dd>
  <dt>Current IP</dt><dd id="wifiIp">—</dd>
  <dt>Gateway</dt><dd id="wifiGw">—</dd>
  <dt>Netmask</dt><dd id="wifiMask">—</dd>
  <dt>DNS</dt><dd id="wifiDns">—</dd>
  <dt>ESP MAC</dt><dd id="wifiMac">—</dd>
</dl>
<p class="hint">Connect this bell to the same router as the LED board. After that, use <b>http://lumosos-bell.local</b> (or the static IP) from your phone — no hotspot needed.</p>
<label>Nearby networks</label>
<div class="row">
  <select id="netlist"><option value="">Scan to list…</option></select>
  <button class="secondary" type="button" onclick="scanWifi()" style="margin-top:0;width:auto;padding:.65rem 1rem">Scan</button>
</div>
<label>SSID</label><input id="ssid" placeholder="Select above or type"/>
<label>Password</label><input id="pass" type="password" placeholder="Leave blank to keep saved"/>
<label>Hostname</label><input id="hostname" placeholder="LumosOS-Bell"/>
<label class="check"><input id="useStatic" type="checkbox" onchange="toggleStatic()"/> Use static IP</label>
<div id="staticFields">
  <div class="grid2">
    <div><label>IP address</label><input id="ip" placeholder="192.168.1.50"/></div>
    <div><label>Gateway</label><input id="gateway" placeholder="192.168.1.1"/></div>
  </div>
  <label>Subnet mask</label><input id="netmask" placeholder="255.255.255.0"/>
  <div class="grid2">
    <div><label>DNS 1</label><input id="dns1"/></div>
    <div><label>DNS 2</label><input id="dns2"/></div>
  </div>
</div>
<button type="button" onclick="saveWifi()">Save &amp; Connect</button>
<button id="retryWifiBtn" class="secondary" type="button" onclick="retryWifi()" style="display:none">Retry home Wi-Fi</button>
<button class="secondary" type="button" onclick="forgetWifi()">Forget Wi-Fi (setup hotspot)</button>
<pre id="wifiMsg"></pre>
</section>
<section>
<h2>Pair LED board</h2>
<p class="hint">On LumosOS open <b>/doorbell</b> → Start pairing, then Find nearby here.</p>
<label>Optocoupler pin</label>
<select id="pin" onchange="updateWireHint()"></select>
<p class="hint" id="wireHint">Collector to D2, emitter to GND.</p>
<pre id="pairBits"></pre>
<button type="button" onclick="savePin()">Save pin</button>
<button class="secondary" type="button" onclick="testSend()">Test send</button>
<button class="secondary" type="button" onclick="findNearby()">Find nearby LED board</button>
<pre id="msg"></pre>
<div id="peers"></div>
</section>
<section>
<h2>Backup &amp; restore</h2>
<p class="hint">Clone pairing + Wi-Fi to another bell ESP32 after flashing the same firmware.</p>
<label class="check"><input id="cfgSecrets" type="checkbox"/> Include Wi-Fi password in download</label>
<label class="check"><input id="cfgClearIp" type="checkbox" checked/> On import: clear static IP</label>
<button type="button" onclick="downloadConfig()">Download config JSON</button>
<button class="secondary" type="button" onclick="cfgFile.click()">Upload config JSON…</button>
<input id="cfgFile" type="file" accept="application/json,.json" style="display:none" onchange="uploadConfig(event)"/>
<pre id="cfgStatus"></pre>
</section>
<section>
<h2>OTA update</h2>
<input id="firmware" type="file" accept=".bin"/>
<button type="button" onclick="uploadOta()">Upload firmware</button>
<pre id="ota"></pre>
</section>
</main>
<script>
let pairTimer=null;
const PINS_8266=[{g:4,n:'D2'},{g:5,n:'D1'},{g:14,n:'D5'},{g:12,n:'D6'},{g:13,n:'D7'}];
const PINS_ESP=[{g:4,n:'GPIO 4'},{g:5,n:'GPIO 5'},{g:12,n:'GPIO 12'},{g:13,n:'GPIO 13'},{g:14,n:'GPIO 14'}];
function pinList(board){return board==='esp8266'?PINS_8266:PINS_ESP;}
function fillPins(board,current){
  const list=pinList(board);
  pin.innerHTML=list.map(p=>'<option value="'+p.g+'">'+p.n+'</option>').join('');
  pin.value=String(current||4);
  if(![].some.call(pin.options,o=>o.value===pin.value)) pin.value='4';
  updateWireHint();
}
function updateWireHint(level){
  const n=pin.selectedOptions[0]?pin.selectedOptions[0].textContent:'D2';
  let live='';
  if(level===0) live=' Right now '+n+' is LOW (that should ring).';
  else if(level===1) live=' Right now '+n+' is HIGH (idle).';
  wireHint.textContent='Collector to '+n+', emitter to GND.'+live;
}
function toggleStatic(){staticFields.classList.toggle('show', useStatic.checked);}
function setPairCard(d){
  const el=document.getElementById('pairCard');
  const pairing=!!(d.pairing||d.scanning);
  const mac=(d.rx_mac||'').trim();
  const paired=!!d.paired && !!mac;
  el.className='pair '+(pairing?'wait':(paired?'ok':'no'));
  const title=pairing?'Pairing…':(paired?'Paired with LED board':'Not paired');
  let detail;
  if(pairing) detail='Searching for LumosOS. Tap Start pairing on the LED board /doorbell page.';
  else if(paired){
    const sent=d.last_send_ms>0?' Last ESP-NOW send this boot at '+d.last_send_ms+' ms.':' No press sent yet this boot — use Test send.';
    detail='Receiver '+mac+'.'+sent;
  } else detail='Same Wi‑Fi is not pairing. Start pairing on LumosOS /doorbell, then Find nearby here.';
  el.innerHTML='<div class="dot"></div><div><h3>'+title+'</h3><p>'+detail+'</p></div>';
}
function renderPeers(d){
  const box=document.getElementById('peers');
  const list=d.peers||[];
  if(!list.length){
    box.innerHTML=d.scanning?'<p class="hint">Searching…</p>':
      (d.pairing?'<p class="hint">No LumosOS receiver heard. Start pairing on /doorbell first.</p>':'');
    return;
  }
  box.innerHTML=list.map(p=>'<button type="button" class="secondary" onclick="pick(\''+p.mac+'\')">'+
    (p.name||'LumosOS')+' · '+p.mac+' · ch '+p.channel+' · RSSI '+p.rssi+'</button>').join('');
}
async function load(){
  const r=await fetch('/api'); const d=await r.json();
  fillPins(d.board, d.opto_pin||4);
  updateWireHint(d.opto_level);
  ssid.value=d.wifi_ssid||ssid.value||'';
  hostname.value=d.hostname||'LumosOS-Bell';
  useStatic.checked=!!d.wifi_use_static; toggleStatic();
  ip.value=d.wifi_ip||''; gateway.value=d.wifi_gateway||'';
  netmask.value=d.wifi_netmask||'255.255.255.0';
  dns1.value=d.wifi_dns1||''; dns2.value=d.wifi_dns2||'';
  setPairCard(d);
  const retryBtn=document.getElementById('retryWifiBtn');
  if(retryBtn) retryBtn.style.display=(d.setup_mode && d.has_saved_wifi)?'block':'none';
  const dash=v=>(v==null||v==='')?'—':v;
  const connected=!!d.wifi_connected;
  const setup=!!d.setup_mode;
  document.getElementById('wifiSsid').textContent=dash(connected?d.wifi_ssid:(setup?'LumosOS-Bell':d.wifi_ssid));
  document.getElementById('wifiStatus').textContent=connected?'Connected':(setup?'Setup AP':'Disconnected');
  document.getElementById('wifiMode').textContent=connected?'sta':(setup?'ap':'—');
  document.getElementById('wifiIp').textContent=dash(connected?d.sta_ip:(setup?(d.ap_ip||'192.168.4.1'):''));
  document.getElementById('wifiGw').textContent=dash(d.sta_gateway);
  document.getElementById('wifiMask').textContent=dash(d.sta_netmask);
  document.getElementById('wifiDns').textContent=dash(d.sta_dns);
  document.getElementById('wifiMac').textContent=dash(d.own_mac);
  status.textContent=[
    'this_mac: '+(d.own_mac||'—'),
    'wifi: '+(d.wifi_connected?(d.wifi_ssid+'  '+d.sta_ip):('setup AP '+ (d.ap_ip||'192.168.4.1'))),
    'hostname: '+(d.hostname||'—')+(d.wifi_connected?'  → http://lumosos-bell.local':''),
    'espnow: '+!!d.espnow_ready+'  paired: '+!!d.paired+'  if: '+(d.sta_linked?'STA':'AP'),
    'channel: '+(d.channel||'—'),
    'opto: '+(d.opto_level===0?'LOW':(d.opto_level===1?'HIGH':'—'))+'  last_seq: '+(d.last_seq||0)
  ].join('\n');
  pairBits.textContent='scanning: '+!!d.scanning+'  pairing: '+!!d.pairing;
  renderPeers(d);
  if((d.pairing||d.scanning) && !pairTimer){ pairTimer=setInterval(load,1000); }
  if(!d.pairing && !d.scanning && pairTimer){ clearInterval(pairTimer); pairTimer=null; }
}
async function scanWifi(){
  netlist.innerHTML='<option>Scanning…</option>';
  try{
    const data=await (await fetch('/api/v1/wifi/scan')).json();
    netlist.innerHTML='';
    const blank=document.createElement('option');
    blank.value=''; blank.textContent=(data.networks&&data.networks.length)?'Select a network…':'No networks found';
    netlist.appendChild(blank);
    for(const n of (data.networks||[])){
      const o=document.createElement('option'); o.value=n.ssid;
      o.textContent=n.ssid+'  ('+n.rssi+' dBm, ch '+n.channel+')'; netlist.appendChild(o);
    }
  }catch{ netlist.innerHTML='<option>Scan failed</option>'; }
}
netlist.addEventListener('change',e=>{ if(e.target.value) ssid.value=e.target.value; });
async function saveWifi(){
  const name=ssid.value.trim();
  if(!name){alert('Select or enter an SSID');return;}
  if(useStatic.checked && (!ip.value.trim()||!gateway.value.trim())){alert('Static IP needs IP and gateway');return;}
  wifiMsg.textContent='Connecting…';
  try{
    const r=await fetch('/api/v1/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({
      ssid:name,password:pass.value,hostname:hostname.value.trim(),
      use_static:useStatic.checked,ip:ip.value.trim(),gateway:gateway.value.trim(),
      netmask:netmask.value.trim()||'255.255.255.0',dns1:dns1.value.trim(),dns2:dns2.value.trim()
    })});
    const t=await r.text();
    wifiMsg.textContent=t;
    alert('Connecting… then open http://lumosos-bell.local or the static IP. Setup hotspot will go away.');
  }catch(e){ wifiMsg.textContent='Failed: '+e.message; }
}
async function retryWifi(){
  wifiMsg.textContent='Retrying saved home Wi-Fi…';
  try{
    const r=await fetch('/api/v1/wifi/retry',{method:'POST'});
    const t=await r.text();
    wifiMsg.textContent=r.ok?'Retrying saved home Wi-Fi… this page stays up if it fails.':t;
  }catch(e){ wifiMsg.textContent='Retry failed: '+e.message; }
}
function pulsePresence(){ fetch('/api/v1/wifi/presence',{method:'POST'}).catch(()=>{}); }
async function forgetWifi(){
  if(!confirm('Drop saved Wi-Fi and reopen LumosOS-Bell hotspot?')) return;
  await fetch('/api/v1/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({forget:true})});
  wifiMsg.textContent='Forgetting… device reboots to setup AP.';
}
async function savePin(){
  msg.textContent='Saving…';
  const body=new URLSearchParams({pin:String(pin.value), active_low:'1'});
  const r=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
  msg.textContent=await r.text(); await load();
}
async function testSend(){
  msg.textContent='Sending…';
  const r=await fetch('/test',{method:'POST'}); msg.textContent=await r.text(); await load();
}
async function findNearby(){
  msg.textContent='Searching…';
  try{ await fetch('/discover',{method:'POST'}); await load(); }
  catch(e){ msg.textContent='Search started. Rejoin if the setup hotspot dropped.'; }
}
async function pick(mac){
  msg.textContent='Pairing '+mac+'…';
  const r=await fetch('/pair',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'rx_mac='+encodeURIComponent(mac)});
  msg.textContent=await r.text(); await load();
}
async function downloadConfig(){
  cfgStatus.textContent='Building…';
  try{
    const r=await fetch('/api/v1/config'+(cfgSecrets.checked?'?secrets=1':''));
    if(!r.ok) throw new Error(await r.text());
    const text=await r.text();
    const a=document.createElement('a');
    a.href=URL.createObjectURL(new Blob([text],{type:'application/json'}));
    a.download='lumosos-bell-config.json'; a.click();
    cfgStatus.textContent='Downloaded lumosos-bell-config.json';
  }catch(e){ cfgStatus.textContent='Download failed: '+e.message; }
}
async function uploadConfig(ev){
  const f=ev.target.files&&ev.target.files[0]; ev.target.value='';
  if(!f) return;
  try{
    const parsed=JSON.parse(await f.text());
    if(cfgClearIp.checked){ parsed.clear_static_ip=true; }
    const r=await fetch('/api/v1/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(parsed)});
    const t=await r.text();
    if(!r.ok) throw new Error(t);
    cfgStatus.textContent=t.indexOf('reboot')>=0?'Applied — rebooting…':t;
  }catch(e){ cfgStatus.textContent='Upload failed: '+e.message; }
}
async function uploadOta(){
  const f=firmware.files&&firmware.files[0];
  if(!f){ota.textContent='Pick a .bin first';return;}
  ota.textContent='Uploading…';
  const fd=new FormData(); fd.append('firmware',f);
  const r=await fetch('/api/v1/ota',{method:'POST',body:fd});
  ota.textContent=await r.text();
}
load(); scanWifi();
pulsePresence();
setInterval(pulsePresence,3000);
setInterval(load,800);
</script>
</body></html>
)HTML";
