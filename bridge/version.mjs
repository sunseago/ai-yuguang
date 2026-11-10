// 版本号的唯一来源：bridge/version.json。
//
// 以前 0.1.4 同时硬编码在 7 个地方（固件、桥接两处、mock、build-app.sh、
// main.swift、Info.plist），升级时漏改一处就会出现「App 显示 0.1.3、设备显示
// 0.1.4」这种自相矛盾。现在只有这个文件是权威值：
//   · 桥接运行时读它（本文件）
//   · package/build-app.sh 读它写 Info.plist
//   · firmware/tools/gen_version.py 读它生成 version_gen.h
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));

function read() {
  for (const p of [path.join(here, 'version.json'), path.join(here, '..', 'version.json')]) {
    try {
      const v = JSON.parse(fs.readFileSync(p, 'utf8')).version;
      if (typeof v === 'string' && v) return v;
    } catch {
      /* 试下一个位置 */
    }
  }
  return '0.0.0';
}

export const BRIDGE_VERSION = read();
