// =============================================================================
// eyes.h - 两颗大眼睛动画引擎 (从 stick_s3_eyes 抽取并简化为"状态表达").
//
// 用颜色 + 动作表达 agent 状态:
//   idle (绿): 平静圆眼, 偶尔眨眼, 缓慢呼吸
//   busy (黄): 左右扫视 (思考状), 稍快眨眼
//   wait (红): 睁大 + 快脉冲呼吸 + 急促眨眼 (在等你批准)
//
// 另有两个"情绪叠加"表情 (盖在状态之上):
//   瞌睡: idle 下长时间无活动 -> 闭眼 + 冒 "z z Z" 入睡
//         唤醒方式: 按键(poke) / 轻轻移动设备(sense) / 用力晃(sense) / 状态变忙
//   晕眩: sense() 检测到用力晃动 -> 双眼反向螺旋 + 整体摇晃, 并唤醒
//
// 颜色在状态切换时平滑过渡. 晃动检测需调用方喂 IMU (sense), 其余自包含.
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

// 外部"活动"信号: 按键/状态变化等. 唤醒瞌睡并重置发呆计时.
void poke();

// 喂加速度计数据 (g). 睡着时轻轻移动即唤醒; 用力晃动 -> 晕眩并唤醒.
void sense(float ax, float ay, float az, uint32_t now);

// 是否正在打瞌睡 (给 HUD/调用方参考).
bool asleep();

}  // namespace eyes
