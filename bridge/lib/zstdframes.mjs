// dsh 的会话日志是“追加写”的多帧 zstd：每次落盘追加一个独立 frame，
// 因此不能整体解压（Node 的 zstdDecompressSync 只吃第一帧）。
// 做法：扫 zstd magic 找帧边界，逐帧解压后拼接。
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
 * @returns {{text: string, frames: number, okFrames: number}}
 */
export function decompressFrames(buf) {
  const offs = findFrameOffsets(buf);
  if (offs.length === 0) return { text: buf.toString('utf8'), frames: 0, okFrames: 0 };

  const parts = [];
  let ok = 0;
  for (let i = 0; i < offs.length; i++) {
    const seg = buf.subarray(offs[i], i + 1 < offs.length ? offs[i + 1] : buf.length);
    try {
      parts.push(zlib.zstdDecompressSync(seg).toString('utf8'));
      ok++;
    } catch {
      // 最后一帧可能正在写入，丢掉即可
    }
  }
  return { text: parts.join(''), frames: offs.length, okFrames: ok };
}
