// =============================================================================
// status_led.h - 用一颗 RGB LED (WS2812/SK6812) 表达 agent 状态.
//
// 给没有屏幕的设备 (如 M5Atom Lite) 用. 颜色 + 呼吸节奏表达状态:
//   idle (绿): 慢呼吸          busy (黄): 中速呼吸
//   wait (红): 快脉冲(等批准)   portal (蓝): 配网中, 快闪
//   离线/未知 (暗白): 极慢微弱
//
// 底层用 ESP32 自带的 neopixelWrite(), 不依赖外部库.
// =============================================================================
#pragma once

#include <Arduino.h>

namespace status_led {

void begin(uint8_t pin);

// "wait" | "busy" | "idle" | "portal" | "" (离线/未知)
void setStatus(const String& state);

void update(uint32_t now);

}  // namespace status_led
