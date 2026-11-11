// 重力方向 -> 画面旋转量（不含用户偏移）。
//
// 单独抽成 header 是为了能在电脑上跑测试：这个映射在写错时很难用眼睛看出来
// ——典型症状是"四种姿态里只有某两种能对上"。改完务必跑：
//     g++ -O1 -I src -o /tmp/rot_test tools/test_rotation.cpp && /tmp/rot_test
#pragma once
#include <math.h>
#include <stdint.h>

// 返回 0..3，含义是 dpRotateFrame 的旋转量（画面顺时针 90°×r）。
//
// 约定：加速度计静止时读到的是"反作用力"，方向指向物理上方，
// 所以 (ax, ay) 就是物理"上"在屏幕坐标系里的方向（屏幕 x 右、y 下）。
// 画面被顺时针转 90°×r 后，画面的"上"落到屏幕上的方向是：
//     r=0 → (0,-1) 屏上    r=1 → (+1,0) 屏右
//     r=2 → (0,+1) 屏下    r=3 → (-1,0) 屏左
// 让画面看着正，就取使"画面上"对齐物理"上"的那个 r。
//
// 竖立正/倒（|ay| 主导）用几何直觉即可；左右横放（|ax| 主导）要用相反的一对。
// 原因是这块板（Waveshare ESP32-S3-Touch-AMOLED-2.16）上 QMI8658 的 +X 与面板
// 的 +X 相反 —— 这是镜像，不是旋转，整体 offset 吸收不掉。实测症状正是
// "竖立两种都对，左右横放两种各差 180°"。
inline uint8_t dpBaseRotationFromGravity(float ax, float ay) {
  const float dx = -ax;  // 物理"上"在屏幕 x 方向的分量
  const float dy = -ay;  // 物理"上"在屏幕 y 方向的分量

  if (fabsf(dy) >= fabsf(dx)) return dy >= 0 ? 0 : 2;  // 竖立：正 / 倒
  return dx >= 0 ? 1 : 3;                              // 横放：见上面关于 X 极性的说明
}
