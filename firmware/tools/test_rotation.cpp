// 旋转公式的主机测试（不需要开发板）。
//
//     g++ -O1 -I src -o /tmp/rot_test tools/test_rotation.cpp && /tmp/rot_test
//
// 参考实现是完全独立的一套：按几何定义把一个像素从 (sx,sy) 挪到 (S-1-sy,sx)，
// 重复 r 次，逐像素生成目标图。不引用被测代码里的任何索引公式。
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../src/rotate.h"
#include "../src/attitude.h"

static const int S = 8;  // 偶数、非 2 的幂，避免行列算错也碰巧对齐

static void buildReference(uint16_t* dst, const uint16_t* src, int r) {
  const int last = S - 1;
  for (int i = 0; i < S * S; i++) dst[i] = 0xBEEF;  // 标记"没被写到"

  for (int sy = 0; sy < S; sy++) {
    for (int sx = 0; sx < S; sx++) {
      int x = sx, y = sy;
      for (int k = 0; k < r; k++) {
        const int nx = last - y;
        const int ny = x;
        x = nx;
        y = ny;
      }
      dst[y * S + x] = src[sy * S + sx];
    }
  }
}

int main() {
  uint16_t src[S * S];
  for (int i = 0; i < S * S; i++) src[i] = (uint16_t)(i + 1);  // 每个像素唯一

  uint16_t got[S * S];
  uint16_t ref[S * S];
  int fails = 0;

  // 1) 正向：和独立参考实现逐像素比对
  for (int r = 0; r < 4; r++) {
    memset(got, 0, sizeof(got));
    dpRotateFrame(got, src, S, (uint8_t)r);
    buildReference(ref, src, r);

    int bad = 0;
    for (int i = 0; i < S * S; i++) {
      if (got[i] != ref[i]) bad++;
    }
    printf("%s r=%d 正向匹配（差异 %d 像素）\n", bad ? "FAIL" : "PASS", r, bad);
    if (bad) fails++;
  }

  // 2) 逆变换：对每个像素，先算它在屏上的位置，再 dpUnrotate 回来必须是原点
  for (int r = 0; r < 4; r++) {
    dpRotateFrame(got, src, S, (uint8_t)r);
    int bad = 0;
    for (int ly = 0; ly < S; ly++) {
      for (int lx = 0; lx < S; lx++) {
        const uint16_t want = src[ly * S + lx];
        // 在旋转后的图里找它
        int px = -1, py = -1;
        for (int i = 0; i < S * S; i++) {
          if (got[i] == want) {
            py = i / S;
            px = i % S;
            break;
          }
        }
        int ux = px, uy = py;
        dpUnrotate(ux, uy, S, (uint8_t)r);
        if (ux != lx || uy != ly) bad++;
      }
    }
    printf("%s r=%d 触摸逆变换（错误 %d 点）\n", bad ? "FAIL" : "PASS", r, bad);
    if (bad) fails++;
  }

  // 3) 四个方向各转一次应该回到原图（旋转 90° 四次 = 恒等）
  {
    uint16_t a[S * S], b[S * S];
    memcpy(a, src, sizeof(a));
    for (int k = 0; k < 4; k++) {
      dpRotateFrame(b, a, S, 1);
      memcpy(a, b, sizeof(a));
    }
    const bool same = memcmp(a, src, sizeof(a)) == 0;
    printf("%s 转 4 次 90° 回到原图\n", same ? "PASS" : "FAIL");
    if (!same) fails++;
  }

  // 4) 姿态映射：两种板子各验一遍
  //
  // 两块板的 QMI8658 安装方向不同：LCD-1.54 的 +X 与面板同向（mirrorX=false），
  // AMOLED-2.16 相反（mirrorX=true）。竖立正/倒两种与镜像无关，必须两块板一样；
  // 左右横放两种在镜像板上要取反。之前把镜像修正写成了全局的，结果修好一块、
  // 弄坏另一块 —— 所以这里两块板都要断言。
  {
    struct Case { float ax, ay; int plain, mirrored; const char* name; };
    const Case cases[] = {
        {0.02f, -0.98f, 0, 0, "竖立（正常）"},
        {0.02f, 0.98f, 2, 2, "竖立（倒过来）"},
        {-0.98f, 0.05f, 3, 1, "横放 A（面板 +X 朝下）"},
        {0.98f, 0.05f, 1, 3, "横放 B（面板 +X 朝上）"},
        // 稍微倾斜也要落在同一象限
        {0.30f, -0.85f, 0, 0, "竖立（倾斜 20°）"},
        {-0.85f, 0.30f, 3, 1, "横放 A（倾斜 20°）"},
    };
    const int n = (int)(sizeof(cases) / sizeof(cases[0]));
    int badPlain = 0, badMirror = 0;
    for (const auto& c : cases) {
      const int gp = (int)dpBaseRotationFromGravity(c.ax, c.ay, false);
      const int gm = (int)dpBaseRotationFromGravity(c.ax, c.ay, true);
      if (gp != c.plain) {
        printf("FAIL [LCD-1.54] %s: ax=%.2f ay=%.2f 期望 r=%d 得到 r=%d\n", c.name, c.ax, c.ay,
               c.plain, gp);
        badPlain++;
      }
      if (gm != c.mirrored) {
        printf("FAIL [AMOLED-2.16] %s: ax=%.2f ay=%.2f 期望 r=%d 得到 r=%d\n", c.name, c.ax, c.ay,
               c.mirrored, gm);
        badMirror++;
      }
    }
    printf("%s 姿态映射 LCD-1.54（%d 个用例）\n", badPlain ? "FAIL" : "PASS", n);
    printf("%s 姿态映射 AMOLED-2.16（%d 个用例）\n", badMirror ? "FAIL" : "PASS", n);
    if (badPlain || badMirror) fails++;
  }

  // 5) 绕一整圈必须是 0,1,2,3 或 0,3,2,1 —— 相邻象限差 90°，
  //    差 180° 就是"倒过来"的 bug。两种板子都要满足。
  {
    const float k = 0.98f;
    const float ring[4][2] = {{0.03f, -k}, {k, 0.03f}, {-0.03f, k}, {-k, -0.03f}};
    for (int mirror = 0; mirror < 2; mirror++) {
      int bad = 0;
      for (int i = 0; i < 4; i++) {
        const int a = (int)dpBaseRotationFromGravity(ring[i][0], ring[i][1], mirror != 0);
        const int b =
            (int)dpBaseRotationFromGravity(ring[(i + 1) % 4][0], ring[(i + 1) % 4][1], mirror != 0);
        const int d = (b - a) & 3;
        if (d != 1 && d != 3) bad++;
      }
      printf("%s 绕一圈相邻象限差 90°（%s，异常 %d 处）\n", bad ? "FAIL" : "PASS",
             mirror ? "AMOLED-2.16" : "LCD-1.54", bad);
      if (bad) fails++;
    }
  }

  printf(fails ? "\n有 %d 项失败\n" : "\n全部通过\n", fails);
  return fails ? 1 : 0;
}
