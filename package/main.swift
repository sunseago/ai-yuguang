// 余光 · 菜单栏程序：管住那个常驻的 Node 桥接。
//
// 名字的由来：这块屏不该被正眼盯着看。你扫一眼余光，就知道三个 Agent 在干什么、
// 还剩多少额度——"余光"和"余额"正好同音。全名「AI余光」，AI 点明它盯的是谁。
// 技术标识符仍然沿用 deskpulse（bundle id、支持目录、日志名），改显示名不该动它们。
//
// 设计取舍：
//   · 不做 Electron / Tauri —— 一个状态点 + 一个菜单，用原生 NSStatusItem 只要两百行，
//     而且不吃 200MB。
//   · node 直接复制进包（Contents/Resources/node），这样不依赖 WorkBuddy 自带的那份，
//     版本也冻结了，不怕 WorkBuddy 升级把它换掉。
//   · 桥接作为本进程的子进程跑；端口已经被别人占（比如手动起的 node）就不重复起，
//     只在菜单里标成"外部进程"。
//
// 编译由 package/build-app.sh 负责，不手搓 swiftc 参数。

import Cocoa
import Foundation
import ServiceManagement

// ---------- 常量 ----------
let kLabel = "cn.deskpulse.app.startup"
/// 显示名。菜单栏、菜单标题、各处提示都用这一个，改名字只改这里。
/// 最终定名「AI余光」——不是「余光」，也不是「Agent余光」，别自作主张简写。
let kAppName = "AI余光"
/// 版本号唯一来源是 Info.plist（build-app.sh 从 bridge/version.json 写入）。
/// 之前这里硬编码 0.1.4，和桥接/固件各写一份，升级时必漏。
let kVersion = (Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String) ?? "dev"

let fm = FileManager.default
let home = fm.homeDirectoryForCurrentUser
let supportDir = home.appendingPathComponent("Library/Application Support/DeskPulse")
let dataDir = supportDir.appendingPathComponent("data")
let logsDir = home.appendingPathComponent("Library/Logs")
let engineLog = logsDir.appendingPathComponent("DeskPulse.log")
let agentsDir = home.appendingPathComponent("Library/LaunchAgents")
let startupPlist = agentsDir.appendingPathComponent("\(kLabel).plist")

/// 端口跟随用户配置：桥接读的是同一份 config.json，
/// 写死 8787 会让「改了端口的用户」在菜单里永远看到"未运行"。
func configuredPort() -> Int {
    let cfg = supportDir.appendingPathComponent("config.json")
    guard
        let data = try? Data(contentsOf: cfg),
        let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
        let p = obj["port"] as? Int,
        (1...65535).contains(p)
    else { return 8787 }
    return p
}
let kPort = configuredPort()
let stateURL = URL(string: "http://127.0.0.1:\(kPort)/v1/state")!
let healthURL = URL(string: "http://127.0.0.1:\(kPort)/healthz")!

func res(_ name: String) -> URL? {
    guard let r = Bundle.main.resourceURL else { return nil }
    return r.appendingPathComponent(name)
}

let nodePath = res("node")
let bridgePath = res("bridge/deskpulse-bridge.mjs")

func ensureDir(_ url: URL) {
    try? fm.createDirectory(at: url, withIntermediateDirectories: true)
}

@discardableResult
func run(_ path: String, _ args: [String]) -> Int32 {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: path)
    p.arguments = args
    p.standardOutput = FileHandle(forWritingAtPath: "/dev/null")
    p.standardError = FileHandle(forWritingAtPath: "/dev/null")
    do { try p.run() } catch { return -1 }
    p.waitUntilExit()
    return p.terminationStatus
}

private func parseProviderNames(_ data: Data?) -> [String] {
    guard
        let d = data,
        let obj = try? JSONSerialization.jsonObject(with: d) as? [String: Any],
        let arr = obj["providers"] as? [[String: Any]]
    else { return [] }
    return arr.compactMap { $0["name"] as? String }
}

/// 同步探测：只给 `--status` 这类一次性命令行用。
/// 菜单刷新绝不能用它 —— 在主线程等信号量会把菜单卡住好几秒。
func probe() -> (up: Bool, providers: [String]) {
    var req = URLRequest(url: healthURL)
    req.timeoutInterval = 0.8
    let sem = DispatchSemaphore(value: 0)
    var up = false
    URLSession.shared.dataTask(with: req) { _, resp, _ in
        if let h = resp as? HTTPURLResponse, h.statusCode == 200 { up = true }
        sem.signal()
    }.resume()
    _ = sem.wait(timeout: .now() + 1.5)
    guard up else { return (false, []) }

    var names: [String] = []
    var sreq = URLRequest(url: stateURL)
    sreq.timeoutInterval = 1.2
    URLSession.shared.dataTask(with: sreq) { data, _, _ in
        names = parseProviderNames(data)
        sem.signal()
    }.resume()
    _ = sem.wait(timeout: .now() + 2.0)
    return (true, names)
}

/// 异步探测：菜单栏专用。回调在后台线程，调用方自己切回主线程。
func probeAsync(_ done: @escaping (Bool, [String]) -> Void) {
    var req = URLRequest(url: healthURL)
    req.timeoutInterval = 0.8
    URLSession.shared.dataTask(with: req) { _, resp, _ in
        guard let h = resp as? HTTPURLResponse, h.statusCode == 200 else {
            done(false, [])
            return
        }
        var sreq = URLRequest(url: stateURL)
        sreq.timeoutInterval = 1.2
        URLSession.shared.dataTask(with: sreq) { data, _, _ in
            done(true, parseProviderNames(data))
        }.resume()
    }.resume()
}

// ---------- 开机自启 ----------
// 两条路：
//   1) macOS 13+ 用 SMAppService.mainApp —— 不写 plist、不调 launchctl，
//      并且会出现在「系统设置 → 通用 → 登录项」，用户能自己看见和关闭。
//   2) 老系统退回 ~/Library/LaunchAgents 里的 LaunchAgent。
// 用两条而不是只留一条，是因为在某些受限环境（被沙箱接管的进程）里
// launchctl bootstrap 会被直接拒绝（返回 5: Input/output error），
// 而 SMAppService 走的是另一条通路。
enum LoginState {
    case on
    case off
    case needsApproval
    case failed(String)
}

func loginState() -> LoginState {
    if #available(macOS 13.0, *) {
        switch SMAppService.mainApp.status {
        case .enabled: return .on
        case .requiresApproval: return .needsApproval
        case .notRegistered: return fm.fileExists(atPath: startupPlist.path) ? .on : .off
        default: return .off
        }
    }
    return fm.fileExists(atPath: startupPlist.path) ? .on : .off
}

func startupPlistXML() -> String {
    let exe = Bundle.main.executableURL?.path ?? ""
    return """
    <?xml version="1.0" encoding="UTF-8"?>
    <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
    <plist version="1.0">
    <dict>
      <key>Label</key><string>\(kLabel)</string>
      <key>ProgramArguments</key>
      <array><string>\(exe)</string></array>
      <key>RunAtLoad</key><true/>
      <key>LimitLoadToSessionType</key><string>Aqua</string>
      <key>KeepAlive</key>
      <dict><key>SuccessfulExit</key><false/></dict>
      <key>ProcessType</key><string>Interactive</string>
    </dict>
    </plist>
    """
}

/// 执行命令并带回 stderr，别再往 /dev/null 一丢了事（上一版就是这么把失败藏起来的）
func runCapture(_ path: String, _ args: [String]) -> (Int32, String) {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: path)
    p.arguments = args
    let pipe = Pipe()
    p.standardOutput = pipe
    p.standardError = pipe
    do { try p.run() } catch { return (-1, "\(error)") }
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    p.waitUntilExit()
    return (p.terminationStatus, String(data: data, encoding: .utf8) ?? "")
}

func enableLoginAgent() -> LoginState {
    ensureDir(agentsDir)
    try? startupPlistXML().write(to: startupPlist, atomically: true, encoding: .utf8)
    let uid = getuid()
    let a = runCapture("/bin/launchctl", ["bootstrap", "gui/\(uid)", startupPlist.path])
    if a.0 != 0 {
        let b = runCapture("/bin/launchctl", ["load", "-w", startupPlist.path])
        if b.0 != 0 {
            try? fm.removeItem(at: startupPlist)
            return .failed("launchctl 拒绝加载：\((a.1 + b.1).trimmingCharacters(in: .whitespacesAndNewlines))")
        }
    }
    return .on
}

func enableLogin() -> LoginState {
    if #available(macOS 13.0, *) {
        do {
            try SMAppService.mainApp.register()
            let s = SMAppService.mainApp.status
            if s == .enabled { return .on }
            if s == .requiresApproval { return .needsApproval }
        } catch {
            NSLog("DeskPulse: SMAppService 登记失败 \(error)，改用 LaunchAgent")
        }
    }
    return enableLoginAgent()
}

func disableLogin() {
    if #available(macOS 13.0, *) {
        if SMAppService.mainApp.status != .notRegistered {
            try? SMAppService.mainApp.unregister()
        }
    }
    let uid = getuid()
    _ = runCapture("/bin/launchctl", ["bootout", "gui/\(uid)/\(kLabel)"])
    _ = runCapture("/bin/launchctl", ["unload", "-w", startupPlist.path])
    try? fm.removeItem(at: startupPlist)
}

// ---------- 桥接子进程 ----------
final class Engine {
    private(set) var child: Process?
    private var recheck: DispatchWorkItem?   // 外部实例消失后的延迟复查（见 start()）
    /// 端口上的实例不是我们起的（比如用户手动 node 起的），这时不抢
    var external = false
    var shouldRun = true

    func start() {
        shouldRun = true
        if probe().up {
            external = true
            child = nil
            // 热替换场景：换 .app 时旧实例的桥接可能还没退干净，此刻 probe()
            // 会误判成"外部实例占用"，于是自己不拉起 —— 旧桥接一退就没人服务了
            // （2026-11-12 实际踩到：替换运行中的 App 后桥接消失，只能再开一次）。
            // 所以每隔几秒复查一次，端口空了就自己拉起。
            scheduleExternalRecheck()
            return
        }
        external = false
        spawn()
    }

    /// probe() 在调用线程等信号量（最多 1.5s），主线程调它会把菜单卡住，
    /// 所以复查放后台队列；检测到端口释放后回主线程走原来的 spawn() 路径。
    private func scheduleExternalRecheck(attempt: Int = 0) {
        recheck?.cancel()
        let item = DispatchWorkItem { [weak self] in
            guard let self, self.shouldRun, self.child == nil else { return }
            if probe().up {
                // 还占着：继续等（最多 20 次 × 3s = 一分钟，之后认命，
                // 用户从菜单里点"重启引擎"还能救）
                if attempt < 20 { self.scheduleExternalRecheck(attempt: attempt + 1) }
                return
            }
            DispatchQueue.main.async { [weak self] in
                guard let self, self.shouldRun, self.child == nil else { return }
                self.external = false
                self.spawn()
            }
        }
        recheck = item
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 3, execute: item)
    }

    func stop() {
        recheck?.cancel()
        recheck = nil
        shouldRun = false
        if let c = child, c.isRunning {
            c.terminate()
            // 等它真的退出：否则下一次启动会撞上还没释放的端口，
            // 菜单里就会莫名其妙变成"外部实例占用"。
            c.waitUntilExit()
        }
        child = nil
    }

    func restart() {
        let wasExternal = external
        child?.terminate()
        child = nil
        shouldRun = true
        if wasExternal {
            // 本来就不是我们起的（用户手动 node 起的），只重新探测，别抢端口
            external = false
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) { [weak self] in self?.start() }
            return
        }
        external = false
        // 自己刚终止的那个实例需要一点时间释放端口；这时候 probe() 会误判成
        // "外部实例"，所以直接 spawn，不走探测。
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) { [weak self] in
            guard let self, self.shouldRun else { return }
            self.spawn()
        }
    }

    private func spawn() {
        // 自己已经拉起桥接了，挂着的"外部实例复查"就该停 —— 否则它会每 3 秒
        // probe 一次然后无限排下去
        recheck?.cancel()
        recheck = nil
        guard let node = nodePath, let bridge = bridgePath else {
            NSLog("DeskPulse: bundle 里缺 node 或 bridge 脚本")
            return
        }
        ensureDir(supportDir)
        ensureDir(dataDir)
        ensureDir(logsDir)

        // 首次运行把包内默认配置拷到用户目录，之后用户改的是这份
        let userCfg = supportDir.appendingPathComponent("config.json")
        if !fm.fileExists(atPath: userCfg.path), let def = res("bridge/config.json") {
            try? fm.copyItem(at: def, to: userCfg)
        }

        let p = Process()
        p.executableURL = node
        p.arguments = ["--no-warnings", bridge.path]
        var env = ProcessInfo.processInfo.environment
        env["DP_PACKAGED"] = "1"
        env["DP_HOME"] = supportDir.path
        // 让桥接盯着父进程：App 被强杀时它自己退出，不留孤儿占端口
        env["DP_SUPERVISED"] = "1"
        p.environment = env

        if !fm.fileExists(atPath: engineLog.path) {
            fm.createFile(atPath: engineLog.path, contents: nil)
        }
        if let fh = try? FileHandle(forWritingTo: engineLog) {
            fh.seekToEndOfFile()
            p.standardOutput = fh
            p.standardError = fh
        }

        p.terminationHandler = { [weak self] proc in
            DispatchQueue.main.async {
                guard let self else { return }
                self.child = nil
                // 崩溃自动拉起，但用户主动退出时不复活
                if self.shouldRun {
                    DispatchQueue.main.asyncAfter(deadline: .now() + 2) { self.start() }
                }
            }
        }

        do {
            try p.run()
            child = p
        } catch {
            NSLog("DeskPulse: 启动桥接失败 \(error)")
        }
    }
}

// ---------- 菜单栏 ----------
final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    let engine = Engine()
    var statusItem: NSStatusItem!
    var menu: NSMenu!
    var statusLine: NSMenuItem!
    var loginItem: NSMenuItem!
    var timer: Timer?
    var online = false
    var providers: [String] = []

    func applicationDidFinishLaunching(_ note: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        // 中文不适合用等宽字体（会掉进 fallback 字体，字重对不齐），用系统字体
        statusItem.button?.font = NSFont.systemFont(ofSize: 12, weight: .medium)

        menu = NSMenu()
        menu.delegate = self

        let title = NSMenuItem(title: "\(kAppName) \(kVersion)", action: nil, keyEquivalent: "")
        title.isEnabled = false
        menu.addItem(title)

        statusLine = NSMenuItem(title: "启动中…", action: nil, keyEquivalent: "")
        statusLine.isEnabled = false
        menu.addItem(statusLine)
        menu.addItem(.separator())

        addItem("打开状态页", #selector(openState))
        addItem("重启桥接", #selector(restartEngine))
        addItem("查看日志", #selector(openLog))
        menu.addItem(.separator())

        loginItem = NSMenuItem(title: "登录时自动启动", action: #selector(toggleLogin), keyEquivalent: "")
        loginItem.target = self
        menu.addItem(loginItem)
        menu.addItem(.separator())

        addItem("退出\(kAppName)", #selector(quit))

        statusItem.menu = menu

        engine.start()
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            self?.refresh()
        }
    }

    private func addItem(_ title: String, _ action: Selector) {
        let i = NSMenuItem(title: title, action: action, keyEquivalent: "")
        i.target = self
        menu.addItem(i)
    }

    func menuWillOpen(_ menu: NSMenu) { refresh() }

    /// 刷新中标记：探针是异步的，上一发没回来就跳过这一拍，
    /// 避免桥接不在线时堆起一串并发请求。
    private var refreshing = false

    func refresh() {
        if refreshing { return }
        refreshing = true
        probeAsync { [weak self] up, names in
            // URLSession 回调在后台线程，UI 必须回主线程改
            DispatchQueue.main.async {
                guard let self else { return }
                self.refreshing = false
                self.online = up
                self.providers = names
                self.applyStatus()
            }
        }
    }

    /// 把探测结果写进菜单。主线程执行，不再有任何同步等待。
    private func applyStatus() {
        updateTitle()

        if online {
            let names = providers.isEmpty ? "" : " · " + providers.joined(separator: " / ")
            statusLine.title = engine.external
                ? "外部实例占用 \(kPort)\(names)"
                : "运行中 · 127.0.0.1:\(kPort)\(names)"
        } else {
            statusLine.title = "未运行（点击“重启桥接”）"
        }

        switch loginState() {
        case .on:
            loginItem.state = .on
            loginItem.title = "登录时自动启动"
        case .needsApproval:
            loginItem.state = .mixed
            loginItem.title = "登录时自动启动（待系统设置里允许）"
        case .off, .failed:
            loginItem.state = .off
            loginItem.title = "登录时自动启动"
        }
    }

    func updateTitle() {
        guard let button = statusItem.button else { return }
        let dot = online ? "●" : "○"
        let color: NSColor = online ? .systemGreen : (engine.child != nil ? .systemOrange : .systemRed)
        let s = NSMutableAttributedString(string: "\(dot) \(kAppName)")
        // 只给那个圆点上色，文字跟随系统菜单栏前景色，深色/浅色主题都不会看丢
        s.addAttribute(.foregroundColor, value: color, range: NSRange(location: 0, length: 1))
        button.attributedTitle = s
    }

    @objc func openState() {
        NSWorkspace.shared.open(stateURL)
    }

    @objc func restartEngine() {
        engine.restart()
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { self.refresh() }
    }

    @objc func openLog() {
        if fm.fileExists(atPath: engineLog.path) {
            NSWorkspace.shared.open(engineLog)
        } else {
            NSWorkspace.shared.open(logsDir)
        }
    }

    @objc func toggleLogin() {
        switch loginState() {
        case .on, .needsApproval:
            disableLogin()
        case .off, .failed:
            if case .failed(let msg) = enableLogin() {
                let alert = NSAlert()
                alert.messageText = "无法设置开机自启"
                alert.informativeText = msg + "\n\n可以手动把「\(kAppName)」拖进「系统设置 → 通用 → 登录项」。"
                alert.addButton(withTitle: "好")
                NSApp.activate(ignoringOtherApps: true)
                alert.runModal()
            }
        }
        refresh()
    }

    @objc func quit() {
        // stop() 里已经 terminate + waitUntilExit（原来是先 stop 再取 child，
        // child 早被置 nil，那段"收干净"其实是死代码）
        engine.stop()
        NSApp.terminate(nil)
    }

    func applicationWillTerminate(_ note: Notification) {
        timer?.invalidate()
    }
}

// ---------- 入口 ----------
let argv = Array(CommandLine.arguments.dropFirst())

// --version 除了给脚本用，也是唯一能在没有屏幕权限时验证「显示名到底是什么」的办法：
// Swift 对 ≤15 字节的字符串字面量走内联小字符串，不落到 __cstring，
// strings(1) 和 grep 都搜不到，只能让它自己吐出来。
if argv.contains("--version") || argv.contains("-v") {
    print("\(kAppName) \(kVersion)")
    exit(0)
}

if argv.contains("--install-login") {
    switch enableLogin() {
    case .on:
        print("已开启登录自启")
    case .needsApproval:
        print("已登记，但需要在「系统设置 → 通用 → 登录项」里允许「\(kAppName)」")
    case .failed(let msg):
        print("设置失败：\(msg)")
        exit(1)
    case .off:
        print("状态未知，请到「系统设置 → 登录项」确认")
    }
    exit(0)
}
if argv.contains("--uninstall-login") {
    disableLogin()
    print("已取消登录自启")
    exit(0)
}
if argv.contains("--status") {
    let p = probe()
    let names = p.providers.joined(separator: ",")
    let login: String
    switch loginState() {
    case .on: login = "on"
    case .needsApproval: login = "needs-approval"
    case .failed: login = "failed"
    case .off: login = "off"
    }
    print("online=\(p.up) port=\(kPort) providers=\(names.isEmpty ? "-" : names) login=\(login)")
    exit(p.up ? 0 : 1)
}

// 单实例：已经在跑就退出（比如登录自启和手动双击撞上）
// 按 PID 比，不按对象相等比：NSRunningApplication 的 == 是对象身份，
// 不保证 runningApplications() 返回的就是 current 那一个实例。
let myPid = ProcessInfo.processInfo.processIdentifier
let others = NSRunningApplication.runningApplications(withBundleIdentifier: Bundle.main.bundleIdentifier ?? "")
    .filter { $0.processIdentifier != myPid }
if !others.isEmpty {
    exit(0)
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
_ = app.setActivationPolicy(.accessory)
app.run()
