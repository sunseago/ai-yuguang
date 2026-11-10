// 库层单测：配置深合并 / 额度文件裁剪 / zstd 只消费完整帧。
//   node tests/lib-units.mjs
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { pickConfig } from '../lib/util.mjs';
import { createQuotaStore } from '../lib/quota.mjs';
import { decompressFrames } from '../lib/zstdframes.mjs';

let fails = 0;
const ok = (label, cond, extra = '') => {
  console.log(`${cond ? 'PASS' : 'FAIL'}  ${label}${extra ? '  ' + extra : ''}`);
  if (!cond) fails++;
};

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'deskpulse-units-'));

// ---- 1. pickConfig：local 只覆盖一个字段，不能把 base 的其余字段吃掉 ----
{
  const dir = path.join(tmp, 'cfg');
  fs.mkdirSync(dir);
  fs.writeFileSync(
    path.join(dir, 'config.json'),
    JSON.stringify({
      port: 8787,
      providers: {
        workbuddy: { enabled: true, dbPath: '~/.workbuddy/workbuddy.db', budgetCredits: 100 },
        kimi: { enabled: true, appDataPath: '/tmp/kimi' },
      },
    })
  );
  fs.writeFileSync(
    path.join(dir, 'config.local.json'),
    JSON.stringify({ providers: { workbuddy: { budgetCredits: 200 } } })
  );
  const c = pickConfig(dir);
  ok('深合并保留 dbPath', c.providers.workbuddy.dbPath === '~/.workbuddy/workbuddy.db', c.providers.workbuddy.dbPath);
  ok('深合并覆盖 budgetCredits', c.providers.workbuddy.budgetCredits === 200);
  ok('未提及的 provider 保留', c.providers.kimi?.appDataPath === '/tmp/kimi');
  ok('顶层字段保留', c.port === 8787);
}

// ---- 2. 额度历史：启动时把 30 天前的旧行裁掉（文件也要真的变小）----
{
  const file = path.join(tmp, 'data', 'quota_history.jsonl');
  fs.mkdirSync(path.dirname(file), { recursive: true });
  const old = Date.now() - 40 * 86400_000;
  const lines = [];
  for (let i = 0; i < 50; i++) lines.push(JSON.stringify({ t: old + i * 1000, id: 'deepseek', v: 10 }));
  lines.push(JSON.stringify({ t: Date.now() - 1000, id: 'deepseek', v: 9 }));
  lines.push(JSON.stringify({ t: Date.now() - 500, id: 'deepseek', v: 8 }));
  fs.writeFileSync(file, lines.join('\n') + '\n');

  const store = createQuotaStore(file);
  ok('只保留 30 天内的样本', store.size === 2, String(store.size));
  const after = fs.readFileSync(file, 'utf8').trim().split('\n');
  ok('文件被重写（不是只裁内存）', after.length === 2, `lines=${after.length}`);
  ok('重写后仍能算出今日消耗', store.spentToday('deepseek') === 1, String(store.spentToday('deepseek')));
}

// ---- 3. zstd：写到一半的帧不能被算进 consumed ----
{
  const f1 = zlib.zstdCompressSync(Buffer.from('{"type":"turn/start","time":1}\n'));
  const f2 = zlib.zstdCompressSync(Buffer.from('{"type":"turn/end","time":2}\n'));
  const whole = Buffer.concat([f1, f2]);

  const full = decompressFrames(whole);
  ok('完整两帧全部消费', full.consumed === whole.length && full.tail === false, `${full.consumed}/${whole.length}`);

  const half = Buffer.concat([f1, f2.subarray(0, Math.floor(f2.length / 2))]);
  const part = decompressFrames(half);
  ok('半帧只消费到第一帧边界', part.consumed === f1.length, `${part.consumed} vs ${f1.length}`);
  ok('半帧被标记为 tail', part.tail === true);

  const tailAllowed = decompressFrames(f1, { allowTail: true });
  ok('allowTail 时接受无换行结尾的帧', tailAllowed.consumed === f1.length);
}

fs.rmSync(tmp, { recursive: true, force: true });
console.log(fails === 0 ? '\n全部通过' : `\n${fails} 项失败`);
process.exit(fails === 0 ? 0 : 1);
