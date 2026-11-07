// 把生成的原图裁成 macOS 应用图标。
//
// 为什么要裁：AI 出的图右下角会带「AI生成 / WORKBUDDY」水印，又在最角落里。
// 涂掉等于编像素，裁掉更干净——眼睛和星芒都在画面中部，裁一个以眼睛为中心的正方形
// 就能完全避开，而且裁出来的奶油底色本身就是图标底板，不存在拼接痕迹。
//
// 为什么要重画而不是直接用：macOS 的图标有固定网格——1024 画布、824 内容区、
// 圆角 185.4、还要有投影。生成图是满版直角方图，直接用会显得比别人的图标大一圈。
//
//   swiftc -O -o make-icon make-icon.swift && ./make-icon in.png out.png

import AppKit
import CoreGraphics
import Foundation

let args = CommandLine.arguments
guard args.count >= 3 else {
    FileHandle.standardError.write("用法: make-icon <输入.png> <输出.png>\n".data(using: .utf8)!)
    exit(2)
}

guard let img = NSImage(contentsOfFile: args[1]),
      let src = img.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
    FileHandle.standardError.write("读不到图: \(args[1])\n".data(using: .utf8)!)
    exit(1)
}

// ---- 裁切 ----
// 默认值按第 3 版素材量的：眼白圆盘约 x:200..835 y:235..840，上眼睑弧 x:172..860 y:175..470，
// 星芒到 x:935 y:75，水印落在 y>950。边长 860、中心 (508,500) 刚好全包主体、挡掉水印。
// 换成别的素材时直接在命令行覆盖这三个数，不用改代码重编。
let cropSide: CGFloat = args.count > 5 ? (CGFloat(Double(args[5]) ?? 860)) : 860
let cropCenter = CGPoint(x: args.count > 3 ? (CGFloat(Double(args[3]) ?? 508)) : 508,
                         y: args.count > 4 ? (CGFloat(Double(args[4]) ?? 500)) : 500)
let cropRect = CGRect(x: cropCenter.x - cropSide / 2,
                      y: cropCenter.y - cropSide / 2,
                      width: cropSide, height: cropSide)

guard let face = src.cropping(to: cropRect) else {
    FileHandle.standardError.write("裁切失败（图可能小于裁切框）\n".data(using: .utf8)!)
    exit(1)
}

// ---- 按 macOS 图标网格重画 ----
let canvas: CGFloat = 1024          // 画布
let inset: CGFloat = 100            // 内容区 824，四周各留 100
let content = canvas - inset * 2
let radius: CGFloat = 185.4         // Big Sur 之后的圆角比例
let faceRect = CGRect(x: inset, y: inset, width: content, height: content)

guard let ctx = CGContext(data: nil,
                          width: Int(canvas), height: Int(canvas),
                          bitsPerComponent: 8, bytesPerRow: 0,
                          space: CGColorSpaceCreateDeviceRGB(),
                          bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
    exit(1)
}
ctx.setAllowsAntialiasing(true)
ctx.interpolationQuality = .high

let squircle = CGPath(roundedRect: faceRect, cornerWidth: radius, cornerHeight: radius, transform: nil)

// 先铺一层实底 + 投影：图标在浅色桌面和深色 Dock 上都立得住
ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: -8), blur: 26,
              color: NSColor.black.withAlphaComponent(0.30).cgColor)
ctx.addPath(squircle)
ctx.setFillColor(NSColor.white.cgColor)
ctx.fillPath()
ctx.restoreGState()

// 再把裁好的画面按圆角切进内容区
ctx.saveGState()
ctx.addPath(squircle)
ctx.clip()
ctx.draw(face, in: faceRect)
ctx.restoreGState()

guard let out = ctx.makeImage() else { exit(1) }
let rep = NSBitmapImageRep(cgImage: out)
guard let data = rep.representation(using: .png, properties: [:]) else { exit(1) }
try data.write(to: URL(fileURLWithPath: args[2]))
print("已写出 \(args[2])")
