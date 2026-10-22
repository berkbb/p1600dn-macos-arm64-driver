// P1600dn Notifier: a background agent (LSUIElement, no windows) that turns
// what the P1600dn driver reports to cupsd into macOS notifications: out of
// paper, paper jams, open doors, "load paper / turn the stack and press Go"
// for manual feed and manual duplex (the PPD's localized text, which the
// driver writes to printer-state-message), engine errors, and toner low,
// very low, empty and replaced. The decisions are made by the C core
// (src/p1600_notify.c); this file watches the queues and shows the notices.
//
// P1600dn Bildirim Aracı: sürücünün cupsd'ye bildirdiği yazıcı durumunu
// (kağıt bitti, sıkışma, kapak açık, elle besleme/çift taraflı, toner)
// macOS bildirimlerine dönüştürür.

import AppKit
import Foundation
import UserNotifications
import os

private let log = Logger(subsystem: "com.berkbb.p1600dn.notifier", category: "agent")

/// A C char array (imported as a tuple) as a Swift string.
func cText<T>(_ value: T) -> String {
    var copy = value
    return withUnsafeBytes(of: &copy) { raw in
        let end = raw.firstIndex(of: 0) ?? raw.endIndex
        return String(decoding: raw[..<end], as: UTF8.self)
    }
}

private func monotonic() -> TimeInterval { ProcessInfo.processInfo.systemUptime }

/// Test hooks from the environment. With P1600DN_NOTIFIER_LOG set, notices
/// are only written to that file and UNUserNotificationCenter is never used.
struct Hooks {
    let server: String?       // P1600DN_CUPS_SERVER=host:port
    let queues: [String]?     // P1600DN_NOTIFIER_QUEUES=a,b
    let logPath: String?      // P1600DN_NOTIFIER_LOG=/path
    let exitAfter: Int        // P1600DN_NOTIFIER_EXIT_AFTER=N
    let language: String?     // P1600DN_NOTIFIER_LANG=tr
    let statePath: String?    // P1600DN_NOTIFIER_STATE=/path.plist (memory instead of UserDefaults)

    init(_ environment: [String: String]) {
        func value(_ name: String) -> String? {
            guard let text = environment[name], !text.isEmpty else { return nil }
            return text
        }
        server = value("P1600DN_CUPS_SERVER")
        queues = value("P1600DN_NOTIFIER_QUEUES").map {
            $0.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        }
        logPath = value("P1600DN_NOTIFIER_LOG")
        exitAfter = value("P1600DN_NOTIFIER_EXIT_AFTER").flatMap { Int($0) } ?? 0
        language = value("P1600DN_NOTIFIER_LANG")
        statePath = value("P1600DN_NOTIFIER_STATE")
    }
}

/// Notifier.strings in the user's language (or the forced one), English for
/// anything missing.
final class Texts {
    private let bundles: [Bundle]

    init(language: String?) {
        var list: [Bundle] = []
        if let language = language, let path = Bundle.main.path(forResource: language, ofType: "lproj"),
           let bundle = Bundle(path: path) {
            list.append(bundle)
        } else {
            list.append(Bundle.main)
        }
        if let path = Bundle.main.path(forResource: "en", ofType: "lproj"), let bundle = Bundle(path: path) {
            list.append(bundle)
        }
        bundles = list
    }

    func text(_ key: String) -> String {
        for bundle in bundles {
            let value = bundle.localizedString(forKey: key, value: "\u{1}", table: "Notifier")
            if value != "\u{1}" { return value }
        }
        return key
    }

    /// The generic text of an attention reason.
    func reason(_ base: String) -> String {
        let key = "Reason-" + base
        let value = text(key)
        return value == key ? text("Reason-other") : value
    }

    /// The driver's message when there is one, else our text.
    func body(for notice: P1600Notice) -> String {
        let level = Int32(max(notice.level, 0))
        switch notice.kind {
        case P1600_NOTICE_ATTENTION:
            let message = cText(notice.message)
            return message.isEmpty ? reason(cText(notice.reason)) : message
        case P1600_NOTICE_TONER_LOW:
            return String(format: text("Toner-low"), level)
        case P1600_NOTICE_TONER_VERY_LOW:
            return String(format: text("Toner-very-low"), level)
        case P1600_NOTICE_TONER_EMPTY:
            return text("Toner-empty")
        default:
            return String(format: text("Toner-replaced"), level)
        }
    }
}

/// What the agent remembers per queue across restarts: the last known toner
/// bucket and the attention reasons already reported. UserDefaults normally;
/// a plist file for tests (P1600DN_NOTIFIER_STATE); nothing at all when
/// only logging, so tests never touch the user's defaults.
final class Memory {
    private static let key = "Queues"
    private var queues: [String: [String: Any]]
    private let path: String?
    private let persistent: Bool

    init(hooks: Hooks) {
        if let path = hooks.statePath {
            self.path = path
            persistent = true
            let stored = NSDictionary(contentsOfFile: path) as? [String: Any]
            queues = stored?[Memory.key] as? [String: [String: Any]] ?? [:]
        } else if hooks.logPath != nil {
            path = nil
            persistent = false
            queues = [:]
        } else {
            path = nil
            persistent = true
            queues = UserDefaults.standard.dictionary(forKey: Memory.key) as? [String: [String: Any]]
                // Driver 5.0 kept it under its old bundle identifier.
                ?? UserDefaults(suiteName: "com.berkbb.hpp1606dn.notifier")?.dictionary(forKey: Memory.key)
                    as? [String: [String: Any]]
                ?? [:]
        }
    }

    func bucket(_ queue: String) -> P1600TonerBucket {
        P1600TonerBucket(rawValue: Int32(queues[queue]?["TonerBucket"] as? Int ?? Int(P1600_TONER_UNKNOWN.rawValue)))
    }

    func reasons(_ queue: String) -> String { queues[queue]?["Reasons"] as? String ?? "" }

    func update(_ queue: String, bucket: P1600TonerBucket, reasons: String) {
        let stored = queues[queue]
        if stored?["TonerBucket"] as? Int == Int(bucket.rawValue), stored?["Reasons"] as? String == reasons { return }
        queues[queue] = ["TonerBucket": Int(bucket.rawValue), "Reasons": reasons]
        guard persistent else { return }
        if let path = path {
            if !(([Memory.key: queues] as NSDictionary).write(toFile: path, atomically: true)) {
                log.error("Could not write \(path, privacy: .public)")
            }
        } else {
            UserDefaults.standard.set(queues, forKey: Memory.key)
        }
    }
}

/// One background thread per queue: an ippget subscription and long polls of
/// Get-Notifications, reconnecting with backoff when cupsd goes away, and
/// Get-Printer-Attributes every 60 s when subscriptions are refused.
final class QueueWatcher {
    static let lease: Int32 = 3600
    static let levelsDelay: TimeInterval = 20           // after the last job-completed
    static let levelsInterval: TimeInterval = 30 * 60   // ReportLevels at most this often
    static let pollInterval: TimeInterval = 60
    static let subscriptionRetry: TimeInterval = 10 * 60
    static let busyWait: TimeInterval = 5               // while the printer is processing
    static let idleWait: TimeInterval = 60
    static let levelsTimeout = TimeInterval(P1600_NOTIFY_LEVELS_TIMEOUT_SECONDS)

    let queue: String
    private let deliver: (QueueWatcher, P1600PrinterSnapshot) -> Void
    private let condition = NSCondition()
    // Guarded by condition.
    private var cancelled = false
    private var snapshotDue: TimeInterval?
    private var subscription: Int32 = 0

    init(queue: String, deliver: @escaping (QueueWatcher, P1600PrinterSnapshot) -> Void) {
        self.queue = queue
        self.deliver = deliver
    }

    func start() {
        let thread = Thread { [self] in run() }
        thread.name = "P1600dn Notifier \(queue)"
        thread.qualityOfService = .utility
        thread.start()
    }

    func cancel() {
        condition.lock()
        cancelled = true
        condition.broadcast()
        condition.unlock()
    }

    /// Takes a fresh snapshot `seconds` from now (a tentative notice is
    /// confirmed or dropped then).
    func snapshot(after seconds: TimeInterval) {
        condition.lock()
        let due = monotonic() + seconds
        if snapshotDue.map({ due < $0 }) ?? true { snapshotDue = due }
        condition.broadcast()
        condition.unlock()
    }

    var subscriptionID: Int32 {
        condition.lock()
        defer { condition.unlock() }
        return subscription
    }

    private var isCancelled: Bool {
        condition.lock()
        defer { condition.unlock() }
        return cancelled
    }

    private func setSubscription(_ id: Int32) {
        condition.lock()
        subscription = id
        condition.unlock()
    }

    /// Sleeps up to `seconds`; a cancel or a snapshot request wakes it.
    private func pause(_ seconds: TimeInterval) {
        condition.lock()
        if !cancelled && seconds > 0 { _ = condition.wait(until: Date(timeIntervalSinceNow: seconds)) }
        condition.unlock()
    }

    private func takeSnapshotDue(_ now: TimeInterval) -> Bool {
        condition.lock()
        defer { condition.unlock() }
        guard let due = snapshotDue, due <= now else { return false }
        snapshotDue = nil
        return true
    }

    private func timeUntilSnapshot(_ now: TimeInterval) -> TimeInterval? {
        condition.lock()
        defer { condition.unlock() }
        return snapshotDue.map { max($0 - now, 0) }
    }

    private func run() {
        var http: OpaquePointer?
        var backoff: TimeInterval = 1
        var sequence: Int32 = 1
        var lease: Int32 = 0
        var subscribedAt: TimeInterval = 0
        var refusedAt: TimeInterval?
        var needSnapshot = true
        var processing = false
        var levelsDue: TimeInterval?
        var levelsAsked: TimeInterval?
        var levelsJob: (id: Int32, cancelAt: TimeInterval)?   // the agent's own ReportLevels job
        var ownLevelsJob: Int32 = 0                            // its id, to ignore its job-completed

        func disconnect() {
            if let connection = http { httpClose(connection) }
            http = nil
        }
        func backOff() {
            disconnect()
            pause(backoff)
            backoff = min(backoff * 2, 60)
        }

        while !isCancelled {
            if http == nil {
                http = p1600_notify_connect(5000)
                if http == nil {
                    backOff()
                    continue
                }
                needSnapshot = true
            }
            guard let connection = http else { continue }
            var now = monotonic()

            // Subscribe before the snapshot, so no change falls between them.
            if subscriptionID == 0, refusedAt.map({ now - $0 >= Self.subscriptionRetry }) ?? true {
                var id: Int32 = 0
                let granted = p1600_notify_subscribe(connection, queue, Self.lease, &id)
                if granted >= 0 {
                    setSubscription(id)
                    sequence = 1
                    lease = granted
                    subscribedAt = now
                    refusedAt = nil
                    needSnapshot = true
                } else if granted == P1600_NOTIFY_UNREACHABLE.rawValue {
                    backOff()
                    continue
                } else {
                    log.info("No subscription for \(self.queue, privacy: .public) (\(granted)); polling")
                    refusedAt = now
                }
            }
            let id = subscriptionID
            if id != 0, lease > 0, now - subscribedAt >= TimeInterval(lease) / 2 {
                let granted = p1600_notify_renew(connection, id, Self.lease)
                if granted >= 0 {
                    lease = granted
                    subscribedAt = now
                } else if granted == P1600_NOTIFY_UNREACHABLE.rawValue {
                    backOff()
                    continue
                } else {
                    setSubscription(0)  // gone: subscribe again
                    continue
                }
            }

            // The toner level after finished jobs (a ReportLevels command job),
            // at most every levelsInterval (p1600_notify_levels_time()).
            if let due = levelsDue, due <= now {
                levelsDue = nil
                levelsAsked = now
                let job = p1600_notify_request_levels(queue)
                log.info("ReportLevels for \(self.queue, privacy: .public): \(job)")
                if job > 0 {
                    levelsJob = (job, now + Self.levelsTimeout)
                    ownLevelsJob = job
                }
                needSnapshot = true
            }
            // Not finished in time: the printer is off. Its backend would
            // wait for it and report connecting-to-device.
            if let pending = levelsJob, pending.cancelAt <= now {
                levelsJob = nil
                let status = p1600_notify_cancel_levels(queue, pending.id)
                log.info("ReportLevels job \(pending.id) for \(self.queue, privacy: .public) ended: \(status)")
                needSnapshot = true
            }
            if takeSnapshotDue(now) { needSnapshot = true }

            if needSnapshot {
                var snapshot = P1600PrinterSnapshot()
                let status = p1600_notify_snapshot(connection, queue, &snapshot)
                if status == P1600_NOTIFY_UNREACHABLE.rawValue {
                    backOff()
                    continue
                }
                needSnapshot = false
                backoff = 1
                if status == P1600_NOTIFY_OK.rawValue {
                    processing = snapshot.state == 4  // IPP_PSTATE_PROCESSING
                    deliver(self, snapshot)
                }
            }

            // Wait for events, or poll.
            now = monotonic()
            let subscribed = subscriptionID
            var wait = subscribed != 0 ? (processing ? Self.busyWait : Self.idleWait) : Self.pollInterval
            if let due = levelsDue { wait = min(wait, max(due - now, 0)) }
            if let pending = levelsJob { wait = min(wait, max(pending.cancelAt - now, 0)) }
            if let left = timeUntilSnapshot(now) { wait = min(wait, left) }
            if subscribed == 0, let refused = refusedAt {
                wait = min(wait, max(refused + Self.subscriptionRetry - now, 0))
            }
            if subscribed != 0 {
                var events = [P1600NotifyEvent](repeating: P1600NotifyEvent(), count: 16)
                let count = p1600_notify_wait(connection, subscribed, &sequence, Int32(wait.rounded(.up)), &events,
                                           Int32(events.count))
                if count > 0 {
                    for var event in events.prefix(Int(count)) where p1600_notify_event_wants_levels(&event, ownLevelsJob) {
                        // Debounced: the last job counts; too soon after the
                        // last query it waits for the interval instead.
                        levelsDue = p1600_notify_levels_time(monotonic() + Self.levelsDelay, levelsAsked ?? -1,
                                                          Self.levelsInterval)
                    }
                    needSnapshot = true
                } else if count == P1600_NOTIFY_NOT_FOUND.rawValue {
                    setSubscription(0)
                    needSnapshot = true
                } else if count == P1600_NOTIFY_UNREACHABLE.rawValue {
                    disconnect()  // the subscription may survive a cupsd restart
                    needSnapshot = true
                } else if count < 0 {
                    setSubscription(0)
                    refusedAt = monotonic()
                    needSnapshot = true
                }
            } else {
                pause(wait)
                needSnapshot = true
            }
        }

        // Removed from the list: end the subscription.
        let id = subscriptionID
        if id != 0, let connection = http {
            _ = p1600_notify_cancel(connection, id)
            setSubscription(0)
        }
        disconnect()
    }
}

final class Agent: NSObject, UNUserNotificationCenterDelegate {
    let hooks = Hooks(ProcessInfo.processInfo.environment)
    private lazy var texts = Texts(language: hooks.language)
    private var watchers: [String: QueueWatcher] = [:]  // main thread
    private var relistTimer: Timer?
    private var relistPending = false
    private var signalSources: [DispatchSourceSignal] = []

    // Decisions run in order on one queue.
    private let stateQueue = DispatchQueue(label: "com.berkbb.p1600dn.notifier.state", qos: .utility)
    private var memory: Memory!
    private var last: [String: P1600PrinterSnapshot] = [:]
    private var tentative: [String: [String: TimeInterval]] = [:]
    private var posted = 0
    private var stopping = false
    private var logHandle: FileHandle?

    func start() {
        if let server = hooks.server { p1600_notify_set_server(server) }
        memory = Memory(hooks: hooks)
        if let path = hooks.logPath {
            if !FileManager.default.fileExists(atPath: path) {
                FileManager.default.createFile(atPath: path, contents: nil)
            }
            logHandle = FileHandle(forWritingAtPath: path)
            logHandle?.seekToEndOfFile()
        } else {
            let center = UNUserNotificationCenter.current()
            center.delegate = self
            center.getNotificationSettings { settings in
                guard settings.authorizationStatus == .notDetermined else { return }
                center.requestAuthorization(options: [.alert, .sound]) { granted, error in
                    log.info("Notification authorization: \(granted) \(error?.localizedDescription ?? "", privacy: .public)")
                }
            }
        }

        for number in [SIGTERM, SIGINT] {
            signal(number, SIG_IGN)
            let source = DispatchSource.makeSignalSource(signal: number, queue: .main)
            source.setEventHandler { [weak self] in self?.shutdown() }
            source.resume()
            signalSources.append(source)
        }
        // cupsd posts this when queues are added, removed or changed.
        CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(),
                                        Unmanaged.passUnretained(self).toOpaque(),
                                        { _, observer, _, _, _ in
                                            guard let observer = observer else { return }
                                            let agent = Unmanaged<Agent>.fromOpaque(observer).takeUnretainedValue()
                                            DispatchQueue.main.async { agent.scheduleRelist() }
                                        },
                                        "com.apple.printerListChange" as CFString, nil, .deliverImmediately)
        relistTimer = Timer.scheduledTimer(withTimeInterval: 10 * 60, repeats: true) { [weak self] _ in self?.relist() }
        relist()
    }

    private func scheduleRelist() {
        guard !relistPending else { return }
        relistPending = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
            self?.relistPending = false
            self?.relist()
        }
    }

    /// The P1600dn queues of this Mac (or the test's list).
    private func relist() {
        if let fixed = hooks.queues {
            apply(fixed)
            return
        }
        DispatchQueue.global(qos: .utility).async {
            var found = [P1600Queue](repeating: P1600Queue(), count: 64)
            let count = Int(p1600_queue_list(&found, Int32(found.count)))
            // cupsd unreachable: keep watching what we have.
            if count < 0 || (count == 0 && cupsLastError().rawValue > IPP_STATUS_OK_EVENTS_COMPLETE.rawValue) { return }
            let names = found.prefix(min(count, found.count)).map { cText($0.name) }
            DispatchQueue.main.async { self.apply(names) }
        }
    }

    private func apply(_ names: [String]) {
        let wanted = Set(names)
        for (name, watcher) in watchers where !wanted.contains(name) {
            watcher.cancel()
            watchers[name] = nil
        }
        for name in names where watchers[name] == nil {
            let watcher = QueueWatcher(queue: name) { [weak self] watcher, snapshot in
                self?.stateQueue.async { self?.observe(watcher, snapshot) }
            }
            watchers[name] = watcher
            watcher.start()
            log.info("Watching \(name, privacy: .public)")
        }
    }

    /// One snapshot of a queue: compare with the last one and show what is new.
    private func observe(_ watcher: QueueWatcher, _ snapshot: P1600PrinterSnapshot) {
        guard !stopping else { return }
        let queue = watcher.queue
        var before = last[queue] ?? P1600PrinterSnapshot()  // no data: the first observation primes
        var after = snapshot
        p1600_notify_carry_toner(&after, &before)
        var notices = [P1600Notice](repeating: P1600Notice(), count: Int(P1600_NOTIFY_MAX_NOTICES))
        let count = Int(p1600_notify_compare(&before, &after, &notices, Int32(notices.count)))
        let remembered = memory.bucket(queue)
        let reported = memory.reasons(queue)
        let info = cText(after.info)
        let title = info.isEmpty ? queue : info
        let settle = TimeInterval(P1600_NOTIFY_SETTLE_SECONDS)

        // The first observation only primes p1600_notify_compare(); a toner
        // level that fell while this agent was not watching still counts.
        var primed = P1600Notice()
        if !before.has_data && p1600_notify_prime_toner(remembered, &after, &primed) != 0 {
            post(primed, queue: queue, title: title)
        }

        for index in 0..<count {
            var notice = notices[index]
            guard p1600_notify_should_post(&notice, remembered) else { continue }
            let reason = cText(notice.reason)
            if notice.kind == P1600_NOTICE_ATTENTION {
                // Reported before this start and still there.
                if !before.has_data && p1600_notify_reason_listed(reported, reason) { continue }
                if notice.tentative {
                    tentative[queue, default: [:]][reason] = monotonic() + settle
                    watcher.snapshot(after: settle)
                    continue
                }
            }
            post(notice, queue: queue, title: title)
        }

        // Tentative notices still active after the settle time are shown.
        if var pending = tentative[queue] {
            for (reason, due) in pending {
                if !p1600_notify_attention_active(&after, reason) {
                    pending[reason] = nil
                } else if due <= monotonic() {
                    let message = cText(after.message)
                    show(name: "attention." + reason, queue: queue, title: title,
                         body: message.isEmpty ? texts.reason(reason) : message,
                         identifier: "\(queue).attention.\(reason)", sound: true)
                    pending[reason] = nil
                }
            }
            tentative[queue] = pending.isEmpty ? nil : pending
        }

        var active = [CChar](repeating: 0, count: 256)
        p1600_notify_active_reasons(&after, &active, active.count)
        memory.update(queue, bucket: p1600_notify_remember(remembered, &after),
                      reasons: String(cString: active))
        last[queue] = after
    }

    private func post(_ notice: P1600Notice, queue: String, title: String) {
        var copy = notice
        var name = [CChar](repeating: 0, count: 96)
        p1600_notice_name(&copy, &name, name.count)
        let attention = notice.kind == P1600_NOTICE_ATTENTION
        // Toner notices replace each other; attention notices per reason.
        let identifier = attention ? "\(queue).attention.\(cText(notice.reason))" : "\(queue).toner"
        show(name: String(cString: name), queue: queue, title: title, body: texts.body(for: notice),
             identifier: identifier, sound: attention)
    }

    private func show(name: String, queue: String, title: String, body: String, identifier: String, sound: Bool) {
        guard !stopping else { return }
        posted += 1
        if let handle = logHandle {
            let fields = [name, queue, title, body].map {
                $0.replacingOccurrences(of: "\t", with: " ").replacingOccurrences(of: "\n", with: " ")
            }
            handle.write(Data((fields.joined(separator: "\t") + "\n").utf8))
        } else {
            let content = UNMutableNotificationContent()
            content.title = title
            content.body = body
            content.threadIdentifier = queue
            content.userInfo = ["queue": queue]
            if sound { content.sound = .default }
            let request = UNNotificationRequest(identifier: identifier, content: content, trigger: nil)
            UNUserNotificationCenter.current().add(request) { error in
                if let error = error { log.error("Notification failed: \(error.localizedDescription, privacy: .public)") }
            }
        }
        log.info("\(name, privacy: .public) for \(queue, privacy: .public)")
        if hooks.exitAfter > 0 && posted >= hooks.exitAfter {
            stopping = true
            DispatchQueue.main.async { self.shutdown() }
        }
    }

    /// Ends the subscriptions (the watcher threads may be in a long poll)
    /// and exits; launchd starts the agent again only after a failure.
    func shutdown() {
        // Let a decision in progress finish and save the memory first.
        stateQueue.sync { stopping = true }
        relistTimer?.invalidate()
        let current = Array(watchers.values)
        watchers.removeAll()
        current.forEach { $0.cancel() }
        let ids = current.map { $0.subscriptionID }.filter { $0 != 0 }
        if !ids.isEmpty, let http = p1600_notify_connect(2000) {
            for id in ids { _ = p1600_notify_cancel(http, id) }
            httpClose(http)
        }
        exit(0)
    }

    // Notices are shown while the agent runs too (it never becomes active).
    func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification,
                                withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void) {
        completionHandler([.banner, .list, .sound])
    }

    /// A click opens the printer utility for that printer, the way Printers &
    /// Scanners does (an Open Application event with the queue in 'prdt').
    func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse,
                                withCompletionHandler completionHandler: @escaping () -> Void) {
        if let queue = response.notification.request.content.userInfo["queue"] as? String {
            Agent.openUtility(queue: queue)
        }
        completionHandler()
    }

    static let utilityPath = "/Library/Printers/p1600dn/P1600dn Series Utility.app"

    static func openUtility(queue: String) {
        guard FileManager.default.fileExists(atPath: utilityPath) else { return }
        let event = NSAppleEventDescriptor(eventClass: AEEventClass(kCoreEventClass), eventID: AEEventID(kAEOpenApplication),
                                           targetDescriptor: nil, returnID: AEReturnID(kAutoGenerateReturnID),
                                           transactionID: AETransactionID(kAnyTransactionID))
        event.setParam(NSAppleEventDescriptor(string: queue), forKeyword: AEKeyword(0x7072_6474))  // 'prdt'
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.appleEvent = event
        configuration.activates = true
        NSWorkspace.shared.openApplication(at: URL(fileURLWithPath: utilityPath), configuration: configuration) { _, error in
            if let error = error { log.error("Opening the utility failed: \(error.localizedDescription, privacy: .public)") }
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    let agent = Agent()

    func applicationDidFinishLaunching(_ notification: Notification) {
        agent.start()
    }
}

@main
enum NotifierMain {
    @MainActor
    static func main() {
        signal(SIGPIPE, SIG_IGN)
        let app = NSApplication.shared
        let delegate = AppDelegate()
        app.delegate = delegate
        app.setActivationPolicy(.accessory)
        withExtendedLifetime(delegate) { app.run() }
    }
}
