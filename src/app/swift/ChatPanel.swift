import SwiftUI

/// Listen Room chat, inline in the monitor dock. Messages ride the relay's
/// `/api/chat` endpoint; "studio" is us, anything else is a listener.
struct ChatPanel: View {
    @EnvironmentObject private var listen: ListenRoomController
    @State private var draft = ""

    var body: some View {
        VStack(spacing: Theme.Space.lg) {
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: Theme.Space.md) {
                        if listen.chatMessages.isEmpty {
                            Text(listen.enabled ? "아직 메시지가 없습니다" : "송출을 시작하면 대화할 수 있습니다")
                                .font(Theme.Font.ui(8.5))
                                .foregroundStyle(Theme.Palette.textFainter)
                                .frame(maxWidth: .infinity, alignment: .center)
                                .padding(.vertical, Theme.Space.xl)
                        }
                        ForEach(listen.chatMessages) { message in
                            bubble(message).id(message.id)
                        }
                    }
                    .padding(Theme.Space.lg)
                }
                .frame(height: 150)
                .onChange(of: listen.chatMessages.count) {
                    if let last = listen.chatMessages.last {
                        withAnimation { proxy.scrollTo(last.id, anchor: .bottom) }
                    }
                }
            }
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.display)
                    .fill(Theme.Palette.recess)
            )

            HStack(spacing: Theme.Space.sm) {
                TextField("메시지", text: $draft)
                    .textFieldStyle(.plain)
                    .font(Theme.Font.ui(9))
                    .foregroundStyle(Theme.Palette.text)
                    .padding(.horizontal, Theme.Space.lg)
                    .frame(height: 24)
                    .background(
                        RoundedRectangle(cornerRadius: Theme.Radius.pill)
                            .fill(Theme.Palette.background)
                            .overlay(
                                RoundedRectangle(cornerRadius: Theme.Radius.pill)
                                    .stroke(Theme.Palette.divider, lineWidth: 1)
                            )
                    )
                    .onSubmit(send)

                Button(action: send) {
                    Text("전송")
                        .font(Theme.Font.ui(9, .medium))
                        .foregroundStyle(Theme.Palette.deepBorder)
                        .padding(.horizontal, Theme.Space.xl)
                        .frame(height: 24)
                        .background(
                            RoundedRectangle(cornerRadius: Theme.Radius.pill)
                                .fill(Theme.Palette.amber)
                        )
                }
                .buttonStyle(.plain)
                .disabled(!listen.enabled)
            }
        }
    }

    private func send() {
        let text = draft
        draft = ""
        listen.sendChat(text)
    }

    private func bubble(_ message: ListenRoomController.ChatMessage) -> some View {
        HStack {
            if message.isStudio { Spacer(minLength: 40) }
            Text(message.text)
                .font(Theme.Font.ui(9))
                .foregroundStyle(message.isStudio ? Theme.Palette.deepBorder : Theme.Palette.text)
                .padding(.horizontal, Theme.Space.lg)
                .padding(.vertical, Theme.Space.md)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.display)
                        .fill(message.isStudio ? Theme.Palette.amber : Theme.Palette.button)
                )
            if !message.isStudio { Spacer(minLength: 40) }
        }
    }
}
