// 主机输入空闲时长：给设备的"变暗"策略用。
//
// 设备自己没法知道"人在不在电脑前"——它只能看到 bridge 通不通。但"电脑开着、
// 人走开了"这种情况 bridge 一直是通的，屏就会整夜亮着。
//
// macOS 的 HIDIdleTime 就是这个信息：距离上一次键鼠输入多少纳秒。读它不需要
// 任何权限（ioreg 是公开接口），实测单次约 10ms。
//
// 刻意做成异步 + 5 秒缓存：/v1/state 每 2 秒被拉一次，同步 fork 一个进程会在
// 事件循环里留下 10ms 的空档。异步刷新后，这个请求永远不会因为探测而变慢。
import { execFile } from 'node:child_process';

const TTL_MS = 5000;

const cache = { at: 0, s: -1, pending: false };

/**
 * 触发一次后台刷新（到 TTL 才会真的执行）。
 * 不做成 await：调用方拿到的永远是"上一次读到的值"，宁可滞后 5 秒，
 * 也不要让设备等下位机的外壳命令。
 */
export function refreshHostIdle() {
  const now = Date.now();
  if (cache.pending || now - cache.at < TTL_MS) return;
  cache.at = now;
  cache.pending = true;

  execFile('ioreg', ['-r', '-c', 'IOHIDSystem'], { timeout: 2000 }, (err, stdout) => {
    cache.pending = false;
    if (err) {
      // 读不到就退回 -1（未知）。设备侧把 -1 当作"在用"，宁可亮着也不要误熄。
      cache.s = -1;
      return;
    }
    const m = String(stdout).match(/"HIDIdleTime"\s*=\s*(\d+)/);
    cache.s = m ? Math.round(Number(m[1]) / 1e9) : -1;
  });
}

/** 距离上次键鼠输入的秒数；-1 表示读不到（未知） */
export function hostIdleSeconds() {
  refreshHostIdle();
  return cache.s;
}
