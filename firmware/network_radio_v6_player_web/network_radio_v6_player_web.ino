/* Network Radio V6: V5 playback with a browser player control console. */

#define NETWORK_RADIO_VERSION "0.6.0-player-web"
#define NETWORK_RADIO_NO_ENTRYPOINT
#include "../network_radio_v5_playback/network_radio_v5_playback.ino"
#undef NETWORK_RADIO_NO_ENTRYPOINT

namespace {

constexpr char kIndexHtmlV6[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>网络收音机 V6</title>
<style>:root{color-scheme:dark}body{max-width:850px;margin:24px auto;padding:0 16px;font:16px system-ui;background:#101827;color:#e5e7eb}section,pre,.station{background:#172234;padding:14px;border-radius:10px;margin:14px 0}button,input,select{box-sizing:border-box;padding:9px;margin:4px;border:0;border-radius:6px}input,select{width:100%}button{background:#38bdf8;color:#062032;font-weight:700}.warn{background:#fbbf24}.stop{background:#fb7185}.station small{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:#b7c6da}.active{outline:2px solid #38bdf8}.state{font-size:1.15em;color:#67e8f9}pre{overflow:auto}.volume{width:calc(100% - 10px)}</style>
<h1>ESP32-S3 网络收音机</h1><p>V6 · 网页播放器控制台</p>
<section><h2>正在播放</h2><div id="now" class="state">读取中…</div><button onclick="post('/api/player/play')">▶ 播放当前电台</button><button class="stop" onclick="post('/api/player/stop')">■ 停止</button><label>音量：<b id="vol">--</b>/21</label><input id="volume" class="volume" type="range" min="0" max="21" value="12" oninput="q('#vol').textContent=this.value" onchange="setVolume(this.value)"></section>
<section><h2>播放列表</h2><div id="stations">加载中…</div><h3 id="formTitle">新增电台</h3><input id="editId" type="hidden"><input id="name" placeholder="电台名称"><input id="url" placeholder="http(s):// 音频流地址"><button onclick="saveStation()">保存</button><button class="warn" onclick="clearForm()">取消编辑</button></section>
<section><h2>Wi-Fi</h2><button onclick="scanWifi()">扫描网络</button><select id="ssid"><option value="">选择 Wi-Fi</option></select><input id="pass" type="password" placeholder="Wi-Fi 密码"><button onclick="saveWifi()">保存并连接</button><button class="warn" onclick="forgetWifi()">清除 Wi-Fi 设置</button></section><pre id="status">读取中…</pre>
<script>const q=s=>document.querySelector(s),enc=o=>new URLSearchParams(o);async function api(u,o){let r=await fetch(u,o),j=await r.json();if(!r.ok)throw Error(j.error||'请求失败');return j}async function post(u){try{await api(u,{method:'POST'});refresh()}catch(e){alert(e)}}async function setVolume(v){try{await api('/api/player/volume?value='+v,{method:'POST'});refresh()}catch(e){alert(e)}}async function refresh(){try{let[s,p,x]=await Promise.all([api('/api/status'),api('/api/stations'),api('/api/player/status')]);q('#status').textContent=JSON.stringify(s,null,2);q('#now').textContent=`${x.state} · ${s.player.selected_name||'未选择'} · ${x.message||''}`;q('#volume').value=x.volume;q('#vol').textContent=x.volume;render(p)}catch(e){q('#status').textContent='错误：'+e}}function render(p){q('#stations').innerHTML=p.stations.map(x=>`<div class="station ${x.id==p.selected?'active':''}"><b>${x.name}</b><small>${x.url}</small><button onclick="post('/api/stations/select?id=${x.id}')">播放此台</button><button onclick="edit(${x.id})">编辑</button><button onclick="post('/api/stations/move?id=${x.id}&direction=up')">↑</button><button onclick="post('/api/stations/move?id=${x.id}&direction=down')">↓</button><button class="warn" onclick="removeStation(${x.id})">删除</button></div>`).join('')}async function saveStation(){let id=q('#editId').value,body=enc({name:q('#name').value,url:q('#url').value});try{await api(id===''?'/api/stations':'/api/stations/update?id='+id,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});clearForm();refresh()}catch(e){alert(e)}}async function edit(id){let p=await api('/api/stations'),x=p.stations[id];q('#editId').value=id;q('#name').value=x.name;q('#url').value=x.url;q('#formTitle').textContent='编辑电台'}function clearForm(){q('#editId').value='';q('#name').value='';q('#url').value='';q('#formTitle').textContent='新增电台'}function removeStation(id){if(confirm('删除该电台？'))post('/api/stations/delete?id='+id)}async function scanWifi(){let d=await api('/api/wifi/scan'),s=q('#ssid');s.innerHTML='<option value="">选择 Wi-Fi</option>';d.networks.forEach(n=>s.innerHTML+=`<option value="${n.ssid}">${n.ssid} (${n.rssi} dBm)</option>`)}async function saveWifi(){try{await api('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:enc({ssid:q('#ssid').value,password:q('#pass').value})});q('#status').textContent='Wi-Fi 已保存，设备正在重启…'}catch(e){alert(e)}}async function forgetWifi(){if(confirm('清除保存的 Wi-Fi？')){await api('/api/wifi/forget',{method:'POST'});q('#status').textContent='Wi-Fi 已清除，设备正在重启…'}}refresh();setInterval(refresh,2000)</script></html>
)HTML";

void configureWebServerV6() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", kIndexHtmlV6); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi", HTTP_POST, handleSaveWifi);
  server.on("/api/wifi/forget", HTTP_POST, handleForgetWifi);
  server.on("/api/stations", HTTP_GET, [] { sendJson(playlistJson()); });
  server.on("/api/stations", HTTP_POST, handleAddStation);
  server.on("/api/stations/update", HTTP_POST, handleUpdateStation);
  server.on("/api/stations/delete", HTTP_POST, handleDeleteStation);
  server.on("/api/stations/select", HTTP_POST, handleSelectStation);
  server.on("/api/stations/move", HTTP_POST, handleMoveStation);
  registerPlayerRoutes();
  server.onNotFound([] { server.sendHeader("Location", "/"); server.send(302, "text/plain", "Redirecting"); });
  server.begin();
}

}  // namespace

void setup() {
  Serial.begin(config::kSerialBaud);
  delay(300);
  snprintf(accessPointSsid, sizeof(accessPointSsid), "%s%04X", config::kAccessPointPrefix, setupAccessPointId());
  loadPlaylist();
  playerPreferences.begin("player", true);
  playerVolume = playerPreferences.getUChar("volume", playerVolume);
  playerPreferences.end();
  Audio::audio_info_callback = audioInfo;
  audio.settings.BUFFER_TRESHOLD_HLS = 32 * 1024;
  audio.setPinout(config::kI2sBclk, config::kI2sLrclk, config::kI2sDataOut);
  audio.setVolume(playerVolume);
  const bool connected = connectSavedStation();
  if (!connected || config::kKeepSetupAccessPointAvailable) startAccessPoint();
  onStationSelected = onPlaylistSelection;
  configureWebServerV6();
  if (connected) startSelectedStation();
  Serial.printf("Network Radio %s ready; web player enabled.\n", config::kFirmwareVersion);
}

void loop() {
  if (accessPointRunning) dnsServer.processNextRequest();
  server.handleClient();
  maintainStationConnection();
  audio.loop();
}
