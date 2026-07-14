import SwiftUI

/// Modal plug-in browser: filter facets on the left, matches in the middle, and the
/// target track's insert chain on the right.
struct PluginBrowser: View {
    @EnvironmentObject private var engine: EngineController

    var body: some View {
        ZStack {
            Color.black.opacity(0.55)
                .ignoresSafeArea()
                .onTapGesture { engine.closePluginBrowser() }

            HStack(spacing: 0) {
                filterColumn
                    .frame(width: 210)

                Rectangle().fill(Theme.Palette.deepBorder).frame(width: 1)

                listColumn
                    .frame(maxWidth: .infinity)

                Rectangle().fill(Theme.Palette.deepBorder).frame(width: 1)

                chainColumn
                    .frame(width: 250)
            }
            .frame(width: 1000, height: 620)
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.modal)
                    .fill(Theme.Palette.rail)
            )
            .clipShape(RoundedRectangle(cornerRadius: Theme.Radius.modal))
            .overlay(
                RoundedRectangle(cornerRadius: Theme.Radius.modal)
                    .stroke(Theme.Palette.deepBorder, lineWidth: 1)
            )
            .shadow(color: .black.opacity(0.7), radius: 60, y: 40)
        }
    }

    // MARK: Filters

    private var filterColumn: some View {
        VStack(alignment: .leading, spacing: Theme.Space.xl) {
            Text("필터")
                .font(Theme.Font.ui(11, .bold))
                .foregroundStyle(Theme.Palette.textBright)

            facetGroup("Format", engine.formats, selection: $engine.pluginFormat)
            facetGroup("Category", engine.categories, selection: $engine.pluginCategory)
            facetGroup("Brand", engine.brands, selection: $engine.pluginBrand)

            Spacer()
        }
        .padding(Theme.Space.xxl)
        .frame(maxHeight: .infinity, alignment: .top)
        .background(Theme.Palette.surface)
    }

    private func facetGroup(_ title: String,
                            _ facets: [EngineController.Facet],
                            selection: Binding<String>) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title)
                .font(Theme.Font.mono(7))
                .tracking(0.6)
                .foregroundStyle(Theme.Palette.textFaint)
                .padding(.bottom, 2)

            facetRow("전체", tally: nil, active: selection.wrappedValue.isEmpty) {
                selection.wrappedValue = ""
            }
            // Long facet lists (20 categories) would push the column off-screen.
            ScrollView {
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(facets) { facet in
                        facetRow(facet.name,
                                 tally: facet.tally,
                                 active: selection.wrappedValue == facet.name) {
                            selection.wrappedValue = selection.wrappedValue == facet.name ? "" : facet.name
                        }
                    }
                }
            }
            .frame(maxHeight: 140)
            .scrollIndicators(.never)
        }
    }

    private func facetRow(_ title: String,
                          tally: Int?,
                          active: Bool,
                          action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack {
                Text(title)
                    .font(Theme.Font.ui(11))
                    .foregroundStyle(active ? Theme.Palette.accent : Theme.Palette.textDim)
                    .lineLimit(1)
                Spacer(minLength: Theme.Space.md)
                if let tally {
                    Text("\(tally)")
                        .font(Theme.Font.mono(8))
                        .foregroundStyle(Theme.Palette.textFainter)
                }
            }
            .padding(.horizontal, Theme.Space.lg)
            .padding(.vertical, 5)
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.button)
                    .fill(active ? Color(hex: 0x20282e) : .clear)
            )
        }
        .buttonStyle(.plain)
    }

    // MARK: List

    private var listColumn: some View {
        VStack(spacing: 0) {
            HStack(spacing: Theme.Space.lg) {
                TextField("플러그인 검색", text: $engine.pluginSearch)
                    .textFieldStyle(.plain)
                    .font(Theme.Font.ui(11))
                    .foregroundStyle(Theme.Palette.text)
                    .padding(.horizontal, Theme.Space.xl)
                    .frame(height: 28)
                    .background(
                        RoundedRectangle(cornerRadius: Theme.Radius.display)
                            .fill(Theme.Palette.background)
                            .overlay(
                                RoundedRectangle(cornerRadius: Theme.Radius.display)
                                    .stroke(Theme.Palette.divider, lineWidth: 1)
                            )
                    )

                Text(matchLabel)
                    .font(Theme.Font.mono(8.5))
                    .foregroundStyle(Theme.Palette.textFaint)
                    .fixedSize()

                // Force a fresh scan so a plug-in installed after launch appears.
                Button { engine.rescanPlugins() } label: {
                    Image(systemName: "arrow.clockwise").font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(Theme.Palette.textSecondary)
                        .frame(width: 28, height: 28)
                        .background(RoundedRectangle(cornerRadius: Theme.Radius.display).fill(Theme.Palette.background)
                            .overlay(RoundedRectangle(cornerRadius: Theme.Radius.display).stroke(Theme.Palette.divider, lineWidth: 1)))
                }
                .buttonStyle(.plain).help("플러그인 재스캔 (새로 설치한 플러그인 반영)")
            }
            .padding(Theme.Space.xxl)

            Divider().overlay(Theme.Palette.border)

            ScrollView {
                LazyVStack(spacing: 1) {
                    ForEach(engine.plugins) { plugin in
                        pluginRow(plugin)
                    }
                }
                .padding(.vertical, Theme.Space.md)
            }
        }
        .background(Theme.Palette.surface)
    }

    /// The engine caps what it materialises; say so rather than pretend the list is whole.
    private var matchLabel: String {
        let shown = engine.plugins.count
        let total = engine.pluginMatchCount
        return shown < total ? "\(shown) / \(total)개 표시" : "\(total)개"
    }

    private func pluginRow(_ plugin: EngineController.PluginCandidate) -> some View {
        HStack(spacing: Theme.Space.xl) {
            RoundedRectangle(cornerRadius: Theme.Radius.pill)
                .fill(Theme.Palette.accent.opacity(0.16))
                .frame(width: 26, height: 26)
                .overlay(
                    Text(String(plugin.brand.prefix(1)))
                        .font(Theme.Font.ui(11, .bold))
                        .foregroundStyle(Theme.Palette.accent)
                )

            VStack(alignment: .leading, spacing: 1) {
                Text(plugin.name)
                    .font(Theme.Font.ui(11, .semibold))
                    .foregroundStyle(Theme.Palette.textBright)
                    .lineLimit(1)
                Text("\(plugin.brand) · \(plugin.category)")
                    .font(Theme.Font.ui(8.5))
                    .foregroundStyle(Theme.Palette.textFaint)
                    .lineLimit(1)
            }

            Spacer(minLength: Theme.Space.lg)

            Text(plugin.format)
                .font(Theme.Font.mono(7.5, .semibold))
                .foregroundStyle(Theme.Palette.teal)
                .padding(.horizontal, Theme.Space.md)
                .padding(.vertical, 2)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.tag)
                        .fill(Theme.Palette.teal.opacity(0.14))
                )

            Button { engine.addInsert(plugin.id) } label: {
                Text("추가")
                    .font(Theme.Font.ui(9, .semibold))
                    .foregroundStyle(Theme.Palette.deepBorder)
                    .padding(.horizontal, Theme.Space.xl)
                    .padding(.vertical, 4)
                    .background(
                        RoundedRectangle(cornerRadius: Theme.Radius.button)
                            .fill(Theme.Palette.accent)
                    )
            }
            .buttonStyle(.plain)
        }
        .padding(.horizontal, Theme.Space.xxl)
        .padding(.vertical, Theme.Space.md)
        .contentShape(Rectangle())
    }

    // MARK: Insert chain

    private var targetTrack: EngineController.Track? {
        guard let id = engine.pluginTargetTrack, id != EngineController.masterInsertTargetId else { return nil }
        return engine.tracks.first { $0.id == id }
    }

    /// The browser also fills the master chain, which belongs to no track.
    private var targetIsMaster: Bool {
        engine.pluginTargetTrack == EngineController.masterInsertTargetId
    }

    private var targetChain: [EngineController.InsertSlot] {
        targetIsMaster ? engine.masterInserts : (targetTrack?.inserts ?? [])
    }

    private var chainColumn: some View {
        VStack(alignment: .leading, spacing: Theme.Space.xl) {
            HStack {
                VStack(alignment: .leading, spacing: 1) {
                    Text("인서트 체인")
                        .font(Theme.Font.ui(11, .bold))
                        .foregroundStyle(Theme.Palette.textBright)
                    Text(targetIsMaster ? "Master" : (targetTrack?.name ?? "—"))
                        .font(Theme.Font.ui(9))
                        .foregroundStyle(Theme.Palette.textFaint)
                }
                Spacer()
                Button { engine.closePluginBrowser() } label: {
                    Text("닫기")
                        .font(Theme.Font.ui(9))
                        .foregroundStyle(Theme.Palette.textSecondary)
                        .padding(.horizontal, Theme.Space.xl)
                        .padding(.vertical, 5)
                        .background(
                            RoundedRectangle(cornerRadius: Theme.Radius.button)
                                .fill(Theme.Palette.button)
                        )
                }
                .buttonStyle(.plain)
            }

            if engine.pluginBrowserOpen {
                // An instrument track carries an instrument slot (+ layers) that is NOT an
                // insert; show it here so a loaded instrument is visible in the browser, not
                // just in the channel strip.
                if let track = targetTrack, track.kind == .instrument {
                    VStack(alignment: .leading, spacing: Theme.Space.sm) {
                        Text("악기")
                            .font(Theme.Font.mono(8)).foregroundStyle(Theme.Palette.textFaint)
                        if track.instrumentLayers.isEmpty {
                            Text("아직 악기가 없습니다 — 목록에서 악기를 추가하세요")
                                .font(Theme.Font.ui(9)).foregroundStyle(Theme.Palette.textFainter)
                                .frame(maxWidth: .infinity).padding(.vertical, Theme.Space.lg)
                        } else {
                            ForEach(Array(track.instrumentLayers.enumerated()), id: \.offset) { idx, layer in
                                HStack(spacing: Theme.Space.md) {
                                    Text("\(idx + 1)").font(Theme.Font.mono(8))
                                        .foregroundStyle(Theme.Palette.textFaint)
                                    Text(layer.name).font(Theme.Font.ui(10))
                                        .foregroundStyle(Theme.Palette.textSecondary).lineLimit(1)
                                    Spacer(minLength: 0)
                                }
                                .padding(.horizontal, Theme.Space.md).padding(.vertical, 5)
                                .background(RoundedRectangle(cornerRadius: Theme.Radius.button).fill(track.kind.accent.opacity(0.14)))
                            }
                        }
                        Text("인서트")
                            .font(Theme.Font.mono(8)).foregroundStyle(Theme.Palette.textFaint)
                            .padding(.top, Theme.Space.sm)
                    }
                }
                VStack(spacing: Theme.Space.sm) {
                    ForEach(targetChain) { slot in
                        chainRow(slot: slot)
                    }
                    if targetChain.isEmpty {
                        Text("아직 인서트가 없습니다")
                            .font(Theme.Font.ui(9))
                            .foregroundStyle(Theme.Palette.textFainter)
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, Theme.Space.xl)
                    }
                }
            }

            Spacer()

            Text("Monitor DSP anchor 보호됨")
                .font(Theme.Font.mono(7.5))
                .foregroundStyle(Theme.Palette.textFainter)
        }
        .padding(Theme.Space.xxl)
        .frame(maxHeight: .infinity, alignment: .top)
        .background(Theme.Palette.surface)
    }

    private func chainRow(slot: EngineController.InsertSlot) -> some View {
        let accent = targetIsMaster ? Theme.Palette.orange : (targetTrack?.kind.accent ?? Theme.Palette.textFaint)
        return chainRowBody(slot: slot, accent: accent)
    }

    private func chainRowBody(slot: EngineController.InsertSlot, accent: Color) -> some View {
        HStack(spacing: Theme.Space.md) {
            Text("⠿")
                .font(Theme.Font.mono(9))
                .foregroundStyle(Theme.Palette.textFainter)

            VStack(alignment: .leading, spacing: 0) {
                Text(slot.isEmpty ? "빈 슬롯" : slot.name)
                    .font(Theme.Font.ui(9.5, .medium))
                    .foregroundStyle(slot.bypassed ? Theme.Palette.textFaint : Theme.Palette.textNumeric)
                    .lineLimit(1)
                if !slot.isEmpty {
                    Text(slot.bypassed ? "bypassed" : slot.modeBadge)
                        .font(Theme.Font.mono(7))
                        .foregroundStyle(badgeColor(slot))
                }
            }

            Spacer(minLength: Theme.Space.sm)

            if !slot.isEmpty {
                iconButton("⏻", tint: slot.bypassed ? Theme.Palette.orange : Theme.Palette.textFaint) {
                    if targetIsMaster { engine.toggleMasterInsertBypass(slot: slot.id) }
                    else if let track = targetTrack { engine.toggleInsertBypass(track: track.id, slot: slot.id) }
                }
                iconButton("↑") {
                    if targetIsMaster { engine.moveMasterInsert(slot: slot.id, direction: -1) }
                    else if let track = targetTrack { engine.moveInsert(track: track.id, slot: slot.id, direction: -1) }
                }
                iconButton("↓") {
                    if targetIsMaster { engine.moveMasterInsert(slot: slot.id, direction: 1) }
                    else if let track = targetTrack { engine.moveInsert(track: track.id, slot: slot.id, direction: 1) }
                }
                iconButton("✕", tint: Theme.Palette.red) {
                    if targetIsMaster { engine.removeMasterInsert(slot: slot.id) }
                    else if let track = targetTrack { engine.removeInsert(track: track.id, slot: slot.id) }
                }
            }
        }
        .padding(.horizontal, Theme.Space.lg)
        .padding(.vertical, Theme.Space.md)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.display)
                .fill(Theme.Palette.background)
                .overlay(alignment: .leading) {
                    Rectangle()
                        .fill(slot.isEmpty ? .clear : accent)
                        .frame(width: 3)
                }
        )
        .clipShape(RoundedRectangle(cornerRadius: Theme.Radius.display))
    }

    /// INT and RINT mean the plug-in is off the audio thread — worth seeing at a glance.
    private func badgeColor(_ slot: EngineController.InsertSlot) -> Color {
        if slot.bypassed { return Theme.Palette.textFainter }
        switch slot.modeBadge {
        case "INT": return Theme.Palette.purple
        case "RINT": return Theme.Palette.teal
        case "EXT": return Theme.Palette.amber
        default: return Theme.Palette.textFaint
        }
    }

    private func iconButton(_ glyph: String,
                            tint: Color = Theme.Palette.textFaint,
                            action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(glyph)
                .font(Theme.Font.ui(9))
                .foregroundStyle(tint)
                .frame(width: 18, height: 18)
                .background(
                    RoundedRectangle(cornerRadius: Theme.Radius.pill)
                        .fill(Theme.Palette.button)
                )
        }
        .buttonStyle(.plain)
    }
}
