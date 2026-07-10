import SwiftUI

// Design tokens lifted from design/Neuracoust DAW v2.dc.html.
// Values are documented in docs/design-tokens.md — change them here, nowhere else.

extension Color {
    init(hex: UInt32) {
        self.init(
            .sRGB,
            red: Double((hex >> 16) & 0xff) / 255.0,
            green: Double((hex >> 8) & 0xff) / 255.0,
            blue: Double(hex & 0xff) / 255.0,
            opacity: 1.0
        )
    }
}

enum Theme {
    enum Palette {
        // Structural neutrals, darkest recess to lightest chrome.
        static let deepBorder = Color(hex: 0x0b0806)
        static let recess = Color(hex: 0x0b0908)
        static let videoCell = Color(hex: 0x1a1510)
        static let border = Color(hex: 0x1b1611)
        static let stripFooter = Color(hex: 0x25201b)
        static let panel = Color(hex: 0x221e1a)
        static let background = Color(hex: 0x282320)
        static let surface = Color(hex: 0x2c2622)
        static let rail = Color(hex: 0x302a24)
        static let ruler = Color(hex: 0x332c26)
        static let toolbar = Color(hex: 0x37302a)
        static let button = Color(hex: 0x3d352e)
        static let titlebarTop = Color(hex: 0x463c33)
        static let buttonProminent = Color(hex: 0x4b4036)
        static let divider = Color(hex: 0x4f4339)
        static let scrollThumb = Color(hex: 0x5f5244)

        // The one cool gray the warm reskin kept: column and transport separators.
        static let coolDivider = Color(hex: 0x3d4650)
        static let coolDividerBright = Color(hex: 0x455060)

        // Text, brightest to faintest.
        static let textBright = Color(hex: 0xf0eadf)
        static let text = Color(hex: 0xe8e1d5)
        static let textNumeric = Color(hex: 0xddd5c8)
        static let textSecondary = Color(hex: 0xc9c0b1)
        static let textDim = Color(hex: 0xaaa08e)
        static let textMuted = Color(hex: 0x918676)
        static let textLabel = Color(hex: 0x867b6a)
        static let textFaint = Color(hex: 0x665c4e)
        static let textFainter = Color(hex: 0x574d40)

        // Accents.
        static let accent = Color(hex: 0x5f9fd6)
        static let accentHover = Color(hex: 0x84b3dd)
        static let purple = Color(hex: 0xa06bff)
        static let purpleLight = Color(hex: 0xc4a6ff)
        static let vca = Color(hex: 0xc56bff)
        static let teal = Color(hex: 0x22c3a6)
        static let green = Color(hex: 0x46d17f)
        static let instrument = Color(hex: 0x3ddc84)
        static let yellow = Color(hex: 0xe6d24a)
        static let red = Color(hex: 0xff5252)
        static let amber = Color(hex: 0xe6a23c)
        static let tabActive = Color(hex: 0xd9a441)
        static let orange = Color(hex: 0xff9f43)
        static let ioValue = Color(hex: 0x5fe38a)

        // Window traffic lights.
        static let trafficRed = Color(hex: 0xff5f57)
        static let trafficYellow = Color(hex: 0xfebc2e)
        static let trafficGreen = Color(hex: 0x28c840)
    }

    enum Space {
        static let xxs: CGFloat = 2
        static let xs: CGFloat = 3
        static let sm: CGFloat = 4
        static let md: CGFloat = 6
        static let lg: CGFloat = 8
        static let xl: CGFloat = 12
        static let xxl: CGFloat = 14
    }

    enum Radius {
        static let meterCell: CGFloat = 1
        static let tag: CGFloat = 2
        static let pill: CGFloat = 3
        static let clip: CGFloat = 4
        static let button: CGFloat = 5
        static let display: CGFloat = 6
        static let card: CGFloat = 7
        static let panel: CGFloat = 8
        static let modal: CGFloat = 12
        static let window: CGFloat = 10
    }

    enum Font {
        static let ui = "Space Grotesk"
        static let mono = "IBM Plex Mono"

        // Space Grotesk and IBM Plex Mono are not system fonts. Until they ship in
        // the bundle, Font.custom falls back to the system face at the same size.
        static func ui(_ size: CGFloat, _ weight: SwiftUI.Font.Weight = .regular) -> SwiftUI.Font {
            .custom(ui, size: size).weight(weight)
        }

        static func mono(_ size: CGFloat, _ weight: SwiftUI.Font.Weight = .regular) -> SwiftUI.Font {
            .custom(mono, size: size).weight(weight)
        }
    }

    enum Gradient {
        static let titlebar = LinearGradient(
            colors: [Palette.titlebarTop, Palette.toolbar],
            startPoint: .top, endPoint: .bottom
        )

        static let transport = LinearGradient(
            colors: [Palette.toolbar, Palette.surface],
            startPoint: .top, endPoint: .bottom
        )

        static let monitorHeader = LinearGradient(
            colors: [Color(hex: 0x2a2137), Palette.panel],
            startPoint: .top, endPoint: .bottom
        )

        /// Master-out bar: green to 60%, yellow to 85%, red beyond.
        static let masterMeter = LinearGradient(
            stops: [
                .init(color: Palette.green, location: 0.0),
                .init(color: Palette.green, location: 0.60),
                .init(color: Palette.yellow, location: 0.60),
                .init(color: Palette.yellow, location: 0.85),
                .init(color: Palette.red, location: 0.85),
                .init(color: Palette.red, location: 1.0),
            ],
            startPoint: .leading, endPoint: .trailing
        )

        /// Channel-strip meter, drawn top-down: red at the top of the scale.
        static let stripMeter = LinearGradient(
            stops: [
                .init(color: Palette.red, location: 0.0),
                .init(color: Palette.red, location: 0.12),
                .init(color: Palette.yellow, location: 0.12),
                .init(color: Palette.yellow, location: 0.32),
                .init(color: Palette.green, location: 0.32),
                .init(color: Palette.green, location: 1.0),
            ],
            startPoint: .top, endPoint: .bottom
        )

        static let dspLoad = LinearGradient(
            colors: [Palette.green, Palette.yellow],
            startPoint: .leading, endPoint: .trailing
        )
    }

    enum Shadow {
        static let card = (color: Color.black.opacity(0.30), radius: CGFloat(3), y: CGFloat(2))
        static let menu = (color: Color.black.opacity(0.60), radius: CGFloat(17), y: CGFloat(14))
        static let modal = (color: Color.black.opacity(0.70), radius: CGFloat(60), y: CGFloat(40))
    }

    /// Fixed artboard the design was drawn against.
    static let artboard = CGSize(width: 1920, height: 1180)
    static let monitorDockWidth: CGFloat = 294
    static let toolRailWidth: CGFloat = 44
    static let laneHeaderWidth: CGFloat = 576
}
