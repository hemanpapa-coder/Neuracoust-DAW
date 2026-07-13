import SwiftUI

// The AI assistant (Phase 0). The ported neuracoust::daw AiAssistant library builds the
// request and validates commands (through the nc_ai_* bridge); this talks to the local
// Ollama server and drives the panel. A reply is one JSON object — a conversational line
// plus zero or more proposed commands — and each command applies as one undo step only
// after the user confirms. Nothing here runs on the audio thread.
@MainActor
final class AiAssistantController: ObservableObject {

    struct ChatMessage: Identifiable {
        enum Role { case user, assistant, system }
        let id = UUID()
        let role: Role
        let text: String
    }

    /// One proposed edit parsed from the assistant's reply. It is not applied until the
    /// user taps Apply — mirroring the library's requiresUserConfirmation contract.
    struct Proposal: Identifiable {
        let id = UUID()
        let type: String
        let track: String
        let gainDb: Float
        let pan: Float
        let enabled: Bool
        let timeSeconds: Double
        let label: String
        let reason: String

        var summary: String {
            switch type {
            case "set_track_gain": return "\(track): 게인 \(String(format: "%+.1f", gainDb)) dB"
            case "set_track_pan": return "\(track): 팬 \(String(format: "%+.2f", pan))"
            case "set_track_mute": return "\(track): \(enabled ? "뮤트" : "뮤트 해제")"
            case "set_track_solo": return "\(track): \(enabled ? "솔로" : "솔로 해제")"
            case "arm_track_for_recording": return "\(track): \(enabled ? "녹음 준비" : "녹음 준비 해제")"
            case "add_marker": return "마커 '\(label)' @ \(String(format: "%.2f", timeSeconds))s"
            default: return type
            }
        }
    }

    @Published var open = false
    @Published var messages: [ChatMessage] = []
    @Published var proposals: [Proposal] = []
    @Published var models: [String] = []
    @Published var model = ""
    @Published var busy = false
    @Published var input = ""
    /// Every supported command is reversible (one undo step), so the assistant acts on its
    /// own by default — the user asked it to do things, not just suggest them. Turn this off
    /// to review each proposed command with an Apply button before it runs.
    @Published var autoApply = true

    private unowned let engine: EngineController
    private let host = "http://127.0.0.1:11434"

    init(engine: EngineController) {
        self.engine = engine
    }

    func toggle() {
        open.toggle()
        if open && models.isEmpty { Task { await refreshModels() } }
    }

    /// Ask the local Ollama server which models are installed, and pick a sensible default —
    /// prefer a general instruct model over a slow reasoning one for an interactive assistant.
    func refreshModels() async {
        guard let url = URL(string: "\(host)/api/tags") else { return }
        do {
            let (data, _) = try await URLSession.shared.data(from: url)
            let decoded = try JSONDecoder().decode(TagsResponse.self, from: data)
            let names = decoded.models.map(\.name).sorted()
            guard !names.isEmpty else { return }   // keep the last good list, never blank it
            models = names
            if model.isEmpty || !names.contains(model) {
                model = Self.preferredModel(from: names) ?? names.first ?? ""
            }
        } catch {
            // Keep whatever list we had; only surface the error if we have nothing.
            if models.isEmpty {
                appendSystem("Ollama에 연결할 수 없어요 (\(host)). ollama serve 가 켜져 있는지 확인하세요.")
            }
        }
    }

    private static func preferredModel(from names: [String]) -> String? {
        // Prefer a strong instruct/coding model that holds up under JSON-constrained,
        // multi-command prompts. gemma:12b degenerates on compound commands (empty/garbage
        // output), so it comes last; reasoning models are avoided (slow, hidden tokens).
        let lowered = names.map { ($0, $0.lowercased()) }
        func firstMatch(_ needles: [String]) -> String? {
            lowered.first { pair in needles.contains { pair.1.contains($0) } && !pair.1.contains("reasoning") }?.0
        }
        return firstMatch(["mistral-small", "qwen3-coder", "qwen3.6", "qwen2.5", "qwen", "llama", "gemma"])
            ?? names.first { !$0.lowercased().contains("reasoning") }
    }

    func send() {
        let text = input.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty, !busy else { return }
        input = ""
        proposals = []
        messages.append(ChatMessage(role: .user, text: text))
        Task { await run(text) }
    }

    private func run(_ userText: String) async {
        guard let handle = engine.rawHandle, !model.isEmpty else {
            appendSystem("사용할 모델이 없어요. Ollama에 모델을 설치하세요.")
            return
        }
        busy = true
        defer { busy = false }

        // The bridge assembles the request: system prompt + command schema + project snapshot.
        let requestBody = engine.readEngineString(capacity: 65536) { buf, len in
            model.withCString { m in
                userText.withCString { u in
                    nc_ai_build_request(handle, m, u, buf, len)
                }
            }
        }
        guard let url = URL(string: "\(host)/api/chat"),
              let bodyData = requestBody.data(using: .utf8) else { return }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = bodyData
        request.timeoutInterval = 180

        do {
            let (data, _) = try await URLSession.shared.data(for: request)
            let chat = try JSONDecoder().decode(ChatResponse.self, from: data)
            parseReply(chat.message.content)
        } catch {
            appendSystem("응답을 받지 못했어요: \(error.localizedDescription)")
        }
    }

    /// The reply content is itself a JSON object (Ollama format:"json"): a conversational
    /// line plus proposed commands. Show the line, stage the commands.
    private func parseReply(_ content: String) {
        guard let data = content.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            messages.append(ChatMessage(role: .assistant, text: content))
            return
        }
        let reply = (obj["reply"] as? String) ?? content
        messages.append(ChatMessage(role: .assistant, text: reply))
        let commands = (obj["commands"] as? [[String: Any]]) ?? []
        let parsed: [Proposal] = commands.compactMap { c in
            guard let type = c["type"] as? String else { return nil }
            return Proposal(
                type: Self.normalizeCommandType(type),
                track: (c["track"] as? String) ?? "",
                gainDb: floatValue(c["gainDb"]),
                pan: floatValue(c["pan"]),
                enabled: (c["enabled"] as? Bool) ?? false,
                timeSeconds: doubleValue(c["timeSeconds"]),
                label: (c["label"] as? String) ?? "",
                reason: (c["reason"] as? String) ?? ""
            )
        }
        // The assistant acts by default: reversible commands run immediately (each one undo
        // step). With auto-apply off, they wait as proposals the user applies by hand.
        if autoApply {
            proposals = []
            for proposal in parsed { apply(proposal) }
        } else {
            proposals = parsed
        }
    }

    func apply(_ proposal: Proposal) {
        guard let handle = engine.rawHandle else { return }
        var applied = false
        let message = engine.readEngineString(capacity: 512) { buf, len in
            proposal.type.withCString { t in
                proposal.track.withCString { tr in
                    proposal.label.withCString { lb in
                        applied = nc_ai_apply_command(handle, t, tr, proposal.gainDb, proposal.pan,
                                                      proposal.enabled, proposal.timeSeconds, lb, buf, len)
                    }
                }
            }
        }
        if applied {
            engine.reloadAfterExternalEdit()
            proposals.removeAll { $0.id == proposal.id }
            appendSystem("적용: \(proposal.summary)")
        } else {
            appendSystem("적용 실패: \(message.isEmpty ? proposal.summary : message)")
        }
    }

    func applyAll() {
        for proposal in proposals { apply(proposal) }
    }

    private func appendSystem(_ text: String) {
        messages.append(ChatMessage(role: .system, text: text))
    }

    /// The engine's command types are snake_case; a model sometimes answers in CamelCase
    /// ("SetTrackSolo"). Normalize so either form applies.
    static func normalizeCommandType(_ t: String) -> String {
        if t.contains("_") { return t.lowercased() }
        var out = ""
        for (i, ch) in t.enumerated() {
            if ch.isUppercase && i > 0 { out.append("_") }
            out.append(Character(ch.lowercased()))
        }
        return out
    }

    private func floatValue(_ v: Any?) -> Float {
        if let d = v as? Double { return Float(d) }
        if let i = v as? Int { return Float(i) }
        if let s = v as? String, let d = Double(s) { return Float(d) }
        return 0
    }
    private func doubleValue(_ v: Any?) -> Double {
        if let d = v as? Double { return d }
        if let i = v as? Int { return Double(i) }
        if let s = v as? String, let d = Double(s) { return d }
        return 0
    }

    // Ollama response shapes.
    private struct TagsResponse: Decodable {
        struct Model: Decodable { let name: String }
        let models: [Model]
    }
    private struct ChatResponse: Decodable {
        struct Message: Decodable { let content: String }
        let message: Message
    }
}

/// The floating AI assistant panel — chat log, staged command chips, model picker, input.
struct AiAssistantPanel: View {
    @EnvironmentObject private var engine: EngineController
    @ObservedObject var ai: AiAssistantController

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider().overlay(Theme.Palette.divider)
            log
            if !ai.proposals.isEmpty { proposalBar }
            Divider().overlay(Theme.Palette.divider)
            inputBar
        }
        .frame(width: 420, height: 560)
        .background(Theme.Palette.panel)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.Palette.divider, lineWidth: 1))
        .shadow(color: .black.opacity(0.5), radius: 20, y: 10)
        .padding(24)
        // Populate the model list before the user opens the picker, so opening it never
        // races an async update (which would dismiss the menu — the flicker).
        .task { await ai.refreshModels() }
    }

    private var header: some View {
        HStack(spacing: 8) {
            Image(systemName: "sparkles").foregroundStyle(Theme.Palette.purple)
            Text("AI 어시스턴트").font(Theme.Font.ui(13, .semibold)).foregroundStyle(Theme.Palette.textBright)
            Spacer()
            if ai.busy { ProgressView().controlSize(.small).padding(.trailing, 4) }
            // Auto-apply: the assistant acts immediately (reversible, one undo step each).
            Button { ai.autoApply.toggle() } label: {
                HStack(spacing: 3) {
                    Image(systemName: ai.autoApply ? "bolt.fill" : "bolt.slash")
                        .font(.system(size: 9))
                    Text("자동").font(Theme.Font.mono(8))
                }
                .foregroundStyle(ai.autoApply ? Theme.Palette.green : Theme.Palette.textFaint)
            }
            .buttonStyle(.plain)
            .help(ai.autoApply ? "자동 적용 켜짐 — AI가 바로 실행 (되돌리기 가능)" : "자동 적용 꺼짐 — 제안만")
            Menu {
                // Flat buttons (not a Picker) so an async model-list update while the menu is
                // open doesn't rebuild and dismiss it — the flicker the Picker caused.
                ForEach(ai.models, id: \.self) { name in
                    Button {
                        ai.model = name
                    } label: {
                        if name == ai.model { Label(name, systemImage: "checkmark") }
                        else { Text(name) }
                    }
                }
                if ai.models.isEmpty {
                    Text("모델 목록 없음").font(Theme.Font.mono(9))
                }
                Divider()
                Button("모델 새로고침") { Task { await ai.refreshModels() } }
            } label: {
                Text(ai.model.isEmpty ? "모델 선택" : ai.model)
                    .font(Theme.Font.mono(9)).foregroundStyle(Theme.Palette.textSecondary)
                    .lineLimit(1).frame(width: 130, alignment: .trailing)
            }
            .menuStyle(.button).buttonStyle(.plain)
            Button { ai.open = false } label: {
                Image(systemName: "xmark").font(.system(size: 11, weight: .bold))
            }
            .buttonStyle(.plain).foregroundStyle(Theme.Palette.textMuted)
        }
        .padding(.horizontal, 14).frame(height: 44)
    }

    private var log: some View {
        ScrollViewReader { proxy in
            ScrollView {
                VStack(alignment: .leading, spacing: 10) {
                    if ai.messages.isEmpty {
                        Text("프로젝트에 대해 물어보거나 지시하세요.\n예: \"보컬을 3dB 올려줘\", \"드럼 뮤트해줘\"")
                            .font(Theme.Font.ui(11)).foregroundStyle(Theme.Palette.textFaint)
                            .padding(.top, 8)
                    }
                    ForEach(ai.messages) { message in
                        bubble(message).id(message.id)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(14)
            }
            .onChange(of: ai.messages.count) { _, _ in
                if let last = ai.messages.last { withAnimation { proxy.scrollTo(last.id, anchor: .bottom) } }
            }
        }
        .frame(maxHeight: .infinity)
    }

    private func bubble(_ message: AiAssistantController.ChatMessage) -> some View {
        HStack {
            if message.role == .user { Spacer(minLength: 40) }
            Text(message.text)
                .font(Theme.Font.ui(11))
                .foregroundStyle(message.role == .system ? Theme.Palette.textMuted : Theme.Palette.text)
                .padding(.horizontal, 10).padding(.vertical, 7)
                .background(
                    RoundedRectangle(cornerRadius: 9).fill(
                        message.role == .user ? Theme.Palette.accent.opacity(0.22)
                        : message.role == .system ? Theme.Palette.surface.opacity(0.5)
                        : Theme.Palette.surface)
                )
                .textSelection(.enabled)
            if message.role != .user { Spacer(minLength: 40) }
        }
    }

    private var proposalBar: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("제안된 작업 \(ai.proposals.count)").font(Theme.Font.ui(9)).foregroundStyle(Theme.Palette.textFaint)
                Spacer()
                Button("모두 적용") { ai.applyAll() }
                    .font(Theme.Font.ui(9, .semibold)).buttonStyle(.plain).foregroundStyle(Theme.Palette.green)
            }
            ForEach(ai.proposals) { proposal in
                HStack(spacing: 8) {
                    VStack(alignment: .leading, spacing: 1) {
                        Text(proposal.summary).font(Theme.Font.mono(10)).foregroundStyle(Theme.Palette.textSecondary)
                        if !proposal.reason.isEmpty {
                            Text(proposal.reason).font(Theme.Font.ui(8)).foregroundStyle(Theme.Palette.textFaint).lineLimit(1)
                        }
                    }
                    Spacer(minLength: 0)
                    Button("적용") { ai.apply(proposal) }
                        .font(Theme.Font.ui(9, .semibold)).buttonStyle(.plain).foregroundStyle(Theme.Palette.accent)
                }
                .padding(.horizontal, 8).padding(.vertical, 5)
                .background(RoundedRectangle(cornerRadius: 6).fill(Theme.Palette.surface))
            }
        }
        .padding(.horizontal, 14).padding(.vertical, 8)
        .background(Theme.Palette.background)
    }

    private var inputBar: some View {
        HStack(spacing: 8) {
            TextField("메시지 또는 지시…", text: $ai.input, axis: .vertical)
                .textFieldStyle(.plain).font(Theme.Font.ui(11)).foregroundStyle(Theme.Palette.text)
                .lineLimit(1...4)
                .onSubmit { ai.send() }
            Button { ai.send() } label: {
                Image(systemName: "arrow.up.circle.fill").font(.system(size: 20))
                    .foregroundStyle(ai.busy || ai.input.isEmpty ? Theme.Palette.textFaint : Theme.Palette.accent)
            }
            .buttonStyle(.plain).disabled(ai.busy || ai.input.isEmpty)
        }
        .padding(.horizontal, 14).frame(height: 48)
    }
}
