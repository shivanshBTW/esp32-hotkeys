#include "lumos/webui/web_ui.hpp"
#include "lumos/wifi/wifi_service.hpp"

namespace lumos {
namespace {

constexpr const char* kIndexHtml = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Hotkeys</title>
<style>
:root{--bg:#0e1116;--card:#171c24;--text:#e8edf5;--muted:#8b95a8;--accent:#6cb6ff;--line:#2a3340}
*{box-sizing:border-box}body{margin:0;font:15px/1.45 system-ui,sans-serif;background:var(--bg);color:var(--text)}
header{padding:1.25rem 1.25rem .5rem}h1{margin:0;font-size:1.4rem;letter-spacing:.04em}
p{color:var(--muted)}main{padding:0 1.25rem 2rem;display:grid;gap:1rem;max-width:720px}
section{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:1rem}
label{display:block;margin:.5rem 0 .25rem;color:var(--muted);font-size:.85rem}
input,select,button{width:100%;padding:.65rem .75rem;border-radius:8px;border:1px solid var(--line);background:#0f141b;color:var(--text)}
button{background:var(--accent);color:#041018;border:none;font-weight:600;margin-top:.75rem;cursor:pointer}
button.secondary{background:transparent;color:var(--accent);border:1px solid var(--accent)}
.row{display:grid;grid-template-columns:1fr auto;gap:.75rem;align-items:end}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:.75rem}
pre{white-space:pre-wrap;background:#0f141b;padding:.75rem;border-radius:8px;font-size:.8rem;color:var(--muted)}
.hint{font-size:.8rem;color:var(--muted);margin-top:.5rem}
.check{display:flex;align-items:center;gap:.5rem;margin:.75rem 0 .25rem;color:var(--text)}
.check input{width:auto}
#staticFields{display:none}
#staticFields.show{display:block}
.kv{display:grid;grid-template-columns:7.5rem 1fr;gap:.2rem .75rem;margin:0 0 .9rem;font-size:.9rem}
.kv dt{color:var(--muted);margin:0}
.kv dd{margin:0;word-break:break-all}
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
</style>
</head>
<body>
<header>
  <h1>Hotkeys</h1>
  <p>Recovery &amp; local configuration · API 0.3</p>
  <p><a href="/doorbell" style="color:var(--accent);font-weight:600;text-decoration:none">Doorbell →</a></p>
</header>
<main>
<section>
<h2>Status</h2>
<div id="pairCard" class="pair no"><div class="dot"></div><div><h3>Doorbell</h3><p>Loading pairing status…</p></div></div>
<pre id="status">Loading…</pre>
</section>
<section>
<h2>Hotkeys</h2>
<p class="hint">Keypad matrix and actions are not implemented yet. This slot is for the later firmware chat.</p>
<pre id="hotkeysStatus">{"enabled":false}</pre>
</section>
<section>
<h2>WiFi</h2>
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
<label>Hostname</label><input id="hostname" placeholder="Hotkeys"/>
<button class="secondary" type="button" onclick="saveHostname()" style="margin-top:.5rem">Save hostname</button>
<label>Nearby networks</label>
<div class="row">
  <select id="netlist"><option value="">Scanning…</option></select>
  <button class="secondary" type="button" onclick="scanWifi()" style="margin-top:0;width:auto;padding:.65rem 1rem">Scan</button>
</div>
<label>SSID</label><input id="ssid" placeholder="Select above or type manually"/>
<label>Password</label><input id="pass" type="password" placeholder="Leave blank to keep saved password"/>
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
<button onclick="saveWifi()">Save &amp; Connect</button>
<button id="retryWifiBtn" class="secondary" type="button" onclick="retryWifi()" style="display:none">Retry home Wi-Fi</button>
<p id="wifiHint" class="hint"></p>
</section>
<section>
<h2>Backup &amp; restore</h2>
<p class="hint">Download a JSON config from this device, then upload it on a new board after flashing the same firmware. Clones Wi‑Fi, doorbell pairing, and the hotkeys blob.</p>
<label class="check"><input id="cfgSecrets" type="checkbox"/> Include Wi‑Fi password in download</label>
<label class="check"><input id="cfgClearIp" type="checkbox" checked/> On import: clear static IP (use for a second device on the same LAN)</label>
<div class="grid2">
  <button type="button" onclick="downloadConfig()">Download config JSON</button>
  <button class="secondary" type="button" onclick="cfgFile.click()" style="margin-top:.75rem">Upload config JSON…</button>
</div>
<input id="cfgFile" type="file" accept="application/json,.json" style="display:none" onchange="uploadConfig(event)"/>
<pre id="cfgStatus"></pre>
</section>
<section>
<h2>Nearby devices</h2>
<pre id="neighbors">Loading…</pre>
<button class="secondary" type="button" onclick="loadNeighbors()">Refresh</button>
<p class="hint">Read-only mDNS discovery on <code>_hotkeys._tcp</code>. Open a peer by IP.</p>
</section>
<section>
<h2>OTA Update</h2>
<input id="firmware" type="file" accept=".bin"/>
<button onclick="uploadOta()">Upload Firmware</button>
<pre id="ota"></pre>
</section>
</main>
<script>
async function j(url,opts){const r=await fetch(url,opts); if(!r.ok) throw new Error(await r.text()); return r.json()}
function toggleStatic(){document.getElementById('staticFields').classList.toggle('show', useStatic.checked);}
function setPairCard(el,d,peer){
  if(!el) return;
  const pairing=!!(d&&(d.pairing||d.scanning));
  const mac=(peer||(d&&(d.paired_tx_mac||d.rx_mac))||'').trim();
  const paired=!!(d&&d.paired&&mac);
  el.className='pair '+(pairing?'wait':(paired?'ok':'no'));
  const title=pairing?'Pairing…':(paired?'Paired with doorbell TX':'Doorbell not paired');
  let detail;
  if(pairing) detail='ESP-NOW discovery is running. Being on the same Wi‑Fi is not enough.';
  else if(paired) detail='Transmitter '+mac+(d.enabled===false?' · receiver is disabled.':' · ESP-NOW link saved.');
  else detail='Same Wi‑Fi does not pair the boards. Open /doorbell → Start pairing, then Find nearby on LumosOS-Bell.';
  el.innerHTML='<div class="dot"></div><div><h3>'+title+'</h3><p>'+detail+'</p></div>';
}
function renderStatus(s){
  const view=Object.assign({},s); delete view.type;
  setPairCard(document.getElementById('pairCard'), s.doorbell||{}, (s.doorbell&&s.doorbell.paired_tx_mac)||'');
  status.textContent=JSON.stringify(view,null,2);
  if(s.hotkeys) hotkeysStatus.textContent=JSON.stringify(s.hotkeys,null,2);
  if(!ip.value && s.wifi && s.wifi.ip) ip.value=s.wifi.ip;
  if(!gateway.value && s.wifi && s.wifi.gateway) gateway.value=s.wifi.gateway;
  if((!netmask.value || netmask.value==='255.255.255.0') && s.wifi && s.wifi.netmask) netmask.value=s.wifi.netmask;
  if(!dns1.value && s.wifi && s.wifi.dns1) dns1.value=s.wifi.dns1;
  if(!dns2.value && s.wifi && s.wifi.dns2) dns2.value=s.wifi.dns2;
  updateWifiRetryUi(s.wifi);
  renderWifiNow(s.wifi);
}
function dash(v){ return (v==null || v==='')?'—':v; }
function renderWifiNow(w){
  if(!w) return;
  const setup=!!w.setup_mode;
  const connected=!!w.connected;
  document.getElementById('wifiSsid').textContent=dash(w.ssid);
  document.getElementById('wifiStatus').textContent=connected?'Connected':(setup?'Setup AP':'Disconnected');
  document.getElementById('wifiMode').textContent=connected?'sta':(setup?'ap':'—');
  document.getElementById('wifiIp').textContent=dash(w.ip);
  document.getElementById('wifiGw').textContent=dash(w.gateway);
  document.getElementById('wifiMask').textContent=dash(w.netmask);
  const dns=[w.dns1,w.dns2].filter(Boolean).join(', ');
  document.getElementById('wifiDns').textContent=dash(dns);
  document.getElementById('wifiMac').textContent=dash(w.mac);
}
function updateWifiRetryUi(w){
  const btn=document.getElementById('retryWifiBtn');
  if(!btn) return;
  btn.style.display=(w && w.setup_mode && w.has_saved_wifi)?'block':'none';
}
async function loadSettings(){
  try{
    const s=await j('/api/v1/settings');
    ssid.value=s.wifi_ssid||'';
    hostname.value=s.hostname||'';
    useStatic.checked=!!s.wifi_use_static;
    ip.value=s.wifi_ip||''; gateway.value=s.wifi_gateway||'';
    netmask.value=s.wifi_netmask||'255.255.255.0';
    dns1.value=s.wifi_dns1||''; dns2.value=s.wifi_dns2||'';
    toggleStatic();
  }catch{}
}
async function refresh(){
  try{ renderStatus(await j('/api/v1/status')); }
  catch(e){ status.textContent='Status unavailable: '+e.message; }
}
async function scanWifi(){
  netlist.innerHTML='<option value="">Scanning…</option>';
  try{
    const data=await j('/api/v1/wifi/scan');
    netlist.innerHTML='';
    const blank=document.createElement('option');
    blank.value=''; blank.textContent=data.networks.length?'Select a network…':'No networks found';
    netlist.appendChild(blank);
    for(const n of data.networks){
      const o=document.createElement('option'); o.value=n.ssid;
      o.textContent=n.ssid+'  ('+n.rssi+' dBm)'; netlist.appendChild(o);
    }
  }catch{ netlist.innerHTML='<option value="">Scan failed</option>'; }
}
netlist.addEventListener('change',e=>{ if(e.target.value) ssid.value=e.target.value; });
async function saveWifi(){
  const name=ssid.value.trim();
  if(!name){alert('Select or enter an SSID');return;}
  if(useStatic.checked && (!ip.value.trim()||!gateway.value.trim())){alert('Static IP needs IP and gateway');return;}
  try{
    await j('/api/v1/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({
      ssid:name,password:pass.value,use_static:useStatic.checked,
      ip:ip.value.trim(),gateway:gateway.value.trim(),netmask:netmask.value.trim()||'255.255.255.0',
      dns1:dns1.value.trim(),dns2:dns2.value.trim()
    })});
    alert('Connecting… then open '+(useStatic.checked?('http://'+ip.value.trim()):'http://hotkeys.local (or your hostname)'));
  }catch(e){ alert('Connect failed: '+e.message); }
}
async function saveHostname(){
  const name=hostname.value.trim();
  if(!name){alert('Enter a hostname');return;}
  try{
    await j('/api/v1/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({hostname:name})});
    alert('Hostname saved');
  }catch(e){ alert('Save failed: '+e.message); }
}
async function retryWifi(){
  const hint=document.getElementById('wifiHint');
  try{
    await j('/api/v1/wifi/retry',{method:'POST'});
    if(hint) hint.textContent='Retrying saved home Wi-Fi… this page stays up if it fails.';
  }catch(e){ if(hint) hint.textContent='Retry failed: '+e.message; }
}
function pulsePresence(){ fetch('/api/v1/wifi/presence',{method:'POST'}).catch(()=>{}); }
async function uploadOta(){
  const f=firmware.files[0]; if(!f){alert('Choose a .bin');return;}
  ota.textContent='Uploading…';
  const r=await fetch('/api/v1/ota',{method:'POST',body:f,headers:{'Content-Type':'application/octet-stream'}});
  ota.textContent=await r.text();
}
async function downloadConfig(){
  cfgStatus.textContent='Building config…';
  try{
    const url='/api/v1/config'+(cfgSecrets.checked?'?secrets=1':'');
    const r=await fetch(url);
    if(!r.ok) throw new Error(await r.text());
    const text=await r.text();
    const blob=new Blob([text],{type:'application/json'});
    const a=document.createElement('a');
    a.href=URL.createObjectURL(blob);
    a.download='hotkeys-config.json';
    a.click();
    URL.revokeObjectURL(a.href);
    cfgStatus.textContent='Downloaded hotkeys-config.json'+(cfgSecrets.checked?' (includes Wi‑Fi password — keep private)':'');
  }catch(e){ cfgStatus.textContent='Download failed: '+e.message; }
}
async function uploadConfig(ev){
  const f=ev.target.files&&ev.target.files[0];
  ev.target.value='';
  if(!f) return;
  cfgStatus.textContent='Reading '+f.name+'…';
  try{
    const text=await f.text();
    const parsed=JSON.parse(text);
    if(parsed&&parsed.device&&cfgClearIp.checked){
      parsed.clear_static_ip=true;
    } else if(parsed&&!parsed.device&&cfgClearIp.checked){
      parsed.wifi_use_static=false;
      parsed.wifi_ip='';
    }
    const r=await fetch('/api/v1/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(parsed)});
    const t=await r.text();
    if(!r.ok) throw new Error(t);
    if(t.indexOf('"reboot":true')>=0){
      cfgStatus.textContent='Config applied — rebooting… refresh shortly.';
      alert('Config applied — device rebooting. Refresh this page in a few seconds.');
      return;
    }
    cfgStatus.textContent='Config applied.';
    await loadSettings(); await refresh();
    alert('Config applied');
  }catch(e){ cfgStatus.textContent='Upload failed: '+e.message; alert('Upload failed: '+e.message); }
}
async function loadNeighbors(){
  try{
    neighbors.textContent='Scanning…';
    const data=await j('/api/v1/neighbors?refresh=1');
    const list=data.neighbors||[];
    if(!list.length){ neighbors.textContent='No other Hotkeys devices found on this LAN.'; return; }
    neighbors.textContent=list.map(n=>{
      const host=n.hostname||'device';
      const ip=n.ip||'?';
      const ver=n.version?(' v'+n.version):'';
      return host+ver+'\n  http://'+ip+(n.port&&n.port!==80?(':'+n.port):'');
    }).join('\n\n');
  }catch(e){ neighbors.textContent='Neighbors unavailable: '+e.message; }
}
loadSettings(); refresh(); scanWifi();
neighbors.textContent='Click Refresh to scan for nearby Hotkeys devices.';
pulsePresence();
setInterval(pulsePresence,3000);
setInterval(refresh,15000);
</script>
</body>
</html>
)HTML";

constexpr const char* kDoorbellHtml = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Hotkeys · Doorbell</title>
<style>
:root{--bg:#0e1116;--card:#171c24;--text:#e8edf5;--muted:#8b95a8;--accent:#6cb6ff;--line:#2a3340}
*{box-sizing:border-box}body{margin:0;font:15px/1.45 system-ui,sans-serif;background:var(--bg);color:var(--text)}
header{padding:1.25rem 1.25rem .5rem}h1{margin:0;font-size:1.4rem;letter-spacing:.04em}
p{color:var(--muted)}main{padding:0 1.25rem 2rem;display:grid;gap:1rem;max-width:720px}
section{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:1rem}
label{display:block;margin:.5rem 0 .25rem;color:var(--muted);font-size:.85rem}
input,select,button{width:100%;padding:.65rem .75rem;border-radius:8px;border:1px solid var(--line);background:#0f141b;color:var(--text)}
button{background:var(--accent);color:#041018;border:none;font-weight:600;margin-top:.75rem;cursor:pointer}
button.secondary{background:transparent;color:var(--accent);border:1px solid var(--accent)}
.hint{font-size:.8rem;color:var(--muted);margin-top:.5rem}
.check{display:flex;align-items:center;gap:.5rem;margin:.75rem 0 .25rem;color:var(--text)}
.check input{width:auto}
pre{white-space:pre-wrap;background:#0f141b;padding:.75rem;border-radius:8px;font-size:.8rem;color:var(--muted)}
a{color:var(--accent);font-weight:600;text-decoration:none}
.peer{display:block;text-align:left;margin-top:.5rem}
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
</style>
</head>
<body>
<header>
  <h1>Doorbell</h1>
  <p>ESP-NOW receiver · relay pulse for battery bell</p>
  <p><a href="/">← Hotkeys</a></p>
</header>
<main>
<section>
<h2>Pairing status</h2>
<div id="pairCard" class="pair no"><div class="dot"></div><div><h3>Doorbell</h3><p>Loading…</p></div></div>
<p class="hint">Same home Wi‑Fi is only for the web UI. Pairing is a separate ESP-NOW MAC link.</p>
</section>
<section>
<h2>Pair nearby transmitter</h2>
<p class="hint">On the doorbell TX, open the LumosOS-Bell page and tap <b>Find nearby</b> at the same time. Then tap the device here (or there). 60 seconds.</p>
<button type="button" onclick="startPair()">Start pairing</button>
<pre id="pairStatus">Idle</pre>
<div id="peers"></div>
</section>
<section>
<h2>Settings</h2>
<label class="check"><input id="enabled" type="checkbox"/> Enable doorbell receiver</label>
<label>Relay GPIO</label>
<input id="relayPin" type="number" min="4" max="33" value="17"/>
<p class="hint">Default 17 (safe on ESP32-WROOM). Avoid 0/2/5/6–12/15 and input-only 34–39.</p>
<label class="check"><input id="activeHigh" type="checkbox" checked/> Relay active-HIGH</label>
<p class="hint">Uncheck for active-LOW / low-level trigger modules.</p>
<label class="check"><input id="tone" type="checkbox"/> Buzzer tone (2.5 kHz PWM)</label>
<p class="hint">Required for a piezo/buzzer on GPIO. Leave unchecked if you use a relay.</p>
<label>Press duration (ms)</label>
<input id="pressMs" type="number" min="100" max="4000" value="1500"/>
<label>Paired transmitter MAC</label>
<input id="txMac" placeholder="AA:BB:CC:DD:EE:FF"/>
<p class="hint">Required if you skip pairing. Empty or invalid MAC = ignore all ESP-NOW doorbell packets.</p>
<button type="button" onclick="saveDoorbell()">Save</button>
<button class="secondary" type="button" onclick="testRelay()">Test relay pulse</button>
<pre id="msg"></pre>
</section>
<section>
<h2>Status</h2>
<pre id="status">Loading…</pre>
<button class="secondary" type="button" onclick="loadDoorbell()">Refresh</button>
</section>
</main>
<script>
let pairTimer=null;
function setPairCard(d){
  const el=document.getElementById('pairCard');
  const pairing=!!d.pairing;
  const mac=(d.paired_tx_mac||'').trim();
  const paired=!!d.paired && !!mac;
  el.className='pair '+(pairing?'wait':(paired?'ok':'no'));
  const title=pairing?'Pairing…':(paired?'Paired with transmitter':'Not paired');
  let detail;
  if(pairing) detail='Listening for LumosOS-Bell. Start Find nearby on the transmitter now.';
  else if(paired){
    const ring=d.last_ring_ms>0?' Last ring this boot at '+d.last_ring_ms+' ms.':' No ring yet this boot — use Test send on the transmitter.';
    detail='Transmitter '+mac+'.'+(d.enabled?' Receiver enabled.':' Receiver disabled.')+ring;
  } else detail='No transmitter MAC saved. Start pairing below, or paste the TX MAC in Settings.';
  el.innerHTML='<div class="dot"></div><div><h3>'+title+'</h3><p>'+detail+'</p></div>';
}
function renderPeers(d){
  const box=document.getElementById('peers');
  const list=d.peers||[];
  if(!list.length){
    box.innerHTML=d.pairing?'<p class="hint">Listening for LumosOS-Bell…</p>':'';
    return;
  }
  box.innerHTML=list.map(p=>'<button type="button" class="peer" onclick="selectPeer(\''+p.mac+'\')">'+
    (p.name||'Doorbell TX')+' · '+p.mac+' · ch '+p.channel+' · RSSI '+p.rssi+'</button>').join('');
}
async function loadDoorbell(){
  try{
    const r=await fetch('/api/v1/doorbell');
    const d=await r.json();
    enabled.checked=!!d.enabled;
    relayPin.value=d.relay_pin??17;
    activeHigh.checked=d.active_high!==false;
    tone.checked=!!d.tone;
    pressMs.value=d.press_ms??400;
    txMac.value=d.paired_tx_mac||'';
    setPairCard(d);
    status.textContent=[
      'own_mac: '+(d.own_mac||'—'),
      'wifi_channel: '+(d.wifi_channel??'—'),
      'enabled: '+!!d.enabled,
      'espnow_ready: '+!!d.espnow_ready,
      'paired: '+!!d.paired,
      'relay_pin: '+(d.relay_pin??'—'),
      'active_high: '+!!d.active_high,
      'tone: '+!!d.tone,
      'press_ms: '+(d.press_ms??'—'),
      'paired_tx_mac: '+(d.paired_tx_mac||'(none)'),
      'last_ring_ms: '+(d.last_ring_ms||0),
      'last_seq: '+(d.last_seq??0),
      'relay_active: '+!!d.relay_active
    ].join('\n');
    pairStatus.textContent=d.pairing?('Pairing… '+Math.round((d.pairing_ms||0)/1000)+'s left'):'Idle';
    renderPeers(d);
    if(d.pairing && !pairTimer){ pairTimer=setInterval(loadDoorbell,1000); }
    if(!d.pairing && pairTimer){ clearInterval(pairTimer); pairTimer=null; }
  }catch(e){ status.textContent='Error: '+e.message; }
}
async function startPair(){
  pairStatus.textContent='Starting…';
  try{
    await fetch('/api/v1/doorbell/pair/start',{method:'POST'});
    await loadDoorbell();
  }catch(e){ pairStatus.textContent='Error: '+e.message; }
}
async function selectPeer(mac){
  pairStatus.textContent='Pairing '+mac+'…';
  try{
    const r=await fetch('/api/v1/doorbell/pair',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mac})});
    const d=await r.json();
    pairStatus.textContent=d.ok?'Paired '+mac:'Error: '+(d.error||r.status);
    await loadDoorbell();
  }catch(e){ pairStatus.textContent='Error: '+e.message; }
}
async function saveDoorbell(){
  msg.textContent='Saving…';
  try{
    const body={
      enabled:enabled.checked,
      relay_pin:Number(relayPin.value)||17,
      active_high:activeHigh.checked,
      tone:tone.checked,
      press_ms:Number(pressMs.value)||1500,
      paired_tx_mac:(txMac.value||'').trim()
    };
    const r=await fetch('/api/v1/doorbell',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    const d=await r.json();
    msg.textContent=d.ok?'Saved.':'Error: '+(d.error||r.status);
    await loadDoorbell();
  }catch(e){ msg.textContent='Error: '+e.message; }
}
async function testRelay(){
  msg.textContent='Pulsing relay…';
  try{
    const r=await fetch('/api/v1/doorbell/test',{method:'POST'});
    const d=await r.json();
    msg.textContent=d.ok?'Pulse sent.':'Error: '+(d.error||r.status);
    await loadDoorbell();
  }catch(e){ msg.textContent='Error: '+e.message; }
}
loadDoorbell();
setInterval(loadDoorbell,10000);
fetch('/api/v1/wifi/presence',{method:'POST'}).catch(()=>{});
setInterval(()=>fetch('/api/v1/wifi/presence',{method:'POST'}).catch(()=>{}),3000);
</script>
</body>
</html>
)HTML";

} // namespace

esp_err_t WebUi::get_index(httpd_req_t* req) {
    WifiService::note_ui_activity_global();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebUi::get_doorbell(httpd_req_t* req) {
    WifiService::note_ui_activity_global();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kDoorbellHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebUi::get_android_probe(httpd_req_t* req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t WebUi::get_apple_probe(httpd_req_t* req) {
    static constexpr const char* kSuccess =
        "<!DOCTYPE HTML PUBLIC \"-//Apple//DTD HTML 3.2//EN\">"
        "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kSuccess, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebUi::get_windows_probe(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "Microsoft NCSI", HTTPD_RESP_USE_STRLEN);
}

Result<void> WebUi::start(httpd_handle_t server) {
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = get_index, .user_ctx = nullptr},
        {.uri = "/doorbell", .method = HTTP_GET, .handler = get_doorbell, .user_ctx = nullptr},
        {.uri = "/generate_204", .method = HTTP_GET, .handler = get_android_probe, .user_ctx = nullptr},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = get_apple_probe, .user_ctx = nullptr},
        {.uri = "/canonical.html", .method = HTTP_GET, .handler = get_apple_probe, .user_ctx = nullptr},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = get_windows_probe, .user_ctx = nullptr},
    };
    for (const auto& route : routes) {
        if (httpd_register_uri_handler(server, &route) != ESP_OK) {
            return Result<void>::fail(ErrorCode::IoError, "failed to register web UI route");
        }
    }
    return Result<void>::ok();
}

} // namespace lumos
