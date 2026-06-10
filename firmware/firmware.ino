// ============================================================================
// agent_pet - M5StickS3 上的 "AI agent 宠物".
//
// 架构 (拆成两条独立通道, 各管各的, 最稳):
//   1) 蓝牙: StickS3 = 一个**纯 HID 键盘**. macOS 当普通蓝牙键盘连接.
//        BtnA 单击    = 回车 (Enter)  -> 批准
//        BtnA 长按1秒 = Shift+回车     -> 批准 (场景二)
//        BtnB 单击    = Esc           -> 取消/拒绝
//   2) WiFi: agent 状态走 WiFi. StickS3 轮询 Mac 上 relay 的 /state, 拿到聚合
//        状态驱动眼睛颜色: 黄=忙, 红=等待批准, 绿=空闲/完成.
//
// 按键:
//   BtnA 单击 = 回车   BtnA 长按1秒 = Shift+回车
//   BtnB 单击 = Esc    BtnB 长按    = 进入 WiFi 配网 (SoftAP)
// ============================================================================

#include <M5Unified.h>

#include "app_prefs.h"
#include "ble_kbd.h"
#include "eyes.h"
#include "net.h"

SET_LOOP_TASK_STACK_SIZE(16 * 1024);

static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;
static const char* BLE_NAME = "AgentPet";

// HID usage codes
static constexpr uint8_t KEY_ENTER = 0x28;
static constexpr uint8_t KEY_ESC   = 0x29;

static M5Canvas canvas(&M5.Display);

static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

static const uint16_t COL_BG    = rgb565(6, 8, 14);
static const uint16_t COL_FG    = rgb565(235, 238, 245);
static const uint16_t COL_DIM   = rgb565(120, 130, 150);
static const uint16_t COL_GREEN = rgb565(60, 200, 110);
static const uint16_t COL_RED   = rgb565(235, 70, 70);
static const uint16_t COL_AMBER = rgb565(255, 186, 64);
static const uint16_t COL_BLUE  = rgb565(70, 160, 255);
static const uint16_t COL_OFF   = rgb565(55, 60, 72);

static const uint8_t BRIGHT_LEVELS[] = {64, 128, 200};
static int g_brightIdx = 1;

static uint32_t g_btnAAt = 0; static bool g_btnAHold = false;
static uint32_t g_btnBAt = 0; static bool g_btnBHold = false;
static constexpr uint32_t BTN_HOLD_MS = 1000;

static String g_toast; static uint32_t g_toastUntil = 0;
static String g_lastState;

static void applyBrightness() { M5.Display.setBrightness(BRIGHT_LEVELS[g_brightIdx]); }
static void setToast(const String& t) { g_toast = t; g_toastUntil = millis() + 1200; }

static uint16_t stateColor(const String& st) {
  if (st == "wait") return COL_RED;
  if (st == "busy") return COL_AMBER;
  if (st == "idle") return COL_GREEN;
  return COL_DIM;
}

static int utf8Len(uint8_t c) {
  if ((c & 0x80) == 0) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}
static String clipToWidth(const String& s, int maxW) {
  if (canvas.textWidth(s) <= maxW) return s;
  String out = "";
  size_t i = 0;
  while (i < s.length()) {
    int len = utf8Len((uint8_t)s[i]);
    if (i + len > s.length()) len = s.length() - i;
    String g = s.substring(i, i + len);
    if (canvas.textWidth(out + g + "\u2026") > maxW) break;
    out += g;
    i += len;
  }
  return out + "\u2026";
}

static void playChime(bool wait) {
  if (!app_prefs::soundOn()) return;
  M5.Speaker.setVolume(180);
  if (wait) { M5.Speaker.tone(1175, 90); delay(110); M5.Speaker.tone(1568, 150); delay(160); }
  else      { M5.Speaker.tone(988, 110); delay(120); M5.Speaker.tone(1319, 170); delay(180); }
}

// 状态变成 wait 时响一声; 由 wait 变回空闲时柔和一声.
static void checkChime(const String& st) {
  if (st == g_lastState) return;
  if (st == "wait") playChime(true);
  else if (g_lastState == "wait") playChime(false);
  g_lastState = st;
}

static String effectiveState() {
  if (net::relayOk()) return net::state();
  return "";  // relay 不通 -> 未知
}

static void renderPortal() {
  canvas.fillScreen(COL_BG);
  canvas.setFont(&fonts::efontCN_16);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(COL_BLUE);
  canvas.drawString("WiFi \u914d\u7f51", SCREEN_W / 2, 24);  // WiFi 配网
  canvas.setFont(&fonts::efontCN_14);
  canvas.setTextColor(COL_FG);
  canvas.drawString("\u8fde\u70ed\u70b9: " + net::apSsid(), SCREEN_W / 2, 58);  // 连热点
  canvas.setTextColor(COL_AMBER);
  canvas.drawString("\u6d4f\u89c8\u5668\u6253\u5f00", SCREEN_W / 2, 84);          // 浏览器打开
  canvas.drawString("http://192.168.4.1", SCREEN_W / 2, 106);
  canvas.pushSprite(0, 0);
}

static void renderFrame() {
  if (net::portalActive()) { renderPortal(); return; }

  canvas.fillScreen(COL_BG);
  eyes::render(canvas, COL_BG);

  // 顶栏: HID(绿) / WiFi(蓝) / relay(白点表示拿到状态) 三个小点
  canvas.fillCircle(9, 9, 4, ble_kbd::connected() ? COL_GREEN : COL_OFF);
  canvas.fillCircle(22, 9, 4, net::wifiConnected() ? COL_BLUE : COL_OFF);
  canvas.fillCircle(35, 9, 4, net::relayOk() ? COL_FG : COL_OFF);

  // 底部 HUD
  canvas.setFont(&fonts::efontCN_14);
  String st = effectiveState();

  // 右下角: 连上 WiFi 后显示本机 IP
  int leftMax = SCREEN_W - 12;
  if (net::wifiConnected()) {
    String ip = net::localIP();
    canvas.setTextDatum(BR_DATUM);
    canvas.setTextColor(COL_DIM);
    canvas.drawString(ip, SCREEN_W - 6, SCREEN_H - 3);
    leftMax = SCREEN_W - canvas.textWidth(ip) - 18;
  }

  canvas.setTextDatum(BL_DATUM);
  if (!net::wifiConnected()) {
    canvas.setTextColor(COL_DIM);
    canvas.drawString("WiFi \u672a\u8fde (\u957f\u6309B\u914d\u7f51)", 6, SCREEN_H - 3);  // WiFi 未连
  } else if (!net::relayOk()) {
    canvas.setTextColor(COL_DIM);
    canvas.drawString(clipToWidth("\u7b49 relay (" + net::relayHost() + ")", leftMax), 6, SCREEN_H - 3);  // 等 relay
  } else {
    String line = net::label().length() ? net::label() : String("agent");
    if (net::text().length()) line += "  " + net::text();
    canvas.setTextColor(stateColor(st));
    canvas.drawString(clipToWidth(line, leftMax), 6, SCREEN_H - 3);
  }

  if (g_toast.length() && millis() < g_toastUntil) {
    canvas.setFont(&fonts::efontCN_14);
    canvas.setTextDatum(TR_DATUM);
    canvas.setTextColor(COL_BLUE);
    canvas.drawString(g_toast, SCREEN_W - 6, 4);
  }
  canvas.pushSprite(0, 0);
}

static constexpr uint8_t MOD_SHIFT = 0x02;  // 左 Shift
static void onAShort() { ble_kbd::tap(0x00, KEY_ENTER); setToast("\u56de\u8f66"); }             // 回车
static void onAHold()  { ble_kbd::tap(MOD_SHIFT, KEY_ENTER); setToast("Shift+\u56de\u8f66"); }  // Shift+回车
static void onBShort() { ble_kbd::tap(0x00, KEY_ESC); setToast("ESC"); }                        // Esc
static void onBHold()  { net::startPortal(); setToast("\u914d\u7f51\u4e2d"); }                  // 配网中

static void handleButtons(uint32_t now) {
  if (M5.BtnA.isPressed()) {
    if (g_btnAAt == 0) { g_btnAAt = now; g_btnAHold = false; }
    else if (!g_btnAHold && now - g_btnAAt >= BTN_HOLD_MS) { g_btnAHold = true; onAHold(); }
  } else {
    if (g_btnAAt != 0 && !g_btnAHold) onAShort();
    g_btnAAt = 0;
  }
  if (M5.BtnB.isPressed()) {
    if (g_btnBAt == 0) { g_btnBAt = now; g_btnBHold = false; }
    else if (!g_btnBHold && now - g_btnBAt >= BTN_HOLD_MS) { g_btnBHold = true; onBHold(); }
  } else {
    if (g_btnBAt != 0 && !g_btnBHold) onBShort();
    g_btnBAt = 0;
  }
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Speaker.begin();
  M5.Power.setExtOutput(false);

  Serial.begin(115200);
  delay(150);
  Serial.println("\n=== agent_pet boot ===");

  app_prefs::begin();
  g_brightIdx = 1;
  for (int i = 0; i < (int)sizeof(BRIGHT_LEVELS); ++i)
    if (BRIGHT_LEVELS[i] == app_prefs::brightness()) g_brightIdx = i;
  applyBrightness();

  canvas.setPsram(true);
  canvas.setColorDepth(16);
  if (!canvas.createSprite(SCREEN_W, SCREEN_H))
    Serial.println("[boot] canvas createSprite FAILED");

  eyes::begin();

  canvas.fillScreen(COL_BG);
  canvas.setFont(&fonts::efontCN_16);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(COL_BLUE);
  canvas.drawString("agent_pet", SCREEN_W / 2, SCREEN_H / 2 - 10);
  canvas.setTextColor(COL_DIM);
  canvas.drawString("\u542f\u52a8\u4e2d...", SCREEN_W / 2, SCREEN_H / 2 + 12);
  canvas.pushSprite(0, 0);

  ble_kbd::begin(BLE_NAME);
  net::begin();
}

void loop() {
  M5.update();
  uint32_t now = millis();

  handleButtons(now);
  net::loop(now);

  String st = effectiveState();
  eyes::setStatus(st.length() ? st : String("idle"));
  eyes::update(now);
  checkChime(st);
  renderFrame();

  static uint32_t lastReport = 0;
  if (now - lastReport >= 10000) {
    lastReport = now;
    Serial.printf("[%lu] hid=%d wifi=%d relay=%d state=%s heap=%u\n",
                  (unsigned long)now, ble_kbd::connected(), net::wifiConnected(),
                  net::relayOk(), st.c_str(), ESP.getFreeHeap());
  }

  delay(33);
}
