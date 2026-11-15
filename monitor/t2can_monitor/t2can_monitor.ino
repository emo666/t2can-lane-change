#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <mcp2515.h>
#include <driver/twai.h>
#include <Update.h>
#include <Preferences.h>
#include "../tlssc_pure.h"
#include "dnd_scroll_cycle.h"

#include "credentials.h"  // Local credentials, excluded from Git
static const int MCP_CS=10, MCP_RESET=9, SCK_PIN=12, MISO_PIN=13, MOSI_PIN=11;
static const int TWAI_TX=7, TWAI_RX=6;
MCP2515 mcp(MCP_CS, 10000000, &SPI);
// Drop idle clients promptly so the synchronous web server keeps servicing CAN.
class ResponsiveWebServer : public WebServer {
 public:
  explicit ResponsiveWebServer(int port) : WebServer(port) {}
  void handleClient() override {
    if (_currentStatus == HC_WAIT_READ && _currentClient.connected() &&
        !_currentClient.available() && uint32_t(millis() - _statusChange) > 1200) {
      _currentClient.stop();
    }
    WebServer::handleClient();
  }
};
ResponsiveWebServer server(80);
struct Bus { bool ready=false; uint32_t last=0; bool seen=false; };
Bus chassis, vehicle;
static MCP2515::ERROR sendOtherA(const struct can_frame* f){ return mcp.sendMessage(f); }
// ---- 打灯自动变道全局状态（声明需早于 status()）----
static const uint32_t UI_CHASSIS_ID = 0x293;
bool autoLcEnabled = false, autoLcNvsReady = false;
// AP 红绿灯识别与高速、NOA 门控。
tlssc::Config tlsRefCfg;
tlssc::State tlsRefState;
bool tlsRefNvsReady=false, tlsRefFreshA=false, tlsRefFreshB=false, tlsRefFlushRx=false;
uint32_t tlsRefLastA=0, tlsRefLastB=0, tlsRefApMs=0;
static bool tlsRefBusesFresh(uint32_t now){
  return tlsRefFreshA && tlsRefFreshB && uint32_t(now-tlsRefLastA)<=2000 && uint32_t(now-tlsRefLastB)<=2000;
}
static bool tlsRefInputFresh(uint32_t now){
  return tlsRefBusesFresh(now) && tlsRefState.ap_valid && uint32_t(now-tlsRefApMs)<=2000;
}
// ---- AP 免打扰（DND）：关座舱摄像头(0x3FD mux1 清 bit43) + 右滚轮加减消提示(0x3C2 mux1 data[3]) ----
// 开启后保留摄像头处理，并在新鲜中立模板可用时循环右滚轮 +1、0、-1、0。
// 步进 100ms，每组完成后等待 2 秒；手动滚轮操作时暂停。默认关。
bool dndEnabled = false, dndNvsReady = false;
bool dndCamActive = false;               // 是否已清过摄像头位（关闭时发一帧还原）
bool dndCamRestorePending = false;       // 关闭开关后：等待用新鲜模板发一帧还原 bit43
uint32_t dndCamOffMs = 0;                // 还原截止时刻（超过则放弃，避免无限等待）
uint8_t dndCamRaw43 = 0xFF;              // 最近观测 0x3FD mux1 byte5 bit3
// 0x3FD 摄像头模板
bool dndHasApTpl = false; uint8_t dndApTpl[8]; uint32_t dndApTplMs = 0;
// 滚轮循环在手动动作或缺少新鲜模板时暂停。
t2can::DndScrollCycle dndScrollCycle;
bool dndDiscardScrollBacklog = false;
// ---- 维护模式（Service Mode）：CAN B 发 0x339 VCSEC_serviceDiagnosticRequest ----
// 进入: 00 00 00 00 00 80 00 00 (byte5 bit7=1)；退出: 全 0；4 帧 @10ms；默认关。
static const uint32_t SERVICE_ID = 0x339;
bool serviceBurstActive = false; // 正在连发
bool serviceBurstValue = false;  // 本次连发是进入(true)还是退出(false)
uint8_t serviceBurstRemain = 0;  // 剩余帧数
uint32_t serviceLastBurstMs = 0; // 上次帧时刻
// ---- OTA ----
bool otaRebootPending = false;   // 上传成功，待重启
const char OTA_USER[] PROGMEM = "admin"; // OTA 登录用户名（密码复用 TOKEN）
const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>T-2CAN 控制台</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#0c1016;color:#f0f2f6;font:15px/1.5 system-ui,-apple-system,"Microsoft YaHei",sans-serif}
main{max-width:450px;margin:auto;padding:20px 12px}
h1{font-size:20px;margin:0 0 4px}.sub{color:#8d99a8;font-size:12px;margin:0 0 18px}
.card{background:#151b22;border-radius:10px;padding:15px;margin-bottom:10px}
.heading{display:flex;justify-content:space-between;align-items:center;font-weight:600;margin-bottom:6px}
.badge{font-size:12px;color:#aeb9c6;background:#29323b;padding:3px 9px;border-radius:15px}
.badge.ok{color:#baf5c8;background:#1f3a29}.badge.warn{color:#f5dca8;background:#3a3220}.badge.err{color:#f5b8b8;background:#3a2323}
.row{display:flex;justify-content:space-between;align-items:center;gap:16px;padding:15px 0;border-top:1px solid #252c35}
.row:first-of-type{border-top:0}.row .lbl{font-weight:500}.sub-row{padding-left:20px}.sub-row .lbl{font-size:14px;color:#bbc6d3}
.row .val{font-size:13px;color:#aab5c4;display:flex;align-items:center;gap:8px}
.dot{width:8px;height:8px;border-radius:50%;background:#3c4652;flex:none}.dot.ok{background:#4ade80}.dot.warn{background:#facc15}.dot.err{background:#f87171}
.muted{color:#8d99a8}.lock{color:#f5dca8}.sub2{font-size:11px;color:#7a8698;margin-top:2px}
.btns{display:flex;gap:8px;margin-top:12px}.btn{flex:1;border:0;border-radius:9px;padding:11px;font-size:14px;cursor:pointer;color:#fff;background:#2f6fed}.btn:hover{background:#3d7bf5}
.switch{position:relative;display:inline-block;width:52px;height:30px;flex:none}.switch input{opacity:0;width:0;height:0}
.slider{position:absolute;cursor:pointer;inset:0;background:#3c4652;transition:.2s;border-radius:30px}
.slider:before{content:"";position:absolute;height:22px;width:22px;left:4px;bottom:4px;background:#fff;transition:.2s;border-radius:50%}
.switch input:checked + .slider{background:#2f6fed}.switch input:checked + .slider:before{transform:translateX(22px)}
.switch input:disabled + .slider{opacity:.5;cursor:default}.footer{color:#5f6b7a;font-size:11px;margin-top:18px;text-align:center}
</style></head>
<body><main>
<h1>T-2CAN</h1><p class="sub">双路监视 · CAN A=Chassis · CAN B=Vehicle</p>
<section class="card">
  <div class="heading"><span>连接状态</span><span class="badge" id="linkBadge">未连接</span></div>
  <div class="row"><span class="lbl">CAN A · CHASSIS</span><span class="val"><span class="dot" id="dA"></span><span id="vA">—</span></span></div>
  <div class="row"><span class="lbl">CAN B · VEHICLE</span><span class="val"><span class="dot" id="dB"></span><span id="vB">—</span></span></div>
</section>
<section class="card">
  <div class="heading" style="margin-bottom:0"><span>功能开关</span></div>
  <div role="group" aria-labelledby="tlsRefLabel">
    <div class="row"><span class="lbl" id="tlsRefLabel">AP 红绿灯识别</span><label class="switch"><input id="tlsRef" type="checkbox" aria-label="AP 红绿灯识别"><span class="slider"></span></label></div>
    <div id="tlsRefMsg" class="muted" aria-live="polite"></div>
    <div class="row sub-row"><span class="lbl">高速暂停</span><label class="switch"><input id="tlsRefHighway" type="checkbox" aria-label="高速暂停"><span class="slider"></span></label></div>
    <div class="row sub-row"><span class="lbl">NOA 暂停</span><label class="switch"><input id="tlsRefNoa" type="checkbox" aria-label="NOA 暂停"><span class="slider"></span></label></div>
  </div>
  <div class="row"><span class="lbl">打灯自动变道设置</span><label class="switch"><input id="autoLc" type="checkbox" aria-label="打灯自动变道设置"><span class="slider"></span></label></div>
  <div id="autoMsg" class="muted" aria-live="polite"></div>
  <div class="row"><span><span class="lbl">AP 免打扰</span><span class="sub2" style="display:block">右滚轮 +1 / -1，每组后间隔约 2 秒</span></span><label class="switch"><input id="dnd" type="checkbox" aria-label="AP 免打扰"><span class="slider"></span></label></div>
  <div id="dndMsg" class="muted" aria-live="polite"></div>
</section>
<section class="card"><div class="heading"><span>维护模式</span></div><div class="btns"><button class="btn" id="btnSvcEnter" type="button">进入维护模式</button></div><div id="svcMsg" class="muted" aria-live="polite"></div></section>
<p class="footer">T-2CAN 控制台 · <a href="/update" style="color:#7aa7ff">OTA 固件更新</a></p>
</main>
<script>
(function(){
  var POLL_MS = 2000;
  function $(id){ return document.getElementById(id); }
  var els = {};
  ['linkBadge','vA','dA','vB','dB','tlsRef','tlsRefHighway','tlsRefNoa','tlsRefMsg','autoLc','autoMsg','dnd','dndMsg','btnSvcEnter','svcMsg'].forEach(function(id){ els[id] = $(id); });
  var messages = {tlsRefMsg:'', autoMsg:'', dndMsg:'', svcMsg:''};
  var errors = {tlsRefMsg:false, autoMsg:false, dndMsg:false, svcMsg:false};
  function message(id, text, error){ messages[id]=text; errors[id]=!!error; els[id].textContent=text; els[id].className=error?'lock':'muted'; }
  function displayMessage(id, fallback, storageBad){
    var text=messages[id] || fallback || '';
    if(storageBad) text+=(text?' · ':'')+'配置存储不可用，重启后状态可能不同';
    els[id].textContent=text; els[id].className=(errors[id] || storageBad)?'lock':'muted';
  }
  function level(txt){
    if(txt.indexOf('接收中')===0 || txt.indexOf('在线')===0) return 'ok';
    if(txt.indexOf('无近期')===0 || txt.indexOf('初始化')===0 || txt.indexOf('离线')===0) return 'warn';
    return 'err';
  }
  function setBadge(text, cls){ els.linkBadge.textContent=text; els.linkBadge.className='badge '+cls; }
  function apply(s){
    var a=typeof s.a==='string'?s.a:'未知', b=typeof s.b==='string'?s.b:'未知';
    els.vA.textContent=a; els.dA.className='dot '+level(a);
    els.vB.textContent=b; els.dB.className='dot '+level(b);
    els.tlsRef.checked=!!s.tlsRefEnabled; els.tlsRefHighway.checked=!!s.tlsRefHighway; els.tlsRefNoa.checked=!!s.tlsRefNoa;
    els.autoLc.checked=!!s.autoLcEnabled; els.dnd.checked=!!s.dndEnabled;
    displayMessage('tlsRefMsg',s.tlsRefInfo,s.tlsRefNvsReady===false);
    displayMessage('autoMsg','',s.autoLcNvsReady===false);
    displayMessage('dndMsg','',s.dndNvsReady===false);
    var anyOk=[a,b].some(function(x){return x.indexOf('接收中')===0 || x.indexOf('在线')===0;});
    setBadge(anyOk?'在线':'已连接 · 无数据',anyOk?'ok':'warn');
  }
  function disconnected(){ setBadge('未连接','err'); els.vA.textContent=els.vB.textContent='连接中断'; els.dA.className=els.dB.className='dot err'; }
  function poll(){ return fetch('/status',{cache:'no-store',credentials:'same-origin'}).then(function(r){if(!r.ok) throw new Error('HTTP '+r.status); return r.json();}).then(apply).catch(disconnected); }
  function post(url,params){
    var body=Object.keys(params).map(function(k){return encodeURIComponent(k)+'='+encodeURIComponent(params[k]);}).join('&');
    return fetch(url,{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body}).then(function(r){
      return r.json().then(function(result){if(!r.ok || result.ok!==true) throw new Error(result.error || ('请求失败 HTTP '+r.status)); return result;});
    });
  }
  function bindSwitch(id,url,param,msg){
    els[id].addEventListener('change',function(){
      var params={}; params[param]=this.checked?'1':'0';
      post(url,params).then(function(){ message(msg,'已保存请求，车辆效果未确认。',false); return poll(); })
        .catch(function(e){ message(msg,'操作失败：'+e.message,true); return poll(); });
    });
  }
  bindSwitch('tlsRef','/tlscc-reference','enable','tlsRefMsg');
  bindSwitch('tlsRefHighway','/tlscc-reference','highway','tlsRefMsg');
  bindSwitch('tlsRefNoa','/tlscc-reference','noa','tlsRefMsg');
  bindSwitch('autoLc','/auto-lane-change','enable','autoMsg');
  bindSwitch('dnd','/dnd','enable','dndMsg');
  els.btnSvcEnter.addEventListener('click',function(){
    post('/maintenance',{enter:'1'}).then(function(){message('svcMsg','已提交进入请求；排队及车辆执行结果尚未确认。',false); return poll();})
      .catch(function(e){message('svcMsg','操作失败：'+e.message,true); return poll();});
  });
  poll(); setInterval(poll,POLL_MS);
})();
</script></body></html>)HTML";
// The control dashboard is available on the device's Wi-Fi; OTA uses separate authentication.
String busStatus(const Bus& b) { if(!b.ready)return "初始化失败"; if(!b.seen || uint32_t(millis()-b.last)>5000)return "无近期数据"; return "接收中"; }
void status(){
  server.sendHeader("Cache-Control","no-store");
  const char* info=!tlsRefNvsReady?"配置存储不可用":!tlsRefCfg.enabled?(tlsRefState.clear_pending?"关闭中：等待清理帧":"AP 红绿灯识别已关闭"):!tlsRefState.ap_valid?"等待 CAN A AP 状态":!tlssc::ap_active(tlsRefState)?"等待 AP 激活":tlssc::highway_blocked(tlsRefCfg,tlsRefState,millis())?"高速门控暂停":(tlsRefCfg.block_noa && tlssc::noa_active(tlsRefState))?"NOA 门控暂停":serviceBurstActive?"维护任务进行中":(!chassis.ready || !vehicle.ready || !tlsRefInputFresh(millis()))?"等待新鲜的两路总线及 AP 数据":"AP 已激活";
  String s="{\"a\":\""+busStatus(chassis)+"\",\"b\":\""+busStatus(vehicle)+"\"";
  s+=",\"autoLcEnabled\":"+String(autoLcEnabled?"true":"false")+",\"autoLcNvsReady\":"+String(autoLcNvsReady?"true":"false");
  s+=",\"dndEnabled\":"+String(dndEnabled?"true":"false")+",\"dndNvsReady\":"+String(dndNvsReady?"true":"false");
  s+=",\"tlsRefEnabled\":"+String(tlsRefCfg.enabled?"true":"false")+",\"tlsRefHighway\":"+String(tlsRefCfg.block_highway?"true":"false")+",\"tlsRefNoa\":"+String(tlsRefCfg.block_noa?"true":"false");
  s+=",\"tlsRefNvsReady\":"+String(tlsRefNvsReady?"true":"false")+",\"tlsRefInfo\":\""+String(info)+"\"}";
  server.send(200,"application/json",s);
}
// Submit the maintenance frame to CAN B.
static bool sendServiceFrame(const uint8_t data[8]){
  if(!vehicle.ready) return false;
  twai_message_t m{}; m.identifier=SERVICE_ID; m.data_length_code=8; memcpy(m.data,data,8);
  return twai_transmit(&m,pdMS_TO_TICKS(5))==ESP_OK;
}
// ---- 维护模式：启动一次 4 帧连发（进入或退出）----
void startServiceBurst(bool enter){
  serviceBurstValue = enter; serviceBurstRemain = 4; serviceLastBurstMs = 0;
  serviceBurstActive = true;
}
// 在 loop 中推进维护模式连发（非阻塞，每 10ms 一帧，共 4 帧）
void serviceTick(){
  if(!serviceBurstActive) return;
  uint32_t now=millis();
  if(serviceLastBurstMs!=0 && uint32_t(now-serviceLastBurstMs)<10) return;
  serviceLastBurstMs = now ? now : 1;
  uint8_t data[8]={0,0,0,0,0,0,0,0};
  if(serviceBurstValue) data[5]=0x80; // byte5 bit7 = 1 进入维护模式
  (void)sendServiceFrame(data);
  if(serviceBurstRemain>0) serviceBurstRemain--;
  if(serviceBurstRemain==0) serviceBurstActive=false;
}
// /maintenance 接口：enter=1 进入，exit=1 退出
void maintenanceHandler(){
  if(!vehicle.ready){ server.send(503,"application/json","{\"ok\":false,\"error\":\"CAN B 未就绪\"}"); return; }
  bool enter=false, exit=false;
  if(server.hasArg("enter") && server.arg("enter")=="1") enter=true;
  if(server.hasArg("exit")  && server.arg("exit")=="1")  exit=true;
  if(!enter && !exit){ server.send(400,"application/json","{\"ok\":false,\"error\":\"缺少 enter/exit\"}"); return; }
  startServiceBurst(enter);
  server.send(200,"application/json",String("{\"ok\":true,\"serviceModeOn\":")+(enter?"true":"false")+"}");
}
// ---- HTTP OTA ----
bool otaAuth(){ if(server.authenticate(OTA_USER, TOKEN)) return true; server.requestAuthentication(); return false; }
// 请求状态独立于 Update 的全局错误值；结束整个 POST 后才提交固件。
bool otaFileSeen = false, otaUpdateStarted = false, otaFileEnded = false;
bool otaInProgress = false; // 上传/写入期间暂停所有 CAN 发送（含自动变道/TLSSC/免打扰）
size_t otaBytesWritten = 0;
int otaHttpError = 0;
void otaResetRequest(){
  if(otaUpdateStarted) Update.abort();
  otaFileSeen = false; otaUpdateStarted = false; otaFileEnded = false;
  otaBytesWritten = 0; otaHttpError = 0; otaRebootPending = false;
  otaInProgress = false;
}
void otaFailRequest(int code){
  if(!otaHttpError) otaHttpError = code;
  if(otaUpdateStarted) Update.abort();
  otaUpdateStarted = false; otaFileEnded = false; otaRebootPending = false;
}
void otaUploadHandler(){
  HTTPUpload& up = server.upload();
  if(up.status==UPLOAD_FILE_ABORTED){
    otaResetRequest(); Serial.println("# ota aborted");
    return;
  }
  if(!server.authenticate(OTA_USER, TOKEN)) return;
  if(up.status==UPLOAD_FILE_START){
    if(otaFileSeen){ otaFailRequest(400); return; } // 一个 POST 只接受一个文件
    otaResetRequest();
    otaFileSeen = true;
    otaInProgress = true; tlsRefFreshA=false; tlsRefFreshB=false; tlsRefState.ap_valid=false; tlsRefState.road_valid=false; tlsRefState.highway=false; tlsRefState.positive=0; tlsRefState.negative=0; tlsRefFlushRx=true; // OTA 后清队列并重新等待输入
    dndScrollCycle.setEnabled(false); dndScrollCycle.invalidateTemplate(); dndDiscardScrollBacklog=true;
    serviceBurstActive=false; // 暂停维护连发
    Serial.printf("# ota start %s\n", up.filename.c_str());
    if(!Update.begin(UPDATE_SIZE_UNKNOWN)){
      Update.printError(Serial); Update.abort(); otaFailRequest(500);
    } else otaUpdateStarted = true;
  } else if(up.status==UPLOAD_FILE_WRITE){
    if(otaHttpError) return;
    if(!otaUpdateStarted || otaFileEnded){ otaFailRequest(400); return; }
    if(Update.write(up.buf, up.currentSize)!=up.currentSize){
      Update.printError(Serial); otaFailRequest(500);
    } else otaBytesWritten += up.currentSize;
  } else if(up.status==UPLOAD_FILE_END){
    if(otaHttpError) return;
    if(!otaUpdateStarted || otaFileEnded || otaBytesWritten==0 || otaBytesWritten!=up.totalSize){
      otaFailRequest(400); return;
    }
    otaFileEnded = true; // 延迟 Update.end，避免后续文件使已提交固件的请求失败
  }
}
void otaFinishedHandler(){
  if(!server.authenticate(OTA_USER, TOKEN)){
    otaResetRequest(); server.requestAuthentication(); return; // 明确返回 401
  }
  int code = otaHttpError;
  if(!code && (!otaFileSeen || !otaUpdateStarted || !otaFileEnded || otaBytesWritten==0)) code = 400;
  if(!code){
    if(Update.end(true)){
      otaUpdateStarted = false;
      Serial.printf("# ota success %u\n", (unsigned)otaBytesWritten);
    } else {
      Update.printError(Serial); code = 500;
    }
  }
  bool ok = code==0;
  otaResetRequest(); // 包括失败的 Update 会话；空 POST 不继承上次成功状态
  server.sendHeader("Connection","close");
  server.send(ok ? 200 : code,"text/html; charset=utf-8",
    ok ? F("<meta charset=utf-8><body style='font-family:system-ui;padding:24px'><h3>OTA 成功，正在重启…</h3></body>")
       : F("<meta charset=utf-8><body style='font-family:system-ui;padding:24px'><h3>OTA 失败，保持当前固件。</h3></body>"));
  otaRebootPending = ok;
}
void otaPageHandler(){
  if(!otaAuth()) return;
  server.send_P(200,"text/html; charset=utf-8",
    "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<body style='font-family:system-ui;max-width:420px;margin:auto;padding:20px;background:#0c1016;color:#f0f2f6'>"
    "<h3>OTA 固件更新</h3><p style='color:#8d99a8;font-size:13px'>选择 PlatformIO 生成的 <code>firmware.bin</code>，上传成功后自动重启。</p>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='firmware' accept='.bin,application/octet-stream' required><br><br>"
    "<button type='submit' style='background:#2f6fed;color:#fff;border:0;border-radius:8px;padding:11px 16px;font-size:15px'>上传并刷写</button></form>"
    "<p><a style='color:#7aa7ff' href='/'>返回控制台</a></p></body>");
}
// ---- 打灯自动变道（UI_autoLaneChangeEnable，chassis CAN 0x293 DLC8）----
// 移植自 P4-CAN：观测 CAN A 上的 0x293，若 bit24..25（byte3 bit0..1）非 ON(1)，
// 则复制该帧、置 ON=1、计数器+1、重算 checksum 后经 CAN A 覆盖重发。开关默认关。
// Tesla 0x293 加和校验：高字节 + 低字节 + data[0..6]，取低 8 位
uint8_t ui293Checksum(const uint8_t* d){ uint16_t s=(uint16_t)(UI_CHASSIS_ID>>8)+(uint16_t)(UI_CHASSIS_ID&0xFF); for(uint8_t i=0;i<7;i++) s+=d[i]; return (uint8_t)s; }
// 将帧改为“自动变道使能=开”：byte3 bit0..1=01，byte6 高 4 位计数+1，byte7 重算校验
void applyAutoLaneChange(uint8_t* d){
  d[3] = (uint8_t)((d[3] & 0xFCu) | 0x01u);
  uint8_t cnt = (uint8_t)(((d[6] >> 4) + 1u) & 0x0Fu);
  d[6] = (uint8_t)((d[6] & 0x0Fu) | (uint8_t)(cnt << 4));
  d[7] = ui293Checksum(d);
}
// Each visible independent switch has its own persistent NVS key.
static bool saveFeatureFlag(const char* key, bool value){
  Preferences p;
  if(!p.begin("feature-ui",false)) return false;
  bool ok=p.putBool(key,value)==1; p.end(); return ok;
}
// /auto-lane-change 接口：enable=1 开启，enable=0 关闭
void autoLcHandler(){
  if(!server.hasArg("enable") || (server.arg("enable")!="0" && server.arg("enable")!="1")){
    server.send(400,"application/json","{\"ok\":false,\"error\":\"enable 必须为0或1\"}"); return;
  }
  bool want=server.arg("enable")=="1";
  if(!want) autoLcEnabled=false;
  if(!saveFeatureFlag("autolc",want)){
    autoLcNvsReady=false;
    server.send(500,"application/json","{\"ok\":false,\"error\":\"配置保存失败，请核对当前状态；重启后设置可能恢复\"}"); return;
  }
  autoLcEnabled=want; autoLcNvsReady=true;
  server.send(200,"application/json",String("{\"ok\":true,\"autoLcEnabled\":")+(autoLcEnabled?"true":"false")+"}");
}
// 观测到 CAN A 的 0x293 帧时调用：更新状态并（若开启）覆盖重发
void autoLcOnFrame(const struct can_frame& f){
  if(otaInProgress) return;
  if(f.can_id != UI_CHASSIS_ID || f.can_dlc < 8) return;
  if(!autoLcEnabled) return;
  if((f.data[3] & 0x03u) == 0x01u) return; // 已经是 ON，不必注入
  struct can_frame out = f; applyAutoLaneChange(out.data);
  (void)sendOtherA(&out);
}
// Persist the three reference-test options together before applying them.
void tlsRefHandler(){
  tlssc::Config next=tlsRefCfg;
  const char* names[]={"enable","highway","noa"};
  for(const char* name:names) if(server.hasArg(name) && server.arg(name)!="0" && server.arg(name)!="1"){
    server.send(400,"application/json","{\"ok\":false,\"error\":\"参数必须为0或1\"}"); return;
  }
  if(!server.hasArg("enable") && !server.hasArg("highway") && !server.hasArg("noa")){
    server.send(400,"application/json","{\"ok\":false,\"error\":\"缺少参数\"}"); return;
  }
  if(server.hasArg("enable")) next.enabled=server.arg("enable")=="1";
  if(server.hasArg("highway")) next.block_highway=server.arg("highway")=="1";
  if(server.hasArg("noa")) next.block_noa=server.arg("noa")=="1";
  if(server.hasArg("enable") && !next.enabled){
    tlsRefCfg.enabled=false;
    if(tlsRefState.injected) tlsRefState.clear_pending=true;
  }
  Preferences p;
  if(!p.begin("tlssc-ref",false)){ tlsRefNvsReady=false; server.send(500,"application/json","{\"ok\":false,\"error\":\"配置保存失败，请核对当前状态；重启后设置可能恢复\"}"); return; }
  uint8_t packed=(next.enabled?1:0)|(next.block_highway?2:0)|(next.block_noa?4:0);
  bool saved=p.putUChar("config",packed)==1; p.end();
  if(!saved){ tlsRefNvsReady=false; server.send(500,"application/json","{\"ok\":false,\"error\":\"配置保存失败，当前状态可能与重启后的状态不同\"}"); return; }
  if(!next.enabled && tlsRefState.injected) tlsRefState.clear_pending=true;
  if(next.enabled) tlsRefState.clear_pending=false;
  tlsRefCfg=next; tlsRefNvsReady=true;
  server.send(200,"application/json","{\"ok\":true}");
}
static bool refCanBRunning();
// T-2CAN physical CAN A is Chassis; P4's reference Chassis input is CAN B.
void tlsRefObserveA(const struct can_frame& f){
  if(f.can_id & (CAN_EFF_FLAG|CAN_RTR_FLAG|CAN_ERR_FLAG)) return;
  if(f.can_dlc>8) return;
  uint32_t id=f.can_id & CAN_SFF_MASK;
  if(id==0x399 && f.can_dlc>=6){
    tlssc::observe_ap(tlsRefState,f.data,f.can_dlc); tlsRefApMs=millis();
  } else if(id==0x238 && f.can_dlc>=5){
    tlssc::observe_road(tlsRefState,f.data,f.can_dlc,millis());
  } else if(id==0x3FD && f.can_dlc==8 && (f.data[0]&7)==0){
    if(tlsRefState.ap_valid && uint32_t(millis()-tlsRefApMs)>2000) tlsRefState.ap_valid=false;
    auto d=tlssc::prepare(tlsRefState,tlsRefCfg,millis(),id,false,false,f.data,f.can_dlc);
    if(d.action==tlssc::Action::none) return;
    // No P4 PARTY/Summon controller exists here: require both T-2CAN buses to
    // have supplied a frame, stay ready and avoid the local maintenance burst.
    if(!refCanBRunning()) tlsRefFreshB=false;
    const bool fresh=(d.action==tlssc::Action::clear)?tlsRefBusesFresh(millis()):tlsRefInputFresh(millis());
    if(otaInProgress || serviceBurstActive || !chassis.ready || !vehicle.ready || !fresh) return;
    struct can_frame out=f; memcpy(out.data,d.data,8);
    bool ok=sendOtherA(&out)==MCP2515::ERROR_OK;
    tlssc::complete(tlsRefState,d.action,ok);
  }
}
// ---- AP 免打扰 ----
void dndHandler(){
  if(!server.hasArg("enable") || (server.arg("enable")!="0" && server.arg("enable")!="1")){
    server.send(400,"application/json","{\"ok\":false,\"error\":\"enable 必须为0或1\"}"); return;
  }
  bool want=server.arg("enable")=="1";
  if(!want){
    // Stop the cycle immediately, even if persisting OFF subsequently fails.
    dndEnabled=false; dndScrollCycle.setEnabled(false);
    if(dndCamActive && !dndCamRestorePending){ dndCamRestorePending=true; dndCamOffMs=millis(); }
  }
  if(!saveFeatureFlag("dnd",want)){
    dndNvsReady=false;
    server.send(500,"application/json","{\"ok\":false,\"error\":\"配置保存失败，请核对当前状态；重启后设置可能恢复\"}"); return;
  }
  if(want){ dndEnabled=true; dndScrollCycle.setEnabled(true); dndCamRestorePending=false; }
  dndNvsReady=true;
  server.send(200,"application/json",String("{\"ok\":true,\"dndEnabled\":")+(dndEnabled?"true":"false")+"}");
}
// 观测 CAN A(Chassis) 的 0x3FD mux1 与 0x3C2 mux1 模板。
void dndObserve(const struct can_frame& f){
  if(f.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG)) return;
  uint32_t id = f.can_id & 0x7FFu;
  if(id==0x3FD && f.can_dlc>=8 && (f.data[0]&0x07)==1){ memcpy(dndApTpl,f.data,8); dndHasApTpl=true; dndApTplMs=millis(); dndCamRaw43=(f.data[5]&0x08)?1:0; }
  else if(id==0x3C2){
    if(dndDiscardScrollBacklog) return;
    uint32_t now=millis();
    dndScrollCycle.observeFrame(now,id,false,f.can_dlc,f.data);
  }
}
static t2can::DndScrollCycle::SendResult dndSendScroll(void*, const uint8_t data[8]){
  // Drain received traffic before sending; do not queue behind pending TX.
  if(dndDiscardScrollBacklog || (mcp.getStatus() & 0x57)) return t2can::DndScrollCycle::SendResult::Busy;
  struct can_frame out{}; out.can_id=0x3C2; out.can_dlc=8; memcpy(out.data,data,8);
  MCP2515::ERROR result=sendOtherA(&out);
  if(result==MCP2515::ERROR_OK) return t2can::DndScrollCycle::SendResult::Submitted;
  if(result==MCP2515::ERROR_ALLTXBUSY) return t2can::DndScrollCycle::SendResult::Busy;
  // The library may have set TXREQ even when it reports an error.
  return t2can::DndScrollCycle::SendResult::Indeterminate;
}
// 主循环调用：清摄像头位 + 滚轮循环（均在 CAN A/Chassis）
void dndTick(){
  if(otaInProgress || !chassis.ready){ dndScrollCycle.setEnabled(false); return; }
  dndScrollCycle.setEnabled(dndEnabled);
  uint32_t now=millis();
  // 0) 关闭后的摄像头位还原：用新鲜 0x3FD mux1 模板把 bit43 置回 1 重发一次。
  if(!dndEnabled && dndCamRestorePending){
    if(dndHasApTpl && (now-dndApTplMs)<30000UL && (now-dndCamOffMs)<30000UL){
      struct can_frame out; out.can_id=0x3FD; out.can_dlc=8; memcpy(out.data,dndApTpl,8);
      out.data[5]=(uint8_t)(out.data[5] | 0x08U); // 置 bit43 UI_enableCabinCamera=1 还原
      if(sendOtherA(&out)==MCP2515::ERROR_OK){ dndCamActive=false; dndCamRestorePending=false; }
    } else if((now-dndCamOffMs)>=30000UL){
      dndCamRestorePending=false; dndCamActive=false;
    }
    return;
  }
  if(!dndEnabled) return;
  // 1) 关摄像头：以最近 0x3FD mux1 模板清 bit43 重发（模板 30s 内有效）
  if(dndHasApTpl && (now-dndApTplMs)<30000UL){
    static uint32_t lastCamMs=0;
    if(dndCamRaw43==1 || !dndCamActive){
      if(now-lastCamMs>=500){
        lastCamMs=now;
        struct can_frame out; out.can_id=0x3FD; out.can_dlc=8; memcpy(out.data,dndApTpl,8);
        out.data[5]=(uint8_t)(out.data[5] & ~0x08U); // 清 bit43 UI_enableCabinCamera
        if(sendOtherA(&out)==MCP2515::ERROR_OK) dndCamActive=true;
      }
    }
  }
  // 2) 右滚轮 +1、0、-1、0，组内 100ms，组间 2 秒。
  dndScrollCycle.service(now,dndSendScroll,nullptr);
}
// CAN B（TWAI）BUS_OFF 检测与恢复：进入 BUS_OFF 时启动恢复，恢复完成后重新启动驱动。
void canBRecoverTick(){
  if(!vehicle.ready) return;
  static uint32_t lastRecoverMs = 0;
  twai_status_info_t st;
  if(twai_get_status_info(&st)!=ESP_OK) return;
  uint32_t now=millis();
  if(st.state!=TWAI_STATE_RUNNING) tlsRefFreshB=false;
  if(st.state==TWAI_STATE_BUS_OFF){
    if(uint32_t(now-lastRecoverMs)<1000) return; // 恢复限速，避免频繁重入
    lastRecoverMs=now;
    tlsRefFreshB=false;
    twai_initiate_recovery(); // 进入 RECOVERING，完成后转 STOPPED
  } else if(st.state==TWAI_STATE_STOPPED){
    if(uint32_t(now-lastRecoverMs)<1000) return;
    lastRecoverMs=now;
    twai_start(); // 重新启动，回到 RUNNING
  }
}
static void loadFeatureConfiguration(){
  { Preferences p; if(p.begin("feature-ui",false)){ autoLcEnabled=p.getBool("autolc",false); dndEnabled=p.getBool("dnd",false); autoLcNvsReady=true; dndNvsReady=true; p.end(); } }
  { Preferences p; if(p.begin("tlssc-ref",false)){ uint8_t v=p.getUChar("config",0); tlsRefCfg.enabled=(v&1)!=0; tlsRefCfg.block_highway=(v&2)!=0; tlsRefCfg.block_noa=(v&4)!=0; tlsRefNvsReady=true; p.end(); } }
}
void setup(){Serial.begin(115200); loadFeatureConfiguration(); pinMode(MCP_RESET,OUTPUT);digitalWrite(MCP_RESET,LOW);delay(10);digitalWrite(MCP_RESET,HIGH);delay(20);SPI.begin(SCK_PIN,MISO_PIN,MOSI_PIN,MCP_CS);
// Official example uses the library default oscillator setting. Verify against your board.
chassis.ready=(mcp.reset()==MCP2515::ERROR_OK && mcp.setBitrate(CAN_500KBPS)==MCP2515::ERROR_OK && mcp.setNormalMode()==MCP2515::ERROR_OK);
twai_general_config_t g=TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)TWAI_TX,(gpio_num_t)TWAI_RX,TWAI_MODE_NORMAL);g.tx_queue_len=8;g.rx_queue_len=64;twai_timing_config_t t=TWAI_TIMING_CONFIG_500KBITS();twai_filter_config_t f=TWAI_FILTER_CONFIG_ACCEPT_ALL();vehicle.ready=(twai_driver_install(&g,&t,&f)==ESP_OK && twai_start()==ESP_OK);
WiFi.mode(WIFI_AP); if(!WiFi.softAP(SSID,PASSWORD)) Serial.println("AP start failed"); server.on("/",HTTP_GET,[]{server.sendHeader("Cache-Control","no-store"); server.send_P(200,"text/html; charset=utf-8",PAGE);});server.on("/status",HTTP_GET,status);server.on("/auto-lane-change",HTTP_POST,autoLcHandler);server.on("/tlscc-reference",HTTP_POST,tlsRefHandler);server.on("/dnd",HTTP_POST,dndHandler);server.on("/maintenance",HTTP_POST,maintenanceHandler);server.on("/update",HTTP_GET,otaPageHandler);server.on("/update",HTTP_POST,otaFinishedHandler,otaUploadHandler);server.begin();Serial.println(WiFi.softAPIP());}
static bool refCanBRunning(){
  twai_status_info_t st;
  return vehicle.ready && twai_get_status_info(&st)==ESP_OK && st.state==TWAI_STATE_RUNNING;
}
// Drain the MCP2515 both before and after synchronous web processing.
void pollChassis(){
  struct can_frame frame;
  if(!chassis.ready) return;
  if(!refCanBRunning()) tlsRefFreshB=false;
  for(int i=0;i<32 && mcp.readMessage(&frame)==MCP2515::ERROR_OK;i++){
    chassis.last=millis(); chassis.seen=true;
    if(!(frame.can_id & (CAN_EFF_FLAG|CAN_RTR_FLAG|CAN_ERR_FLAG)) && frame.can_dlc<=8){ tlsRefFreshA=true; tlsRefLastA=millis(); }
    tlsRefObserveA(frame);
    if(frame.can_id==0x293 && frame.can_dlc==8) autoLcOnFrame(frame);
    dndObserve(frame);
  }
  if(dndDiscardScrollBacklog && !mcp.checkReceive()){
    dndDiscardScrollBacklog=false;
    dndScrollCycle.requireQuietAfterBacklog(millis());
  }
}
void loop(){
  if(!otaInProgress && !tlsRefFlushRx && !otaRebootPending) pollChassis();
  uint32_t webStart=millis(); server.handleClient();
  uint32_t webMs=uint32_t(millis()-webStart);
  if(webMs>=t2can::DndScrollCycle::kTemplateMaxAgeMs){ dndScrollCycle.invalidateTemplateAt(millis()); dndDiscardScrollBacklog=true; }
  if(otaRebootPending){ delay(800); ESP.restart(); return; }
// CAN B 总线故障（BUS_OFF）检测与恢复；恢复后重新计入就绪
canBRecoverTick();
// OTA 上传期间：完全暂停全部 CAN 发送，只保留网页/接收推进
if(otaInProgress){ delay(1); return; }
if(tlsRefFlushRx){
  struct can_frame stale; for(int i=0;i<32 && mcp.readMessage(&stale)==MCP2515::ERROR_OK;i++){}
  twai_clear_receive_queue(); tlsRefFreshA=false; tlsRefFreshB=false; tlsRefState.ap_valid=false; tlsRefFlushRx=false;
}
// 维护模式连发推进（非阻塞，10ms/帧 ×4）
serviceTick();
// OTA 成功在网页处理后立即重启，避免再运行发送任务。
pollChassis();
// Read recent manual input before scheduling the next automatic pulse.
dndTick();
twai_message_t msg;if(vehicle.ready)for(int i=0;i<32 && twai_receive(&msg,0)==ESP_OK;i++){vehicle.last=millis();vehicle.seen=true;if(refCanBRunning() && !msg.extd && !msg.rtr && msg.data_length_code<=8 && msg.identifier<=0x7ff){ tlsRefFreshB=true; tlsRefLastB=millis(); }}delay(1);}
