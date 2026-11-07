# 把桥接封装成 macOS 应用：可行性调研

结论先说：**可行，而且比一般 Node 项目容易**——桥接零第三方依赖。下面三条路线，
推荐先做 B，SEA 单文件（C）留作可选。

---

## 一、本机已核实的事实

| 项目 | 实测结果 | 对方案的影响 |
| --- | --- | --- |
| 系统 / 架构 | macOS（Build 26B123）/ arm64 | 只需出 arm64 包，不用做通用二进制 |
| 编译工具链 | Xcode CLT 已装，`swiftc 6.4.2` | **可以写原生菜单栏 App**，不需要 Electron |
| Node | v22.22.2 | 有 `--experimental-sea-config`，**没有** `--build-sea`（那是 Node 26 才有的） |
| 桥接依赖 | 9 个 `node:` 内置模块 + 自己的 8 个 `.mjs`，**零第三方包** | SEA 最理想的情况：不用处理原生模块、不用 external |
| npm registry | 可访问（能查到 `postject`） | 需要时能装 `esbuild` / `postject` |
| node 二进制 | **108 MB**（自带 hardened runtime 签名） | 任何"自带运行时"的方案体积都 ≥ 100 MB |
| 签名工具 | `codesign` / `plutil` 可用 | 本机自用 ad-hoc 签名即可 |

## 二、三个硬约束（决定了方案形状）

**1. Node 22 的 SEA 不支持 ESM 入口**

SEA 要求入口是单个脚本，ESM 入口到 Node 26 才支持（`mainFormat` 字段）。
我们的桥接是 `.mjs` + 相对导入，所以走 SEA 必须先 `esbuild --bundle --format=cjs`
打成单文件。这不是障碍，是多一道工序。

**2. 进了 `.app` 就不能再往自己所在目录写文件**

现在 `deskpulse-bridge.mjs` 用 `import.meta.url` 算出 `__dirname`，
把 `data/quota_history.jsonl` 写在脚本旁边，配置也从脚本旁边读。
一旦进包，那就是**往 bundle 里写**——会破坏签名封条（`codesign --verify` 直接失败），
而且 `/Applications` 下的包对普通用户也不该是可写的。

所以无论走哪条路，都要先把路径改成可配置：

```
优先级：环境变量 DP_CONFIG / DP_DATA_DIR  >  默认 ~/Library/Application Support/DeskPulse/
日志：~/Library/Logs/DeskPulse.log
```

这条改造对现在的运行方式**完全向后兼容**（环境变量不设就是原来的行为）。

**3. 分发范围决定签名要求**

| 场景 | 要求 |
| --- | --- |
| 只在这台 Mac 上用 | `codesign --sign -`（ad-hoc）就够，双击不拦 |
| 拷给别人 | 需要 Developer ID 证书 + 公证（notarization），否则对方要右键→打开 |

自用场景不用碰证书，这是省掉最大一块工作量的地方。

## 三、三条路线对比

### A. 纯服务型 `.app`（无界面）

`AI余光.app` 里放 node + 桥接脚本 + 一份 launchd plist，双击即安装并启动，
`LSUIElement=1` 所以没有 Dock 图标。

- 体积 ~110 MB｜工作量 **小**｜依赖：无
- 缺点：**看不见**。想知道它在不在跑，只能看开发板或 `curl`

### B. 菜单栏 App + 内置 node（推荐第一版）

原生 Swift 菜单栏程序（`NSStatusItem`），菜单里：状态、打开 <http://127.0.0.1:8787/v1/state>、
重启桥接、查看日志、开机自启开关、退出。桥接作为子进程由它拉起。

- 体积 ~110 MB｜工作量 **中**（~200 行 Swift）｜依赖：无
- 优点：脱离 WorkBuddy 的 node（把 node 复制进包，版本也冻结了，反而更稳）；
  JS 仍可直接改，出问题好查；不需要 esbuild/postject/改签名二进制
- 缺点：源码在包里是明文（自用无所谓）

### C. 菜单栏 App + SEA 单文件内核

在 B 的基础上，把 JS 打成单文件二进制：`esbuild` → `--experimental-sea-config` →
`postject --macho-segment-name NODE_SEA` → `codesign --remove-signature` / `--sign -`。

- 体积 ~110 MB（没变小，Sea 里就是整个 node）｜工作量 **中大**
- 优点：内核是单个可执行文件，源码不裸露
- 缺点：多三个构建依赖；每次改一行 JS 都要重打包；Node 升级要重新走一遍注入流程
- 注意：`useCodeCache` 会禁用动态 `import()`，`import.meta` 在 CJS bundle 里也不存在，
  这两点对现有代码都要处理

### 对比表

| | A 纯服务 | **B 菜单栏+node** | C 菜单栏+SEA |
| --- | --- | --- | --- |
| 体积 | ~110 MB | ~110 MB | ~110 MB |
| 构建依赖 | 无 | 无 | esbuild + postject |
| 脱离 WorkBuddy 的 node | ✅ | ✅ | ✅ |
| 有可视界面 | ❌ | ✅ | ✅ |
| 改代码后要重打包 | 否 | 否（换 JS 即可） | **是** |
| 失败面 | 最小 | 小 | 中（注入/签名环节） |

## 四、推荐

**第一版做 B。** 理由：

1. 同样能解决"关掉 WorkBuddy 就没了"和"node 在 WorkBuddy 目录里"这两个真实痛点；
2. 不引入 esbuild/postject，也不必对 node 二进制做外科手术，出问题的概率最低；
3. 界面能显示"桥接在不在跑 / 端口 / 今天读到了哪几家"，这是纯服务型给不了的；
4. 以后想升级成 C，只是把「Resources 里的 JS 目录」换成「一个 SEA 可执行文件」，
   App 外壳和 launchd 那套不动。

## 五、不管选哪条，都要先做的小改造

1. `deskpulse-bridge.mjs`：`__dirname` 改成「配置目录解析函数」，支持 `DP_CONFIG` / `DP_DATA_DIR`
2. 首次运行把默认 `config.json` 拷到 `~/Library/Application Support/DeskPulse/`
3. 加一个 `--health` 子命令（退出码 0/1），给 App 轮询用，比解析 stdout 可靠
4. `data/quota_history.jsonl` 跟着搬到 Application Support
5. README 增加"以 App 方式运行"的一节

## 六、两个容易忽略的坑

- **端口占用**：App 内置 launchd 自启 + 手动双击同时跑，会抢 8787。
  需要在启动前探一次 `/healthz`，已在跑就直接唤起页面而不是再起一个。
- **数据源没变**：打包不改变"读本地登录态"这件事。DeepSeek / Kimi 仍然要求那两个
  app 在本机登录过，WorkBuddy 仍然读 `~/.workbuddy/workbuddy.db`。
  App 化解决的只是"谁来跑、怎么常驻、依赖谁的 node"。
