// =============================================================================
// ble_kbd.h - StickS3 当一个**纯** BLE HID 键盘 (只敲键, 不带任何数据服务).
//
// macOS 把它当普通蓝牙键盘连接/配对. 按键直接敲出 HID usage code:
//   BtnA -> 回车 (Enter)  用于批准
//   BtnB -> Esc          用于取消/拒绝
//
// agent 状态不走蓝牙了 (改走 WiFi, 见 net.*), 所以这里没有 NUS, 避免 macOS 上
// "HID + 自定义 GATT 服务共存" 的各种坑.
// =============================================================================
#pragma once

#include <Arduino.h>

namespace ble_kbd {

void begin(const char* name);

// HID host (macOS) 是否已连接.
bool connected();

// 敲一个 chord: 同时按下 mods + key, 再松开. 常用:
//   tap(0x00, 0x28) = Enter;  tap(0x00, 0x29) = Esc;  tap(0x02, 0x28) = Shift+Enter
void tap(uint8_t mods, uint8_t key);

// 清 BLE 绑定 (重新配对用).
void clearBonds();

}  // namespace ble_kbd
