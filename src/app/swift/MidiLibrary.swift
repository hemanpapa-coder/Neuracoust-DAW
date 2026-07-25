import Foundation
import Combine

/// One indexed MIDI file, classified by the bundled neuracoust_midi_indexer (drum vs. melodic, instrument
/// family, genre, mood — all from the file's contents, not just its name).
struct MidiLibraryEntry: Identifiable, Hashable {
    let path: String
    let name: String
    let pack: String
    let genre: String
    let mood: String        // "" for drums; else bright/dark/energetic/calm/tense/melancholic
    let instrument: String  // Drums / Piano / Guitar / Bass / Strings / Brass / Reed / …
    let isDrum: Bool
    let bpm: Int?
    let isFill: Bool
    var id: String { path }
}

/// The MIDI library: discovers the mounted collection, has the C++ indexer classify it into a TSV, and
/// answers filtered queries. The full index `all` is touched ONLY on `queue`; every @Published mutation
/// hops to main, so 580k+ entries never reach SwiftUI (it sees a capped, filtered slice).
final class MidiLibrary: ObservableObject {
    @Published private(set) var results: [MidiLibraryEntry] = []
    @Published private(set) var totalIndexed: Int = 0
    @Published private(set) var isIndexing: Bool = false
    @Published private(set) var indexProgress: String = ""
    @Published private(set) var genres: [String] = []
    @Published private(set) var instruments: [String] = []
    @Published private(set) var hasCollection: Bool = false

    @Published var searchText: String = "" { didSet { scheduleFilter() } }
    @Published var genreFilter: String? = nil { didSet { scheduleFilter() } }
    @Published var instrumentFilter: String? = nil { didSet { scheduleFilter() } }
    @Published var moodFilter: String? = nil { didSet { scheduleFilter() } }
    @Published var drumFilter: DrumFilter = .all { didSet { scheduleFilter() } }
    @Published var bpmMin: Int = 0 { didSet { scheduleFilter() } }
    @Published var bpmMax: Int = 300 { didSet { scheduleFilter() } }
    @Published var fillFilter: FillFilter = .all { didSet { scheduleFilter() } }

    enum FillFilter { case all, groovesOnly, fillsOnly }
    enum DrumFilter { case all, drumsOnly, melodicOnly }

    static let displayCap = 500

    private var all: [MidiLibraryEntry] = []     // queue-confined
    private let queue = DispatchQueue(label: "midi.library.index", qos: .utility)
    private var loadStarted = false

    init() {
        queue.async { [weak self] in
            let found = !Self.discoverRoots().isEmpty
            DispatchQueue.main.async { self?.hasCollection = found }
        }
    }

    // Folder names that mark a directory as the MIDI collection root; any is enough to recognise it.
    private static let signatureNames = [
        "Mega Drums Pack (for Drums Synths)", "Various Drum Midis", "Updated EZ Drummer Midi Packs",
        "Piano Collection", "Jazz Mega Drums and Instruments Pack",
    ]

    private static var cacheURL: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Neuracoust", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("midi-index.tsv")
    }

    /// Find the pack folders to index: the immediate sub-folders of any collection root (a volume or the
    /// Desktop that contains a signature folder). Covers the WHOLE collection — drum and non-drum alike.
    static func discoverRoots() -> [URL] {
        let fm = FileManager.default
        var searchDirs: [URL] = []
        if let vols = try? fm.contentsOfDirectory(at: URL(fileURLWithPath: "/Volumes"),
                                                  includingPropertiesForKeys: nil) { searchDirs += vols }
        if let home = ProcessInfo.processInfo.environment["HOME"] {
            searchDirs.append(URL(fileURLWithPath: home).appendingPathComponent("Desktop"))
        }
        var collectionRoots: [URL] = []
        for dir in searchDirs {
            for name in signatureNames {
                var isDir: ObjCBool = false
                if fm.fileExists(atPath: dir.appendingPathComponent(name).path, isDirectory: &isDir), isDir.boolValue {
                    collectionRoots.append(dir); break
                }
            }
        }
        var packs: [URL] = []
        for root in collectionRoots {
            guard let subs = try? fm.contentsOfDirectory(at: root, includingPropertiesForKeys: [.isDirectoryKey],
                                                         options: [.skipsHiddenFiles]) else { continue }
            for s in subs {
                if (try? s.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true { packs.append(s) }
            }
        }
        return packs
    }

    var hasCollectionSync: Bool { !Self.discoverRoots().isEmpty }

    func loadCacheIfAvailable() {
        guard !loadStarted else { return }
        loadStarted = true
        queue.async { [weak self] in
            guard let self,
                  let data = try? String(contentsOf: Self.cacheURL, encoding: .utf8), !data.isEmpty
            else { return }
            self.adopt(Self.parseCache(data))
        }
    }

    /// (Re)build the index by spawning the bundled classifier over the discovered packs, then load its TSV.
    func rebuildIndex() {
        guard !isIndexing else { return }
        isIndexing = true
        indexProgress = "색인 준비 중…"
        let helper = Bundle.main.bundlePath + "/Contents/MacOS/neuracoust_midi_indexer"
        let cache = Self.cacheURL
        queue.async { [weak self] in
            guard let self else { return }
            let roots = Self.discoverRoots()
            if roots.isEmpty {
                DispatchQueue.main.async { self.isIndexing = false; self.indexProgress = "MIDI 컬렉션을 찾을 수 없습니다" }
                return
            }
            guard FileManager.default.isExecutableFile(atPath: helper) else {
                DispatchQueue.main.async { self.isIndexing = false; self.indexProgress = "색인 도구가 번들에 없습니다" }
                return
            }
            let proc = Process()
            proc.executableURL = URL(fileURLWithPath: helper)
            proc.arguments = [cache.path] + roots.map { $0.path }
            let pipe = Pipe(); proc.standardOutput = pipe
            var carry = ""
            pipe.fileHandleForReading.readabilityHandler = { fh in
                let d = fh.availableData
                guard !d.isEmpty, let s = String(data: d, encoding: .utf8) else { return }
                carry += s
                while let nl = carry.firstIndex(of: "\n") {
                    let line = String(carry[carry.startIndex..<nl]); carry = String(carry[carry.index(after: nl)...])
                    let p = line.split(separator: " ").map(String.init)
                    if p.first == "PROGRESS", p.count >= 3 {
                        DispatchQueue.main.async { self.indexProgress = "분류 중… \(p[1]) / \(p[2])" }
                    }
                }
            }
            do { try proc.run() } catch {
                DispatchQueue.main.async { self.isIndexing = false; self.indexProgress = "색인 실행 실패" }
                return
            }
            proc.waitUntilExit()
            pipe.fileHandleForReading.readabilityHandler = nil
            DispatchQueue.main.async { self.isIndexing = false; self.indexProgress = "" }
            if let data = try? String(contentsOf: cache, encoding: .utf8), !data.isEmpty {
                self.adopt(Self.parseCache(data))
            } else {
                DispatchQueue.main.async { self.indexProgress = "색인 결과를 읽을 수 없습니다" }
            }
        }
    }

    // MARK: - Filtering (on `queue`)

    private var filterWork: DispatchWorkItem?
    private func scheduleFilter() {
        filterWork?.cancel()
        let needle = searchText.lowercased(), genre = genreFilter, inst = instrumentFilter, mood = moodFilter
        let drum = drumFilter, lo = bpmMin, hi = bpmMax, fill = fillFilter
        let work = DispatchWorkItem { [weak self] in
            self?.runFilter(needle, genre, inst, mood, drum, lo, hi, fill)
        }
        filterWork = work
        queue.asyncAfter(deadline: .now() + 0.12, execute: work)
    }

    private func runFilter(_ needle: String, _ genre: String?, _ inst: String?, _ mood: String?,
                           _ drum: DrumFilter, _ lo: Int, _ hi: Int, _ fill: FillFilter) {
        var out: [MidiLibraryEntry] = []
        out.reserveCapacity(Self.displayCap)
        for e in all {
            switch drum {
            case .drumsOnly where !e.isDrum: continue
            case .melodicOnly where e.isDrum: continue
            default: break
            }
            if let g = genre, e.genre != g { continue }
            if let i = inst, e.instrument != i { continue }
            if let m = mood, e.mood != m { continue }
            switch fill {
            case .groovesOnly where e.isFill: continue
            case .fillsOnly where !e.isFill: continue
            default: break
            }
            if lo > 0 || hi < 300 {
                guard let b = e.bpm, b >= lo, b <= hi else { continue }
            }
            if !needle.isEmpty,
               !e.name.lowercased().contains(needle), !e.genre.lowercased().contains(needle),
               !e.pack.lowercased().contains(needle) { continue }
            out.append(e)
            if out.count >= Self.displayCap { break }
        }
        DispatchQueue.main.async { [weak self] in self?.results = out }
    }

    /// Runs on `queue`. Adopts the index and publishes the derived facet lists + first page.
    private func adopt(_ entries: [MidiLibraryEntry]) {
        all = entries
        var gSeen = Set<String>(), iSeen = Set<String>()
        var gs: [String] = [], ins: [String] = []
        for e in entries {
            if !e.genre.isEmpty && gSeen.insert(e.genre).inserted { gs.append(e.genre) }
            if !e.instrument.isEmpty && iSeen.insert(e.instrument).inserted { ins.append(e.instrument) }
        }
        let sortedGenres = gs.sorted()
        let sortedInstruments = ins.sorted()
        let total = entries.count
        DispatchQueue.main.async { [weak self] in
            self?.totalIndexed = total
            self?.genres = sortedGenres
            self?.instruments = sortedInstruments
            if total > 0 { self?.hasCollection = true }
        }
        runFilter("", nil, nil, nil, .all, 0, 300, .all)
    }

    // MARK: - Cache (TSV: path name pack genre mood instrument isDrum bpm isFill)

    private static func parseCache(_ data: String) -> [MidiLibraryEntry] {
        var out: [MidiLibraryEntry] = []
        out.reserveCapacity(1_000_000)
        data.enumerateLines { line, _ in
            let f = line.components(separatedBy: "\t")
            guard f.count >= 9 else { return }
            out.append(MidiLibraryEntry(path: f[0], name: f[1], pack: f[2], genre: f[3], mood: f[4],
                                        instrument: f[5], isDrum: f[6] == "1", bpm: Int(f[7]), isFill: f[8] == "1"))
        }
        return out
    }
}
