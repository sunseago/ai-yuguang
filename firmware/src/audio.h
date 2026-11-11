// 提示音：ES8311 DAC + NS4150B 功放，走 I2S 输出。
//
// 两种音效都是**代码合成**的，不打包 WAV：
//   · 水滴：音高快速上滑的正弦（700→1600Hz）+ 指数衰减，短促清脆 = "有事找你"
//   · 海浪：低通白噪声 + 慢起慢落包络，柔和收尾 = "做完了"
// 好处是不占 flash、参数随时能调；代价是每帧都要算，所以放在独立任务里。
#pragma once
#include <stdint.h>

enum : uint8_t {
  DP_SND_OFF = 0,   // 不响
  DP_SND_DROP = 1,  // 两种事件都用水滴
  DP_SND_WAVE = 2,  // 两种事件都用海浪
  DP_SND_BOTH = 3,  // 弹窗用水滴、完成用海浪（默认）
};

// 初始化 I2S + ES8311。失败只打日志、不阻塞启动：后续 play 全是空操作，
// 屏幕/网络/按键照常工作（音频是锦上添花，不能因为它起不来就黑屏）。
bool audioInit();

// 当前音效，持久化在 NVS
uint8_t audioMode();
void audioSetMode(uint8_t mode);
// 0→1→2→3→0 循环，返回切换后的值
uint8_t audioCycleMode();

// 异步播放（投进队列就返回，不阻塞主循环）
void audioPlay(uint8_t kind);

// 按事件播：complete=false（弹窗/等确认）complete=true（任务完成）
void audioPlayEvent(bool complete);

// 给界面/配网页显示用："关" / "水滴" / "海浪" / "水滴+海浪"
const char* audioModeLabel(uint8_t mode);
