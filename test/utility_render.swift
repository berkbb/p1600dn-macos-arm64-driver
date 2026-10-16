// Draws the utility window to a PNG, so its layout can be looked at and
// checked: the real window (makeUtilityWindow) with a stand-in printer, placed
// off screen and captured with ScreenCaptureKit. An app may capture its own
// windows without the Screen Recording permission (macOS 14.4+).
//
// Built with the app's source and -D UTILITY_RENDER, and run from inside a
// copy of the app bundle so its localizations load (language: -AppleLanguages).
//
// Usage: utility_render <out.png> grouped|legacy light|dark printer|printers|empty|error
// Prints "<width>x<height> colors=<distinct colors>".
import AppKit
import ScreenCaptureKit
import SwiftUI

@main
struct UtilityRender {
    static func main() {
        let arguments = CommandLine.arguments
        // AppKit reads -AppleLanguages ("(tr)") from the arguments after these.
        guard arguments.count >= 5 else {
            FileHandle.standardError.write("Usage: utility_render <out.png> grouped|legacy light|dark printer|printers|empty|error\n".data(using: .utf8)!)
            exit(2)
        }
        let application = NSApplication.shared
        application.setActivationPolicy(.accessory)
        UtilityView.legacyLayout = arguments[2] == "legacy"

        let model = UtilityModel.shared
        // The selection first: with no queues yet, queueName's didSet reads
        // nothing from a printer.
        model.queueName = "HP_LaserJet_P1606dn"
        let usb = PrinterQueue(name: "HP_LaserJet_P1606dn", info: "HP LaserJet Professional P1606dn",
                               deviceURI: "usb://HP/LaserJet%20Professional%20P1606dn?serial=SYNTH0001", state: 3, tonerLevel: 62)
        let office = PrinterQueue(name: "Office", info: "Office P1606dn", deviceURI: "socket://192.0.2.7", state: 4, tonerLevel: 9)
        switch arguments[4] {
        case "empty":
            model.queues = []
        case "printers":
            model.queues = [usb, office]
        default:
            model.queues = [usb]
        }
        model.task = nil
        model.settingsRead = arguments[4] != "error"
        model.readFailed = arguments[4] == "error"
        model.sleepIndex = arguments[4] == "error" ? -1 : 2
        model.autoOffIndex = arguments[4] == "error" ? -1 : 5
        model.quietMode = arguments[4] == "error" ? -1 : 1
        model.message = arguments[4] == "error" ? ownText("NotConnected") : ""
        model.messageIsError = arguments[4] == "error"

        let window = makeUtilityWindow(model: model)
        window.appearance = NSAppearance(named: arguments[3] == "dark" ? .darkAqua : .aqua)
        window.setFrameOrigin(NSPoint(x: -8000, y: -8000))
        window.orderFrontRegardless()

        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 1_000_000_000)
            do {
                let content = try await SCShareableContent.currentProcess
                guard let captured = content.windows.first(where: { $0.windowID == CGWindowID(window.windowNumber) }) else {
                    print("capture failed: window not found"); exit(1)
                }
                let filter = SCContentFilter(desktopIndependentWindow: captured)
                let configuration = SCStreamConfiguration()
                configuration.width = Int(filter.contentRect.width * CGFloat(filter.pointPixelScale))
                configuration.height = Int(filter.contentRect.height * CGFloat(filter.pointPixelScale))
                configuration.ignoreShadowsSingleWindow = true
                let image = try await SCScreenshotManager.captureImage(contentFilter: filter, configuration: configuration)
                let bitmap = NSBitmapImageRep(cgImage: image)
                try bitmap.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: arguments[1]))
                var colors = Set<UInt32>()
                for y in stride(from: 0, to: bitmap.pixelsHigh, by: 2) {
                    for x in stride(from: 0, to: bitmap.pixelsWide, by: 2) {
                        guard let color = bitmap.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB) else { continue }
                        colors.insert(UInt32(color.redComponent * 255) << 16 | UInt32(color.greenComponent * 255) << 8 | UInt32(color.blueComponent * 255))
                    }
                }
                print("\(Int(window.frame.width))x\(Int(window.frame.height)) colors=\(colors.count)")
                exit(0)
            } catch {
                print("capture failed: \(error)")
                exit(1)
            }
        }
        application.run()
    }
}
