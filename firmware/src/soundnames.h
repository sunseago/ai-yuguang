// 音效编号 -> 中文名。
//
// 单独放 header（纯 inline，不依赖 Arduino）是为了 UI 能在电脑上预览：
// ui.cpp 会被 tools/preview.cpp 链接，如果它直接调 audio.cpp 里的函数，
// 主机端就得把 I2S/ES8311 也一起拖进来。
#pragma once
#include <stdint.h>

inline const char* dpSoundName(uint8_t id) {
  switch (id) {
    case 1: return "水滴";
    case 2: return "海浪";
    case 3: return "气泡";
    case 4: return "木琴";
    case 5: return "竖琴";
    case 6: return "软铃";
    case 7: return "钢琴";
    case 8: return "马林巴";
    case 9: return "八音盒";
    case 10: return "风铃";
    case 11: return "雨声";
    case 12: return "鸟鸣";
    default: return "关";
  }
}

// 音效总数（含"关"），UI 网格按这个上限画
constexpr uint8_t DP_SOUND_COUNT = 13;
