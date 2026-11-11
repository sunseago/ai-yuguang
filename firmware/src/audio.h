// 提示音：ES8311 DAC + NS4150B 功放，走 I2S 输出。
//
// 所有音效都是**代码合成**的，不打包 WAV：
//   · 打击类（木琴/马林巴/钢琴/竖琴/软铃/八音盒）用「基频 + 分音表」叠加，
//     每个分音有自己的幅度和衰减时间常数 —— 这正是真实乐器音色的主要差别：
//     木琴的高次分音衰减极快（叮），钢琴的衰减慢且有二三次谐波（厚），
//     八音盒几乎是纯高频金属音（脆）。
//   · 水滴/气泡用扫频，海浪用低通噪声，轻风铃是多声错开敲击。
// 好处是不占 flash、每个参数都能按听感调；代价是每帧要算，所以放独立任务里。
#pragma once
#include <stdint.h>

enum : uint8_t {
  DP_SND_OFF = 0,       // 不响
  DP_SND_DROP = 1,      // 水滴
  DP_SND_WAVE = 2,      // 海浪
  DP_SND_BUBBLE = 3,    // 气泡
  DP_SND_XYLO = 4,      // 木琴
  DP_SND_HARP = 5,      // 竖琴
  DP_SND_SOFTBELL = 6,  // 软铃
  DP_SND_PIANO = 7,     // 钢琴单音
  DP_SND_MARIMBA = 8,   // 马林巴
  DP_SND_MUSICBOX = 9,  // 八音盒
  DP_SND_CHIME = 10,    // 轻风铃
  DP_SND_RAIN = 11,     // 雨声
  DP_SND_BIRD = 12,     // 远处鸟鸣
  DP_SND_COUNT = 13,
};

// 初始化 I2S + ES8311。失败只打日志、不阻塞启动：后续 play 全是空操作。
bool audioInit();

// 两个事件各有一个音效槽（0 = 关）：
//   alert = 进 WAIT（弹窗/等确认），done = 进 DONE（任务完成）
uint8_t audioAlertMode();
uint8_t audioDoneMode();
void audioSetAlertMode(uint8_t mode);
void audioSetDoneMode(uint8_t mode);
// 异步播放指定音效（投进队列就返回，不阻塞主循环）
void audioPlay(uint8_t kind);

// 按事件播：complete=false 播 alert 槽，true 播 done 槽
void audioPlayEvent(bool complete);

// 界面/配网页显示用："水滴" / "木琴" / "关" …
const char* audioModeLabel(uint8_t mode);

// 音量 0..100（持久化在 NVS）
uint8_t audioVolume();
void audioSetVolume(uint8_t v);
