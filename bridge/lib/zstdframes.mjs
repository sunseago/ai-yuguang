// dsh 的会话日志是“追加写”的多帧 zstd：每次落盘追加一个独立 frame，
// 因此不能整体解压（Node 的 zstdDecompressSync 只吃第一帧）。
// 做法：扫 zstd magic 找帧边界，逐帧解压后拼接。
//
// 关键约定（调用方 dsh.mjs 依赖）：
//   · consumed 是「从 buf 开头算起、可以安全跳过」的字节数；
//   · 只把「解压成功且以换行结尾」的帧算进 consumed。写到一半的帧必须留在
//     原地等下一轮，否则那批事件会被永久丢掉——之前正是这么丢的。
import zlib from 'node:zlib';

const MAGIC = [0x28, 0xb5, 0x2f, 0xfd];

function findFrameOffsets(buf) {
  const offs = [];
  const end = buf.length - 4;
  for (let i = 0; i <= end; i++) {
    if (buf[i] === MAGIC[0] && buf[i + 1] === MAGIC[1] && buf[i + 2] === MAGIC[2] && buf[i + 3] === MAGIC[3]) {
      offs.push(i);
      i += 3;
    }
  }
  return offs;
}

/**
 * @param {Buffer} buf
 * @param {{ allowTail?: boolean }} [opts] allowTail=true 时，最后一帧哪怕没有
 *   以换行结尾也接受（仅在文件已经很久没变、确定不会再补齐时使用）。
 * @returns {{text: string, frames: number, okFrames: number, consumed: number, tail: boolean}}
 *   consumed：可以安全跳过的字节数（未消费的尾部留给下一轮）
 *   tail：是否因为最后一帧不完整而停下
 */
export function decompressFrames(buf, opts = {}) {
  const offs = findFrameOffsets(buf);
  if (offs.length === 0) {
    // 没有帧头：可能是还没刷出 magic 的半帧，原样交给调用方判断
    return { text: buf.toString('utf8'), frames: 0, okFrames: 0, consumed: 0, tail: false };
  }

  const parts = [];
  let ok = 0;
  let consumed = 0;
  let tail = false;

  for (let i = 0; i < offs.length; i++) {
    const start = offs[i];
    const end = i + 1 < offs.length ? offs[i + 1] : buf.length;
    let text;
    try {
      text = zlib.zstdDecompressSync(buf.subarray(start, end)).toString('utf8');
    } catch {
      // 这一帧还写不完整（或后面的 magic 是数据里的巧合），到此为止
      tail = true;
      break;
    }
    if (!text.endsWith('\n') && !opts.allowTail) {
      tail = true;
      break;
    }
    parts.push(text);
    ok++;
    consumed = end;
  }

  return { text: parts.join(''), frames: offs.length, okFrames: ok, consumed, tail };
}
