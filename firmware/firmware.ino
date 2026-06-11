// ============================================================================
// agent_pet - "AI agent 宠物" 固件. 一份代码支持两种 M5Stack 设备:
//
//   * M5StickS3 (有屏): 两颗大眼睛 + 颜色表达 agent 状态, 并走 WiFi mTLS 拉 relay
//        状态. 蓝牙名 "AgentPet".
//       BtnA 单击 = 回车   BtnA 长按1秒 = Shift+回车
//       BtnB 单击 = Esc    BtnB 长按    = 进入 WiFi 配网
//
//   * M5Atom Lite (单键 + 一颗 RGB LED, 无屏): **纯蓝牙键盘**, 不联网、不报状态.
//        蓝牙名 "AgentPet-AtomLite". LED 只做配对/连接指示:
//          蓝色呼吸 = 待配对    绿色呼吸 = 已连接
//       单击 = 回车   长按1秒 = Shift+回车   双击 = Esc
//
// 两者共用纯 BLE HID 键盘 (敲回车/Shift+回车/Esc). Atom 没有 PSRAM, BLE 与 WiFi
// 共存会把内部 RAM 吃光导致 TLS 握手失败, 所以 Atom 干脆只做键盘.
//
// 编译 target 由宏决定: 定义 AGENTPET_ATOM (或板子宏 ARDUINO_M5STACK_ATOM) 走 Atom,
// 否则走 StickS3. 见 firmware/flash.sh 的 TARGET 参数.
// ============================================================================

#if defined(AGENTPET_ATOM) || defined(ARDUINO_M5STACK_ATOM)
  #define IS_ATOM 1
#else
  #define IS_ATOM 0
#endif

#include <M5Unified.h>

#include "app_prefs.h"
#include "ble_kbd.h"

#if IS_ATOM
  #include "status_led.h"
#else
  #include "eyes.h"
  #include "net.h"
#endif

SET_LOOP_TASK_STACK_SIZE(16 * 1024);

#if IS_ATOM
static const char* BLE_NAME = "AgentPet-AtomLite";
#else
static const char* BLE_NAME = "AgentPet";
#endif

// HID usage codes
static constexpr uint8_t KEY_ENTER = 0x28;
static constexpr uint8_t KEY_ESC   = 0x29;
static constexpr uint8_t MOD_SHIFT = 0x02;  // 左 Shift

// 按键动作 (两 target 共用同一套 HID 输出)
static void doEnter()      { ble_kbd::tap(0x00, KEY_ENTER); }
static void doShiftEnter() { ble_kbd::tap(MOD_SHIFT, KEY_ENTER); }
static void doEsc()        { ble_kbd::tap(0x00, KEY_ESC); }

#if !IS_ATOM
// ============================ StickS3 (有屏 + 联网) =========================
static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;

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

static String effectiveState() {
  if (net::relayOk()) return net::state();
  return "";  // relay 不通 -> 未知
}

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

static void checkChime(const String& st) {
  if (st == g_lastState) return;
  if (st == "wait") playChime(true);
  else if (g_lastState == "wait") playChime(false);
  g_lastState = st;
}

static void renderPortal() {
  canvas.fillScreen(COL_BG);
  canvas.setFont(&fonts::efontCN_16);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextColor(COL_BLUE);
  canvas.drawString("WiFi \u914d\u7f51", SCREEN_W / 2, 24);
  canvas.setFont(&fonts::efontCN_14);
  canvas.setTextColor(COL_FG);
  canvas.drawString("\u8fde\u70ed\u70b9: " + net::apSsid(), SCREEN_W / 2, 58);
  canvas.setTextColor(COL_AMBER);
  canvas.drawString("\u6d4f\u89c8\u5668\u6253\u5f00", SCREEN_W / 2, 84);
  canvas.drawString("http://192.168.4.1", SCREEN_W / 2, 106);
  canvas.pushSprite(0, 0);
}

static void renderFrame() {
  if (net::portalActive()) { renderPortal(); return; }

  canvas.fillScreen(COL_BG);
  eyes::render(canvas, COL_BG);

  canvas.fillCircle(9, 9, 4, ble_kbd::connected() ? COL_GREEN : COL_OFF);
  canvas.fillCircle(22, 9, 4, net::wifiConnected() ? COL_BLUE : COL_OFF);
  canvas.fillCircle(35, 9, 4, net::relayOk() ? COL_FG : COL_OFF);

  canvas.setFont(&fonts::efontCN_14);
  String st = effectiveState();

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
    canvas.drawString("WiFi \u672a\u8fde (\u957f\u6309B\u914d\u7f51)", 6, SCREEN_H - 3);
  } else if (!net::relayOk()) {
    canvas.setTextColor(COL_DIM);
    canvas.drawString(clipToWidth("\u7b49 relay (" + net::relayHost() + ")", leftMax), 6, SCREEN_H - 3);
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

static void onAShort() { doEnter();      setToast("\u56de\u8f66"); }
static void onAHold()  { doShiftEnter(); setToast("Shift+\u56de\u8f66"); }
static void onBShort() { doEsc();        setToast("ESC"); }
static void onBHold()  { net::startPortal(); setToast("\u914d\u7f51\u4e2d"); }

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

#else
// ============================ Atom Lite (单键 + RGB LED, 纯键盘) =============
static constexpr uint8_t LED_PIN = 27;  // Atom Lite 板载 SK6812 在 GPIO27
#endif

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  Serial.begin(115200);
  delay(150);
  Serial.println("\n=== agent_pet boot ===");

  app_prefs::begin();

#if !IS_ATOM
  M5.Display.setRotation(1);
  M5.Speaker.begin();
  M5.Power.setExtOutput(false);
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
#else
  status_led::begin(LED_PIN);
  M5.BtnA.setHoldThresh(1000);  // 长按 >= 1s 判为 hold
  ble_kbd::begin(BLE_NAME);     // 纯键盘, 不联网
  #if defined(AGENTPET_CLEAR_BONDS)
  // 专用"清配对"固件: 开机清掉设备侧旧 bond, 解决 macOS 那边忘了/这边没忘导致的
  // 重连密钥不匹配 ("点连接就消失"). 清完正常配一次后, 再烧回普通固件即可.
  ble_kbd::clearBonds();
  Serial.println("[boot] 已执行开机清除蓝牙配对记录");
  #endif
#endif
}

void loop() {
  M5.update();
  uint32_t now = millis();

#if IS_ATOM
  if (M5.BtnA.wasHold())               doShiftEnter();  // 长按1秒
  else if (M5.BtnA.wasDoubleClicked()) doEsc();          // 双击
  else if (M5.BtnA.wasSingleClicked()) doEnter();        // 单击

  // LED 只做配对/连接指示: 绿=已连, 蓝=待配对
  status_led::setStatus(ble_kbd::connected() ? "idle" : "portal");
  status_led::update(now);

  static uint32_t lastReport = 0;
  if (now - lastReport >= 10000) {
    lastReport = now;
    Serial.printf("[%lu] hid=%d heap=%u\n",
                  (unsigned long)now, ble_kbd::connected(), ESP.getFreeHeap());
  }
#else
  handleButtons(now);
  String st = effectiveState();
  eyes::setStatus(st.length() ? st : String("idle"));
  eyes::update(now);
  checkChime(st);
  renderFrame();
  net::loop(now);

  static uint32_t lastReport = 0;
  if (now - lastReport >= 10000) {
    lastReport = now;
    Serial.printf("[%lu] hid=%d wifi=%d relay=%d state=%s heap=%u\n",
                  (unsigned long)now, ble_kbd::connected(), net::wifiConnected(),
                  net::relayOk(), st.c_str(), ESP.getFreeHeap());
  }
#endif

  delay(33);
}
