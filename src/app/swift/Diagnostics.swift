import Foundation
import AppKit

/// App diagnostics: a persistent on-disk log for after-the-fact debugging, plus optional error
/// reporting to a server. Logs always save locally. Server reporting only fires when an endpoint
/// is configured (env `NEURACOUST_ERROR_REPORT_URL` or `~/.neuracoust/error_report_url`), so nothing
/// ever leaves the machine unless the user points it at their own reporter.
final class Diagnostics: @unchecked Sendable {
    static let shared = Diagnostics()

    private let queue = DispatchQueue(label: "com.neuracoust.daw.diagnostics")
    private var logURL: URL?
    private var reportURL: URL?
    private var started = false
    private var savedStderr: Int32 = -1   // original stderr, so tee'd output still reaches a terminal

    /// The Logs directory (~/Library/Logs/Neuracoust DAW).
    private var logDirectory: URL? {
        let fm = FileManager.default
        guard let base = fm.urls(for: .libraryDirectory, in: .userDomainMask).first else { return nil }
        let dir = base.appendingPathComponent("Logs/Neuracoust DAW", isDirectory: true)
        try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    /// Open the session log, tee stderr into it (so C++/engine `fprintf` and system warnings are
    /// captured too when launched via Finder), prune old logs, and install a crash handler.
    func start() {
        queue.sync {
            guard !started else { return }
            started = true
            guard let dir = logDirectory else { return }

            let stamp = Self.fileStamp()
            let url = dir.appendingPathComponent("session-\(stamp).log")
            logURL = url
            FileManager.default.createFile(atPath: url.path, contents: nil)

            // Resolve the reporting endpoint (opt-in). Env wins over the config file.
            if let env = ProcessInfo.processInfo.environment["NEURACOUST_ERROR_REPORT_URL"],
               let u = URL(string: env.trimmingCharacters(in: .whitespacesAndNewlines)) {
                reportURL = u
            } else {
                let cfg = FileManager.default.homeDirectoryForCurrentUser
                    .appendingPathComponent(".neuracoust/error_report_url")
                if let s = try? String(contentsOf: cfg, encoding: .utf8),
                   let u = URL(string: s.trimmingCharacters(in: .whitespacesAndNewlines)) {
                    reportURL = u
                }
            }

            // Tee stderr (C++/engine `fprintf`, system warnings) into the log through the SAME
            // single file writer, while still echoing to any real terminal. A pipe + reader thread
            // avoids the two-writers-one-file corruption a bare dup2 would cause.
            captureStderr()

            pruneOldLogs(in: dir, keeping: 20)
            NSSetUncaughtExceptionHandler { ex in
                Diagnostics.shared.reportError("Uncaught exception: \(ex.name.rawValue) — \(ex.reason ?? "")",
                                               context: ex.callStackSymbols.prefix(20).joined(separator: "\n"))
            }
            appendRaw("==== Neuracoust DAW session \(Self.readableStamp()) — report=\(reportURL?.absoluteString ?? "off") ====")
        }
    }

    /// Timestamped informational line to the session log (no server report).
    func log(_ message: String) {
        appendRaw("[\(Self.readableStamp())] \(message)")
    }

    /// Log an error AND, if a reporter endpoint is configured, POST it. Never blocks the caller.
    func reportError(_ message: String, context: String = "") {
        let entry = "[\(Self.readableStamp())] ERROR: \(message)" + (context.isEmpty ? "" : "\n\(context)")
        appendRaw(entry)
        guard let url = reportURL else { return }
        let payload: [String: Any] = [
            "app": "Neuracoust DAW",
            "version": Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "dev",
            "build": Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "0",
            "os": ProcessInfo.processInfo.operatingSystemVersionString,
            "timestamp": Self.readableStamp(),
            "message": message,
            "context": context,
            "logTail": logTail(maxBytes: 16_384),
        ]
        guard let body = try? JSONSerialization.data(withJSONObject: payload) else { return }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = body
        req.timeoutInterval = 10
        URLSession.shared.dataTask(with: req).resume()   // fire-and-forget
    }

    /// Redirect the process's stderr into a pipe; a reader forwards each chunk to the log file and,
    /// only when the original stderr is a real terminal, echoes it there too. One file writer, no races.
    ///
    /// The reader MUST never block: it runs on the same pipe that in-process `fprintf(stderr, …)`
    /// (including CoreAudio's own internal logging) writes to. If the reader ever stalls, the 64 KB
    /// pipe fills and every stderr write in the process blocks — which once deadlocked the main
    /// thread inside `AudioComponentInstanceNew` during device open. So the terminal echo is gated
    /// to a genuine tty (absent under `open`/launchd) and the echo fd is non-blocking; the only other
    /// work is a non-blocking `queue.async` to the log file.
    private func captureStderr() {
        // Escape hatch: redirecting a process-wide fd is invasive, so allow disabling it. The log
        // file, explicit Diagnostics.log() calls, and the crash handler all keep working without it.
        if ProcessInfo.processInfo.environment["NC_NO_STDERR_CAPTURE"] != nil { return }
        savedStderr = dup(STDERR_FILENO)
        var fds: [Int32] = [0, 0]
        guard pipe(&fds) == 0 else { return }
        let readFd = fds[0], writeFd = fds[1]
        dup2(writeFd, STDERR_FILENO)
        close(writeFd)
        // Once stderr points at a pipe instead of a tty, C stdio may switch it to full buffering, so
        // in-process C/C++ `fprintf(stderr, …)` would never reach the log. Force it unbuffered.
        setvbuf(stderr, nil, _IONBF, 0)
        // Only echo to the original stderr if it is a terminal; never when launched via Finder/launchd
        // (where that fd is undrained and a blocking write would stall the reader — see above).
        let echoFd = (savedStderr >= 0 && isatty(savedStderr) != 0) ? savedStderr : -1
        if echoFd >= 0 {
            let flags = fcntl(echoFd, F_GETFL, 0)
            if flags >= 0 { _ = fcntl(echoFd, F_SETFL, flags | O_NONBLOCK) }   // never block the reader
        }
        Thread.detachNewThread {
            var buf = [UInt8](repeating: 0, count: 4096)
            while true {
                let n = read(readFd, &buf, buf.count)
                if n <= 0 { break }
                let chunk = Data(buf[0..<n])
                if echoFd >= 0 { chunk.withUnsafeBytes { _ = write(echoFd, $0.baseAddress, n) } }   // best-effort, non-blocking
                Diagnostics.shared.writeToLog(chunk)
            }
        }
    }

    private func writeToLog(_ data: Data) {
        queue.async { [weak self] in
            guard let self, let url = self.logURL else { return }
            if let fh = try? FileHandle(forWritingTo: url) {
                defer { try? fh.close() }
                fh.seekToEndOfFile()
                fh.write(data)
            }
        }
    }
    private func appendRaw(_ line: String) {
        if let data = (line + "\n").data(using: .utf8) { writeToLog(data) }
    }

    private func logTail(maxBytes: Int) -> String {
        guard let url = logURL, let data = try? Data(contentsOf: url) else { return "" }
        let slice = data.count > maxBytes ? data.suffix(maxBytes) : data
        return String(data: slice, encoding: .utf8) ?? ""
    }

    private func pruneOldLogs(in dir: URL, keeping: Int) {
        let fm = FileManager.default
        guard let files = try? fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.contentModificationDateKey])
            .filter({ $0.lastPathComponent.hasPrefix("session-") }) else { return }
        let sorted = files.sorted {
            (try? $0.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate ?? .distantPast) ?? .distantPast >
            (try? $1.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate ?? .distantPast) ?? .distantPast
        }
        for old in sorted.dropFirst(keeping) { try? fm.removeItem(at: old) }
    }

    /// Reveal the current log in Finder (for the Help menu).
    func revealLogInFinder() {
        if let url = logURL { NSWorkspace.shared.activateFileViewerSelecting([url]) }
    }

    private static func fileStamp() -> String {
        let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmmss"; f.locale = Locale(identifier: "en_US_POSIX")
        return f.string(from: Date())
    }
    private static func readableStamp() -> String {
        let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd HH:mm:ss.SSS"; f.locale = Locale(identifier: "en_US_POSIX")
        return f.string(from: Date())
    }
}
