// Model probe for test/test_utility_app.sh: the printer utility's header
// follows CUPS. Built with the app's source and -D UTILITY_RENDER, with
// CUPS_SERVER at the stand-in cupsd of test/test_notify.c (--serve <port
// file> utility), which starts printing with the toner at 12% after the
// utility's first lookup. Prints the selected queue's "state=<n> toner=<n>"
// once they have changed, or what it saw last after 20 s.
// Usage: utility_probe <queue>
import AppKit

@main
struct UtilityProbe {
    static func main() {
        let arguments = CommandLine.arguments
        guard arguments.count == 2 else {
            print("Usage: utility_probe <queue>")
            exit(2)
        }
        let model = UtilityModel.shared
        model.start(queue: arguments[1])
        let deadline = Date().addingTimeInterval(20)
        var queue = model.selectedQueue
        while Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.2))
            queue = model.selectedQueue
            if let queue = queue, queue.state == 4, queue.tonerLevel == 12 { break }
        }
        print("state=\(queue?.state ?? -1) toner=\(queue?.tonerLevel ?? -1)")
        exit(0)
    }
}
