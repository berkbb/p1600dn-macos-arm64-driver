// P1600dn Series Utility: the printer utility of the P1600dn series as a
// native arm64 app (macOS 11+). It offers information pages (cleaning, demo,
// configuration, supply status, and the driver's diagnostic page), EcoSMART
// (sleep after, auto off after), Quiet Mode and HTML Config, in the layout of
// current macOS settings windows: a printer header with its state and toner
// level, grouped sections (System Settings style from macOS 13), a Print
// button per page, and settings that take effect when they are changed. The
// work is done by the C core (src/p1600_utility.c, src/p1600_diagnostic.c).
//
// P1600dn Serisi Yardımcı Programı: P1600dn serisinin yerel arm64 yazıcı
// yardımcı programı, güncel macOS ayar pencereleri düzeninde.

import AppKit
import SwiftUI

/// The utility's texts (Utility.strings).
func ownText(_ key: String) -> String { NSLocalizedString(key, tableName: "Utility", comment: "") }

private func cString(_ buffer: [CChar]) -> String {
    buffer.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
}

/// The C core's reason, in the user's language where it is a common one.
private func failureText(_ error: [CChar]) -> String {
    let detail = cString(error)
    if detail.contains("not found") { return ownText("NotConnected") }
    if detail.contains("busy") { return ownText("Busy") }
    return ownText("Unreachable") + (detail.isEmpty ? "" : " " + detail)
}

/// The utility window: fixed width, as tall as its content, not resizable.
/// SwiftUI's WindowGroup would restore any earlier size around the content.
func makeUtilityWindow(model: UtilityModel) -> NSWindow {
    let controller = NSHostingController(rootView: UtilityView(model: model))
    if #available(macOS 13, *) { controller.sizingOptions = [.preferredContentSize] }
    let window = NSWindow(contentViewController: controller)
    window.styleMask = [.titled, .closable, .miniaturizable]
    window.title = ownText("AppTitle")
    window.isReleasedWhenClosed = false
    window.center()
    return window
}

/// The CUPS queue in an Open Application event's 'prdt' parameter.
private func eventPrinter(_ event: NSAppleEventDescriptor?) -> String? {
    event?.paramDescriptor(forKeyword: AEKeyword(0x7072_6474))?.stringValue  // 'prdt'
}

/// macOS opens the printer utility (`*APPrinterUtilityPath`) with an Open
/// Application event whose 'prdt' parameter is the CUPS queue name. A running
/// utility gets the same event (Printers & Scanners, a notification for
/// another printer) and switches to that printer.
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var window: NSWindow?
    private var proxyWatch: Timer?
    /// Launch check for the test suite: the queues macOS handed over, one per
    /// line, then the app quits after P1600DN_UTILITY_LAUNCH_EVENTS events.
    private let launchReport = ProcessInfo.processInfo.environment["P1600DN_UTILITY_LAUNCH_REPORT"]
    private let launchEvents = Int(ProcessInfo.processInfo.environment["P1600DN_UTILITY_LAUNCH_EVENTS"] ?? "") ?? 1
    private var reported: [String] = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        let printer = eventPrinter(NSAppleEventManager.shared().currentAppleEvent)
        // Later Open and Reopen Application events reach the running utility.
        for id in [kAEOpenApplication, kAEReopenApplication] {
            NSAppleEventManager.shared().setEventHandler(self, andSelector: #selector(openEvent(_:withReply:)),
                                                         forEventClass: AEEventClass(kCoreEventClass), andEventID: AEEventID(id))
        }
        if launchReport != nil {
            report(printer)
            return
        }
        UtilityModel.shared.start(queue: printer)
        let window = makeUtilityWindow(model: UtilityModel.shared)
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        self.window = window
    }

    @objc private func openEvent(_ event: NSAppleEventDescriptor, withReply reply: NSAppleEventDescriptor) {
        let printer = eventPrinter(event)
        if launchReport != nil {
            report(printer)
            return
        }
        if let printer = printer, !printer.isEmpty { UtilityModel.shared.select(queue: printer) }
        window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    private func report(_ printer: String?) {
        guard let path = launchReport else { return }
        reported.append(printer ?? "")
        try? reported.joined(separator: "\n").write(toFile: path, atomically: true, encoding: .utf8)
        if reported.count >= launchEvents { NSApp.terminate(nil) }
    }

    /// HTML Config over USB runs in this process: with its window closed the
    /// utility keeps serving the printer's pages until they are idle
    /// (P1600_EWS_IDLE_SECONDS), then quits.
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        guard UtilityModel.shared.webPagesServed else { return true }
        proxyWatch?.invalidate()
        proxyWatch = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] timer in
            guard let self = self else { return }
            if self.window?.isVisible == true {
                timer.invalidate()  // closing the window again asks again
            } else if !UtilityModel.shared.webPagesServed {
                timer.invalidate()
                NSApp.terminate(nil)
            }
        }
        return false
    }
}

struct PrinterQueue: Identifiable, Hashable {
    let name: String
    let info: String
    let deviceURI: String
    var state: Int32 = 0           // printer-state: 3 idle, 4 printing, 5 stopped
    var tonerLevel: Int32 = -1     // percent, -1 unknown
    var id: String { name }

    var title: String { info.isEmpty ? name : info }
    var connection: String { deviceURI.hasPrefix("usb:") ? "USB" : ownText("Network") }
    var stateText: String? {
        switch state {
        case 3: return ownText("Ready")
        case 4: return ownText("Printing")
        case 5: return ownText("Stopped")
        default: return nil
        }
    }

    init(name: String, info: String, deviceURI: String, state: Int32 = 0, tonerLevel: Int32 = -1) {
        self.name = name
        self.info = info
        self.deviceURI = deviceURI
        self.state = state
        self.tonerLevel = tonerLevel
    }

    init(_ entry: P1600Queue) {
        var entry = entry
        func string<T>(_ field: inout T) -> String {
            withUnsafeBytes(of: &field) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
        }
        self.init(name: string(&entry.name), info: string(&entry.info), deviceURI: string(&entry.device_uri),
                  state: entry.state, tonerLevel: entry.toner_level)
    }
}

/// Information page rows: the printer's own pages (raw values 1-4), then the
/// driver's diagnostic page, which is printed through the CUPS queue.
enum InformationPage: Int, CaseIterable {
    case cleaning = 1, demo, configuration, supplyStatus, diagnostic

    var title: String {
        switch self {
        case .cleaning: return ownText("CleaningPage")
        case .demo: return ownText("DemoPage")
        case .configuration: return ownText("ConfigurationPage")
        case .supplyStatus: return ownText("SuppliesPage")
        case .diagnostic: return ownText("DiagnosticPage")
        }
    }
}

/// What the utility is doing with the printer; one thing at a time.
enum UtilityTask: Equatable {
    case reading, saving, printing(InformationPage), openingWebPages
}

final class UtilityModel: ObservableObject {
    static let shared = UtilityModel()

    @Published var queues: [PrinterQueue] = []
    @Published var queueName = "" {
        didSet { if oldValue != queueName { readSettings() } }
    }
    /// Menu positions; -1 shows "—" (not read, or a value the menu lacks).
    @Published var sleepIndex = -1
    @Published var autoOffIndex = -1
    @Published var quietMode = -1          // 1 on, 0 off, -1 unknown
    /// The printer answered the PJL INQUIRE; until then the settings stay off.
    @Published var settingsRead = false
    @Published var readFailed = false
    @Published var task: UtilityTask? = nil
    @Published var message = ""
    @Published var messageIsError = false
    @Published var confirmCleaning = false

    private var readSleep = -1, readAutoOff = -1, readQuiet = -1
    private var webProxy: OpaquePointer?
    /// Printer I/O, in order.
    private let printer = DispatchQueue(label: "com.berkbb.p1600dn.utility.printer", qos: .userInitiated)

    var busy: Bool { task != nil }
    var selectedQueue: PrinterQueue? { queues.first { $0.name == queueName } }
    /// HTML Config's USB proxy is serving the printer's pages.
    var webPagesServed: Bool { webProxy.map { p1600_ews_proxy_running($0) } ?? false }

    func start(queue: String?) {
        var found = [P1600Queue](repeating: P1600Queue(), count: 32)
        let count = Int(p1600_queue_list(&found, 32))
        queues = (0..<min(max(count, 0), 32)).map { PrinterQueue(found[$0]) }
        let named = queue.map { !$0.isEmpty && select(queue: $0) } ?? false
        if !named, let first = queues.first { queueName = first.name }
        refreshTimer?.invalidate()
        refreshTimer = Timer.scheduledTimer(withTimeInterval: Self.refreshInterval, repeats: true) { [weak self] _ in
            self?.refreshQueue()
        }
    }

    /// The header's printer state and toner level follow CUPS: looked up
    /// again every few seconds, after each printer task and when macOS names
    /// the printer again. Off the printer queue, which can wait 10 s on USB.
    static let refreshInterval: TimeInterval = 5
    private var refreshTimer: Timer?
    private var refreshing = false
    private let lookup = DispatchQueue(label: "com.berkbb.p1600dn.utility.cups", qos: .utility)

    func refreshQueue() {
        guard !refreshing, let name = selectedQueue?.name else { return }
        refreshing = true
        lookup.async {
            var entry = P1600Queue()
            var error = [CChar](repeating: 0, count: 256)
            let found = p1600_queue_find(name, &entry, &error, error.count) == 0
            DispatchQueue.main.async {
                self.refreshing = false
                guard found, let index = self.queues.firstIndex(where: { $0.name == name }) else { return }
                let fresh = PrinterQueue(entry)
                if self.queues[index].state != fresh.state { self.queues[index].state = fresh.state }
                if self.queues[index].tonerLevel != fresh.tonerLevel { self.queues[index].tonerLevel = fresh.tonerLevel }
            }
        }
    }

    /// The queue macOS names, at launch or while the utility runs; it is used
    /// even if its model name was changed. False when CUPS does not know it.
    @discardableResult
    func select(queue: String) -> Bool {
        if !queues.contains(where: { $0.name == queue }) {
            var entry = P1600Queue()
            var error = [CChar](repeating: 0, count: 256)
            guard p1600_queue_find(queue, &entry, &error, error.count) == 0 else {
                show(cString(error), error: true)
                return false
            }
            queues.insert(PrinterQueue(entry), at: 0)
        }
        if queueName != queue {
            // A switch waits for the printer task under way.
            if busy {
                pendingQueue = queue
            } else {
                queueName = queue
            }
        }
        refreshQueue()
        return true
    }

    /// A printer named while a task ran; selected when it ends.
    private var pendingQueue: String?

    private func finishTask() {
        task = nil
        if let queue = pendingQueue {
            pendingQueue = nil
            queueName = queue
        }
        refreshQueue()
    }

    /// Reads the chosen printer's settings: POWERSAVETIME, INACTIVEOFFTIME, QUIETMODE.
    func readSettings() {
        guard let queue = selectedQueue else { return }
        task = .reading
        settingsRead = false
        readFailed = false
        message = ""
        printer.async {
            var settings = P1600PrinterSettings(sleep_minutes: -1, auto_off_minutes: -1, quiet_mode: -1)
            var error = [CChar](repeating: 0, count: 256)
            let read = p1600_utility_read_settings(queue.deviceURI, &settings, &error, error.count)
            DispatchQueue.main.async {
                self.finishTask()
                self.readSleep = Int(settings.sleep_minutes)
                self.readAutoOff = Int(settings.auto_off_minutes)
                self.readQuiet = Int(settings.quiet_mode)
                self.sleepIndex = Int(p1600_sleep_index(settings.sleep_minutes))
                self.autoOffIndex = Int(p1600_auto_off_index(settings.auto_off_minutes))
                self.quietMode = self.readQuiet
                self.settingsRead = read >= 0
                self.readFailed = read < 0
                if read < 0 { self.show(failureText(error), error: true) }
            }
        }
    }

    /// A setting takes effect when it is changed: the PJL SET command for that
    /// setting alone, and only for a new value.
    func setSleep(_ index: Int) {
        let minutes = Int(p1600_sleep_choice(Int32(index)))
        guard minutes >= 0, minutes != readSleep else { return }
        sleepIndex = index
        var buffer = [CChar](repeating: 0, count: 256)
        let length = p1600_utility_format_sleep(Int32(minutes), &buffer, buffer.count)
        send(Array(buffer[0..<length]), saved: { self.readSleep = minutes },
             reverted: { self.sleepIndex = Int(p1600_sleep_index(Int32(self.readSleep))) })
    }

    func setAutoOff(_ index: Int) {
        let minutes = Int(p1600_auto_off_choice(Int32(index)))
        guard minutes >= 0, minutes != readAutoOff else { return }
        autoOffIndex = index
        var buffer = [CChar](repeating: 0, count: 256)
        let length = p1600_utility_format_auto_off(Int32(minutes), &buffer, buffer.count)
        send(Array(buffer[0..<length]), saved: { self.readAutoOff = minutes },
             reverted: { self.autoOffIndex = Int(p1600_auto_off_index(Int32(self.readAutoOff))) })
    }

    func setQuietMode(_ on: Bool) {
        let value = on ? 1 : 0
        guard value != readQuiet else { return }
        quietMode = value
        var buffer = [CChar](repeating: 0, count: 256)
        let length = p1600_utility_format_quiet_mode(on, &buffer, buffer.count)
        send(Array(buffer[0..<length]), saved: { self.readQuiet = value },
             reverted: { self.quietMode = self.readQuiet })
    }

    private func send(_ command: [CChar], saved: @escaping () -> Void, reverted: @escaping () -> Void) {
        guard let queue = selectedQueue, !command.isEmpty else { reverted(); return }
        task = .saving
        message = ""
        printer.async {
            var error = [CChar](repeating: 0, count: 256)
            let failed = command.withUnsafeBufferPointer { bytes in
                p1600_utility_send(queue.deviceURI, bytes.baseAddress, bytes.count, &error, error.count) != 0
            }
            DispatchQueue.main.async {
                self.finishTask()
                if failed {
                    reverted()
                    self.show(failureText(error), error: true)
                } else {
                    saved()
                    self.show(ownText("SettingSaved"), error: false)
                }
            }
        }
    }

    /// The printer's information pages (PJL, in the window's language) and the
    /// driver's diagnostic page, which goes through the CUPS queue: driver
    /// filter and page guard. The cleaning page asks first.
    func printPage(_ page: InformationPage, confirmed: Bool = false) {
        guard let queue = selectedQueue, !busy else { return }
        if page == .cleaning && !confirmed {
            confirmCleaning = true
            return
        }
        task = .printing(page)
        message = ""
        let localization = Bundle.main.preferredLocalizations.first ?? "en"
        printer.async {
            var error = [CChar](repeating: 0, count: 256)
            var failed: Bool
            if page == .diagnostic {
                var info = P1600DiagnosticInfo()
                var job: Int32 = 0
                p1600_diagnostic_defaults(&info, localization)
                failed = p1600_diagnostic_info_for_queue(queue.name, &info, &error, error.count) != 0 ||
                    p1600_diagnostic_print(&info, &job, &error, error.count) != 0
            } else {
                var buffer = [CChar](repeating: 0, count: 256)
                let length: Int
                if page == .cleaning {
                    length = p1600_utility_format_cleaning(&buffer, buffer.count)
                } else {
                    let kind: P1600InfoPage = page == .demo ? P1600_INFO_DEMO : page == .configuration ? P1600_INFO_CONFIGURATION : P1600_INFO_SUPPLY_STATUS
                    length = p1600_utility_format_info_page(kind, p1600_utility_pjl_language(localization), &buffer, buffer.count)
                }
                failed = length == 0 || buffer.withUnsafeBufferPointer { bytes in
                    p1600_utility_send(queue.deviceURI, bytes.baseAddress, length, &error, error.count) != 0
                }
            }
            DispatchQueue.main.async {
                self.finishTask()
                self.show(failed ? failureText(error) : ownText("PageSent"), error: failed)
            }
        }
    }

    /// A result line; success fades after a few seconds, errors stay.
    private func show(_ text: String, error: Bool) {
        message = text
        messageIsError = error
        guard !error else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 4) {
            if self.message == text && !self.messageIsError { self.message = "" }
        }
    }

    /// HTML Config: the printer's web pages; for a USB printer through a local
    /// proxy over its LEDM interface. On the printer queue: a Bonjour printer is
    /// resolved (up to 5 s) and an old proxy is stopped (joining its thread)
    /// without holding up the window.
    func openWebPages() {
        guard let queue = selectedQueue, !busy else { return }
        task = .openingWebPages
        message = ""
        let oldProxy = webProxy
        webProxy = nil
        printer.async {
            // An old proxy is stopped and freed (one that went idle has
            // already let go of the printer's USB interface).
            if let proxy = oldProxy { p1600_ews_proxy_stop(proxy) }
            var url = [CChar](repeating: 0, count: 512)
            var error = [CChar](repeating: 0, count: 256)
            let network = p1600_utility_web_address(queue.deviceURI, &url, url.count, &error, error.count)
            var proxy: OpaquePointer?
            var failed = network < 0
            if network == 1 {
                proxy = p1600_ews_proxy_start_usb(queue.deviceURI, Int32(P1600_EWS_IDLE_SECONDS), &url, url.count, &error, error.count)
                failed = proxy == nil
            }
            let address = failed ? nil : URL(string: cString(url))
            DispatchQueue.main.async {
                self.webProxy = proxy
                self.finishTask()
                if failed {
                    self.show(failureText(error), error: true)
                } else if let address = address {
                    NSWorkspace.shared.open(address)
                }
            }
        }
    }

}

/// Labels as row titles: without the trailing colon or ellipsis a label next
/// to its control carries ("Auto Off after:", "HTML Config...").
private func rowTitle(_ text: String) -> String {
    var title = text.trimmingCharacters(in: .whitespaces)
    while let last = title.last, ":：.…".contains(last) { title.removeLast() }
    return title.trimmingCharacters(in: .whitespaces)
}

private extension InformationPage {
    var symbol: String {
        switch self {
        case .cleaning: return "sparkles"
        case .demo: return "doc.richtext"
        case .configuration: return "list.bullet.rectangle"
        case .supplyStatus: return "drop"
        case .diagnostic: return "stethoscope"
        }
    }
}

private func openPrintersAndScanners() {
    let address: String
    if #available(macOS 13, *) {
        address = "x-apple.systempreferences:com.apple.Print-Scan-Settings.extension"
    } else {
        address = "x-apple.systempreferences:com.apple.preference.printfax"
    }
    if let url = URL(string: address) { NSWorkspace.shared.open(url) }
}

struct UtilityView: View {
    @ObservedObject var model: UtilityModel
    /// The macOS 11-12 layout; the render check also draws it on newer macOS.
    static var legacyLayout = false
    static let width: CGFloat = 540

    private var grouped: Bool {
        if #available(macOS 13, *) { return !Self.legacyLayout }
        return false
    }

    var body: some View {
        content
            .frame(width: Self.width)
            .alert(isPresented: $model.confirmCleaning) {
                Alert(title: Text(ownText("CleaningPage")),
                      message: Text(ownText("CleaningPageNote")),
                      primaryButton: .default(Text(ownText("OK"))) { model.printPage(.cleaning, confirmed: true) },
                      secondaryButton: .cancel(Text(ownText("Cancel"))))
            }
    }

    @ViewBuilder private var content: some View {
        if model.queues.isEmpty {
            NoPrinterView(message: model.message)
        } else if #available(macOS 13, *), grouped {
            Form {
                Section { printerHeader }
                Section(header: Text(ownText("InformationPages"))) { pageRows }
                Section(header: Text(ownText("EnergySaving"))) { ecoRows }
                Section { quietRow }
                Section(footer: statusLine) { webRow }
            }
            .formStyle(.grouped)
            .scrollDisabled(true)
            .fixedSize(horizontal: false, vertical: true)
        } else {
            VStack(alignment: .leading, spacing: 16) {
                PlainSection(title: nil) { printerHeader }
                PlainSection(title: ownText("InformationPages")) { pageRows }
                PlainSection(title: ownText("EnergySaving")) { ecoRows }
                PlainSection(title: nil) { quietRow }
                PlainSection(title: nil) { webRow }
                statusLine.padding(.horizontal, 4)
            }
            .padding(20)
        }
    }

    // MARK: Rows

    @ViewBuilder private var printerHeader: some View {
        if let queue = model.selectedQueue {
            HStack(spacing: 14) {
                Image(nsImage: NSApp.applicationIconImage)
                    .resizable()
                    .frame(width: 56, height: 56)
                VStack(alignment: .leading, spacing: 3) {
                    Text(queue.title).font(.headline)
                    Text([queue.connection, queue.stateText].compactMap { $0 }.joined(separator: " · "))
                        .font(.subheadline)
                        .foregroundColor(.secondary)
                    TonerGauge(level: Int(queue.tonerLevel))
                }
                Spacer(minLength: 0)
            }
            .padding(.vertical, 4)
            if model.queues.count > 1 {
                divider
                row(Text(ownText("Printer"))) {
                    Picker(ownText("Printer"), selection: $model.queueName) {
                        ForEach(model.queues) { queue in Text(queue.title).tag(queue.name) }
                    }
                    .labelsHidden()
                    .frame(maxWidth: 260)
                    .disabled(model.busy)
                }
            }
        }
    }

    @ViewBuilder private var pageRows: some View {
        ForEach(InformationPage.allCases, id: \.self) { page in
            if page != InformationPage.allCases.first { divider }
            row(HStack(spacing: 8) {
                Image(systemName: page.symbol).frame(width: 22).foregroundColor(.secondary)
                Text(page.title)
            }) {
                if model.task == .printing(page) { Spinner() }
                Button(ownText("Print")) { model.printPage(page) }
                    .disabled(model.busy)
            }
        }
    }

    /// Both menus stay off until the printer answered (a modifier after the
    /// rows would reach only the last one).
    private var ecoRows: some View {
        Group {
            row(Text(rowTitle(ownText("SleepAfter")))) {
                Picker(rowTitle(ownText("SleepAfter")), selection: Binding(get: { model.sleepIndex }, set: model.setSleep)) {
                    if model.sleepIndex < 0 { Text("—").tag(-1) }
                    ForEach(0..<Int(P1600_SLEEP_CHOICES), id: \.self) { index in
                        Text(minutesText(p1600_sleep_choice(Int32(index)), after: true)).tag(index)
                    }
                }
                .labelsHidden()
                .fixedSize()
            }
            divider
            row(Text(rowTitle(ownText("TurnOffAfter")))) {
                Picker(rowTitle(ownText("TurnOffAfter")), selection: Binding(get: { model.autoOffIndex }, set: model.setAutoOff)) {
                    if model.autoOffIndex < 0 { Text("—").tag(-1) }
                    ForEach(0..<Int(P1600_AUTO_OFF_CHOICES), id: \.self) { index in
                        Text(minutesText(p1600_auto_off_choice(Int32(index)), after: false)).tag(index)
                    }
                }
                .labelsHidden()
                .fixedSize()
            }
        }
        .disabled(!model.settingsRead || model.busy)
    }

    private var quietRow: some View {
        row(Text(ownText("QuietMode"))) {
            Toggle(ownText("QuietMode"), isOn: Binding(get: { model.quietMode == 1 }, set: model.setQuietMode))
                .labelsHidden()
                .toggleStyle(SwitchToggleStyle())
        }
        .disabled(!model.settingsRead || model.busy || model.quietMode < 0)
    }

    private var webRow: some View {
        row(Text(rowTitle(ownText("PrinterWebPage")))) {
            if model.task == .openingWebPages { Spinner() }
            Button(ownText("Open")) { model.openWebPages() }
                .disabled(model.busy)
        }
    }

    /// Reading, the last result, or nothing — one line, so the window keeps its height.
    private var statusLine: some View {
        HStack(spacing: 6) {
            if model.task == .reading || model.task == .saving {
                Spinner()
            } else if !model.message.isEmpty {
                Image(systemName: model.messageIsError ? "exclamationmark.triangle.fill" : "checkmark.circle.fill")
                    .foregroundColor(model.messageIsError ? .orange : .green)
            }
            Text(model.task == .reading ? ownText("Reading") : model.message.isEmpty ? " " : model.message)
                .font(.callout)
                .foregroundColor(.secondary)
                .lineLimit(2)
                .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 8)
            if model.readFailed && !model.busy {
                Button(ownText("TryAgain")) { model.readSettings() }
            }
        }
        .frame(minHeight: 22)
    }

    // MARK: Building blocks

    /// Title on the left, control on the right: System Settings rows from
    /// macOS 13, the same shape by hand before.
    @ViewBuilder private func row<Title: View, Control: View>(_ title: Title, @ViewBuilder control: () -> Control) -> some View {
        if #available(macOS 13, *), grouped {
            LabeledContent { HStack(spacing: 8) { control() } } label: { title }
        } else {
            HStack(spacing: 8) {
                title
                Spacer(minLength: 12)
                control()
            }
            .padding(.vertical, 7)
        }
    }

    /// Row separators are drawn by the grouped form itself.
    @ViewBuilder private var divider: some View {
        if !grouped { Divider() }
    }

    private func minutesText(_ minutes: Int32, after: Bool) -> String {
        switch minutes {
        case 0: return ownText("Never")
        case let value where value % 60 == 0: return ownText((after ? "AfterHours" : "Hours") + "\(value / 60)")
        default: return ownText((after ? "AfterMinutes" : "Minutes") + "\(minutes)")
        }
    }
}

/// A grouped box for macOS 11-12, shaped like the grouped form of later releases.
struct PlainSection<Content: View>: View {
    let title: String?
    @ViewBuilder let content: () -> Content

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            if let title = title {
                Text(title).font(.subheadline.weight(.semibold)).foregroundColor(.secondary).padding(.leading, 12)
            }
            VStack(alignment: .leading, spacing: 0) { content() }
                .padding(.horizontal, 12)
                .padding(.vertical, 3)
                .background(RoundedRectangle(cornerRadius: 10).fill(Color(NSColor.controlBackgroundColor)))
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color(NSColor.separatorColor), lineWidth: 0.5))
        }
    }
}

/// The toner level CUPS last received from the driver's ReportLevels.
struct TonerGauge: View {
    let level: Int

    var body: some View {
        HStack(spacing: 8) {
            Text(ownText("Toner")).font(.subheadline).foregroundColor(.secondary)
            if level >= 0 {
                ProgressView(value: Double(level), total: 100)
                    .frame(width: 120)
                    .accentColor(level <= 15 ? .orange : .accentColor)
                Text(NumberFormatter.localizedString(from: NSNumber(value: Double(level) / 100), number: .percent))
                    .font(.subheadline.monospacedDigit())
                    .foregroundColor(.secondary)
            } else {
                Text("—").font(.subheadline).foregroundColor(.secondary)
            }
        }
    }
}

struct Spinner: View {
    var body: some View {
        ProgressView().scaleEffect(0.5).frame(width: 16, height: 16)
    }
}

struct NoPrinterView: View {
    let message: String

    var body: some View {
        VStack(spacing: 12) {
            Image(nsImage: NSApp.applicationIconImage)
                .resizable()
                .frame(width: 96, height: 96)
            Text(ownText("NoPrinter"))
                .font(.headline)
                .multilineTextAlignment(.center)
            Text(ownText("NoPrinterHint"))
                .foregroundColor(.secondary)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
            if !message.isEmpty {
                Text(message).font(.callout).foregroundColor(.secondary).multilineTextAlignment(.center)
            }
            Button(ownText("OpenPrintersScanners"), action: openPrintersAndScanners)
                .padding(.top, 4)
        }
        .padding(.horizontal, 40)
        .padding(.vertical, 32)
        .frame(maxWidth: .infinity)
    }
}

#if !UTILITY_RENDER
/// The window comes from AppDelegate (makeUtilityWindow); the scene only
/// gives the app its standard menus, without a Settings item.
@main
struct P1600dnUtilityApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate

    var body: some Scene {
        Settings { EmptyView() }
            .commands {
                CommandGroup(replacing: .appSettings) {}
                CommandGroup(replacing: .newItem) {}
            }
    }
}
#endif
