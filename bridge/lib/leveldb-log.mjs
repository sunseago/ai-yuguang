// 极简 LevelDB 读取器：只解析 Chromium 的 Local Storage WAL（*.log）。
// 用途：Kimi 桌面版把登录态放在 localStorage 里，那是 LevelDB 存的，
// 但值本身没有加密，只有 Electron safeStorage 那套（token-store.json）才是密文。
// 所以这里只要把 log 里的 WriteBatch 拆出来，就能拿到 access_token / refresh_token。
//
// 不引第三方依赖是 PRD 的硬约束，所以自己解。
// 格式参考 leveldb/doc/log_format.md + WriteBatch 编码。
import fs from 'node:fs';
import path from 'node:path';

const BLOCK_SIZE = 32768;
const HEADER_SIZE = 7;

const T_FULL = 1;
const T_FIRST = 2;
const T_MIDDLE = 3;
const T_LAST = 4;

/** 把 *.log 里的物理记录还原成逻辑记录（跨 block 的会拼起来） */
function readLogicalRecords(buf) {
  const out = [];
  let pending = null;
  let pos = 0;

  while (pos + HEADER_SIZE <= buf.length) {
    const inBlock = pos % BLOCK_SIZE;
    // 块尾不足 7 字节时剩下的是填充
    if (BLOCK_SIZE - inBlock < HEADER_SIZE) {
      pos += BLOCK_SIZE - inBlock;
      continue;
    }
    const length = buf.readUInt16LE(pos + 4);
    const type = buf[pos + 6];
    pos += HEADER_SIZE;

    if (type === 0) {
      // 预分配的零区：整块跳到下一个块开头
      const off = pos % BLOCK_SIZE;
      pos += off === 0 ? BLOCK_SIZE : BLOCK_SIZE - off;
      continue;
    }
    if (length === 0 || pos + length > buf.length) break;

    const chunk = buf.subarray(pos, pos + length);
    pos += length;

    if (type === T_FULL) {
      out.push(chunk);
      pending = null;
    } else if (type === T_FIRST) {
      pending = [chunk];
    } else if (type === T_MIDDLE) {
      if (pending) pending.push(chunk);
    } else if (type === T_LAST) {
      if (pending) {
        pending.push(chunk);
        out.push(Buffer.concat(pending));
        pending = null;
      }
    }
  }
  return out;
}

function readVarint(buf, i) {
  let result = 0;
  let shift = 0;
  while (i < buf.length) {
    const b = buf[i++];
    result += (b & 0x7f) * 2 ** shift;
    if ((b & 0x80) === 0) return [result, i];
    shift += 7;
    if (shift > 63) break;
  }
  return [null, i];
}

/** WriteBatch: seq(8) count(4) 然后每条 tag/varint keylen/key/[varint vallen/value] */
function* parseBatch(buf) {
  if (buf.length < 12) return;
  const count = buf.readUInt32LE(8);
  let i = 12;
  for (let n = 0; n < count; n++) {
    if (i >= buf.length) return;
    const tag = buf[i++];
    let klen;
    [klen, i] = readVarint(buf, i);
    if (klen == null || i + klen > buf.length) return;
    const key = buf.subarray(i, i + klen);
    i += klen;

    if (tag === 1) {
      let vlen;
      [vlen, i] = readVarint(buf, i);
      if (vlen == null || i + vlen > buf.length) return;
      const value = buf.subarray(i, i + vlen);
      i += vlen;
      yield [key, value];
    } else {
      yield [key, null]; // 删除
    }
  }
}

// Chromium 的 localStorage 键形如 _<origin>\x00\x01<key>
// 值首字节是编码标记：0 = UTF-16LE，1 = Latin-1
function decodeEntry(keyBuf, valBuf) {
  if (keyBuf.length < 2 || keyBuf[0] !== 0x5f) return null; // '_'
  const sep = keyBuf.indexOf(0x00);
  if (sep < 0) return null;
  const origin = keyBuf.subarray(1, sep).toString('utf8');
  let k = keyBuf.subarray(sep + 1);
  while (k.length && (k[0] === 0x00 || k[0] === 0x01)) k = k.subarray(1);
  const key = k.toString('utf8');
  if (!key) return null;

  if (valBuf == null) return { origin, key, value: null };
  if (valBuf.length === 0) return { origin, key, value: '' };
  const enc = valBuf[0];
  const body = valBuf.subarray(1);
  const value = enc === 0 ? body.toString('utf16le') : body.toString('latin1');
  return { origin, key, value };
}

/**
 * 读一个 Chromium Local Storage 目录，返回 Map<"origin\tkey", value>。
 * 后写入的覆盖先写入的，跟浏览器行为一致。
 */
export function readLocalStorage(dir) {
  const map = new Map();
  let files = [];
  try {
    files = fs.readdirSync(dir).filter((f) => f.endsWith('.log')).sort();
  } catch {
    return map;
  }
  for (const f of files) {
    let buf;
    try {
      buf = fs.readFileSync(path.join(dir, f));
    } catch {
      continue;
    }
    for (const rec of readLogicalRecords(buf)) {
      for (const [k, v] of parseBatch(rec)) {
        const e = decodeEntry(k, v);
        if (!e) continue;
        if (e.value === null) map.delete(`${e.origin}\t${e.key}`);
        else map.set(`${e.origin}\t${e.key}`, e.value);
      }
    }
  }
  return map;
}

export function getLocalStorage(map, origin, key) {
  return map.get(`${origin}\t${key}`);
}

/** 只解 JWT 的 payload，不验签（我们只读声明，不做授权判断） */
export function decodeJwtPayload(token) {
  try {
    const part = String(token).split('.')[1];
    if (!part) return null;
    const b64 = part.replace(/-/g, '+').replace(/_/g, '/');
    const pad = b64 + '='.repeat((4 - (b64.length % 4)) % 4);
    return JSON.parse(Buffer.from(pad, 'base64').toString('utf8'));
  } catch {
    return null;
  }
}
