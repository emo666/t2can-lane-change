#include <Arduino.h>
#include <SPI.h>
#include <mcp2515.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include "credentials.h"  // Local credentials, excluded from Git
#include "page.h"

// T-2CAN CAN A (MCP2515), connected to Chassis CAN.
static constexpr int MCP_CS = 10, MCP_RESET = 9;
static constexpr int SPI_SCK = 12, SPI_MISO = 13, SPI_MOSI = 11;
static constexpr uint32_t UI_CHASSIS_ID = 0x293;
static constexpr char WEB_USER[] = "admin";
MCP2515 mcp(MCP_CS, 10000000, &SPI);

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
bool canReady = false;
bool enabled = true; // Reverts to enabled on every boot; switch is temporary.
bool frameSeen = false;
uint8_t frameRaw = 0;
uint32_t frameAt = 0, txOk = 0, txFail = 0, lastSendErrorLog = 0;
bool otaFileSeen = false, otaUpdateStarted = false, otaFileEnded = false;
bool otaInProgress = false, otaRebootPending = false;
size_t otaBytesWritten = 0;
int otaHttpError = 0;

bool authenticate() {
  if (server.authenticate(WEB_USER, TOKEN)) return true;
  server.requestAuthentication();
  return false;
}

// Browser POSTs must originate from this device, in addition to Basic auth.
bool sameOrigin() {
  if (!server.hasHeader("Origin")) return false;
  String host = server.hostHeader();
  if (host.length() == 0) return false;
  if (host.endsWith(":80")) host.remove(host.length() - 3);
  return server.header("Origin") == String("http://") + host;
}

// Preserve the original monitor's 0x293 transformation.
static void enableAutoLaneChange(can_frame& frame) {
  frame.data[3] = (frame.data[3] & 0xFCu) | 0x01u;
  const uint8_t counter = ((frame.data[6] >> 4) + 1u) & 0x0Fu;
  frame.data[6] = (frame.data[6] & 0x0Fu) | (counter << 4);
  uint16_t checksum = (UI_CHASSIS_ID >> 8) + (UI_CHASSIS_ID & 0xFFu);
  for (uint8_t i = 0; i < 7; ++i) checksum += frame.data[i];
  frame.data[7] = static_cast<uint8_t>(checksum);
}

void statusHandler() {
  if (!authenticate()) return;
  const bool recent = frameSeen && uint32_t(millis() - frameAt) < 5000;
  String body = String("{\"enabled\":") + (enabled ? "true" : "false") +
      ",\"canReady\":" + (canReady ? "true" : "false") +
      ",\"frameRecent\":" + (recent ? "true" : "false") +
      ",\"raw\":" + String(frameRaw) + ",\"txOk\":" + String(txOk) +
      ",\"txFail\":" + String(txFail) + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void toggleHandler() {
  if (!authenticate()) return;
  if (!sameOrigin()) { server.send(403, "application/json", "{\"ok\":false}"); return; }
  if (!server.hasArg("enable") ||
      (server.arg("enable") != "0" && server.arg("enable") != "1")) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  enabled = server.arg("enable") == "1";
  server.send(200, "application/json", "{\"ok\":true}");
}

// Each OTA POST has its own state. No CAN transmission occurs during upload.
void otaResetRequest() {
  if (otaUpdateStarted) Update.abort();
  otaFileSeen = otaUpdateStarted = otaFileEnded = false;
  otaBytesWritten = 0;
  otaHttpError = 0;
  otaInProgress = false;
}
void otaFailRequest(int code) {
  if (!otaHttpError) otaHttpError = code;
  if (otaUpdateStarted) Update.abort();
  otaUpdateStarted = false;
  otaFileEnded = false;
}
void otaUploadHandler() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_ABORTED) {
    otaResetRequest();
    return;
  }
  if (!server.authenticate(WEB_USER, TOKEN) || !sameOrigin()) return;
  if (up.status == UPLOAD_FILE_START) {
    if (otaFileSeen) { otaFailRequest(400); return; }
    otaResetRequest();
    otaFileSeen = true;
    otaInProgress = true;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.abort();
      otaFailRequest(500);
    } else otaUpdateStarted = true;
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (otaHttpError) return;
    if (!otaUpdateStarted || otaFileEnded) { otaFailRequest(400); return; }
    if (Update.write(up.buf, up.currentSize) != up.currentSize) otaFailRequest(500);
    else otaBytesWritten += up.currentSize;
  } else if (up.status == UPLOAD_FILE_END) {
    if (otaHttpError) return;
    if (!otaUpdateStarted || otaFileEnded || otaBytesWritten == 0 ||
        otaBytesWritten != up.totalSize) { otaFailRequest(400); return; }
    otaFileEnded = true;
  }
}
void otaFinishedHandler() {
  if (!server.authenticate(WEB_USER, TOKEN)) {
    otaResetRequest();
    server.requestAuthentication();
    return;
  }
  if (!sameOrigin()) {
    otaResetRequest();
    server.send(403, "text/plain", "Forbidden");
    return;
  }
  int code = otaHttpError;
  if (!code && (!otaFileSeen || !otaUpdateStarted || !otaFileEnded || otaBytesWritten == 0)) code = 400;
  if (!code) {
    if (Update.end(true)) otaUpdateStarted = false;
    else code = 500;
  }
  const bool ok = code == 0;
  otaResetRequest();
  // A successful update keeps transmission paused until the restart.
  otaRebootPending = ok;
  otaInProgress = ok;
  server.sendHeader("Connection", "close");
  server.send(ok ? 200 : code, "text/html; charset=utf-8",
    ok ? F("<meta charset=utf-8><body style='font-family:system-ui;padding:24px'><h3>OTA 成功，正在重启…</h3></body>")
       : F("<meta charset=utf-8><body style='font-family:system-ui;padding:24px'><h3>OTA 失败，保持当前固件。</h3><a href='/update'>返回</a></body>"));
}
void otaPageHandler() {
  if (!authenticate()) return;
  server.send_P(200, "text/html; charset=utf-8", R"HTML(<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>OTA 固件更新</title><style>*{box-sizing:border-box}body{font-family:system-ui,sans-serif;max-width:520px;margin:auto;padding:30px 20px;background:#0d131a;color:#eef4f7}p{color:#a6b7c2;line-height:1.6}form{background:#151f29;border:1px solid #293744;border-radius:16px;padding:20px}input{max-width:100%;margin:12px 0 20px}button{display:block;border:0;border-radius:9px;padding:12px 18px;background:#20a873;color:white;font-weight:700}a{color:#8cdec0}</style></head><body><p><a href="/">← 返回控制台</a></p><h1>OTA 固件更新</h1><p>选择兼容的 firmware.bin，上传成功后设备会自动重启。</p><form method="POST" action="/update" enctype="multipart/form-data"><input type="file" name="firmware" accept=".bin,application/octet-stream" required><button type="submit">上传并更新</button></form></body></html>)HTML");
}

void setup() {
  Serial.begin(115200);
  pinMode(MCP_RESET, OUTPUT);
  digitalWrite(MCP_RESET, LOW); delay(10);
  digitalWrite(MCP_RESET, HIGH); delay(20);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, MCP_CS);
  canReady = mcp.reset() == MCP2515::ERROR_OK &&
             mcp.setBitrate(CAN_500KBPS) == MCP2515::ERROR_OK &&
             mcp.setNormalMode() == MCP2515::ERROR_OK;
  Serial.println(canReady ? "CAN A ready; auto lane change enabled" : "CAN A initialization failed");
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(SSID, PASSWORD)) Serial.println("AP start failed");
  const char* headerKeys[] = {"Origin"};
  server.collectHeaders(headerKeys, 1);
  server.on("/", HTTP_GET, [] { if (!authenticate()) return; server.sendHeader("Cache-Control", "no-store"); server.send_P(200, "text/html; charset=utf-8", PAGE); });
  server.on("/status", HTTP_GET, statusHandler);
  server.on("/auto-lane-change", HTTP_POST, toggleHandler);
  server.on("/update", HTTP_GET, otaPageHandler);
  server.on("/update", HTTP_POST, otaFinishedHandler, otaUploadHandler);
  server.begin();
  Serial.println(WiFi.softAPIP());
}

void loop() {
  server.handleClient();
  if (otaRebootPending) { delay(800); ESP.restart(); return; }
  if (otaInProgress || !canReady) { delay(1); return; }
  can_frame frame;
  for (int i = 0; i < 32 && mcp.readMessage(&frame) == MCP2515::ERROR_OK; ++i) {
    if (frame.can_id != UI_CHASSIS_ID || frame.can_dlc != 8) continue;
    frameRaw = frame.data[3] & 0x03u;
    frameSeen = true;
    frameAt = millis();
    if (!enabled || frameRaw == 0x01u) continue;
    enableAutoLaneChange(frame);
    if (mcp.sendMessage(&frame) == MCP2515::ERROR_OK) ++txOk;
    else {
      ++txFail;
      const uint32_t now = millis();
      if (now - lastSendErrorLog >= 1000) {
        lastSendErrorLog = now;
        Serial.println("0x293 send failed");
      }
    }
  }
  delay(1);
}
