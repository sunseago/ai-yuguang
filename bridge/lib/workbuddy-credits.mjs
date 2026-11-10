// WorkBuddy 真实积分余额。
//
// 客户端 2026-09 起把 accessToken 存成 at-rest 信封（AES-GCM，保护钥来自定制
// Electron 的原生绑定），磁盘上没有明文 token 可用。这里复刻社区 oracle 的做法：
// 让 WorkBuddy 客户端自己当解释器跑 tools/wb-oracle（进程内解密 + 发请求），
// 桥接只负责取它 stdout 里的 WBSUMMARY 行。token 不经过本进程、不落盘。
//
// 成本：每轮会起一个 Electron 进程（约 1–2 秒），所以缓存 5 分钟；
// 失败后 1 分钟内不重试，避免 WorkBuddy 没开时每 2 秒 fork 一次。
import { execFile } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { log } from './util.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const ORACLE_DIR = path.join(here, '..', 'tools', 'wb-oracle');

const TTL_MS = 5 * 60 * 1000;
const RETRY_MS = 60 * 1000;
const TIMEOUT_MS = 15000;

const cache = { at: 0, value: null, error: null, pending: null, lastTry: 0 };

function electronBin() {
  const env = process.env.WB_ELECTRON_BIN;
  if (env) return env;
  const cands = [
    '/Applications/WorkBuddy.app/Contents/MacOS/Electron',
    '/Applications/WorkBuddy.app/Contents/MacOS/WorkBuddy',
  ];
  if (process.env.LOCALAPPDATA) {
    cands.push(path.join(process.env.LOCALAPPDATA, 'Programs', 'WorkBuddy', 'WorkBuddy.exe'));
  }
  for (const p of cands) {
    try {
      if (fs.existsSync(p)) return p;
    } catch {
      /* 试下一个 */
    }
  }
  return null;
}

function parseSummary(stdout) {
  // Electron 偶尔会往 stdout 打噪音，按标记行找，不假定行号
  for (const line of String(stdout).split('\n')) {
    if (!line.startsWith('WBSUMMARY ')) continue;
    try {
      const v = JSON.parse(line.slice('WBSUMMARY '.length));
      if (v && Number.isFinite(v.total)) return v;
    } catch {
      /* 坏行，继续找 */
    }
  }
  return null;
}

export function refreshWorkBuddyCredits(force = false) {
  if (cache.pending) return cache.pending;
  // 未过期就别再起 Electron：TTL 内的调用直接返回缓存
  if (!force && cache.value && Date.now() - cache.at < TTL_MS) {
    return Promise.resolve(cache.value);
  }
  const bin = electronBin();
  if (!bin) {
    cache.error = '未找到 WorkBuddy 客户端';
    return Promise.resolve(null);
  }

  cache.lastTry = Date.now();
  cache.pending = new Promise((resolve) => {
    execFile(
      bin,
      [ORACLE_DIR],
      {
        timeout: TIMEOUT_MS,
        maxBuffer: 1024 * 1024,
        // WorkBuddy 的 Electron 启动器靠这个变量决定「按自定义 app 目录跑」还是
        // 「加载它自己的 app.asar」。从 LaunchServices 启动的 App（没有终端环境）
        // 里这个变量是空的，不补就会跑成 WorkBuddy 本体、拿不到 WBSUMMARY。
        env: { ...process.env, ELECTRON_RUN_AS_NODE: '1' },
      },
      (err, stdout, stderr) => {
        cache.pending = null;
        if (err) {
          const msg = (String(stderr || '').trim().split('\n').pop() || err.message).slice(0, 160);
          if (cache.error !== msg) log('workbuddy credits:', msg);
          cache.error = msg;
          return resolve(null);
        }
        const v = parseSummary(stdout);
        if (!v) {
          // 留一条短诊断：App 内起子进程和终端里行为可能不同，先看它到底输出了什么
          const out = String(stdout || '').replace(/\s+/g, ' ').slice(0, 180);
          const errTail = String(stderr || '').trim().split('\n').pop().slice(0, 160);
          log('workbuddy credits: 没有 WBSUMMARY | stdout=', out, '| stderr=', errTail);
          cache.error = 'oracle 没有返回余额';
          return resolve(null);
        }
        cache.value = v;
        cache.at = Date.now();
        cache.error = null;
        resolve(v);
      }
    );
  });
  return cache.pending;
}

/** 同步读取上次的余额；过期时在后台刷新。 */
export function workBuddyCredits() {
  const now = Date.now();
  if (!cache.pending) {
    const stale = !cache.value || now - cache.at > TTL_MS;
    const cooled = now - cache.lastTry > RETRY_MS;
    if (stale && cooled) refreshWorkBuddyCredits().catch(() => {});
  }
  return cache.value;
}

export function workBuddyCreditsError() {
  return cache.error;
}
