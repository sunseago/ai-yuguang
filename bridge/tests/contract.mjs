// 契约测试：固件只认 PRD §9.1 里那些字段。这里逐项核对桥接的输出，
// 少字段或类型不对都会在烧录前暴露，而不是上板后对着花屏猜。
import { execFile } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const bridge = path.join(here, '..', 'deskpulse-bridge.mjs');

const STATES = ['idle', 'thinking', 'tool', 'awaiting', 'done', 'error', 'offline'];
const HEALTH = ['ok', 'warn', 'critical', 'unknown'];

let fails = 0;
const ok = (cond, label, extra = '') => {
  console.log(`${cond ? 'PASS' : 'FAIL'}  ${label}${extra ? '  ' + extra : ''}`);
  if (!cond) fails++;
};

const state = await new Promise((resolve, reject) => {
  execFile(process.execPath, ['--no-warnings', bridge, '--once'], { timeout: 20000 }, (err, stdout) => {
    if (err) return reject(err);
    try {
      resolve(JSON.parse(stdout));
    } catch (e) {
      reject(new Error('输出不是合法 JSON: ' + stdout.slice(0, 200)));
    }
  });
});

ok(typeof state.ts === 'number' && state.ts > 1e12, 'ts 是毫秒时间戳', String(state.ts));
ok(/^\d{2}:\d{2}$/.test(state.bridge?.localTime || ''), 'bridge.localTime 是 HH:MM', state.bridge?.localTime);
ok(Array.isArray(state.providers) && state.providers.length > 0, 'providers 非空', `${state.providers.length} 个`);
ok(state.providers.length <= 5, 'providers 不超过固件上限 5');

for (const p of state.providers) {
  const tag = `[${p.id}]`;
  ok(typeof p.id === 'string' && p.id.length > 0 && p.id.length < 12, `${tag} id 非空且 <12 字节`, p.id);
  ok(typeof p.name === 'string' && p.name.length > 0, `${tag} name 非空`, p.name);
  ok(STATES.includes(p.state), `${tag} state 合法`, p.state);
  ok(HEALTH.includes(p.health), `${tag} health 合法`, p.health);
  ok(typeof p.state_since === 'number', `${tag} state_since 是数字`);
  ok(p.task && typeof p.task.title === 'string', `${tag} task.title 是字符串`);
  ok(p.quota && ['balance', 'credit'].includes(p.quota.kind), `${tag} quota.kind 合法`, p.quota?.kind);

  if (p.quota?.kind === 'balance') {
    ok(p.quota.remaining === null || typeof p.quota.remaining === 'number', `${tag} remaining 是数字或 null`, String(p.quota.remaining));
    ok(typeof p.quota.currency === 'string', `${tag} currency 存在`, p.quota.currency);
  }
  if (p.quota?.kind === 'credit') {
    ok(typeof p.quota.used === 'number' && typeof p.quota.budget === 'number', `${tag} credit 有 used/budget`);
  }
  if (p.recent_tools) {
    ok(Array.isArray(p.recent_tools) && p.recent_tools.length <= 3, `${tag} recent_tools ≤3`);
    for (const t of p.recent_tools) ok(typeof t.name === 'string' && t.name.length < 16, `${tag} 工具名长度 <16`, t.name);
  }
  console.log('');
  console.log(`  屏上会显示：${p.name}  ${p.quota?.kind === 'balance' ? '¥' + p.quota.remaining : p.quota?.used + '/' + p.quota?.budget}`);
  console.log(`  状态行     ：${p.state.toUpperCase()} · ${p.task?.title || '-'}`);
}

console.log(fails === 0 ? '\n契约一致，固件可以直接对接' : `\n${fails} 项不符合契约`);
process.exit(fails === 0 ? 0 : 1);
