// 屏幕电源策略单测：纯逻辑，不需要板子。
//
//   cd firmware
//   g++ -std=c++17 -O1 -I src -o /tmp/scr tools/test_screenlogic.cpp && /tmp/scr
//
// 这块逻辑分支多（配网 / 唤醒 / 淡出 / 墙插与电池两套阈值 / 熄屏的两个来源），
// 靠"在板子上看现象"覆盖不全，所以先在这里把边界钉死。
#include <cstdio>
#include <cstring>

#include "screenlogic.h"

static int fails = 0;

static const char* name(ScrMode m) {
  switch (m) {
    case SCR_ON: return "ON";
    case SCR_DIM: return "DIM";
    case SCR_FADE: return "FADE";
    case SCR_OFF: return "OFF";
  }
  return "?";
}

static void check(const char* label, ScrMode got, ScrMode want) {
  const bool ok = got == want;
  if (!ok) fails++;
  printf("%s  %-46s got=%-4s want=%s\n", ok ? "PASS" : "FAIL", label, name(got), name(want));
}

// 默认阈值：墙插 5 分钟变暗 / 30 分钟熄；电池 1 分钟变暗 / 10 分钟熄；断链 60 秒熄
static const ScrCfg kCfg = {
    300000,   // dimAfterIdleWallMs
    60000,    // dimAfterIdleBattMs
    1800000,  // offAfterIdleWallMs
    600000,   // offAfterIdleBattMs
    60000,    // offAfterNolinkMs
    10000,    // peekMs
    1500,     // fadeMs
};

// 一个"正常在用"的基线，各用例只改关心的字段
static ScrIn base() {
  ScrIn in;
  memset(&in, 0, sizeof(in));
  in.linkOk = true;
  in.onBattery = false;
  in.hostIdleS = 5;
  in.linkAgeMs = 0;
  in.sinceInputMs = 999999;  // 很久没摸过板子
  in.modeAgeMs = 999999;
  return in;
}

int main() {
  // ---- 1. 开机但连不上桥接（电脑关机/只插充电器）----
  // 配网门户在 netBegin() 里阻塞运行，根本不走这里，所以这里不需要"一直亮"的保护。
  // 曾经有这条保护，结果板子插充电器重上电后永远连不上桥接 → 永远亮着 NO LINK。
  {
    ScrIn in = base();
    in.linkOk = false;
    in.linkAgeMs = 3600000;  // 早就超了断链阈值
    check("开机连不上桥接 → 也要按断链熄屏", scrDecide(SCR_ON, in, kCfg), SCR_FADE);
  }

  // ---- 2. 正常在用的几种 ----
  check("电脑在用（刚有输入） → 亮", scrDecide(SCR_ON, base(), kCfg), SCR_ON);
  {
    ScrIn in = base();
    in.hostIdleS = 301;  // 刚过 5 分钟
    check("墙插：闲置 301 秒 → 变暗", scrDecide(SCR_ON, in, kCfg), SCR_DIM);
  }
  {
    ScrIn in = base();
    in.hostIdleS = 299;
    check("墙插：闲置 299 秒 → 仍亮", scrDecide(SCR_DIM, in, kCfg), SCR_ON);
  }
  {
    ScrIn in = base();
    in.hostIdleS = -1;  // 桥接读不到
    check("闲置读数未知(-1) → 当作在用，保持亮", scrDecide(SCR_DIM, in, kCfg), SCR_ON);
  }

  // ---- 3. 电池模式阈值更紧 ----
  {
    ScrIn in = base();
    in.onBattery = true;
    in.hostIdleS = 61;
    check("电池：闲置 61 秒 → 变暗（阈值 1 分钟）", scrDecide(SCR_ON, in, kCfg), SCR_DIM);
  }
  {
    ScrIn in = base();
    in.onBattery = true;
    in.hostIdleS = 59;
    check("电池：闲置 59 秒 → 仍亮", scrDecide(SCR_DIM, in, kCfg), SCR_ON);
  }

  // ---- 4. 熄屏的两个来源：断链 & 闲置过久 ----
  {
    ScrIn in = base();
    in.linkOk = false;
    in.linkAgeMs = 59000;
    check("断链 59 秒 → 还亮着（60 秒阈值内）", scrDecide(SCR_ON, in, kCfg), SCR_ON);
  }
  {
    ScrIn in = base();
    in.linkOk = false;
    in.linkAgeMs = 61000;
    check("断链 61 秒 → 进入淡出", scrDecide(SCR_ON, in, kCfg), SCR_FADE);
  }
  {
    ScrIn in = base();
    in.linkOk = true;
    in.hostIdleS = 1799;  // 29.98 分钟
    check("墙插：闲置 1799 秒 → 仍是暗档（未到 30 分钟）", scrDecide(SCR_DIM, in, kCfg), SCR_DIM);
  }
  {
    ScrIn in = base();
    in.linkOk = true;
    in.hostIdleS = 1801;  // 30 分钟过一点，电脑还开着（显示器休眠的场景）
    check("墙插：闲置 1801 秒 → 进入淡出（电脑还开着也要熄）", scrDecide(SCR_ON, in, kCfg), SCR_FADE);
  }
  {
    ScrIn in = base();
    in.onBattery = true;
    in.linkOk = true;
    in.hostIdleS = 601;  // 10 分钟过一点
    check("电池：闲置 601 秒 → 进入淡出（阈值 10 分钟）", scrDecide(SCR_DIM, in, kCfg), SCR_FADE);
  }
  {
    ScrIn in = base();
    in.linkOk = false;
    in.linkAgeMs = 61000;
    in.modeAgeMs = 1499;
    check("淡出未满 1.5 秒 → 仍是淡出", scrDecide(SCR_FADE, in, kCfg), SCR_FADE);
  }
  {
    ScrIn in = base();
    in.linkOk = false;
    in.linkAgeMs = 61000;
    in.modeAgeMs = 1500;
    check("淡出满 1.5 秒 → 熄屏", scrDecide(SCR_FADE, in, kCfg), SCR_OFF);
  }

  // ---- 5. 唤醒 ----
  {
    ScrIn in = base();
    in.linkOk = false;
    in.linkAgeMs = 3600000;
    in.sinceInputMs = 100;
    check("熄屏后摸一下 → 亮（peek）", scrDecide(SCR_OFF, in, kCfg), SCR_ON);
  }
  {
    ScrIn in = base();
    in.linkOk = false;
    in.linkAgeMs = 3600000;
    in.sinceInputMs = 10001;
    check("peek 10 秒到期 → 回到淡出/熄屏", scrDecide(SCR_ON, in, kCfg), SCR_FADE);
  }
  {
    ScrIn in = base();
    in.sinceInputMs = 5000;
    in.hostIdleS = 3600;
    check("有人正在摸板子 → 亮，不改暗", scrDecide(SCR_DIM, in, kCfg), SCR_ON);
  }

  // ---- 6. 电脑回来了 ----
  {
    ScrIn in = base();
    in.linkOk = true;
    in.hostIdleS = 10;
    in.sinceInputMs = 200;  // 由 screenpower 在 link 恢复时置位
    check("电脑重新上线 → 先亮一下（peek）", scrDecide(SCR_OFF, in, kCfg), SCR_ON);
  }
  {
    // 鼠标一动，hostIdleS 掉回 0，即使板子没被摸也该自己亮
    ScrIn in = base();
    in.hostIdleS = 0;
    in.sinceInputMs = 999999;
    check("鼠标动了一下（idle 归零）→ 自己亮", scrDecide(SCR_OFF, in, kCfg), SCR_ON);
  }

  // ---- 7. 一直在熄屏，别把 OFF 自己抖回 FADE ----
  {
    ScrIn in = base();
    in.linkOk = false;
    in.linkAgeMs = 3600000;
    check("已经是 OFF → 保持 OFF（不反复走淡出）", scrDecide(SCR_OFF, in, kCfg), SCR_OFF);
  }

  printf("\n%s\n", fails == 0 ? "全部通过" : "有失败项");
  return fails == 0 ? 0 : 1;
}
