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

// 重新蓝牙配对: 断开当前 host + 清掉**全部主机槽**的配对记录 + 用当前槽身份重新广播,
// 让设备回到"可被重新配对"的全新状态. 配合在 macOS 蓝牙设置里"移除此设备"即可干净重配.
void repair();

// ---- 多主机切换 (像 Logitech Easy-Switch: 每个槽一个独立蓝牙身份) ----
// StickS3 (NimBLE) 支持多个主机槽, 每槽用一个独立的静态随机蓝牙地址, 各台 Mac 当成
// 各自独立的一个键盘分别配对. Atom (Bluedroid) 不支持, 退化为单槽.

// 主机槽数量 (StickS3 = HOST_COUNT, 其它 = 1).
uint8_t hostCount();

// 当前主机槽下标 (0..hostCount()-1).
uint8_t currentHost();

// 切到下一个主机槽 (循环): 断开当前连接 -> 换成目标槽的蓝牙地址 -> 重新广播,
// 之前在该槽配过对的那台 Mac 会自动重连. 返回切换后的槽下标.
uint8_t switchHost();

}  // namespace ble_kbd
