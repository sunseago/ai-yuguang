# 第三方组件与许可

本仓库的 `LICENSE`（MIT）只覆盖我自己写的代码。下面这些不是我写的，各自沿用原有许可。
写这份文件不是走形式——**`firmware/src/font_data.h` 确实不能算 MIT**，把它标清楚比含糊过去好。

---

## 1. GNU Unifont — `firmware/src/font_data.h`

| | |
| --- | --- |
| 上游 | <https://unifoundry.com/unifont/> |
| 版本 | GNU Unifont 16.0.04 |
| 许可 | **GPL-2.0-or-later，附字体嵌入例外（font embedding exception）** |

`font_data.h` 是用 `firmware/tools/gen_font.py` 把 `unifont.hex` 里的字形转成 C 数组得到的，
包含 6763 个汉字 + ASCII。它是字体的衍生作品，**这一个文件仍然受 GPL-2.0-or-later 约束，
MIT 不适用于它**。

GPL 的字体嵌入例外允许把字体（或未经改动的部分）嵌入文档/软件而不使整个作品变成 GPL，
所以把它编进固件、随固件分发是允许的。如果你要拿这个项目做闭源产品，稳妥做法是换掉字库
（比如用自己买的商用点阵字，或换成更宽松的字体），然后重跑 `gen_font.py`。

字体的完整许可原文请以上游仓库为准。

## 2. Node.js — 仅存在于 Release 里的 `.app`

| | |
| --- | --- |
| 上游 | <https://nodejs.org/> |
| 许可 | MIT |

`package/build-app.sh` 会把一份 node 可执行文件复制进 `AI余光.app/Contents/Resources/node`，
这样使用 App 的人不用自己装 Node。**仓库源码里不含 node 二进制**，只有打包出来的
Release 附件里有。分发携带 Node.js 的二进制时，应随附其许可证与版权声明，
许可证全文见 <https://github.com/nodejs/node/blob/main/LICENSE>。

## 3. 固件构建依赖（不随仓库分发）

`firmware/platformio.ini` 通过 PlatformIO 拉取，`firmware/.pio/` 已在 `.gitignore` 里排除：

| 组件 | 许可 |
| --- | --- |
| `moononournation/GFX Library for Arduino` | 见上游仓库声明 |
| `bblanchon/ArduinoJson` | MIT |
| ESP32 Arduino Core / ESP-IDF | LGPL-2.1 / Apache-2.0，见上游 |

## 4. 应用图标

图标素材由 AI 图像模型生成，之后经 `package/icon/make-icon.swift` 裁切并重绘为 macOS 图标网格。
生成的图标（`AppIcon.icns`、`icon-1024.png`）随本仓库以 MIT 分发。
生成用的原始图片带模型水印，已在 `.gitignore` 中排除、不随仓库分发。

## 5. 商标

WorkBuddy、DeepSeek、Kimi 等名称与商标归各自所有者。本项目只是读取本机这些应用的
本地数据做展示，与上述公司无任何隶属或背书关系。
