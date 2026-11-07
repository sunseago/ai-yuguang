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
let kPort = 8787
let kLabel = "cn.deskpulse.app.startup"
let kVersion = "0.1.3"
/// 显示名。菜单栏、菜单标题、各处提示都用这一个，改名字只改这里。
/// 最终定名「AI余光」——不是「余光」，也不是「Agent余光」，别自作主张简写。
let kAppName = "AI余光"

let fm = FileManager.default
let home = fm.homeDirectoryForCurrentUser
let supportDir = home.appendingPathComponent("Library/Application Support/DeskPulse")
let dataDir = supportDir.appendingPathComponent("data")
let logsDir = home.appendingPathComponent("Library/Logs")
let engineLog = logsDir.appendingPathComponent("DeskPulse.log")
let agentsDir = home.appendingPathComponent("Library/LaunchAgents")
let startupPlist = agentsDir.appendingPathComponent("\(kLabel).plist")
let stateURL = URL(string: "http://127.0.0.1:\(kPort)/v1/state")!

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

/// 探一下端口上有没有活的桥接。返回 (是否在线, 供应商标识列表)
func probe() -> (up: Bool, providers: [String]) {
    var req = URLRequest(url: URL(string: "http://127.0.0.1:\(kPort)/healthz")!)
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
        if let d = data,
           let obj = try? JSONSerialization.jsonObject(with: d) as? [String: Any],
           let arr = obj["providers"] as? [[String: Any]] {
            names = arr.compactMap { $0["name"] as? String }
        }
        sem.signal()
    }.resume()
    _ = sem.wait(timeout: .now() + 2.0)
    return (true, names)
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
    /// 端口上的实例不是我们起的（比如用户手动 node 起的），这时不抢
    var external = false
    var shouldRun = true

    func start() {
        shouldRun = true
        if probe().up {
            external = true
            child = nil
            return
        }
        external = false
        spawn()
    }

    func stop() {
        shouldRun = false
        child?.terminate()
        child = nil
    }

    func restart() {
        child?.terminate()
        child = nil
        external = false
        shouldRun = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { [weak self] in self?.start() }
    }

    private func spawn() {
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

    func refresh() {
        let p = probe()
        online = p.up
        providers = p.providers
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
        engine.stop()
        // 退出前把子进程收干净，避免留下孤儿占着端口
        if let c = engine.child, c.isRunning {
            c.terminate()
            c.waitUntilExit()
        }
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
let others = NSRunningApplication.runningApplications(withBundleIdentifier: Bundle.main.bundleIdentifier ?? "")
    .filter { $0 != NSRunningApplication.current }
if !others.isEmpty {
    exit(0)
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
_ = app.setActivationPolicy(.accessory)
app.run()
