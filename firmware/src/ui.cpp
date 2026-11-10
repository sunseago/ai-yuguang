#include "ui.h"

#include <stdio.h>
#include <string.h>

namespace {

constexpr int kCardX = 8;
constexpr int kCardW = DP_W - 16;
constexpr int kCardH = 62;
constexpr int kCardY0 = 22;
constexpr int kCardGap = 6;
constexpr int kBarY = 18;

// 等待确认（agent 弹框问你要权限/问你问题）时的呼吸节奏。
//
// 为什么先闪 30 秒再常亮，而不是一直闪：这块屏是摆在桌上全天看着的，
// 红了半小时还在晃，人会开始主动无视它——那比不提醒更糟。
// 闪 30 秒足够把注意力拉过来，之后转常亮表示"还等着呢，但你不用被它催"。
constexpr uint64_t kAwaitBlinkMs = 30000;
constexpr uint64_t kBlinkHalfMs = 500;   // 1 Hz：亮 0.5s、暗 0.5s

uint16_t healthColor(uint8_t h) {
  switch (h) {
    case DP_H_WARN: return dpcolor::WAIT;
    case DP_H_CRITICAL: return dpcolor::ERR;
    case DP_H_UNKNOWN: return dpcolor::FAINT;
    default: return dpcolor::DONE;
  }
}

// ---- 状态胶囊 ----
// 任务进行中（THINKING/TOOL）用红底白字，结束（DONE）绿底白字，其余压成浅灰不去抢眼。
//
// AWAITING 是"等你确认"，它会呼吸：亮的时候实心红，暗的时候淡红底 + 红字。
// 注意暗的那一半**不能整格消失**——那会变成"闪没了"，你还是看不出这张卡在等人。
// 用红而不是原来的琥珀，是因为琥珀在浅色卡片上太温和，扫一眼根本注意不到；
// 而 ERROR 也是红，靠"闪 vs 不闪"区分，不用再加一种颜色。
struct BadgeStyle {
  uint16_t fill;
  uint16_t text;
};

BadgeStyle badgeStyle(uint8_t s, bool lit = true) {
  switch (s) {
    case DP_ST_THINKING:
    case DP_ST_TOOL:
      return {dpcolor::RUN, dpcolor::WHITE};
    case DP_ST_AWAITING:
      return lit ? BadgeStyle{dpcolor::RUN, dpcolor::WHITE}
                 : BadgeStyle{dpcolor::RUN_SOFT, dpcolor::RUN};
    case DP_ST_DONE:
      return {dpcolor::DONE, dpcolor::WHITE};
    case DP_ST_ERROR:
      return {dpcolor::ERR, dpcolor::WHITE};
    case DP_ST_OFFLINE:
      return {dpcolor::LINE, dpcolor::FAINT};
    default:
      return {dpcolor::TRACK, dpcolor::MUTED};
  }
}

// 这个状态已经持续多久。stateSinceMs 是桥接给的 epoch 时间，
// 和 in.nowMs（同样校时过）可以直接相减；桥接没给就退化成 0。
uint64_t stateAgeMs(const DpProvider& p, uint64_t nowMs) {
  return nowMs > p.stateSinceMs ? nowMs - p.stateSinceMs : 0;
}

// 此刻该"亮"还是"暗"。超过 30 秒恒亮。
bool awaitingLit(const DpProvider& p, uint64_t nowMs) {
  if (stateAgeMs(p, nowMs) >= kAwaitBlinkMs) return true;
  return ((nowMs / kBlinkHalfMs) & 1ULL) == 0;
}

// 画一枚圆角状态标签（胶囊），返回它占的宽度。
// 圆角半径取高度的一半，scale=1/2 都是正圆头。
int drawStateBadge(Canvas& c, int x, int y, uint8_t state, int scale, bool lit = true) {
  const char* t = dpStateLabel(state);
  const BadgeStyle b = badgeStyle(state, lit);
  const int padX = scale >= 2 ? 9 : 6;
  const int th = c.textHeight(scale);
  const int h = th + (scale >= 2 ? 4 : 2);
  const int w = c.textWidth(t, scale) + padX * 2;
  c.fillRoundRect(x, y, w, h, h / 2, b.fill);
  c.text(x + padX, y + (h - th) / 2, t, b.text, scale);
  return w;
}

void fmtMoney(char* out, size_t n, float v) {
  // 整数不显示小数位：100 -> "100"，34.34 -> "34.34"
  if (v >= 1000 || v == (float)(int)v) snprintf(out, n, "%.0f", v);
  else if (v >= 100) snprintf(out, n, "%.1f", v);
  else snprintf(out, n, "%.2f", v);
}

void fmtDuration(char* out, size_t n, uint64_t ms) {
  if (ms > 1000000000000ULL) ms = 0;
  uint32_t s = (uint32_t)(ms / 1000);
  if (s < 60) snprintf(out, n, "%us", (unsigned)s);
  else if (s < 3600) snprintf(out, n, "%um%02us", (unsigned)(s / 60), (unsigned)(s % 60));
  else snprintf(out, n, "%uh%02um", (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
}

void statusBar(Canvas& c, const UiInput& in) {
  bool online = in.model->bridgeOnline && in.wifiOk;
  c.fillRect(0, 0, DP_W, kBarY, dpcolor::BG);
  c.fillCircle(11, 9, 3, online ? dpcolor::DONE : dpcolor::ERR);
  c.text(20, 1, online ? "LINK" : "NO LINK", online ? dpcolor::MUTED : dpcolor::ERR, 1);

  if (online) {
    // 右上角：Wi-Fi 信号格（4 格）+ 时间
    int bars = in.rssi > -55 ? 4 : in.rssi > -67 ? 3 : in.rssi > -75 ? 2 : 1;
    for (int i = 0; i < 4; i++) {
      int h = 3 + i * 3;
      c.fillRect(146 + i * 5, 15 - h, 3, h, i < bars ? dpcolor::MUTED : dpcolor::LINE);
    }
    // 时间用桥接给的本地时间字符串，设备不需要知道时区
    const char* tm = (in.clockText && in.clockText[0]) ? in.clockText : "--:--";
    c.textAligned(168, 1, DP_W - 8 - 168, tm, dpcolor::MUTED, 1, DP_RIGHT);
  } else {
    c.textAligned(120, 1, 104, "RETRY", dpcolor::ERR, 1, DP_RIGHT);
  }
  c.hline(0, kBarY, DP_W, dpcolor::LINE);
}

// 拆成「货币符号 / 主数字 / 后缀」三段，是为了让 ¥ 保持 1 倍宽，
// 不然放大后货币符号会比数字宽一倍，看着很怪。
void quotaLine(const DpProvider& p, char* prefix, size_t prefixN, char* big, size_t bigN, char* small, size_t smallN) {
  prefix[0] = 0;
  big[0] = 0;
  small[0] = 0;
  if (!p.hasQuota) return;
  if (p.quotaKind == DP_Q_BALANCE) {
    snprintf(prefix, prefixN, "\xC2\xA5");  // ¥
    if (p.remaining < 0) snprintf(big, bigN, "--");
    else snprintf(big, bigN, "%.2f", p.remaining);
  } else if (p.quotaKind == DP_Q_CREDIT) {
    // 优先显示真实剩余积分（桥接从 billing 接口拿的）；
    // 老桥接没有 remaining 时退回"已用/预算"，至少不显示空白。
    char val[12], total[12];
    fmtMoney(val, sizeof(val), p.remaining >= 0 ? p.remaining : p.used);
    fmtMoney(total, sizeof(total), p.budget);
    snprintf(big, bigN, "%s", val);
    snprintf(small, smallN, "/%s", total);
  }
}

void drawCard(Canvas& c, const DpProvider& p, int y, bool highlight, uint64_t nowMs,
              uint8_t st) {
  // 等确认时整张卡的描边也一起呼吸，和胶囊同步 —— 余光扫过去先看到的是边框在跳
  const bool awaiting = (st == DP_ST_AWAITING);
  const bool lit = awaiting ? awaitingLit(p, nowMs) : true;
  const uint16_t edge = awaiting ? (lit ? dpcolor::RUN : dpcolor::RUN_SOFT) : dpcolor::LINE;
  const uint16_t fill = highlight ? dpRGB(0xEC, 0xF1, 0xF9) : dpcolor::CARD;

  c.roundRect(kCardX, y, kCardW, kCardH, 8, edge, fill);
  if (awaiting) {
    // 再套一道内圈凑成 2px：1px 的线在 240 像素的屏上几乎看不见
    c.roundRect(kCardX + 1, y + 1, kCardW - 2, kCardH - 2, 7, edge, fill);
  }
  // 左侧色条跟状态标签同色，一眼就能扫出哪张卡在跑、哪张已经完成
  const BadgeStyle bs = badgeStyle(st, lit);
  c.fillRect(kCardX + 1, y + 10, 3, kCardH - 20,
             st == DP_ST_OFFLINE ? dpcolor::FAINT : bs.fill);

  c.text(kCardX + 10, y + 8, p.name, dpcolor::TEXT, 1);

  char prefix[8], big[16], small[12];
  quotaLine(p, prefix, sizeof(prefix), big, sizeof(big), small, sizeof(small));
  // 卡片上把 "/总额" 换成 "left"：一眼看出这是余额而不是用量。
  // 详细页仍保留 "/总额" 的口径。
  if (p.quotaKind == DP_Q_CREDIT && p.remaining >= 0) {
    snprintf(small, sizeof(small), "left");
  }
  const int rightEdge = kCardX + kCardW - 9;
  int pw = c.textWidth(prefix, 1);
  int sw = c.textWidth(small, 1);
  int bigScale = 2;
  int bw = c.textWidth(big, bigScale);
  int nameW = c.textWidth(p.name, 1);
  int avail = rightEdge - (kCardX + 10 + nameW) - 6;  // 不能压到名字上
  if (bw + sw + pw > avail) {
    bigScale = 1;
    bw = c.textWidth(big, bigScale);
  }
  uint16_t numColor = p.health == DP_H_CRITICAL ? dpcolor::ERR : (p.health == DP_H_WARN ? dpcolor::WAIT : dpcolor::TEXT);
  // 断链时这些数字是上一轮的存量，不是实时值 —— 整体压灰，别让人误以为还在涨
  if (st == DP_ST_OFFLINE) numColor = dpcolor::FAINT;
  int bigX = rightEdge - sw - bw - pw;
  if (prefix[0]) c.text(bigX, y + 12, prefix, numColor, 1);
  if (big[0]) c.text(bigX + pw, y + 2, big, numColor, bigScale);
  if (small[0]) c.text(rightEdge - sw + 1, y + 20, small, dpcolor::FAINT, 1);

  // 第二行：状态胶囊 + 任务摘要
  const int bx = kCardX + 10;
  const int by = y + 32;
  const int badgeW = drawStateBadge(c, bx, by, st, 1, lit);
  const char* tail = (st == DP_ST_TOOL && p.tool[0]) ? p.tool : p.title;
  const int tx = bx + badgeW + 6;
  const int tailW = kCardX + kCardW - 9 - tx;
  if (tail && tail[0] && tailW > 12) {
    c.textEllipsis(tx, by + 2, tailW, tail, dpcolor::MUTED, 1);
  }

  // 底部细进度：credit=预算消耗，balance=上下文占用
  float frac = 0;
  uint16_t barColor = healthColor(p.health);
  if (p.quotaKind == DP_Q_CREDIT && p.budget > 0) {
    // 进度条也改成"余额占比"：满格=充裕，短格=快用完，和右侧数字同向
    frac = (p.remaining >= 0 ? p.remaining : p.budget - p.used) / p.budget;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
  } else if (p.hasCtx && p.ctxSize > 0) {
    frac = (float)p.ctxUsed / (float)p.ctxSize;
    barColor = frac > 0.9f ? dpcolor::ERR : (frac > 0.75f ? dpcolor::WAIT : dpcolor::THINK);
  }
  if (st == DP_ST_OFFLINE) barColor = dpcolor::LINE;
  c.bar(kCardX + 10, y + kCardH - 7, kCardW - 18, 3, frac, barColor, dpcolor::TRACK);
}

void overview(Canvas& c, const UiInput& in) {
  const DpModel& m = *in.model;
  statusBar(c, in);
  // 桥接断了就别再显示最后那次的 THINKING —— 那是在骗自己。
  // 数字还是旧的（冻住），但状态一律按 OFFLINE 画。
  for (int i = 0; i < m.count && i < 3; i++) {
    const uint8_t s = m.bridgeOnline ? m.items[i].state : DP_ST_OFFLINE;
    drawCard(c, m.items[i], kCardY0 + i * (kCardH + kCardGap), in.pendingIndex == i,
             in.nowMs, s);
  }
  if (m.count == 0) {
    c.textAligned(0, 100, DP_W, "no providers", dpcolor::FAINT, 1, DP_CENTER);
  }
  // 分页点
  c.fillCircle(112, 230, 3, dpcolor::TEXT);
  c.fillCircle(124, 230, 3, dpcolor::LINE);
}

void detail(Canvas& c, const UiInput& in) {
  const DpModel& m = *in.model;
  int idx = in.selected;
  if (idx < 0 || idx >= m.count) idx = 0;
  if (m.count == 0) return;
  const DpProvider& p = m.items[idx];

  statusBar(c, in);

  c.text(10, 24, "<", dpcolor::MUTED, 1);
  c.text(24, 24, p.name, dpcolor::TEXT, 1);

  // 状态胶囊（放大一档），耗时跟着标签右边自适应，避免长标签压到时间上
  const uint8_t st = in.model->bridgeOnline ? p.state : DP_ST_OFFLINE;
  const int bw = drawStateBadge(c, 10, 42, st, 2);

  char dur[16] = "";
  // 断链时"已经持续多久"无从得知，干脆不显示，免得那个数字还在自己涨
  if (st != DP_ST_OFFLINE && p.stateSinceMs && in.nowMs > p.stateSinceMs) {
    fmtDuration(dur, sizeof(dur), in.nowMs - p.stateSinceMs);
  }
  if (dur[0]) {
    const int durX = 10 + bw + 8;
    const int durW = DP_W - 10 - durX;
    if (durW >= 30) c.textAligned(durX, 52, durW, dur, dpcolor::MUTED, 1, DP_RIGHT);
  }

  // 任务标题，最多两行
  if (p.title[0]) {
    c.textWrap(10, 84, DP_W - 20, 18, p.title, dpcolor::TEXT, 1, 2);
  } else {
    c.text(10, 84, "-", dpcolor::FAINT, 1);
  }

  if (p.step > 0) {
    char buf[32];
    snprintf(buf, sizeof(buf), "step %d", (int)p.step);
    c.text(10, 120, buf, dpcolor::MUTED, 1);
  }
  if (p.jobs > 0) {
    char buf[24];
    snprintf(buf, sizeof(buf), "+%d jobs", (int)p.jobs);
    c.textAligned(120, 120, 110, buf, dpcolor::TOOL, 1, DP_RIGHT);
  }

  c.hline(10, 140, DP_W - 20, dpcolor::LINE);

  // 最近工具
  for (int i = 0; i < 3; i++) {
    if (!p.recent[i][0]) continue;
    int y = 148 + i * 18;
    c.fillCircle(14, y + 7, 3, p.recent[i][0] == '!' ? dpcolor::ERR : dpcolor::DONE);
    c.text(24, y, p.recent[i] + (p.recent[i][0] == '!' ? 1 : 0), dpcolor::MUTED, 1);
  }

  // 底部额度
  char prefix[8], big[16], small[12];
  quotaLine(p, prefix, sizeof(prefix), big, sizeof(big), small, sizeof(small));
  c.hline(10, 204, DP_W - 20, dpcolor::LINE);
  int y = 212;
  if (p.quotaKind == DP_Q_BALANCE) {
    char money[24];
    snprintf(money, sizeof(money), "%s%s", prefix, big);
    c.text(10, y, money, p.health == DP_H_CRITICAL ? dpcolor::ERR : dpcolor::TEXT, 1);
    if (small[0]) c.text(10 + c.textWidth(big, 1) + 4, y, small, dpcolor::FAINT, 1);
    char burn[32] = "";
    if (p.hasBurn && p.burnDays > 0) snprintf(burn, sizeof(burn), "%.0fd left", p.burnDays);
    if (burn[0]) c.textAligned(120, y, 110, burn, dpcolor::MUTED, 1, DP_RIGHT);
  } else if (p.quotaKind == DP_Q_CREDIT) {
    c.text(10, y, big, dpcolor::TEXT, 1);
    c.text(10 + c.textWidth(big, 1) + 2, y, small, dpcolor::FAINT, 1);
    if (p.hasCtx && p.ctxSize > 0) {
      char ctx[32];
      snprintf(ctx, sizeof(ctx), "ctx %d%%", (int)(100.0f * p.ctxUsed / p.ctxSize));
      c.textAligned(120, y, 110, ctx, dpcolor::MUTED, 1, DP_RIGHT);
    }
  } else {
    c.text(10, y, "no quota data", dpcolor::FAINT, 1);
  }
}

void infoPage(Canvas& c, const UiInput& in) {
  statusBar(c, in);
  c.text(10, 26, "AI余光", dpcolor::TEXT, 1);
  c.textAligned(120, 26, 110, in.fwVersion, dpcolor::FAINT, 1, DP_RIGHT);

  struct Row { const char* k; const char* v; };
  char ip[24], host[40], bri[8], tilt[20];
  snprintf(ip, sizeof(ip), "%s", in.ip ? in.ip : "-");
  snprintf(host, sizeof(host), "%s", in.bridgeHost ? in.bridgeHost : "-");
  snprintf(bri, sizeof(bri), "%u%%", (unsigned)in.brightness);
  if (!in.imuOk) {
    snprintf(tilt, sizeof(tilt), "no imu");
  } else {
    // 把当前档位和整体偏移一起显示，现场校准的时候才知道自己在哪一档
    snprintf(tilt, sizeof(tilt), "%u deg  off %u", (unsigned)in.rotation * 90,
             (unsigned)in.rotOffset);
  }
  Row rows[] = {
      {"bridge", host},
      {"this ip", ip},
      {"wifi", in.wifiOk ? "connected" : "down"},
      {"power", in.power ? in.power : "-"},
      {"screen", bri},
      {"tilt", tilt},
  };
  int y = 56;
  for (auto& r : rows) {
    c.text(10, y, r.k, dpcolor::FAINT, 1);
    c.textEllipsis(90, y, DP_W - 100, r.v, dpcolor::TEXT, 1);
    y += 20;
  }
  c.hline(10, y + 2, DP_W - 20, dpcolor::LINE);
  c.text(10, y + 10, "tap BOOT = rotate 90", dpcolor::MUTED, 1);
  c.text(10, y + 26, "hold BOOT 5s = re-pair", dpcolor::MUTED, 1);

  c.fillCircle(112, 230, 3, dpcolor::LINE);
  c.fillCircle(124, 230, 3, dpcolor::TEXT);
}

}  // namespace

void uiRender(Canvas& c, const UiInput& in) {
  c.clear(dpcolor::BG);
  if (in.page == 1) {
    detail(c, in);
  } else if (in.page == 2) {
    infoPage(c, in);
  } else {
    overview(c, in);
  }
}

int uiHitCard(const DpModel& m, int x, int y) {
  for (int i = 0; i < m.count && i < 3; i++) {
    int cy = kCardY0 + i * (kCardH + kCardGap);
    if (y >= cy && y < cy + kCardH && x >= kCardX && x < kCardX + kCardW) return i;
  }
  return -1;
}

// 有卡片正在呼吸时才值得提高重绘频率；其余情况维持 1 秒一次。
// 单独抽出来是给 main 的节流逻辑用 —— 让"要不要快刷"这件事只有一个判据，
// 免得绘制端在闪、调度端还按 1 秒刷，看起来一顿一顿的。
bool uiWantsFastRedraw(const DpModel& m, uint64_t nowMs) {
  if (!m.bridgeOnline) return false;
  for (int i = 0; i < m.count && i < 3; i++) {
    if (m.items[i].state != DP_ST_AWAITING) continue;
    if (stateAgeMs(m.items[i], nowMs) < kAwaitBlinkMs) return true;
  }
  return false;
}
