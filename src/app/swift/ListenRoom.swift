import AppKit
import Foundation

/// Listen Room orchestration, ported from DawWindowController.mm.
///
/// The engine encodes and pushes audio; it does not run the relay. The relay is a
/// Python process (shipped by the Neuracoust Listen project) that this controller
/// launches, plus an optional reverse SSH tunnel that exposes it off the LAN.
/// Chat rides the relay's `/api/chat` endpoint.
@MainActor
final class ListenRoomController: ObservableObject {
    struct ChatMessage: Identifiable, Equatable {
        let id: Int
        let sender: String
        let text: String

        var isStudio: Bool { sender == "studio" }
    }

    @Published private(set) var enabled = false
    @Published private(set) var relayReachable = false
    @Published private(set) var senderRunning = false
    @Published private(set) var offerReady = false
    @Published private(set) var packetsQueued: UInt64 = 0
    @Published private(set) var packetsDropped: UInt64 = 0
    @Published private(set) var transportMode = ""
    @Published private(set) var statusMessage = ""
    @Published private(set) var shareUrl = ""

    @Published private(set) var chatMessages: [ChatMessage] = []
    @Published private(set) var chatUnread = 0
    @Published var chatOpen = false
    @Published var qrOpen = false

    /// Surfaced in the dock when the relay script is missing or fails to launch.
    @Published private(set) var lastError: String?

    private var relayTask: Process?
    private var tunnelTask: Process?
    private var chatPollTimer: Timer?
    private var chatPollInFlight = false
    private var chatLastId = 0

    /// True once we have launched the detached relay daemon, so we know it is ours
    /// to reap. start-relay.sh pkills any earlier instance, so nothing else owns it.
    private var startedRelayDaemon = false

    private unowned let engine: EngineController

    init(engine: EngineController) {
        self.engine = engine

        // onDisappear does not fire on Cmd-Q, and the relay daemon and ssh tunnel
        // are detached children that would survive us.
        NotificationCenter.default.addObserver(
            forName: NSApplication.willTerminateNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.shutdown() }
        }
    }

    // MARK: Relay script discovery

    /// Bundle first, then the Neuracoust Listen source tree, then a system install.
    private var relayScriptPath: String? {
        var candidates: [String] = []
        if let bundled = Bundle.main.path(forResource: "start-relay", ofType: "sh") {
            candidates.append(bundled)
        }
        candidates.append("/Volumes/Program Dev/Neuracoust Listen/scripts/start-relay.sh")
        candidates.append("/Library/Application Support/Neuracoust Listen/scripts/start-relay.sh")
        return candidates.first { FileManager.default.isExecutableFile(atPath: $0) }
    }

    // MARK: Enable / disable

    func toggle() {
        enabled ? stop() : start()
    }

    private func start() {
        guard let handle = engine.rawHandle else { return }

        guard let scriptPath = relayScriptPath else {
            lastError = "Listen relay script를 찾을 수 없습니다"
            return
        }

        // Mint the access token before reading settings — the relay needs it.
        nc_listen_set_enabled(handle, true)

        let sessionName = engine.readEngineString { nc_listen_session_name(handle, $0, $1) }
        let accessToken = engine.readEngineString { nc_listen_access_token(handle, $0, $1) }
        let quality = engine.readEngineString { nc_listen_quality(handle, $0, $1) }
        let latencyMode = engine.readEngineString { nc_listen_latency_mode(handle, $0, $1) }
        let httpPort = Int(nc_listen_relay_http_port(handle))
        let ingestPort = Int(nc_listen_relay_tcp_ingest_port(handle))

        let task = Process()
        task.executableURL = URL(fileURLWithPath: scriptPath)
        task.arguments = ["--host", "0.0.0.0", "--session", sessionName]

        var environment = ProcessInfo.processInfo.environment
        environment["NEURACOUST_LISTEN_HTTP_PORT"] = String(httpPort)
        environment["NEURACOUST_LISTEN_TCP_INGEST_PORT"] = String(ingestPort)
        environment["LISTEN_ACCESS_TOKEN"] = accessToken
        environment["LISTEN_QUALITY"] = quality
        environment["LISTEN_LATENCY_MODE"] = latencyMode
        environment["LISTEN_TRANSPORT_MODE"] = "direct_fallback"
        task.environment = environment
        task.standardOutput = Pipe()
        task.standardError = Pipe()

        // start-relay.sh is a launcher: it nohups the relay daemon and exits 0 once
        // the daemon answers /api/stats. A non-zero exit is the only failure signal;
        // the script exiting is normal and must not read as "the relay died".
        // First run can take a minute while it installs Python dependencies.
        task.terminationHandler = { finished in
            let code = finished.terminationStatus
            guard code != 0, code != 15, code != 9 else { return }
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.lastError = "Listen relay 시작 실패 (exit \(code)) — /tmp/neuracoust-listen-relay.log 확인"
                self.stop()
            }
        }

        do {
            try task.run()
            relayTask = task
            startedRelayDaemon = true
            enabled = true
            lastError = nil
            startChatPolling()
        } catch {
            nc_listen_set_enabled(handle, false)
            lastError = "Listen relay 시작 실패: \(error.localizedDescription)"
            return
        }

        // The relay needs a moment to bind before the tunnel can forward to it.
        Task { [weak self] in
            try? await Task.sleep(for: .seconds(1))
            guard let self, self.enabled else { return }
            self.startExternalTunnel()
        }
    }

    private func stop() {
        guard let handle = engine.rawHandle else { return }
        nc_listen_set_enabled(handle, false)
        enabled = false

        stopChatPolling()
        terminate(&tunnelTask)
        terminate(&relayTask)
        killRelayDaemon()
    }

    private func terminate(_ task: inout Process?) {
        if let running = task, running.isRunning {
            running.terminate()
        }
        task = nil
    }

    /// start-relay.sh nohups the daemon, so terminating the launcher leaves it
    /// running. Reap it explicitly — otherwise it outlives the app.
    private func killRelayDaemon() {
        guard startedRelayDaemon else { return }
        startedRelayDaemon = false

        let reaper = Process()
        reaper.executableURL = URL(fileURLWithPath: "/usr/bin/pkill")
        reaper.arguments = ["-f", "listen_relay.py"]
        try? reaper.run()
        reaper.waitUntilExit()
    }

    /// Called when the app is going away, so nothing we spawned outlives it.
    func shutdown() {
        stopChatPolling()
        terminate(&tunnelTask)
        terminate(&relayTask)
        killRelayDaemon()
    }

    // MARK: External tunnel

    /// Reverse-forwards the local relay port to a jump host so listeners outside
    /// the LAN can reach it. Failure is not fatal — the LAN share link still works.
    private func startExternalTunnel() {
        guard tunnelTask == nil || tunnelTask?.isRunning == false else { return }

        let sshPath = "/usr/bin/ssh"
        guard FileManager.default.isExecutableFile(atPath: sshPath) else { return }

        let target = ProcessInfo.processInfo.environment["NEURACOUST_LISTEN_SSH_TARGET"]?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let host = (target?.isEmpty == false) ? target! : "windows11-server"

        let task = Process()
        task.executableURL = URL(fileURLWithPath: sshPath)
        task.arguments = [
            "-N", "-T",
            "-o", "BatchMode=yes",
            "-o", "ExitOnForwardFailure=yes",
            "-o", "ServerAliveInterval=20",
            "-o", "ServerAliveCountMax=3",
            "-R", "127.0.0.1:18787:127.0.0.1:8787",
            host,
        ]
        task.standardOutput = Pipe()
        task.standardError = Pipe()
        task.terminationHandler = { finished in
            let code = finished.terminationStatus
            // 0 normal, 15 SIGTERM, 9 SIGKILL — all expected when we stop it.
            guard code != 0, code != 15, code != 9 else { return }
            Task { @MainActor [weak self] in
                guard let self, self.enabled else { return }
                self.lastError = "Listen 외부 터널 실패: \(code)"
            }
        }

        do {
            try task.run()
            tunnelTask = task
        } catch {
            // The LAN link is unaffected; report but keep streaming.
            lastError = "Listen 외부 터널 시작 실패: \(error.localizedDescription)"
        }
    }

    // MARK: Share link

    func copyShareLink() {
        guard let handle = engine.rawHandle else { return }
        let link = engine.readEngineString(capacity: 256) { nc_listen_public_share_url(handle, $0, $1) }
        guard !link.isEmpty else { return }
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(link, forType: .string)
        statusMessage = "링크 복사됨"
    }

    // MARK: Chat

    private var chatUrlBase: String? {
        guard let handle = engine.rawHandle else { return nil }
        let host = engine.readEngineString { nc_listen_relay_host(handle, $0, $1) }
        let port = Int(nc_listen_relay_http_port(handle))
        let token = engine.readEngineString { nc_listen_access_token(handle, $0, $1) }
        guard !host.isEmpty else { return nil }
        return "http://\(host):\(port)/api/chat?token=\(token)"
    }

    private func startChatPolling() {
        stopChatPolling()
        let timer = Timer(timeInterval: 1.0, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.pollChat() }
        }
        RunLoop.main.add(timer, forMode: .common)
        chatPollTimer = timer
    }

    private func stopChatPolling() {
        chatPollTimer?.invalidate()
        chatPollTimer = nil
        chatPollInFlight = false
    }

    private func pollChat() {
        guard enabled, !chatPollInFlight,
              let base = chatUrlBase,
              let url = URL(string: "\(base)&since=\(chatLastId)") else { return }

        chatPollInFlight = true
        URLSession.shared.dataTask(with: url) { [weak self] data, _, error in
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.chatPollInFlight = false
                guard error == nil, let data, !data.isEmpty else { return }
                self.appendMessages(from: data, countUnread: true)
            }
        }.resume()
    }

    func sendChat(_ text: String) {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, let base = chatUrlBase, let url = URL(string: base) else { return }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try? JSONSerialization.data(
            withJSONObject: ["sender": "studio", "text": trimmed]
        )

        URLSession.shared.dataTask(with: request) { [weak self] data, _, error in
            Task { @MainActor [weak self] in
                guard let self, error == nil, let data, !data.isEmpty else { return }
                self.appendMessages(from: data, countUnread: false)
            }
        }.resume()
    }

    /// The relay answers a GET with `{"messages": [...]}` and a POST with the
    /// single message it just stored.
    private func appendMessages(from data: Data, countUnread: Bool) {
        guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return }

        let raw: [[String: Any]]
        if let list = json["messages"] as? [[String: Any]] {
            raw = list
        } else if json["id"] != nil {
            raw = [json]
        } else {
            return
        }

        for entry in raw {
            guard let id = entry["id"] as? Int,
                  let sender = entry["sender"] as? String,
                  let text = entry["text"] as? String,
                  id > chatLastId || !chatMessages.contains(where: { $0.id == id })
            else { continue }

            chatMessages.append(ChatMessage(id: id, sender: sender, text: text))
            chatLastId = max(chatLastId, id)
            if countUnread, !chatOpen, sender != "studio" {
                chatUnread += 1
            }
        }
    }

    func markChatRead() {
        chatUnread = 0
    }

    // MARK: Poll

    /// Called from EngineController's 30 Hz tick.
    func refresh() {
        guard let handle = engine.rawHandle else { return }

        var status = NCListenStatus()
        nc_listen_status(handle, &status)

        senderRunning = status.senderRunning
        relayReachable = status.relayReachable
        offerReady = status.nativeWebRtcOfferReady
        packetsQueued = status.packetsQueued
        packetsDropped = status.packetsDropped
        transportMode = withUnsafePointer(to: status.transportMode) {
            $0.withMemoryRebound(to: CChar.self, capacity: Int(NC_TEXT_LEN)) { String(cString: $0) }
        }
        shareUrl = withUnsafePointer(to: status.shareUrl) {
            $0.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) }
        }
    }
}
