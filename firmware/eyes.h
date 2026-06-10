// =============================================================================
// eyes.h - 两颗大眼睛动画引擎 (从 stick_s3_eyes 抽取并简化为"状态表达").
//
// 用颜色 + 动作表达 agent 状态:
//   idle (绿): 平静圆眼, 偶尔眨眼, 缓慢呼吸
//   busy (黄): 左右扫视 (思考状), 稍快眨眼
//   wait (红): 睁大 + 快脉冲呼吸 + 急促眨眼 (在等你批准)
//
// 颜色在状态切换时平滑过渡. 自包含, 不依赖 WiFi/ASR/IMU.
// =============================================================================
#pragma once

#include <M5Unified.h>

namespace eyes {

void begin();

// "wait" | "busy" | "idle" (其它当 idle).
void setStatus(const String& state);

// 推进动画 (内部按 millis 算 dt).
void update(uint32_t now);

// 画到 canvas (调用方负责 pushSprite).
void render(M5Canvas& canvas, uint16_t bg);

// 当前眼睛主色 (565), 给 HUD 用.
uint16_t color();

}  // namespace eyes
