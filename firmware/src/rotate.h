// 整帧旋转与触摸逆变换。
//
// 单独抽出来是为了能在电脑上直接跑测试（不依赖 Arduino）——
// 之前这块写错过两次（把 270° 写成了垂直翻转），肉眼没人看得出来，
// 上机才发现画面是镜像的。改这里的公式之后务必跑：
//     g++ -O1 -I src -o /tmp/rot_test tools/test_rotation.cpp && /tmp/rot_test
#pragma once
#include <stdint.h>

// 把 S×S 的 RGB565 帧缓冲顺时针旋转 90°×r（r=0..3）写进 dst。
// dst 与 src 必须是两块不同的缓冲（r=0 时允许相同）。
inline void dpRotateFrame(uint16_t* dst, const uint16_t* src, int S, uint8_t r) {
  const int last = S - 1;
  switch (r & 3) {
    case 0:
      if (dst != src) {
        for (int i = 0; i < S * S; i++) dst[i] = src[i];
      }
      return;
    case 1:  // dst[y][x] = src[last-x][y]
      for (int y = 0; y < S; y++) {
        uint16_t* d = dst + y * S;
        for (int x = 0; x < S; x++) d[x] = src[(last - x) * S + y];
      }
      return;
    case 2:  // dst[y][x] = src[last-y][last-x]
      for (int y = 0; y < S; y++) {
        uint16_t* d = dst + y * S;
        const uint16_t* s = src + (last - y) * S;
        for (int x = 0; x < S; x++) d[x] = s[last - x];
      }
      return;
    default:  // 270°：dst[y][x] = src[x][last-y]
      for (int y = 0; y < S; y++) {
        uint16_t* d = dst + y * S;
        for (int x = 0; x < S; x++) d[x] = src[x * S + (last - y)];
      }
      return;
  }
}

// 屏幕坐标 -> UI 逻辑坐标（dpRotateFrame 的逆运算）。
// 触摸必须过这一道，否则转过之后点卡片会点到别的地方、滑动手势方向也是反的。
inline void dpUnrotate(int& x, int& y, int S, uint8_t r) {
  const int last = S - 1;
  const int px = x;
  const int py = y;
  switch (r & 3) {
    case 1:  // 屏上 (px,py) 存的是 src[last-px][py]
      x = py;
      y = last - px;
      break;
    case 2:
      x = last - px;
      y = last - py;
      break;
    case 3:  // 屏上 (px,py) 存的是 src[px][last-py]
      x = last - py;
      y = px;
      break;
    default:
      break;
  }
}
