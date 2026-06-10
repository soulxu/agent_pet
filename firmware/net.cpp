#include "net.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "certs.h"  // CLIENT_CERT / CLIENT_KEY / RELAY_CERT (由 relay/gen_certs.sh 生成)

namespace net {

namespace {

constexpr const char* NS = "petnet";
constexpr uint32_t POLL_MS = 1500;        // 轮询 relay 间隔 (TLS 握手有成本, 别太频)
constexpr uint32_t WIFI_WAIT_MS = 15000;  // 连不上多久就开配网

String g_ssid, g_pass, g_relay;
String g_state, g_label, g_text;
bool   g_relayOk = false;

SemaphoreHandle_t g_mutex = nullptr;       // 保护上面几个跨 task 共享的字段

WebServer g_server(80);
bool      g_portal = false;
bool      g_httpUp = false;
String    g_apSsid;

uint32_t g_lastPoll = 0;
uint32_t g_staStart = 0;
bool     g_staTried = false;

void lock()   { if (g_mutex) xSemaphoreTake(g_mutex, portMAX_DELAY); }
void unlock() { if (g_mutex) xSemaphoreGive(g_mutex); }

void loadCfg() {
  Preferences p;
  if (p.begin(NS, true)) {
    g_ssid = p.getString("ssid", "");
    g_pass = p.getString("pass", "");
    g_relay = p.getString("relay", "");
    p.end();
  }
}

void saveCfg(const String& ssid, const String& pass, const String& relay) {
  Preferences p;
  if (p.begin(NS, false)) {
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    p.putString("relay", relay);
    p.end();
  }
}

String macTail() {
  uint8_t m[6];
  WiFi.macAddress(m);
  char buf[5];
  snprintf(buf, sizeof(buf), "%02X%02X", m[4], m[5]);
  return String(buf);
}

String htmlPage() {
  String s = F("<!doctype html><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>agent_pet 配网</title>"
               "<style>body{font-family:-apple-system,system-ui,sans-serif;max-width:420px;"
               "margin:24px auto;padding:0 14px}input{width:100%;padding:8px;margin:6px 0 14px;"
               "box-sizing:border-box;font-size:15px}button{padding:10px 16px;font-size:15px}"
               "label{font-weight:600}</style><h2>agent_pet 配网</h2>"
               "<form method=POST action=/save>"
               "<label>WiFi 名称 (SSID)</label><input name=ssid value='");
  s += g_ssid;
  s += F("'><label>WiFi 密码</label><input name=pass type=password value='");
  s += g_pass;
  s += F("'><label>Relay 地址 (Mac IP:端口)</label><input name=relay placeholder='192.168.1.10:8443' value='");
  s += g_relay;
  s += F("'><button type=submit>保存并重启</button></form>"
         "<p style='color:#888;font-size:13px'>在 Mac 上跑 relay.py (HTTPS mTLS), 这里填 Mac 的"
         "局域网 IP 和 HTTPS 端口 (默认 8443).</p>");
  return s;
}

void handleRoot() { g_server.send(200, "text/html; charset=utf-8", htmlPage()); }

void handleSave() {
  String ssid = g_server.arg("ssid");
  String pass = g_server.arg("pass");
  String relay = g_server.arg("relay");
  relay.trim();
  saveCfg(ssid, pass, relay);
  g_server.send(200, "text/html; charset=utf-8",
                "<meta charset=utf-8><h3>已保存, 正在重启...</h3>");
  delay(800);
  ESP.restart();
}

void startHttp() {
  if (g_httpUp) return;
  g_server.on("/", handleRoot);
  g_server.on("/save", HTTP_POST, handleSave);
  g_server.onNotFound(handleRoot);
  g_server.begin();
  g_httpUp = true;
}

void beginPortal() {
  g_portal = true;
  g_apSsid = "AgentPet-" + macTail();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(g_apSsid.c_str());
  startHttp();
  Serial.printf("[net] portal up: SSID '%s'  http://192.168.4.1\n", g_apSsid.c_str());
}

void beginSta() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(g_ssid.c_str(), g_pass.c_str());
  g_staStart = millis();
  g_staTried = true;
  Serial.printf("[net] connecting WiFi '%s' ...\n", g_ssid.c_str());
}

void poll() {
  if (g_relay.length() == 0) {
    Serial.println("[net] 未配置 relay 地址");
    lock(); g_relayOk = false; unlock();
    return;
  }

  String url = g_relay;
  if (!url.startsWith("http")) url = "https://" + url;  // 默认 https
  url += "/state";

  // 持久复用 TLS 客户端: 避免每次 new WiFiClientSecure 解析证书造成的堆泄漏
  static WiFiClientSecure client;
  static bool inited = false;
  if (!inited) {
    // 注意: 这个 core 里 setInsecure() 会导致**不发送**客户端证书 (insecure 与
    // client cert 互斥), 所以必须走 secure 模式: 用 relay 自签证书当 CA 验证 server.
    // server 证书的 SAN 里带了 Mac 局域网 IP, 所以按 IP 连也能通过校验.
    client.setCACert(RELAY_CERT);
    client.setCertificate(CLIENT_CERT);   // 出示客户端证书 -> relay 做"公钥认证"
    client.setPrivateKey(CLIENT_KEY);
    client.setHandshakeTimeout(8);
    inited = true;
  }

  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(3000);
  http.setReuse(false);

  bool ok = false;
  String st, lb, tx;
  if (http.begin(client, url)) {
    int code = http.GET();
    if (code == 200) {
      String body = http.getString();
      JsonDocument doc;
      if (!deserializeJson(doc, body)) {
        st = String((const char*)(doc["state"] | ""));
        lb = String((const char*)(doc["label"] | ""));
        tx = String((const char*)(doc["text"] | ""));
        ok = true;
      } else {
        Serial.println("[net] JSON 解析失败");
      }
    } else {
      char errbuf[160] = {0};
      client.lastError(errbuf, sizeof(errbuf));
      Serial.printf("[net] GET 失败 code=%d %s\n", code, errbuf);
    }
    http.end();
  } else {
    Serial.println("[net] http.begin 失败 (URL?)");
  }
  client.stop();

  lock();
  g_relayOk = ok;
  if (ok) { g_state = st; g_label = lb; g_text = tx; }
  unlock();
}

void netTask(void*) {
  if (g_ssid.length() == 0) beginPortal();
  else beginSta();

  for (;;) {
    uint32_t now = millis();
    if (g_portal) {
      g_server.handleClient();
    } else if (WiFi.status() == WL_CONNECTED) {
      startHttp();
      g_server.handleClient();
      if (now - g_lastPoll >= POLL_MS) { g_lastPoll = now; poll(); }
    } else if (g_staTried && now - g_staStart > WIFI_WAIT_MS) {
      beginPortal();  // 连不上 -> 开配网门户
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

}  // namespace

void begin() {
  g_mutex = xSemaphoreCreateMutex();
  loadCfg();
  // mbedTLS 握手(ECDHE/ECDSA/cipher)很吃栈: 16~20KB 会栈溢出, 握手失败报成
  // -1(connection refused). 参考 agent_terminal_arduino 的 SSH 任务, 给 48KB.
  xTaskCreatePinnedToCore(netTask, "netTask", 49152, nullptr, 1, nullptr, 1);
}

void loop(uint32_t) { /* 网络都在 netTask 里, 主循环只读 getters */ }

bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }
bool portalActive()  { return g_portal; }
String apSsid()      { return g_apSsid; }
String localIP()     { return WiFi.localIP().toString(); }
String relayHost()   { return g_relay; }

bool relayOk()   { lock(); bool v = g_relayOk; unlock(); return v; }
String state()   { lock(); String v = g_state; unlock(); return v; }
String label()   { lock(); String v = g_label; unlock(); return v; }
String text()    { lock(); String v = g_text;  unlock(); return v; }

void startPortal() {
  if (!g_portal) beginPortal();
}

}  // namespace net
