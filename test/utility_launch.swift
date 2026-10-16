// Launches the utility app the way macOS's Printers & Scanners does: an Open
// Application event whose 'prdt' parameter names the CUPS queue. The app
// writes what it received to the report file and quits.
// Usage: utility_launch <app> <report file> <queue name>
import AppKit

let arguments = CommandLine.arguments
guard arguments.count == 4 else { print("Usage: utility_launch <app> <report> <queue>"); exit(2) }
let event = NSAppleEventDescriptor(eventClass: AEEventClass(kCoreEventClass), eventID: AEEventID(kAEOpenApplication),
                                   targetDescriptor: nil, returnID: AEReturnID(kAutoGenerateReturnID),
                                   transactionID: AETransactionID(kAnyTransactionID))
event.setParam(NSAppleEventDescriptor(string: arguments[3]), forKeyword: AEKeyword(0x7072_6474))  // 'prdt'
let configuration = NSWorkspace.OpenConfiguration()
configuration.appleEvent = event
configuration.environment = ["P1600DN_UTILITY_LAUNCH_REPORT": arguments[2]]
configuration.createsNewApplicationInstance = true
configuration.activates = false
configuration.addsToRecentItems = false
var launchError: Error?
let done = DispatchSemaphore(value: 0)
NSWorkspace.shared.openApplication(at: URL(fileURLWithPath: arguments[1]), configuration: configuration) { _, error in
    launchError = error
    done.signal()
}
done.wait()
if let error = launchError { print("launch failed: \(error)"); exit(1) }
for _ in 0..<200 {
    if let report = try? String(contentsOfFile: arguments[2], encoding: .utf8) { print(report); exit(0) }
    Thread.sleep(forTimeInterval: 0.1)
}
print("no report"); exit(1)
