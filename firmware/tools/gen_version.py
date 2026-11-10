# 编译前生成 src/version_gen.h。
#
# 版本号唯一来源是 bridge/version.json：固件、桥接、App 都读它。
# 以前 DP_FW_VERSION 手写在 board_config.h 里，升级时漏改就成了
# 「App 显示 0.1.3、设备显示 0.1.4」——这种不一致很难在真机上看出来。
#
# PlatformIO 通过 platformio.ini 的 extra_scripts 调到这里。
Import("env")  # noqa: F821

import json
import os

project_dir = env.subst("$PROJECT_DIR")  # noqa: F821
version_file = os.path.join(project_dir, "..", "bridge", "version.json")
out_file = os.path.join(project_dir, "src", "version_gen.h")

version = None
try:
    with open(version_file, "r", encoding="utf-8") as f:
        version = str(json.load(f)["version"])
except Exception as exc:  # 文件缺失/格式错都不该让编译直接炸
    print("[gen_version] 读取 %s 失败: %s" % (version_file, exc))

if version:
    content = (
        "// 自动生成，勿手改；来源 bridge/version.json\n"
        "#pragma once\n"
        '#define DP_FW_VERSION "%s"\n' % version
    )
    old = None
    if os.path.exists(out_file):
        with open(out_file, "r", encoding="utf-8") as f:
            old = f.read()
    if old != content:
        with open(out_file, "w", encoding="utf-8") as f:
            f.write(content)
        print("[gen_version] DP_FW_VERSION=%s" % version)
