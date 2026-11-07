// QMI8658 六轴 IMU，只启用加速度计（判姿态够用，还省电）。
//
// 用途：根据重力方向自动把画面转正。设备平放在桌上时 in-plane 重力接近 0，
// 这时候"哪边是上"本来就没有意义，所以保持当前方向不动；只有明显立起来
// （倾角超过约 30°）才按重力方向切换 0/90/180/270。
#pragma once
#include <stdint.h>

bool imuInit();

bool imuReady();
uint8_t imuAddress();

// 读一次加速度（单位 g）。失败时保持原值不变并返回 false。
bool imuReadAccel(float& ax, float& ay, float& az);

// 采样 + 去抖，返回当前应采用的旋转（0..3，顺时针 90°×n）
uint8_t imuPoll();

// 当前生效的旋转（没变化时不会跳）
uint8_t imuRotation();

// 整体旋转偏移（0..3）。用来校正"面板正上方"相对 IMU 坐标系的绝对朝向，
// 只影响四个方向的绝对角度，不影响它们之间的 90° 关系。
// persist=true 时写进 NVS，重启后仍然有效。
uint8_t imuOffset();
void imuSetOffset(uint8_t off, bool persist);

// 最近一次采样时设备是否接近水平
bool imuIsFlat();

// 给串口/信息页看的原始值
void imuLastSample(float& ax, float& ay, float& az, float& inPlane);
