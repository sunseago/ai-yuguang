// 屏幕电源策略单测：纯逻辑，不需要板子。
//
//   cd firmware
//   g++ -std=c++17 -O1 -I src -o /tmp/scr tools/test_screenlogic.cpp && /tmp/scr
//
// 这块逻辑分支多（配网 / 唤醒 / 淡出 / USB 与电池两套阈值），
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

static const char* name2(ScrMode m) {
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
  printf("%s  %-46s got=%-4s want=%s\n", ok ? "PASS" : "FAIL", label, name(got), name2(want));
}

// 默认阈值：USB 5 分钟变暗 / 15 分钟熄；电池 1 分钟变暗 / 15 秒熄
static const ScrCfg kCfg = {
    300000,  // dimAfterIdleUsbMs
    60000,   // dimAfterIdleBatMs
    900000,  // offAfterNolinkUsbMs
    15000,   // offAfterNolinkBatMs
    10000,   // peekMs
    1500,    // fadeMs
};

// 一个"正常在用"的基线，各用例只改关心的字段
static ScrIn base() {
  ScrIn in;
  memset(&in, 0, sizeof(in));
  in.linkOk = true;
  in.usbHost = true;
  in.hostIdleS = 5;
  in.linkAgeMs = 0;
  in.sinceInputMs = 999999;  // 很久没摸过板子
  in.everLinked = true;
  in.modeAgeMs = 999999;
  return in;
}

int main() {
  // ---- 1. 配网 / 从没连上过 ----
  {
    ScrIn in = base();
    in.everLinked = false;
    in.linkOk = false;
    in.usbHost = false;
    in.linkAgeMs = 3600000;
    check("从没连上过 → 一直亮（配网门户）", scrDecide(SCR_OFF, in, kCfg), SCR_ON);
  }

  // ---- 2. 正常在用的几种 ----
  {
    ScrIn in = base();
    check("电脑在用（刚有输入） → 亮", scrDecide(SCR_ON, in, kCfg), SCR_ON);
  }
  {
    ScrIn in = base();
    in.hostIdleS = 301;  // 刚过 5 分钟
    check("USB：闲置 301 秒 → 变暗", scrDecide(SCR_ON, in, kCfg), SCR_DIM);
  }
  {
    ScrIn in = base();
    in.hostIdleS = 299;  // 差一点
    check("USB：闲置 299 秒 → 仍亮", scrDecide(SCR_DIM, in, kCfg), SCR_ON);
  }
  {
    ScrIn in = base();
    in.hostIdleS = -1;  // 桥接读不到
    check("闲置读数未知(-1) → 当作在用，保持亮", scrDecide(SCR_DIM, in, kCfg), SCR_ON);
  }

  // ---- 3. 电池模式阈值更紧 ----
  {
    ScrIn in = base();
    in.usbHost = false;  // 没接电脑 = 电池
    in.hostIdleS = 61;
    check("电池：闲置 61 秒 → 变暗（阈值 1 分钟）", scrDecide(SCR_ON, in, kCfg), SCR_DIM);
  }
  {
    ScrIn in = base();
    in.usbHost = false;
    in.hostIdleS = 59;
    check("电池：闲置 59 秒 → 仍亮", scrDecide(SCR_DIM, in, kCfg), SCR_ON);
  }

  // ---- 4. 断链：USB 下给 15 分钟，无主机 15 秒 ----
  {
    ScrIn in = base();
    in.linkOk = false;
    in.linkAgeMs = 60000;  // 1 分钟
    check("USB：断链 1 分钟 → 还亮着显示 NO LINK", scrDecide(SCR_ON, in, kCfg), SCR_ON);
  }
  {
    ScrIn in = base();
    in.linkOk = false;
    in.linkAgeMs = 900001;
    check("USB：断链超 15 分钟 → 进入淡出", scrDecide(SCR_ON, in, kCfg), SCR_FADE);
  }
  {
    ScrIn in = base();
    in.linkOk = false;
    in.linkAgeMs = 900001;
    in.modeAgeMs = 1499;
    check("淡出未满 1.5 秒 → 仍是淡出", scrDecide(SCR_FADE, in, kCfg), SCR_FADE);
  }
  {
    ScrIn in = base();
    in.linkOk = false;
    in.linkAgeMs = 900001;
    in.modeAgeMs = 1500;
    check("淡出满 1.5 秒 → 熄屏", scrDecide(SCR_FADE, in, kCfg), SCR_OFF);
  }
  {
    ScrIn in = base();
    in.usbHost = false;   // 关键：没有 USB 主机才走 15 秒那套阈值
    in.linkOk = false;
    in.linkAgeMs = 16000;
    check("无 USB 主机：断链 16 秒 → 淡出（只给 15 秒）", scrDecide(SCR_ON, in, kCfg), SCR_FADE);
  }
  {
    ScrIn in = base();
    in.usbHost = false;
    in.linkOk = false;
    in.linkAgeMs = 14000;
    check("无 USB 主机：断链 14 秒 → 还亮着", scrDecide(SCR_ON, in, kCfg), SCR_ON);
  }

  // ---- 5. 唤醒 ----
  {
    ScrIn in = base();
    in.linkOk = false;
    in.linkAgeMs = 3600000;  // 早就该熄了
    in.sinceInputMs = 100;   // 但刚被摸了一下
    check("熄屏后摸一下 → 亮（peek）", scrDecide(SCR_OFF, in, kCfg), SCR_ON);
  }
  {
    ScrIn in = base();
    in.linkOk = false;
    in.linkAgeMs = 3600000;
    in.sinceInputMs = 10001;  // peek 到期
    check("peek 10 秒到期 → 回到淡出/熄屏", scrDecide(SCR_ON, in, kCfg), SCR_FADE);
  }
  {
    ScrIn in = base();
    in.sinceInputMs = 5000;
    in.hostIdleS = 3600;  // 电脑闲置很久，但人正在摸板子
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

  // ---- 7. 一直在熄屏，别把 OFF 自己抖回 FADE ----
  {
    ScrIn in = base();
    in.linkOk = false;
    in.linkAgeMs = 3600000;
    check("已经是 OFF → 保持 OFF（不反复走淡出）", scrDecide(SCR_OFF, in, kCfg), SCR_OFF);
  }

  printf("\n%s\n", fails == 0 ? "全部通过" : (char*)"有失败项");
  return fails == 0 ? 0 : 1;
}
