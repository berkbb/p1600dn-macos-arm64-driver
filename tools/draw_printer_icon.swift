// Draws the driver's printer icon and the apps' icon (the project's own
// artwork) as PNG files.
//
// Usage: swift tools/draw_printer_icon.swift <output directory>
// Writes printer-<size>.png (the free-standing printer for the PPD and
// AirPrint) and app-<size>.png (the printer on a rounded square on the macOS
// app icon grid, for the two apps) for 16, 32, 64, 128, 256, 512 and 1024
// pixels; tools/make_printer_icon.sh turns them into assets/P1600dnPrinter.png,
// assets/P1600dnPrinter.icns and assets/P1600dnApp.icns.
import CoreGraphics
import Foundation
import ImageIO
import SwiftUI
import UniformTypeIdentifiers

func color(_ hex: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((hex >> 16) & 0xff) / 255, green: CGFloat((hex >> 8) & 0xff) / 255,
            blue: CGFloat(hex & 0xff) / 255, alpha: alpha)
}

func fillGradient(_ ctx: CGContext, _ path: CGPath, top: UInt32, bottom: UInt32) {
    ctx.saveGState()
    ctx.addPath(path)
    ctx.clip()
    let box = path.boundingBox
    let gradient = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB),
                              colors: [color(top), color(bottom)] as CFArray, locations: [0, 1])!
    ctx.drawLinearGradient(gradient, start: CGPoint(x: box.midX, y: box.maxY),
                           end: CGPoint(x: box.midX, y: box.minY), options: [])
    ctx.restoreGState()
}

func rounded(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat, _ r: CGFloat) -> CGPath {
    CGPath(roundedRect: CGRect(x: x, y: y, width: w, height: h), cornerWidth: r, cornerHeight: r, transform: nil)
}

/// The printer on a 1024-unit canvas (y up).
func drawPrinter(_ ctx: CGContext) {
    // Shadow under the printer.
    ctx.setFillColor(color(0x000000, 0.18))
    ctx.fillEllipse(in: CGRect(x: 170, y: 150, width: 684, height: 70))

    // Sheet coming out of the top, with lines of text and a gray ramp.
    let sheet = rounded(322, 560, 380, 380, 14)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -6), blur: 18, color: color(0x000000, 0.25))
    ctx.addPath(sheet)
    ctx.setFillColor(color(0xFFFFFF))
    ctx.fillPath()
    ctx.restoreGState()
    ctx.setFillColor(color(0x9AA3AE))
    for (i, width) in [260, 300, 220, 280].enumerated() {
        ctx.fill(CGRect(x: 372, y: 870 - CGFloat(i) * 34, width: CGFloat(width), height: 14))
    }
    for step in 0..<6 {
        let gray = UInt32(0x22 + step * 0x26)
        ctx.setFillColor(color(gray << 16 | gray << 8 | gray))
        ctx.fill(CGRect(x: 372 + CGFloat(step) * 47, y: 700, width: 47, height: 40))
    }

    // Body.
    let body = rounded(140, 210, 744, 430, 76)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -10), blur: 30, color: color(0x000000, 0.22))
    ctx.addPath(body)
    ctx.setFillColor(color(0xD9DDE2))
    ctx.fillPath()
    ctx.restoreGState()
    fillGradient(ctx, body, top: 0xF6F7F9, bottom: 0xC3C8CF)

    // Top cover with the output slot the sheet comes from.
    let cover = rounded(184, 560, 656, 92, 40)
    fillGradient(ctx, cover, top: 0x4A505A, bottom: 0x23272E)
    ctx.setFillColor(color(0x101317))
    ctx.fill(CGRect(x: 312, y: 600, width: 400, height: 18))
    ctx.setFillColor(color(0xFFFFFF))
    ctx.fill(CGRect(x: 322, y: 612, width: 380, height: 40))

    // Front: paper tray seam and the control panel with its ready light.
    ctx.setFillColor(color(0x8E959E))
    ctx.fill(CGRect(x: 200, y: 330, width: 624, height: 10))
    let panel = rounded(650, 450, 170, 64, 22)
    fillGradient(ctx, panel, top: 0x3A3F47, bottom: 0x1F2329)
    ctx.setFillColor(color(0x34C759))
    ctx.fillEllipse(in: CGRect(x: 676, y: 466, width: 32, height: 32))
    ctx.setFillColor(color(0xC8CDD3))
    ctx.fillEllipse(in: CGRect(x: 740, y: 466, width: 32, height: 32))
}

/// The app icon on the macOS grid: the 824-unit rounded square with Apple's
/// continuous corners (radius 185.4) and its drop shadow, a blue gradient, and
/// the printer at 74% inside it. macOS shows such an icon as it is; a
/// free-standing shape would be put on a gray or dark square instead.
func drawApp(_ ctx: CGContext) {
    let tile = RoundedRectangle(cornerRadius: 185.4, style: .continuous)
        .path(in: CGRect(x: 100, y: 100, width: 824, height: 824)).cgPath
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -10), blur: 20, color: color(0x000000, 0.3))
    ctx.addPath(tile)
    ctx.setFillColor(color(0x2F7FEA))
    ctx.fillPath()
    ctx.restoreGState()
    fillGradient(ctx, tile, top: 0x5AACFF, bottom: 0x1858CC)

    // The printer (x 140-884, y 150-940 with its sheet) centered a little low.
    ctx.saveGState()
    ctx.addPath(tile)
    ctx.clip()
    ctx.translateBy(x: 512, y: 498)
    ctx.scaleBy(x: 0.74, y: 0.74)
    ctx.translateBy(x: -512, y: -545)
    drawPrinter(ctx)
    ctx.restoreGState()
}

/// One drawing on the 1024-unit canvas, scaled to `size` pixels.
func render(size: Int, _ draw: (CGContext) -> Void) -> CGImage {
    let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.scaleBy(x: CGFloat(size) / 1024, y: CGFloat(size) / 1024)
    ctx.setShouldAntialias(true)
    draw(ctx)
    return ctx.makeImage()!
}

let out = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : ".")
for (name, draw) in [("printer", drawPrinter), ("app", drawApp)] {
    for size in [16, 32, 64, 128, 256, 512, 1024] {
        let url = out.appendingPathComponent("\(name)-\(size).png")
        let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
        CGImageDestinationAddImage(dest, render(size: size, draw), nil)
        guard CGImageDestinationFinalize(dest) else { fatalError("cannot write \(url.path)") }
    }
}
