#include "status_led.h"

#include <math.h>

namespace status_led {

namespace {

uint8_t g_pin = 27;
String  g_state = "";
uint8_t g_r = 60, g_g = 60, g_b = 60;
uint8_t g_brightCap = 60;   // 整体亮度上限 (LED 很刺眼, 压一压)

void baseColor(const String& s, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (s == "wait")        { r = 255; g = 30;  b = 30;  }
  else if (s == "busy")   { r = 255; g = 150; b = 10;  }
  else if (s == "idle")   { r = 30;  g = 220; b = 90;  }
  else if (s == "portal") { r = 40;  g = 120; b = 255; }
  else                    { r = 120; g = 120; b = 120; }  // 离线/未知
}

}  // namespace

void begin(uint8_t pin) {
  g_pin = pin;
  baseColor("", g_r, g_g, g_b);
  neopixelWrite(g_pin, 0, 0, 0);
}

void setStatus(const String& s) {
  if (s == g_state) return;
  g_state = s;
  baseColor(s, g_r, g_g, g_b);
}

void update(uint32_t now) {
  float amp, period;
  if (g_state == "wait")        { amp = 0.85f; period = 480.0f; }
  else if (g_state == "busy")   { amp = 0.45f; period = 1500.0f; }
  else if (g_state == "portal") { amp = 0.90f; period = 600.0f; }
  else if (g_state == "idle")   { amp = 0.30f; period = 3600.0f; }
  else                          { amp = 0.20f; period = 4000.0f; }  // 离线

  float lo = 1.0f - amp;
  float f = lo + amp * 0.5f * (1.0f + sinf((now % (uint32_t)period) / period * 2.0f * (float)PI));
  float k = f * (g_brightCap / 255.0f);
  neopixelWrite(g_pin, (uint8_t)(g_r * k), (uint8_t)(g_g * k), (uint8_t)(g_b * k));
}

}  // namespace status_led
