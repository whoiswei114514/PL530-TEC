#pragma once

#include <Arduino.h>

const char WEB_UI_PAGE[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PL530 TEC 温控</title>
<style>
:root{--bg:#101315;--panel:#181d21;--panel2:#20272c;--line:#2d363d;--text:#edf2f4;--muted:#92a0aa;--teal:#20c9a6;--red:#ff5f5f;--amber:#f2b04a;--blue:#4fa9ff}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;letter-spacing:0}
body{min-height:100vh;padding:22px}
.wrap{max-width:1180px;margin:0 auto}
.topbar{display:flex;align-items:center;justify-content:space-between;gap:16px;margin-bottom:18px}
.brand{display:flex;align-items:center;gap:10px;font-weight:650}
.brand-mark{width:10px;height:10px;border-radius:2px;background:var(--teal);box-shadow:0 0 16px rgba(32,201,166,.5)}
.subtitle{color:var(--muted);font-size:13px}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:16px}
.panel h2{margin:0 0 13px;font-size:14px;font-weight:650;color:var(--muted);text-transform:uppercase}
.layout{display:grid;grid-template-columns:minmax(260px,1.1fr) minmax(330px,1.5fr);gap:14px}
.temp-readout{display:flex;align-items:end;justify-content:space-between;gap:16px}
.temp-value{font-size:64px;line-height:1;font-weight:700;font-variant-numeric:tabular-nums}
.temp-unit{color:var(--muted);font-size:18px;margin-left:6px}
.state{display:inline-flex;align-items:center;gap:8px;padding:8px 10px;border:1px solid var(--line);border-radius:6px;color:var(--muted);font-size:13px;max-width:100%;overflow-wrap:anywhere}
.state::before{content:"";width:8px;height:8px;border-radius:50%;background:var(--muted)}
.state.ok{color:#b9eadc;border-color:rgba(32,201,166,.35)}
.state.ok::before{background:var(--teal);box-shadow:0 0 12px var(--teal)}
.state.warn{color:#ffe0b0;border-color:rgba(242,176,74,.4)}
.state.warn::before{background:var(--amber);box-shadow:0 0 12px var(--amber)}
.state.bad{color:#ffd2d2;border-color:rgba(255,95,95,.4)}
.state.bad::before{background:var(--red);box-shadow:0 0 12px var(--red)}
.metrics{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:10px;margin-top:14px}
.metric{background:var(--panel2);border:1px solid var(--line);border-radius:6px;padding:11px 12px;min-width:0}
.metric label{display:block;color:var(--muted);font-size:11px;margin-bottom:4px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.metric b{font-size:17px;font-variant-numeric:tabular-nums;overflow:hidden;text-overflow:ellipsis;display:block}
.form-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:11px}
.field label{display:block;color:var(--muted);font-size:12px;margin-bottom:6px}
.field input[type=number],.field input[type=text],.field input[type=password],select{width:100%;height:39px;color:var(--text);background:#0f1315;border:1px solid var(--line);border-radius:6px;padding:0 10px;font:inherit}
input:focus,select:focus{outline:1px solid var(--teal)}
.actions{display:flex;align-items:center;gap:9px;margin-top:13px;flex-wrap:wrap}
button{min-height:36px;border:1px solid var(--line);border-radius:6px;background:var(--panel2);color:var(--text);padding:0 13px;cursor:pointer;font:inherit}
button:hover{border-color:var(--muted)}
button.primary{background:var(--teal);border-color:var(--teal);color:#06110e;font-weight:650}
button.danger{color:#ffb8b8;border-color:rgba(255,95,95,.45)}
.switch{display:inline-flex;align-items:center;gap:9px;color:var(--muted);font-size:13px;cursor:pointer;user-select:none}
.switch input{width:34px;height:19px;accent-color:var(--teal)}
.wifi-head{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:10px}
.wifi-status{font-size:13px;color:var(--muted);overflow-wrap:anywhere;min-width:0}
.wifi-form{display:grid;grid-template-columns:minmax(160px,1fr) minmax(130px,1fr) auto auto;gap:9px;align-items:end}
.message{display:none;margin:12px 0;padding:10px 12px;border-radius:6px;border:1px solid var(--line);font-size:13px}
.message.show{display:block}
.message.info{color:#d6eaff;border-color:rgba(79,169,255,.4);background:rgba(79,169,255,.08)}
.message.ok{color:#b9eadc;border-color:rgba(32,201,166,.4);background:rgba(32,201,166,.08)}
.message.bad{color:#ffd2d2;border-color:rgba(255,95,95,.4);background:rgba(255,95,95,.08)}
.notice{margin:0 0 12px;padding:10px 12px;border-left:3px solid var(--red);color:#ffd2d2;overflow-wrap:anywhere}
.notice[hidden]{display:none}
button:disabled{opacity:.55;cursor:wait}
.calibration{border-top:1px solid var(--line);margin-top:16px;padding-top:14px}
.calibration-head{display:flex;justify-content:space-between;gap:12px;align-items:center;flex-wrap:wrap}
.calibration p{margin:8px 0 0;color:var(--muted);font-size:13px;overflow-wrap:anywhere}
.calibration progress{display:block;width:100%;height:8px;margin-top:10px;accent-color:var(--amber)}
.calibration progress[hidden]{display:none}
.safety{margin-top:14px;color:var(--muted);font-size:12px;text-align:right}
@media(max-width:820px){.layout{grid-template-columns:1fr}.metrics{grid-template-columns:repeat(2,minmax(0,1fr))}.form-grid{grid-template-columns:1fr}.wifi-form{grid-template-columns:1fr 1fr;grid-auto-rows:auto}.wifi-form button:last-child{grid-column:1/-1}}
@media(max-width:480px){body{padding:12px}.temp-readout{align-items:start;flex-direction:column}.temp-value{font-size:54px}.wifi-form{grid-template-columns:1fr}.topbar{flex-wrap:wrap}}
</style>
</head>
<body>
<div class="wrap">
  <header class="topbar">
    <div class="brand"><span class="brand-mark"></span><span>PL530 TEC 温控</span></div>
    <div id="clock" class="subtitle"></div>
  </header>
  <div id="message" class="message"></div>
  <div id="faultNotice" class="notice" role="alert" hidden></div>
  <div id="configurationError" class="notice" role="status" hidden></div>
  <main class="layout">
    <section class="panel">
      <h2>当前状态</h2>
      <div class="temp-readout">
        <div><div class="temp-value"><span id="temp">--</span><span class="temp-unit">°C</span></div></div>
        <div id="state" class="state">等待状态</div>
      </div>
      <div class="metrics">
        <div class="metric"><label>制冷</label><b id="cooling">--</b></div>
        <div class="metric"><label>加热</label><b id="heating">--</b></div>
        <div class="metric"><label>变化率</label><b id="slope">--</b></div>
        <div class="metric"><label>平衡目标</label><b id="balance">--</b></div>
      </div>
    </section>
    <section class="panel">
      <h2>控制参数</h2>
      <form id="settingsForm" class="form-grid">
        <div class="field"><label for="target">目标温度 °C</label><input id="target" name="target" type="number" min="10.2" max="54.8" step="0.01" required></div>
        <div class="field"><label for="high">PWM 高温 °C</label><input id="high" name="high" type="number" min="10.4" max="55" step="0.01" required></div>
        <div class="field"><label for="low">PWM 低温 °C</label><input id="low" name="low" type="number" min="10" max="54.6" step="0.01" required></div>
        <label class="switch" title="启用后 TEC 输出才允许打开"><input id="enabled" name="enabled" type="checkbox"><span>启用温控</span></label>
      </form>
      <div class="actions">
        <button class="primary" type="submit" form="settingsForm">保存参数</button>
        <button id="resetFault" class="danger" type="button">解除故障锁定</button>
        <button id="stopOutput" class="danger" type="button">停止 TEC</button>
      </div>
      <div class="calibration">
        <div class="calibration-head">
          <span id="directionStatus">TEC 方向未确认</span>
          <button id="detectDirection" type="button" disabled>检测 TEC 方向</button>
        </div>
        <p id="calibrationStatus" role="status">等待设备状态</p>
        <progress id="calibrationProgress" value="0" max="3000" hidden></progress>
        <p id="calibrationDeltas" hidden></p>
      </div>
    </section>
    <section class="panel" style="grid-column:1/-1">
      <h2>WiFi 配网</h2>
      <div class="wifi-head"><span class="wifi-status" id="wifiStatus">AP: PL530_CTRL · 192.168.1.1</span></div>
      <form id="wifiForm" class="wifi-form">
        <div class="field"><label for="ssidSelect">附近 SSID</label><select id="ssidSelect" name="ssid" required></select></div>
        <div class="field"><label for="wifiPass">密码</label><input id="wifiPass" name="password" type="password" maxlength="64" autocomplete="current-password" placeholder="开放网络可留空"></div>
        <button type="button" id="scan">扫描网络</button>
        <button class="primary" type="submit">连接</button>
        <button type="button" id="disconnect">断开并清除</button>
      </form>
    </section>
  </main>
  <div class="safety">安全限制固定为 10.0°C / 55.0°C，与 PWM 高低温参数相互独立</div>
</div>
<script>
const $=id=>document.getElementById(id);
let currentState="";
let settingsDirty=false;
let enabledDirty=false;
let deviceEnabled=null;
let refreshing=false;
let refreshAgain=false;
let statusKnown=false;
let statusRevision=0;
let controlEpoch=0;
let commandPending=Promise.resolve();
let commandActive=false;
let settingsBusy=false;
let stopPending=false;
let resetPending=false;
let calibrationActive=false;
let calibrationParameters={pulseMs:4000,dutyLimitPercent:30};
let directionKnown=false;
let detectionPending=false;
let detectionAllowed=false;
let wifiConnecting=false;
let wifiDisconnecting=false;
let wifiScanning=false;
function command(action,kind="",epoch=controlEpoch){
  if(kind!=="stop"&&stopPending)return Promise.reject(new Error("请求已取消，请确认设备状态后重试"));
  const run=async()=>{
    // Invalidate every unsent mutation; fresh WiFi recovery does not require status.
    if(kind!=="stop"&&(epoch!==controlEpoch||stopPending||(kind==="control"&&!statusKnown)))throw new Error("请求已取消，请确认设备状态后重试");
    commandActive=true;statusRevision++;
    if(kind){statusKnown=false;settingsLock(settingsBusy)}
    try{return await action()}
    finally{commandActive=false;statusRevision++}
  };
  const pending=commandPending.then(run,run);commandPending=pending.catch(()=>{});return pending;
}
function controlsLocked(){return !statusKnown||settingsBusy||stopPending||resetPending||detectionPending}
function settingsLock(locked){
  settingsBusy=locked;const disabled=controlsLocked()||calibrationActive;
  document.querySelectorAll("#settingsForm input,button[form=settingsForm]").forEach(el=>el.disabled=disabled);
  $("enabled").disabled=disabled||!directionKnown;
  $("detectDirection").disabled=disabled||!detectionAllowed;
  $("resetFault").disabled=disabled;
  wifiLock();
}
function wifiLock(){
  document.querySelector("#wifiForm button[type=submit]").disabled=stopPending||wifiConnecting;
  $("disconnect").disabled=stopPending||wifiDisconnecting;
  $("scan").disabled=stopPending||wifiScanning;
}
function updateDirection(d){
  const dir=d.direction||{},c=dir.calibration||{};
  calibrationParameters={pulseMs:c.pulseMs||4000,dutyLimitPercent:c.dutyLimitPercent||30};
  calibrationActive=!!c.active;
  directionKnown=dir.value==="normal"||dir.value==="reversed";
  const labels={normal:"IN1 制冷 / IN2 制热",reversed:"IN1 制热 / IN2 制冷",unknown:"TEC 方向未确认"};
  $("directionStatus").textContent=labels[dir.value]||labels.unknown;
  const stages={idle:directionKnown?"方向已保存":"TEC 保持停用",baseline_forward:"检测中：采集正向温度基线",forward:"检测中：正向 IN1 限幅脉冲",settle:"检测中：停机等待",baseline_reverse:"检测中：采集反向温度基线",reverse:"检测中：反向 IN2 限幅脉冲",complete:dir.saved?(d.settings.enabled?"检测完成，方向已保存，温控已启用":"检测完成，方向已保存，TEC 保持停用"):"检测结束，方向尚未保存",failed:"方向无法可靠确认，TEC 已停止",cancelled:"方向检测已取消，TEC 已停止"};
  let detail=stages[c.phase]||stages.idle;
  if(c.active&&Number.isFinite(c.rawDutyPercent)&&Math.abs(c.rawDutyPercent)>0)detail+=` · 上限 ${calibrationParameters.dutyLimitPercent}%`;
  if(c.error)detail+=` · ${calibrationErrorText(c.error)}`;
  if(dir.error)detail+=` · ${dir.error}`;
  $("calibrationStatus").textContent=detail;
  const powered=c.active&&(c.phase==="forward"||c.phase==="reverse");
  const progress=$("calibrationProgress");progress.hidden=!powered;progress.max=c.pulseMs||3000;progress.value=Math.min(c.phaseElapsedMs||0,progress.max);
  const deltas=$("calibrationDeltas");deltas.hidden=!Number.isFinite(c.forwardDeltaC)&&!Number.isFinite(c.reverseDeltaC);
  deltas.textContent=`正向 ΔT ${num(c.forwardDeltaC,2)} °C · 反向 ΔT ${num(c.reverseDeltaC,2)} °C`;
  detectionAllowed=!d.settings.enabled&&d.sensorOk&&Number.isFinite(d.temperatureC)&&d.temperatureC>=15&&d.temperatureC<=45&&!d.stateKey.endsWith("Latched")&&!d.wifi.connecting;
  $("detectDirection").title="确认散热器与探头安装后，执行双向限幅检测";
  settingsLock(settingsBusy);
}
function calibrationErrorText(error){
  const messages={sensor_invalid:"传感器数据无效",sensor_stale:"温度数据过期",driver_fault:"驱动器故障",safety_fault:"输出保护未就绪",temperature_bounds:"温度超出检测安全范围",temperature_excursion:"温度变化超过检测安全范围",baseline_unstable:"静止温度不稳定或采样不足",pulse_samples:"有效脉冲采样不足",response_ambiguous:"温差不足或双向趋势不一致，不能判断",total_timeout:"检测超时",invalid_phase:"检测状态异常",cancelled:"已取消"};
  return messages[error]||error;
}
function faultMessage(d){
  const messages={
    ThermalFaultLatched:"热失控保护已停止 TEC，请检查系统后解除锁定",
    DriverFaultLatched:"驱动器故障，TEC 已停止；检查供电、短路与散热后再解除锁定",
    SensorTimeoutLatched:"温度样本超时，TEC 已停机并锁定",
    SafetyFaultLatched:"输出保护初始化失败，TEC 保持停用",
    HardOverTemperature:"达到 55°C 限制，正在全力制冷",
    HardUnderTemperature:"达到 10°C 限制，正在全力加热"
  };
  return messages[d.stateKey]||(!d.sensorOk?(d.sensorError||"TMP117 传感器异常，输出已停止"):"");
}
function setMessage(text,type){const el=$("message");el.textContent=text;el.className="message show "+(type||"info");clearTimeout(setMessage.t);setMessage.t=setTimeout(()=>el.className="message",4000)}
function num(v,d=2){return Number.isFinite(v)?v.toFixed(d):"--"}
function stateClass(s){if(s==="ThermalFaultLatched"||s==="DriverFaultLatched"||s==="SensorTimeoutLatched"||s==="SafetyFaultLatched"||s==="HardOverTemperature"||s==="HardUnderTemperature")return "bad";if(s==="WaitingForSensor"||s==="Disabled"||s==="DirectionCalibration")return "warn";return "ok"}
async function requestJson(url,options={}){
  const abort=new AbortController();const timeout=setTimeout(()=>abort.abort(),4000);
  try{const r=await fetch(url,{...options,headers:{"X-PL530-Request":"1",...options.headers},signal:abort.signal});const j=await r.json();if(!r.ok||j.ok===false)throw new Error(j.error||"请求失败");return j}
  finally{clearTimeout(timeout)}
}
async function refresh(){
  if(refreshing||commandActive){refreshAgain=true;return}
  refreshing=true;refreshAgain=false;
  const revision=statusRevision;
  try{
    const d=await requestJson("/api/status",{cache:"no-store"});
    // A snapshot started before a command cannot unlock its controls afterwards.
    if(revision!==statusRevision||commandActive){refreshAgain=true;return}
    statusKnown=true;
    $("temp").textContent=d.sensorOk?num(d.temperatureC,2):"--";
    $("cooling").textContent=num(d.coolingDutyPercent,1)+"%";
    $("heating").textContent=num(d.heatingDutyPercent,1)+"%";
    $("slope").textContent=num(d.slopeCPerSecond,2)+" °C/s";
    $("balance").textContent=num(d.balanceDrivePercent,1)+"%";
    const st=$("state");st.textContent=d.stateText;st.className="state "+stateClass(d.stateKey);
    const fault=faultMessage(d);$("faultNotice").textContent=fault;$("faultNotice").hidden=!fault;
    $("configurationError").textContent=d.configurationError||"";$("configurationError").hidden=!d.configurationError;
    if(currentState!==d.stateKey){
      currentState=d.stateKey;
      if(d.stateKey==="ThermalFaultLatched")setMessage("热失控保护已停止 TEC，请检查系统后解除锁定","bad");
      else if(d.stateKey==="DriverFaultLatched")setMessage("驱动器故障，TEC 已停止；检查供电、短路与散热后再解除锁定","bad");
      else if(d.stateKey==="SensorTimeoutLatched")setMessage("温度样本超时，TEC 已停机并锁定","bad");
      else if(d.stateKey==="SafetyFaultLatched")setMessage("输出保护初始化失败，TEC 保持停用","bad");
      else if(d.stateKey==="HardOverTemperature")setMessage("超过 55°C，正在全力制冷","bad");
      else if(d.stateKey==="HardUnderTemperature")setMessage("低于 10°C，正在全力加热","bad");
      else if(d.stateKey==="WaitingForSensor")setMessage(d.sensorError||"TMP117 传感器异常，输出已停止","bad");
    }
    if(!d.sensorOk&&(d.stateKey==="WaitingForSensor"||d.stateKey==="Disabled"))setMessage(d.sensorError||"TMP117 传感器异常，输出已停止","bad");
    const s=d.settings;
    updateDirection(d);
    if(!settingsDirty&&!settingsBusy){$("target").value=s.targetC;$("high").value=s.pwmHighC;$("low").value=s.pwmLowC}
    if(!directionKnown||calibrationActive||(deviceEnabled===true&&!s.enabled)){enabledDirty=false;$("enabled").checked=false}
    if(!enabledDirty&&!settingsBusy&&!stopPending)$("enabled").checked=directionKnown&&!calibrationActive&&s.enabled;
    deviceEnabled=s.enabled;
    let ws=`AP: PL530_CTRL · 192.168.1.1`;
    if(d.wifi.connecting)ws=`正在连接 ${d.wifi.ssid||""} ...`;
    else if(d.wifi.connected)ws=`STA 已连接: ${d.wifi.ssid||""} · ${d.wifi.ip||""} · ${d.wifi.rssi} dBm`;
    else if(d.wifi.retrying)ws=`STA 已断开，等待重连 ${d.wifi.ssid||""}`;
    if(d.wifi.error)ws+=` · ${d.wifi.error}`;
    $("wifiStatus").textContent=ws;
    $("clock").textContent=new Date().toLocaleTimeString();
  }catch(e){
    if(revision!==statusRevision||commandActive){refreshAgain=true;return}
    statusKnown=false;controlEpoch++;enabledDirty=false;$("enabled").checked=false;settingsLock(settingsBusy);
    ["temp","cooling","heating","slope","balance"].forEach(id=>$(id).textContent="--");$("state").textContent="连接中断";$("state").className="state bad";$("faultNotice").textContent="无法读取设备状态，TEC 实际状态未知";$("faultNotice").hidden=false;$("calibrationStatus").textContent="检测状态未知";$("calibrationProgress").hidden=true;currentState="";
  }
  finally{refreshing=false;if(refreshAgain&&!commandActive)refresh()}
}
["target","high","low","enabled"].forEach(id=>{["input","change"].forEach(evt=>$(id).addEventListener(evt,()=>{if(id==="enabled")enabledDirty=true;else settingsDirty=true}))});
$("settingsForm").addEventListener("submit",async e=>{
  e.preventDefault();if(controlsLocked()||calibrationActive)return;
  const data=new URLSearchParams(new FormData(e.target));settingsLock(true);
  try{await command(()=>requestJson("/api/settings",{method:"POST",body:data}),"control");if(!stopPending){settingsDirty=false;enabledDirty=false}setMessage("控制参数已保存","ok")}
  catch(err){setMessage(err.message,"bad")}
  finally{settingsLock(false);refresh()}
});
$("scan").addEventListener("click",async()=>{
  if(stopPending||wifiScanning)return;
  const epoch=controlEpoch;
  const scan=()=>command(()=>requestJson("/api/wifi/scan",{cache:"no-store"}),"",epoch);
  const b=$("scan");wifiScanning=true;wifiLock();b.textContent="扫描中...";
  try{
    let j=await scan();
    const startedAt=Date.now();
    while(j.scanning&&Date.now()-startedAt<20000){
      await new Promise(resolve=>setTimeout(resolve,300));
      j=await scan();
    }
    if(j.scanning)throw new Error("WiFi 扫描超时");
    const sel=$("ssidSelect");sel.innerHTML="";
    (j.networks||[]).forEach(n=>{const o=document.createElement("option");o.value=n.ssid;o.textContent=`${n.ssid}  (${n.rssi} dBm)`;sel.appendChild(o)});
    if(!sel.options.length){const o=document.createElement("option");o.value="";o.textContent="未发现网络";sel.appendChild(o)}
  }catch(err){setMessage(err.message,"bad")}finally{wifiScanning=false;wifiLock();b.textContent="扫描网络";refresh()}
});
$("wifiForm").addEventListener("submit",async e=>{
  e.preventDefault();if(stopPending||wifiConnecting)return;
  const data=new URLSearchParams(new FormData(e.target));wifiConnecting=true;wifiLock();
  try{await command(()=>requestJson("/api/wifi/connect",{method:"POST",body:data}));$("wifiPass").value="";setMessage("已开始连接，请稍候","info")}
  catch(err){setMessage(err.message,"bad")}finally{wifiConnecting=false;wifiLock();refresh()}
});
$("disconnect").addEventListener("click",async()=>{if(stopPending||wifiDisconnecting)return;wifiDisconnecting=true;wifiLock();try{await command(()=>requestJson("/api/wifi/disconnect",{method:"POST"}));setMessage("STA 连接已断开并清除","ok")}catch(err){setMessage(err.message,"bad")}finally{wifiDisconnecting=false;wifiLock();refresh()}});
$("resetFault").addEventListener("click",async()=>{if(controlsLocked()||calibrationActive||!confirm("确认已排除故障并解除锁定？"))return;resetPending=true;settingsLock(settingsBusy);try{await command(()=>requestJson("/api/control/reset-fault",{method:"POST"}),"control");enabledDirty=false;$("enabled").checked=false;setMessage("已解除，TEC 保持停用","ok")}catch(err){setMessage(err.message,"bad")}finally{resetPending=false;settingsLock(settingsBusy);refresh()}});
$("stopOutput").addEventListener("click",async()=>{if(stopPending)return;const b=$("stopOutput");b.disabled=true;stopPending=true;controlEpoch++;statusKnown=false;enabledDirty=false;$("enabled").checked=false;settingsDirty=true;settingsLock(settingsBusy);try{await command(()=>requestJson("/api/control/stop",{method:"POST"}),"stop");setMessage("TEC 已停止","ok")}catch(err){setMessage("停止未确认："+err.message,"bad")}finally{stopPending=false;$("enabled").checked=false;settingsDirty=true;b.disabled=false;settingsLock(settingsBusy);refresh()}});
$("detectDirection").addEventListener("click",async()=>{
  if(controlsLocked()||calibrationActive||!detectionAllowed)return;
  const calibration=calibrationParameters;
  const pulseSeconds=(calibration.pulseMs||4000)/1000;
  const dutyLimit=calibration.dutyLimitPercent||30;
  if(!confirm(`确认 TEC 已安装散热器，TMP117 已贴合被控面，电源限流已设置？将正向、反向各运行 ${pulseSeconds} 秒，PWM 上限 ${dutyLimit}%，中间停机采样；温差不足将判为失败。重新检测会清除旧方向。`))return;
  detectionPending=true;settingsLock(settingsBusy);
  try{await command(()=>requestJson("/api/control/detect-direction",{method:"POST",body:new URLSearchParams({confirm:"full_power"})}),"control");enabledDirty=false;$("enabled").checked=false;settingsDirty=true;setMessage("方向检测已开始，TEC 将自动执行双向测试","info")}
  catch(err){setMessage(err.message,"bad")}
  finally{detectionPending=false;settingsLock(settingsBusy);refresh()}
});
settingsLock(false);refresh();setInterval(refresh,100);
</script>
</body>
</html>)rawliteral";
