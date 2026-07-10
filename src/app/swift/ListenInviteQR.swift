import AppKit
import CoreImage
import CoreImage.CIFilterBuiltins
import SwiftUI

/// The QR invite for the Listen Room, ported from the old UI's Core Image card.
///
/// A listener on the same network scans the code; the text link is shown beneath it
/// for anyone who would rather paste it. The code is only as reachable as the URL in
/// it — `listenRoomShareHost` puts a LAN address there, not loopback, so a phone on
/// the same WiFi can actually open it.
enum ListenInviteQR {
    /// A crisp black-on-white QR for `text`, or nil if it could not be built.
    static func image(for text: String, side: CGFloat) -> NSImage? {
        guard !text.isEmpty, let data = text.data(using: .utf8) else { return nil }

        let filter = CIFilter.qrCodeGenerator()
        filter.message = data
        // M tolerates a little smudging on a phone camera without ballooning the code.
        filter.correctionLevel = "M"
        guard let output = filter.outputImage, output.extent.width > 0 else { return nil }

        // Scale by integer pixels so the modules stay square and sharp.
        let scale = side / output.extent.width
        let scaled = output.transformed(by: CGAffineTransform(scaleX: scale, y: scale))

        let context = CIContext(options: nil)
        guard let cgImage = context.createCGImage(scaled, from: scaled.extent) else { return nil }
        return NSImage(cgImage: cgImage, size: NSSize(width: side, height: side))
    }
}

/// The popover shown from the Listen Room's QR button.
struct ListenInvitePanel: View {
    let shareURL: String
    let onCopyLink: () -> Void

    private var host: String {
        // The address between "http://" and the port, for the reachability note.
        guard let start = shareURL.range(of: "://")?.upperBound else { return "" }
        let rest = shareURL[start...]
        return String(rest.prefix { $0 != ":" && $0 != "/" })
    }

    private var reachabilityNote: String {
        if host.hasPrefix("127.") || host == "localhost" {
            return "이 기계에서만 열립니다 — 네트워크에 연결되어 있지 않습니다."
        }
        if host.hasPrefix("169.254.") {
            return "링크-로컬 주소입니다. 같은 케이블/직결 링크의 기기만 접속됩니다."
        }
        return "같은 네트워크(\(host))의 기기에서 스캔하세요."
    }

    var body: some View {
        VStack(spacing: Theme.Space.md) {
            Text("Listen Room 초대")
                .font(Theme.Font.ui(11, .bold))
                .foregroundStyle(Theme.Palette.text)

            if let qr = ListenInviteQR.image(for: shareURL, side: 220) {
                Image(nsImage: qr)
                    .interpolation(.none)   // keep the modules hard-edged
                    .resizable()
                    .frame(width: 220, height: 220)
                    .padding(10)
                    .background(RoundedRectangle(cornerRadius: 12).fill(.white))
            } else {
                Text("송출을 시작하면 QR이 나타납니다")
                    .font(Theme.Font.ui(9))
                    .foregroundStyle(Theme.Palette.textFaint)
                    .frame(width: 220, height: 220)
            }

            Text(reachabilityNote)
                .font(Theme.Font.mono(8))
                .foregroundStyle(host.hasPrefix("127.") || host.hasPrefix("169.254.")
                                 ? Theme.Palette.amber : Theme.Palette.textFaint)
                .multilineTextAlignment(.center)
                .frame(width: 240)

            Text(shareURL)
                .font(Theme.Font.mono(7.5))
                .foregroundStyle(Theme.Palette.textDim)
                .lineLimit(2)
                .truncationMode(.middle)
                .frame(width: 240)
                .textSelection(.enabled)

            Button(action: onCopyLink) {
                Text("⧉ 링크 복사")
                    .font(Theme.Font.ui(9, .semibold))
                    .foregroundStyle(Theme.Palette.deepBorder)
                    .padding(.horizontal, Theme.Space.xl)
                    .padding(.vertical, 5)
                    .background(RoundedRectangle(cornerRadius: Theme.Radius.button).fill(Theme.Palette.accent))
            }
            .buttonStyle(.plain)
        }
        .padding(Theme.Space.xxl)
        .frame(width: 280)
        .background(Theme.Palette.surface)
    }
}
