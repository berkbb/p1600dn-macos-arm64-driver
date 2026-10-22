// Launches the utility app the way macOS's Printers & Scanners does: an Open
// Application event whose 'prdt' parameter names the CUPS queue. The app
// writes what it received to the report file and quits. With a second queue,
// the running app gets a second Open Application event (Printers & Scanners
// or a notification for another printer) and reports that too.
// Usage: utility_launch <app> <report file> <queue name> [<next queue name>]
import AppKit

let arguments = CommandLine.arguments
guard arguments.count == 4 || arguments.count == 5 else {
    print("Usage: utility_launch <app> <report> <queue> [<next queue>]")
    exit(2)
}
let appURL = URL(fileURLWithPath: arguments[1])
let reportPath = arguments[2]
let queues = Array(arguments[3...])

func openEvent(_ queue: String) -> NSAppleEventDescriptor {
    let event = NSAppleEventDescriptor(eventClass: AEEventClass(kCoreEventClass), eventID: AEEventID(kAEOpenApplication),
                                       targetDescriptor: nil, returnID: AEReturnID(kAutoGenerateReturnID),
                                       transactionID: AETransactionID(kAnyTransactionID))
    event.setParam(NSAppleEventDescriptor(string: queue), forKeyword: AEKeyword(0x7072_6474))  // 'prdt'
    return event
}

func open(_ queue: String, newInstance: Bool) -> NSRunningApplication? {
    let configuration = NSWorkspace.OpenConfiguration()
    configuration.appleEvent = openEvent(queue)
    configuration.environment = ["P1600DN_UTILITY_LAUNCH_REPORT": reportPath,
                                 "P1600DN_UTILITY_LAUNCH_EVENTS": String(queues.count)]
    configuration.createsNewApplicationInstance = newInstance
    configuration.activates = false
    configuration.addsToRecentItems = false
    var launched: NSRunningApplication?
    var launchError: Error?
    let done = DispatchSemaphore(value: 0)
    NSWorkspace.shared.openApplication(at: appURL, configuration: configuration) { application, error in
        launched = application
        launchError = error
        done.signal()
    }
    done.wait()
    if let error = launchError { print("launch failed: \(error)"); exit(1) }
    return launched
}

/// The report once it has `lines` lines.
func waitForReport(lines: Int) -> String? {
    for _ in 0..<200 {
        if let report = try? String(contentsOfFile: reportPath, encoding: .utf8),
           report.split(separator: "\n", omittingEmptySubsequences: false).count >= lines {
            return report
        }
        Thread.sleep(forTimeInterval: 0.1)
    }
    return nil
}

let first = open(queues[0], newInstance: true)
guard var report = waitForReport(lines: 1) else { print("no report"); exit(1) }
if queues.count == 2 {
    let again = open(queues[1], newInstance: false)
    if let first = first, let again = again, first.processIdentifier != again.processIdentifier {
        print("a second instance was started")
        exit(1)
    }
    guard let both = waitForReport(lines: 2) else { print("no report for the second event: \(report)"); exit(1) }
    report = both
}
print(report)
