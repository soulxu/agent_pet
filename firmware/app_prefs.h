// =============================================================================
// app_prefs.h - 轻量配置 (NVS Preferences), namespace = "apprefs".
//   - brightness: 屏幕亮度 0..255 (默认 128)
//   - soundOn:    提示音开关 (默认开)
// 蓝牙连接信息 (HID 配对 + relay NUS) 由 BLE 栈各自管理, 不在这里.
// =============================================================================
#pragma once

#include <Arduino.h>

namespace app_prefs {

void begin();

uint8_t brightness();
void    setBrightness(uint8_t v);

bool soundOn();
void setSoundOn(bool v);

}  // namespace app_prefs
