#include "eyes.h"

#include <math.h>

namespace eyes {

namespace {

constexpr int SCREEN_W = 240;
constexpr int SCREEN_H = 135;

// 眼睛几何
constexpr int EYE_R       = 40;
constexpr int EYE_GAP     = 26;
constexpr int EYE_PAIR_W  = 4 * EYE_R + EYE_GAP;
constexpr int EYE_LEFT_CX = (SCREEN_W - EYE_PAIR_W) / 2 + EYE_R;
constexpr int EYE_RIGHT_CX = SCREEN_W - EYE_LEFT_CX;
constexpr int EYE_CY      = SCREEN_H / 2 + 6;
constexpr int LOOK_RANGE_X = 18;
constexpr int LOOK_RANGE_Y = 10;

enum class Mode { Idle, Busy, Wait };
Mode g_mode = Mode::Idle;

// 颜色 (目标 / 当前, 浮点便于过渡)
float g_curR = 60, g_curG = 200, g_curB = 110;
float g_tgtR = 60, g_tgtG = 200, g_tgtB = 110;

// 动画状态
float g_curLookX = 0, g_curLookY = 0, g_tgtLookX = 0, g_tgtLookY = 0;
float g_curBase = 1.0f, g_tgtBase = 1.0f;   // 基础大小 (eased)
float g_curH = 1.0f, g_tgtH = 1.0f;         // 高度系数 (眨眼压扁)

uint32_t g_lastMs = 0;
uint32_t g_nextBlinkMs = 0;
uint32_t g_blinkUntilMs = 0;
int      g_pendingBlinks = 0;
uint32_t g_nextSaccadeMs = 0;

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float frand(float lo, float hi) { return lo + (hi - lo) * (random(10001) / 10000.0f); }
uint32_t randRange(uint32_t lo, uint32_t hi) { return hi <= lo ? lo : lo + (uint32_t)random((long)(hi - lo + 1)); }

float ease(float cur, float tgt, float dt, float tau) {
  if (tau <= 0) return tgt;
  return cur + (tgt - cur) * (1.0f - expf(-dt / tau));
}

uint16_t to565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void setTargetColor(uint8_t r, uint8_t g, uint8_t b) { g_tgtR = r; g_tgtG = g; g_tgtB = b; }

}  // namespace

void begin() {
  g_lastMs = millis();
  g_nextBlinkMs = g_lastMs + randRange(2000, 5000);
  g_nextSaccadeMs = g_lastMs + randRange(1500, 4000);
}

void setStatus(const String& state) {
  Mode m = Mode::Idle;
  if (state == "wait") m = Mode::Wait;
  else if (state == "busy") m = Mode::Busy;
  if (m == g_mode) return;
  g_mode = m;
  switch (m) {
    case Mode::Idle: setTargetColor(60, 200, 110); g_tgtBase = 1.0f; break;
    case Mode::Busy: setTargetColor(255, 186, 64); g_tgtBase = 1.0f; break;
    case Mode::Wait: setTargetColor(235, 70, 70);  g_tgtBase = 1.12f; break;
  }
}

void update(uint32_t now) {
  float dt = (float)(now - g_lastMs);
  if (dt <= 0) dt = 1;
  if (dt > 100) dt = 100;
  g_lastMs = now;

  // 颜色过渡
  g_curR = ease(g_curR, g_tgtR, dt, 220);
  g_curG = ease(g_curG, g_tgtG, dt, 220);
  g_curB = ease(g_curB, g_tgtB, dt, 220);
  g_curBase = ease(g_curBase, g_tgtBase, dt, 200);

  // 视线: busy 左右扫视; wait 盯住中间; idle 随机扫视
  if (g_mode == Mode::Busy) {
    float t = (now % 1200) / 1200.0f;
    g_tgtLookX = sinf(t * 2.0f * (float)PI) * 0.8f;
    g_tgtLookY = 0;
  } else if (g_mode == Mode::Wait) {
    g_tgtLookX = 0;
    g_tgtLookY = 0;
  } else {
    if (now >= g_nextSaccadeMs) {
      float ang = frand(0, 2 * (float)PI), rad = frand(0.3f, 0.9f);
      g_tgtLookX = clampf(cosf(ang) * rad, -1, 1);
      g_tgtLookY = clampf(sinf(ang) * rad * 0.7f, -1, 1);
      g_nextSaccadeMs = now + randRange(1800, 5000);
    }
  }
  g_curLookX = ease(g_curLookX, g_tgtLookX, dt, 120);
  g_curLookY = ease(g_curLookY, g_tgtLookY, dt, 120);

  // 眨眼调度: wait 更急促
  uint32_t blMin = g_mode == Mode::Wait ? 600 : (g_mode == Mode::Busy ? 1500 : 3000);
  uint32_t blMax = g_mode == Mode::Wait ? 1600 : (g_mode == Mode::Busy ? 4000 : 7000);
  if (now >= g_nextBlinkMs && now > g_blinkUntilMs) {
    g_blinkUntilMs = now + 90;
    if (g_pendingBlinks == 0 && (g_mode == Mode::Wait ? random(100) < 45 : random(100) < 20))
      g_pendingBlinks = 1;  // 偶尔双眨 (wait 概率更高)
    g_nextBlinkMs = now + randRange(blMin, blMax);
  }
  bool blinking = now < g_blinkUntilMs;
  if (!blinking && g_pendingBlinks > 0 && now > g_blinkUntilMs + 80) {
    g_pendingBlinks--;
    g_blinkUntilMs = now + 90;
    blinking = true;
  }
  g_tgtH = blinking ? 0.08f : 1.0f;
  g_curH = ease(g_curH, g_tgtH, dt, 55);
}

void render(M5Canvas& canvas, uint16_t bg) {
  uint32_t now = millis();
  // 呼吸/脉冲: wait 快而强, busy 中等, idle 慢而弱
  float amp = g_mode == Mode::Wait ? 0.12f : (g_mode == Mode::Busy ? 0.05f : 0.03f);
  float period = g_mode == Mode::Wait ? 520.0f : (g_mode == Mode::Busy ? 1600.0f : 3800.0f);
  float breath = 1.0f + amp * sinf((now % (uint32_t)period) / period * 2.0f * (float)PI);
  float scale = g_curBase * breath;

  uint16_t col = to565((uint8_t)g_curR, (uint8_t)g_curG, (uint8_t)g_curB);

  int dx = (int)(clampf(g_curLookX, -1, 1) * LOOK_RANGE_X);
  int dy = (int)(clampf(g_curLookY, -1, 1) * LOOK_RANGE_Y);

  const int cxs[2] = {EYE_LEFT_CX, EYE_RIGHT_CX};
  for (int i = 0; i < 2; ++i) {
    int cx = cxs[i] + dx;
    int cy = EYE_CY + dy;
    int w = (int)(EYE_R * 2.0f * scale + 0.5f);
    int h = (int)(EYE_R * 2.0f * scale * g_curH + 0.5f);
    if (w < 4) w = 4;
    if (h < 2) h = 2;
    int r = std::min(w, h) / 2;
    canvas.fillRoundRect(cx - w / 2, cy - h / 2, w, h, r, col);
  }
}

uint16_t color() { return to565((uint8_t)g_curR, (uint8_t)g_curG, (uint8_t)g_curB); }

}  // namespace eyes
