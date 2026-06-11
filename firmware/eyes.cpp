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

// ---- 瞌睡 / 晕眩 叠加状态 ----
constexpr uint32_t SLEEP_AFTER_MS = 20000;  // idle 下无活动多久后入睡
constexpr uint32_t DIZZY_MS       = 2200;   // 一次晕眩持续时长
constexpr float    SHAKE_TRIGGER  = 7.0f;   // 晃动强度累积阈值 (越大越难触发晕眩)
constexpr float    WAKE_JERK      = 0.12f;  // 睡着时, 单帧加速度变化超过此值即唤醒
                                            // (略高于静置噪声, 轻轻动设备就能触发)

uint32_t g_lastActiveMs = 0;   // 最近一次活动
bool     g_asleep       = false;
uint32_t g_dizzyUntil   = 0;   // <now 表示不在晕眩
float    g_shakeAccum   = 0;   // 晃动强度 (衰减累积)
float    g_prevMag      = 1.0f;

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
  g_lastActiveMs = g_lastMs;
  g_nextBlinkMs = g_lastMs + randRange(2000, 5000);
  g_nextSaccadeMs = g_lastMs + randRange(1500, 4000);
}

void poke() {
  g_lastActiveMs = millis();
  if (g_asleep) {
    g_asleep = false;
    g_curH = 0.05f;  // 从闭眼状态睁开 (随后 ease 回 1)
  }
}

void sense(float ax, float ay, float az, uint32_t now) {
  // 用加速度模相对前一帧的变化 (jerk) 衰减累积. 只有持续用力晃才会越过阈值,
  // 缓慢倾斜/自动旋转那种小抖动会被衰减掉.
  float mag = sqrtf(ax * ax + ay * ay + az * az);
  float jerk = fabsf(mag - g_prevMag);
  g_prevMag = mag;
  g_shakeAccum = g_shakeAccum * 0.80f + jerk;

  // 用力晃 -> 晕眩 (顺带唤醒)
  if (g_shakeAccum > SHAKE_TRIGGER && now >= g_dizzyUntil) {
    g_dizzyUntil = now + DIZZY_MS;
    g_shakeAccum = 0;
    poke();
    return;
  }
  // 睡着时, 轻轻移动一下也能唤醒 (不晕眩). 醒着时小幅移动不重置发呆计时,
  // 这样静置仍能正常入睡.
  if (g_asleep && jerk > WAKE_JERK) poke();
}

bool asleep() { return g_asleep; }

void setStatus(const String& state) {
  Mode m = Mode::Idle;
  if (state == "wait") m = Mode::Wait;
  else if (state == "busy") m = Mode::Busy;
  if (m == g_mode) return;
  g_mode = m;
  if (m != Mode::Idle) poke();  // 进入忙/等 = 有活动, 唤醒并续命
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

  // 瞌睡判定: 只有 idle 且不在晕眩时才会发呆入睡; 忙/等表示有事做, 保持清醒.
  // 注意: poke() 用 millis() 写 g_lastActiveMs, 可能比循环顶部取的 now 略大,
  // 无符号相减会下溢成超大值 -> 刚唤醒又秒睡. 故用 saturating 减法.
  bool dizzy = now < g_dizzyUntil;
  uint32_t idleFor = (now > g_lastActiveMs) ? (now - g_lastActiveMs) : 0;
  if (g_mode != Mode::Idle || dizzy) {
    g_lastActiveMs = now;
    g_asleep = false;
  } else if (!g_asleep && idleFor > SLEEP_AFTER_MS) {
    g_asleep = true;
  }

  // 睡着时: 视线归中、闭眼, 跳过扫视/眨眼调度.
  if (g_asleep) {
    g_tgtLookX = 0; g_tgtLookY = 0;
    g_curLookX = ease(g_curLookX, 0, dt, 200);
    g_curLookY = ease(g_curLookY, 0, dt, 200);
    g_tgtH = 0.06f;
    g_curH = ease(g_curH, g_tgtH, dt, 260);  // 慢慢闭上
    return;
  }

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

namespace {

// 晕眩: 两眼各画一条阿基米德螺旋, 左右反向旋转, 整体随时间左右摇晃.
void renderDizzy(M5Canvas& canvas, uint16_t col, uint32_t now) {
  int wob = (int)(sinf(now / 70.0f) * 8.0f);          // 整体摇晃
  float phase = now / 130.0f;                          // 旋转相位
  const int cxs[2] = {EYE_LEFT_CX, EYE_RIGHT_CX};
  const int TURNS = 3;
  const int STEPS = TURNS * 26;
  for (int i = 0; i < 2; ++i) {
    int cx = cxs[i] + wob, cy = EYE_CY;
    int px = cx, py = cy;
    for (int s = 0; s <= STEPS; ++s) {
      float t = (float)s / 26.0f;                      // 圈数 0..TURNS
      float ang = t * 2.0f * (float)PI + phase * (i == 0 ? 1.0f : -1.0f);
      float r = (t / TURNS) * (EYE_R - 2);
      int x = cx + (int)(cosf(ang) * r);
      int y = cy + (int)(sinf(ang) * r);
      canvas.drawLine(px, py, x, y, col);
      // 加粗一点, 看得清楚
      canvas.drawLine(px + 1, py, x + 1, y, col);
      px = x; py = y;
    }
  }
}

// 瞌睡: 右上方循环升起的 "z z Z", 越往上越大, 营造冒泡感.
void drawZzz(M5Canvas& canvas, uint16_t col, uint32_t now) {
  const lgfx::IFont* fnts[3] = {&fonts::Font2, &fonts::Font4, &fonts::Font6};
  int bx = EYE_RIGHT_CX + 24;
  int by = EYE_CY - 6;
  canvas.setTextColor(col);
  canvas.setTextDatum(textdatum_t::middle_center);
  for (int i = 0; i < 3; ++i) {
    float ph = now / 900.0f + i * 0.55f;
    float f = ph - floorf(ph);                          // 0..1 循环
    int x = bx + i * 14 + (int)(f * 6);
    int y = by - i * 16 - (int)(f * 18);
    canvas.setFont(fnts[i]);
    canvas.drawString("z", x, y);
  }
}

}  // namespace

void render(M5Canvas& canvas, uint16_t bg) {
  uint32_t now = millis();
  uint16_t col = to565((uint8_t)g_curR, (uint8_t)g_curG, (uint8_t)g_curB);

  // 晕眩盖在一切之上
  if (now < g_dizzyUntil) { renderDizzy(canvas, col, now); return; }

  // 呼吸/脉冲: wait 快而强, busy 中等, idle/睡眠 慢而弱
  float amp = g_mode == Mode::Wait ? 0.12f : (g_mode == Mode::Busy ? 0.05f : 0.03f);
  float period = g_mode == Mode::Wait ? 520.0f : (g_mode == Mode::Busy ? 1600.0f : 3800.0f);
  if (g_asleep) { amp = 0.02f; period = 4200.0f; }     // 睡着时极缓的"起伏"
  float breath = 1.0f + amp * sinf((now % (uint32_t)period) / period * 2.0f * (float)PI);
  float scale = g_curBase * breath;

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

  if (g_asleep) drawZzz(canvas, col, now);
}

uint16_t color() { return to565((uint8_t)g_curR, (uint8_t)g_curG, (uint8_t)g_curB); }

}  // namespace eyes
