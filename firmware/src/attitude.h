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
// 竖立正/倒（|ay| 主导）与 IMU 的安装方向无关，两种板子一样。
// 左右横放（|ax| 主导）取决于 IMU 的 +X 是否与面板 +X 同向：
//
//   · LCD-1.54：同向，用几何直觉（ax<0 → r=3）—— 0.1.6 之前一直是这个，
//     四种姿态实测全对。
//   · AMOLED-2.16：相反（镜像），横放这一对要取反，否则左右横放各差 180°。
//     这是镜像不是旋转，整体 offset 吸收不掉，所以只能按板子区分。
//
// 这个标志放在板级 header 里（IMU_MIRROR_X），不要在这里写死。
inline uint8_t dpBaseRotationFromGravity(float ax, float ay, bool mirrorX) {
  const float dx = -ax;  // 物理"上"在屏幕 x 方向的分量
  const float dy = -ay;  // 物理"上"在屏幕 y 方向的分量

  if (fabsf(dy) >= fabsf(dx)) return dy >= 0 ? 0 : 2;  // 竖立：正 / 倒

  // 横放：镜像板取反
  return mirrorX ? (dx >= 0 ? 1 : 3) : (dx >= 0 ? 3 : 1);
}
