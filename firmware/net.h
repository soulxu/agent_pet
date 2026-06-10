// =============================================================================
// net.h - WiFi: 连接 + 首次/手动 SoftAP 配网 + 轮询 relay 的 /state 拿 agent 状态.
//
// agent 状态改走 WiFi (不再走蓝牙). StickS3 连上家里/公司 WiFi 后, 定时向 Mac 上
// relay 的 http://<relay>/state 发 GET, 拿到聚合状态驱动眼睛颜色.
//
// 配置存 NVS (namespace "petnet"): ssid / pass / relay(形如 "192.168.1.10:8799").
// 没配置或连不上 -> 开 SoftAP "AgentPet-XXXX", 浏览器打开 http://192.168.4.1 填写.
// =============================================================================
#pragma once

#include <Arduino.h>

namespace net {

void begin();
void loop(uint32_t now);

bool   wifiConnected();
bool   portalActive();     // 正在 SoftAP 配网
String apSsid();
String localIP();
String relayHost();

// 最近从 relay /state 拿到的快照
bool   relayOk();          // 最近一次轮询成功
String state();            // "wait" | "busy" | "idle" | "" (未知)
String label();
String text();

void startPortal();        // 手动进入配网 (长按按键触发)

}  // namespace net
