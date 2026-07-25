import SwiftUI
import AppKit
import AVFoundation
import UniformTypeIdentifiers

struct VocalSingerProfile: Codable, Identifiable, Equatable {
    let id: UUID
    var name: String
    var referenceFile: String
    var identityStrength: Double
    var semitoneShift: Int
    let createdAt: Date
    var additionalReferenceFiles: [String]?
}

@main
struct NeuracoustVocalConverterApp: App {
    @StateObject private var controller = VocalConverterController()

    var body: some Scene {
        Window("Neuracoust Vocal Converter", id: "main") {
            VocalConverterView()
                .environmentObject(controller)
                .frame(minWidth: 880, minHeight: 650)
                .onAppear { controller.refreshRuntimeStatus() }
        }
        .windowStyle(.hiddenTitleBar)
        .defaultSize(width: 1040, height: 760)
    }
}

@MainActor
final class VocalConverterController: NSObject, ObservableObject, AVAudioPlayerDelegate {
    enum Quality: String, CaseIterable, Identifiable {
        case preview, standard, highest
        var id: String { rawValue }
        var title: String {
            switch self {
            case .preview: return "빠른 미리듣기"
            case .standard: return "표준"
            case .highest: return "최고 품질"
            }
        }
        var steps: Int {
            switch self {
            case .preview: return 20
            case .standard: return 30
            case .highest: return 45
            }
        }
    }

    @Published var sourceURL: URL?
    @Published var referenceURL: URL?
    @Published var resultURL: URL?
    @Published var singerProfiles: [VocalSingerProfile] = []
    @Published var selectedSingerID: UUID?
    @Published var quality: Quality = .standard
    @Published var semitoneShift = 0
    @Published var identityStrength = 0.70
    @Published var runtimeReady = false
    @Published var isBusy = false
    @Published var status = "보컬 파일과 새 가수의 참조 음성을 넣어주세요."
    @Published var logText = ""
    @Published var playing: URL?
    @Published var trainingSteps = 300
    @Published var trainingSourceCount = 0

    private var process: Process?
    private var player: AVAudioPlayer?
    private let lastSingerKey = "nc.vocalConverter.lastSinger"
    private let restoreNaturalIdentityMigrationKey = "nc.vocalConverter.restoreNaturalIdentityV2"

    private var supportRoot: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Neuracoust Vocal Converter", isDirectory: true)
    }
    private var repositoryURL: URL { supportRoot.appendingPathComponent("seed-vc", isDirectory: true) }
    private var runtimePythonURL: URL {
        supportRoot.appendingPathComponent(".venv/bin/python", isDirectory: false)
    }
    private var singersRoot: URL { supportRoot.appendingPathComponent("Singers", isDirectory: true) }
    private var singerLibraryURL: URL { singersRoot.appendingPathComponent("library.json") }

    override init() {
        super.init()
        loadSingerLibrary()
    }

    func refreshRuntimeStatus() {
        runtimeReady = FileManager.default.isExecutableFile(atPath: runtimePythonURL.path)
            && FileManager.default.fileExists(atPath: repositoryURL.appendingPathComponent("inference.py").path)
        if runtimeReady && !isBusy {
            status = "Seed-VC 준비 완료 · Apple Silicon 오프라인 처리"
        }
    }

    func chooseSource() { chooseAudio(title: "Suno 보컬 스템 선택") { sourceURL = $0; resultURL = nil } }
    func registerSinger() {
        chooseAudio(title: "가수 라이브러리에 등록할 참조 음성") { [weak self] url in
            self?.registerSinger(from: url)
        }
    }

    var selectedSinger: VocalSingerProfile? {
        singerProfiles.first(where: { $0.id == selectedSingerID })
    }

    func selectSinger(_ profile: VocalSingerProfile) {
        let url = singersRoot.appendingPathComponent(profile.referenceFile)
        guard FileManager.default.fileExists(atPath: url.path) else {
            status = "\(profile.name)의 참조 음성 파일을 찾지 못했습니다."
            return
        }
        selectedSingerID = profile.id
        referenceURL = url
        identityStrength = profile.identityStrength
        semitoneShift = profile.semitoneShift
        refreshTrainingSourceCount()
        UserDefaults.standard.set(profile.id.uuidString, forKey: lastSingerKey)
        status = "\(profile.name) 선택됨 · 새 보컬을 렌더링할 준비가 됐습니다."
    }

    func renameSinger(_ profile: VocalSingerProfile) {
        guard let name = promptForSingerName(defaultValue: profile.name, title: "가수 이름 변경"),
              let index = singerProfiles.firstIndex(where: { $0.id == profile.id }) else { return }
        singerProfiles[index].name = name
        sortAndSaveSingerLibrary()
    }

    func saveCurrentSettingsToSinger() {
        guard let id = selectedSingerID,
              let index = singerProfiles.firstIndex(where: { $0.id == id }) else { return }
        singerProfiles[index].identityStrength = identityStrength
        singerProfiles[index].semitoneShift = semitoneShift
        saveSingerLibrary()
        status = "\(singerProfiles[index].name)의 기본 설정을 저장했습니다."
    }

    func deleteSinger(_ profile: VocalSingerProfile) {
        let alert = NSAlert()
        alert.messageText = "\(profile.name)을(를) 라이브러리에서 삭제할까요?"
        alert.informativeText = "참조 음성은 휴지통으로 이동됩니다."
        alert.addButton(withTitle: "삭제")
        alert.addButton(withTitle: "취소")
        guard alert.runModal() == .alertFirstButtonReturn else { return }

        let folder = singersRoot.appendingPathComponent(profile.id.uuidString, isDirectory: true)
        NSWorkspace.shared.recycle([folder]) { [weak self] _, error in
            Task { @MainActor in
                guard let self else { return }
                if let error {
                    self.status = "가수를 삭제하지 못했습니다: \(error.localizedDescription)"
                    return
                }
                self.singerProfiles.removeAll(where: { $0.id == profile.id })
                if self.selectedSingerID == profile.id {
                    self.selectedSingerID = nil
                    self.referenceURL = nil
                    UserDefaults.standard.removeObject(forKey: self.lastSingerKey)
                }
                self.saveSingerLibrary()
                self.status = "\(profile.name)을(를) 라이브러리에서 삭제했습니다."
            }
        }
    }

    func revealSingerLibrary() {
        try? FileManager.default.createDirectory(at: singersRoot, withIntermediateDirectories: true)
        NSWorkspace.shared.open(singersRoot)
    }

    var selectedSingerReferenceCount: Int {
        guard let singer = selectedSinger else { return 0 }
        return 1 + (singer.additionalReferenceFiles?.count ?? 0)
    }

    var selectedSingerHasModel: Bool {
        guard let id = selectedSingerID else { return false }
        return FileManager.default.fileExists(
            atPath: singersRoot.appendingPathComponent("\(id.uuidString)/Model/ft_model.pth").path
        )
    }

    func addReferencesToSelectedSinger() {
        guard let id = selectedSingerID,
              let index = singerProfiles.firstIndex(where: { $0.id == id }) else { return }
        let panel = NSOpenPanel()
        panel.title = "음색 혼합에 사용할 레퍼런스 보컬 추가"
        panel.allowedContentTypes = [.wav, .aiff, .mp3, .mpeg4Audio, .audio]
        panel.allowsMultipleSelection = true
        guard panel.runModal() == .OK else { return }
        let folder = singersRoot.appendingPathComponent("\(id.uuidString)/References", isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            var references = singerProfiles[index].additionalReferenceFiles ?? []
            for source in panel.urls {
                let ext = source.pathExtension.isEmpty ? "wav" : source.pathExtension.lowercased()
                let destination = folder.appendingPathComponent("\(UUID().uuidString).\(ext)")
                try FileManager.default.copyItem(at: source, to: destination)
                references.append("\(id.uuidString)/References/\(destination.lastPathComponent)")
            }
            singerProfiles[index].additionalReferenceFiles = references
            saveSingerLibrary()
            status = "\(panel.urls.count)개 레퍼런스를 추가했습니다 · 총 \(1 + references.count)개 음색 혼합"
        } catch {
            status = "레퍼런스 추가 실패: \(error.localizedDescription)"
        }
    }

    func addTrainingAudio() {
        guard let id = selectedSingerID else { return }
        let panel = NSOpenPanel()
        panel.title = "가수 전용 모델 학습용 보컬 추가"
        panel.allowedContentTypes = [.wav, .aiff, .mp3, .mpeg4Audio, .audio]
        panel.allowsMultipleSelection = true
        guard panel.runModal() == .OK else { return }
        let folder = singersRoot.appendingPathComponent("\(id.uuidString)/Training Sources", isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            for source in panel.urls {
                let ext = source.pathExtension.isEmpty ? "wav" : source.pathExtension.lowercased()
                let destination = folder.appendingPathComponent("\(UUID().uuidString).\(ext)")
                try FileManager.default.copyItem(at: source, to: destination)
            }
            refreshTrainingSourceCount()
            status = "학습용 보컬 \(panel.urls.count)개를 추가했습니다."
        } catch {
            status = "학습 데이터 추가 실패: \(error.localizedDescription)"
        }
    }

    func trainSelectedSinger() {
        guard !isBusy, let singer = selectedSinger,
              let trainer = Bundle.main.url(forResource: "vocal_converter_train", withExtension: "py") else { return }
        let singerFolder = singersRoot.appendingPathComponent(singer.id.uuidString, isDirectory: true)
        let trainingFolder = singerFolder.appendingPathComponent("Training Sources", isDirectory: true)
        let referenceFolder = singerFolder.appendingPathComponent("References", isDirectory: true)
        let datasetFolder = singerFolder.appendingPathComponent("Dataset", isDirectory: true)
        let modelFolder = singerFolder.appendingPathComponent("Model", isDirectory: true)
        var arguments = [
            trainer.path,
            "--repo", repositoryURL.path,
            "--dataset", datasetFolder.path,
            "--model-output", modelFolder.path,
            "--run-name", "singer_\(singer.id.uuidString.lowercased())",
            "--steps", "\(trainingSteps)",
            "--input-file", singersRoot.appendingPathComponent(singer.referenceFile).path
        ]
        if FileManager.default.fileExists(atPath: trainingFolder.path) {
            arguments += ["--input-folder", trainingFolder.path]
        }
        if FileManager.default.fileExists(atPath: referenceFolder.path) {
            arguments += ["--input-folder", referenceFolder.path]
        }
        startProcess(
            executable: runtimePythonURL,
            arguments: arguments,
            workingDirectory: repositoryURL,
            busyStatus: "\(singer.name) 전용 모델 학습 준비 중…"
        ) { [weak self] success in
            self?.status = success
                ? "\(singer.name) 전용 모델 학습 완료 · 다음 변환부터 자동 적용됩니다."
                : "전용 모델 학습에 실패했습니다. 아래 로그를 확인해주세요."
            self?.refreshTrainingSourceCount()
        }
    }

    private func refreshTrainingSourceCount() {
        guard let id = selectedSingerID else {
            trainingSourceCount = 0
            return
        }
        let folder = singersRoot.appendingPathComponent("\(id.uuidString)/Training Sources", isDirectory: true)
        trainingSourceCount = (try? FileManager.default.contentsOfDirectory(
            at: folder, includingPropertiesForKeys: nil
        ).filter { !$0.hasDirectoryPath }.count) ?? 0
    }

    func acceptDropped(_ providers: [NSItemProvider], asReference: Bool) -> Bool {
        guard let provider = providers.first(where: { $0.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier) }) else {
            return false
        }
        provider.loadItem(forTypeIdentifier: UTType.fileURL.identifier, options: nil) { [weak self] item, _ in
            let url: URL?
            if let data = item as? Data { url = URL(dataRepresentation: data, relativeTo: nil) }
            else if let value = item as? URL { url = value }
            else { url = nil }
            guard let url else { return }
            Task { @MainActor in
                if asReference { self?.registerSinger(from: url) }
                else { self?.sourceURL = url; self?.resultURL = nil }
            }
        }
        return true
    }

    private func registerSinger(from source: URL) {
        guard let name = promptForSingerName(
            defaultValue: source.deletingPathExtension().lastPathComponent,
            title: "새 가수 등록"
        ) else { return }

        do {
            let audio = try AVAudioFile(forReading: source)
            guard audio.length > 0 else {
                status = "비어 있는 오디오 파일은 등록할 수 없습니다."
                return
            }
            let id = UUID()
            let folder = singersRoot.appendingPathComponent(id.uuidString, isDirectory: true)
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            let ext = source.pathExtension.isEmpty ? "wav" : source.pathExtension.lowercased()
            let destination = folder.appendingPathComponent("reference.\(ext)")
            try FileManager.default.copyItem(at: source, to: destination)
            let relativePath = "\(id.uuidString)/\(destination.lastPathComponent)"
            let profile = VocalSingerProfile(
                id: id,
                name: name,
                referenceFile: relativePath,
                identityStrength: identityStrength,
                semitoneShift: semitoneShift,
                createdAt: Date(),
                additionalReferenceFiles: nil
            )
            singerProfiles.append(profile)
            sortAndSaveSingerLibrary()
            selectSinger(profile)
            status = "\(name)을(를) 가수 라이브러리에 등록했습니다."
        } catch {
            status = "가수 등록 실패: \(error.localizedDescription)"
        }
    }

    private func promptForSingerName(defaultValue: String, title: String) -> String? {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = "라이브러리에 표시할 가수 이름을 입력하세요."
        alert.addButton(withTitle: "저장")
        alert.addButton(withTitle: "취소")
        let field = NSTextField(string: defaultValue)
        field.frame = NSRect(x: 0, y: 0, width: 300, height: 24)
        alert.accessoryView = field
        guard alert.runModal() == .alertFirstButtonReturn else { return nil }
        let name = field.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty else {
            status = "가수 이름을 입력해주세요."
            return nil
        }
        return name
    }

    private func loadSingerLibrary() {
        try? FileManager.default.createDirectory(at: singersRoot, withIntermediateDirectories: true)
        if let data = try? Data(contentsOf: singerLibraryURL),
           let profiles = try? JSONDecoder().decode([VocalSingerProfile].self, from: data) {
            singerProfiles = profiles.filter {
                FileManager.default.fileExists(atPath: singersRoot.appendingPathComponent($0.referenceFile).path)
            }.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
        }
        if !UserDefaults.standard.bool(forKey: restoreNaturalIdentityMigrationKey) {
            var changed = false
            for index in singerProfiles.indices where singerProfiles[index].identityStrength >= 0.57
                && singerProfiles[index].identityStrength <= 0.59 {
                singerProfiles[index].identityStrength = 0.70
                changed = true
            }
            UserDefaults.standard.set(true, forKey: restoreNaturalIdentityMigrationKey)
            if changed { saveSingerLibrary() }
        }
        if let raw = UserDefaults.standard.string(forKey: lastSingerKey),
           let id = UUID(uuidString: raw),
           let profile = singerProfiles.first(where: { $0.id == id }) {
            selectSinger(profile)
        }
    }

    private func sortAndSaveSingerLibrary() {
        singerProfiles.sort { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
        saveSingerLibrary()
    }

    private func saveSingerLibrary() {
        do {
            try FileManager.default.createDirectory(at: singersRoot, withIntermediateDirectories: true)
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
            try encoder.encode(singerProfiles).write(to: singerLibraryURL, options: .atomic)
        } catch {
            status = "가수 라이브러리 저장 실패: \(error.localizedDescription)"
        }
    }

    private func chooseAudio(title: String, completion: (URL) -> Void) {
        let panel = NSOpenPanel()
        panel.title = title
        panel.allowedContentTypes = [.wav, .aiff, .mp3, .mpeg4Audio, .audio]
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        completion(url)
    }

    func installRuntime() {
        guard !isBusy else { return }
        guard let script = Bundle.main.url(forResource: "vocal_converter_setup", withExtension: "sh") else {
            status = "설치 도구를 앱에서 찾지 못했습니다."
            return
        }
        try? FileManager.default.createDirectory(at: supportRoot, withIntermediateDirectories: true)
        startProcess(
            executable: URL(fileURLWithPath: "/bin/zsh"),
            arguments: [script.path, supportRoot.path],
            workingDirectory: supportRoot,
            busyStatus: "Seed-VC 환경 설치 중 · 최초 한 번만 필요합니다…"
        ) { [weak self] success in
            self?.refreshRuntimeStatus()
            self?.status = success
                ? "설치 완료 · 첫 변환 때 AI 모델을 자동으로 내려받습니다."
                : "설치에 실패했습니다. 아래 로그를 확인해주세요."
        }
    }

    func convert() {
        guard !isBusy, let sourceURL, let referenceURL else { return }
        guard runtimeReady else {
            status = "먼저 Seed-VC 엔진을 설치해주세요."
            return
        }

        let resultFolder = FileManager.default.urls(for: .musicDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Neuracoust Vocal Converter", isDirectory: true)
        let tempFolder = supportRoot.appendingPathComponent("Renders/\(UUID().uuidString)", isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: resultFolder, withIntermediateDirectories: true)
            try FileManager.default.createDirectory(at: tempFolder, withIntermediateDirectories: true)
        } catch {
            status = "출력 폴더를 만들 수 없습니다: \(error.localizedDescription)"
            return
        }

        let base = sourceURL.deletingPathExtension().lastPathComponent
        var destination = resultFolder.appendingPathComponent("\(base) — Converted.wav")
        var suffix = 2
        while FileManager.default.fileExists(atPath: destination.path) {
            destination = resultFolder.appendingPathComponent("\(base) — Converted \(suffix).wav")
            suffix += 1
        }

        let inference = repositoryURL.appendingPathComponent("inference.py")
        guard let pipeline = Bundle.main.url(forResource: "vocal_converter_pipeline", withExtension: "py") else {
            status = "고품질 변환 도구를 앱에서 찾지 못했습니다."
            return
        }
        var conversionArguments = [
            pipeline.path,
            "--python", runtimePythonURL.path,
            "--inference", inference.path,
            "--source", sourceURL.path,
            "--target", referenceURL.path,
            "--output", destination.path,
            "--diffusion-steps", "\(quality.steps)",
            "--identity", String(format: "%.2f", identityStrength),
            "--semitone-shift", "\(semitoneShift)"
        ]
        if let singer = selectedSinger {
            for relativePath in singer.additionalReferenceFiles ?? [] {
                let url = singersRoot.appendingPathComponent(relativePath)
                if FileManager.default.fileExists(atPath: url.path) {
                    conversionArguments += ["--target", url.path]
                }
            }
            let modelFolder = singersRoot.appendingPathComponent(
                "\(singer.id.uuidString)/Model", isDirectory: true
            )
            let checkpoint = modelFolder.appendingPathComponent("ft_model.pth")
            let config = modelFolder.appendingPathComponent("config.yml")
            if FileManager.default.fileExists(atPath: checkpoint.path),
               FileManager.default.fileExists(atPath: config.path) {
                conversionArguments += [
                    "--checkpoint", checkpoint.path,
                    "--config", config.path
                ]
            }
        }
        startProcess(
            executable: runtimePythonURL,
            arguments: conversionArguments,
            workingDirectory: repositoryURL,
            busyStatus: "원본 Seed-VC 음색으로 변환 중 · \(quality.title) · \(quality.steps) steps…"
        ) { [weak self] success in
            guard let self else { return }
            defer { try? FileManager.default.removeItem(at: tempFolder) }
            guard success, FileManager.default.fileExists(atPath: destination.path) else {
                self.status = "변환 결과를 만들지 못했습니다. 아래 로그를 확인해주세요."
                return
            }
            self.resultURL = destination
            self.status = "완료 · 인공 혼합 없이 원본 Seed-VC 음색으로 변환했습니다."
            NSSound(named: "Glass")?.play()
        }
    }

    func cancel() {
        process?.terminate()
        status = "취소 요청 중…"
    }

    func play(_ url: URL?) {
        guard let url else { return }
        if playing == url {
            player?.stop()
            playing = nil
            return
        }
        do {
            player?.stop()
            let newPlayer = try AVAudioPlayer(contentsOf: url)
            newPlayer.delegate = self
            newPlayer.prepareToPlay()
            newPlayer.play()
            player = newPlayer
            playing = url
        } catch {
            status = "재생할 수 없습니다: \(error.localizedDescription)"
        }
    }

    nonisolated func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully flag: Bool) {
        Task { @MainActor [weak self] in self?.playing = nil }
    }

    func revealResult() {
        guard let resultURL else { return }
        NSWorkspace.shared.activateFileViewerSelecting([resultURL])
    }

    private func startProcess(
        executable: URL,
        arguments: [String],
        workingDirectory: URL,
        busyStatus: String,
        completion: @escaping @MainActor (Bool) -> Void
    ) {
        let task = Process()
        let pipe = Pipe()
        task.executableURL = executable
        task.arguments = arguments
        task.currentDirectoryURL = workingDirectory
        task.standardOutput = pipe
        task.standardError = pipe
        process = task
        isBusy = true
        logText = ""
        status = busyStatus

        pipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
            Task { @MainActor in
                self?.logText.append(text)
                if let last = text.split(whereSeparator: \.isNewline).last {
                    self?.status = String(last)
                }
            }
        }

        task.terminationHandler = { [weak self] finished in
            pipe.fileHandleForReading.readabilityHandler = nil
            Task { @MainActor in
                self?.process = nil
                self?.isBusy = false
                completion(finished.terminationStatus == 0)
            }
        }

        do {
            try task.run()
        } catch {
            pipe.fileHandleForReading.readabilityHandler = nil
            process = nil
            isBusy = false
            status = "실행 실패: \(error.localizedDescription)"
            completion(false)
        }
    }
}

struct VocalConverterView: View {
    @EnvironmentObject private var controller: VocalConverterController

    private let background = Color(red: 0.035, green: 0.043, blue: 0.065)
    private let panel = Color(red: 0.065, green: 0.078, blue: 0.11)
    private let accent = Color(red: 0.47, green: 0.39, blue: 0.96)
    private let teal = Color(red: 0.18, green: 0.78, blue: 0.74)

    var body: some View {
        VStack(spacing: 0) {
            header
            ScrollView {
                VStack(spacing: 18) {
                    HStack(spacing: 14) {
                        dropCard(
                            title: "1 · SUNO 보컬",
                            subtitle: "반주가 없는 보컬 스템",
                            url: controller.sourceURL,
                            icon: "waveform",
                            isReference: false
                        ) { controller.chooseSource() }
                        singerLibrary
                    }
                    if controller.selectedSinger != nil { trainingPanel }
                    settings
                    renderSection
                    if controller.resultURL != nil { audition }
                    if !controller.logText.isEmpty { logPanel }
                }
                .padding(22)
            }
            footer
        }
        .background(background)
        .foregroundStyle(.white)
    }

    private var singerLibrary: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("2 · 가수 라이브러리").font(.system(size: 13, weight: .semibold))
                    Text(controller.selectedSinger?.name ?? "등록된 가수를 선택하세요")
                        .font(.system(size: 9)).foregroundStyle(.secondary)
                }
                Spacer()
                Button { controller.registerSinger() } label: {
                    Label("가수 등록", systemImage: "plus")
                }
                .buttonStyle(.bordered).controlSize(.small)
                Menu {
                    Button("레퍼런스 보컬 추가") {
                        controller.addReferencesToSelectedSinger()
                    }
                    .disabled(controller.selectedSinger == nil)
                    Button("학습용 보컬 추가") {
                        controller.addTrainingAudio()
                    }
                    .disabled(controller.selectedSinger == nil)
                    Divider()
                    Button("현재 설정을 이 가수의 기본값으로 저장") {
                        controller.saveCurrentSettingsToSinger()
                    }
                    .disabled(controller.selectedSinger == nil)
                    Button("라이브러리 폴더 열기") { controller.revealSingerLibrary() }
                } label: {
                    Image(systemName: "ellipsis.circle")
                }
                .menuStyle(.borderlessButton).frame(width: 24)
            }

            if controller.singerProfiles.isEmpty {
                Button { controller.registerSinger() } label: {
                    VStack(spacing: 7) {
                        Image(systemName: "person.crop.circle.badge.plus")
                            .font(.system(size: 25)).foregroundStyle(accent)
                        Text("첫 가수 등록").font(.system(size: 11, weight: .semibold))
                        Text("깨끗한 참조 음성 10–30초")
                            .font(.system(size: 8.5)).foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
                .buttonStyle(.plain)
                .onDrop(of: [UTType.fileURL.identifier], isTargeted: nil) {
                    controller.acceptDropped($0, asReference: true)
                }
            } else {
                ScrollView(.horizontal) {
                    HStack(spacing: 9) {
                        ForEach(controller.singerProfiles) { singer in
                            singerCard(singer)
                        }
                        Button { controller.registerSinger() } label: {
                            VStack(spacing: 6) {
                                Image(systemName: "plus").font(.system(size: 18, weight: .medium))
                                Text("추가").font(.system(size: 9, weight: .medium))
                            }
                            .foregroundStyle(.secondary)
                            .frame(width: 76, height: 82)
                            .background(RoundedRectangle(cornerRadius: 11).fill(background.opacity(0.65)))
                        }
                        .buttonStyle(.plain)
                    }
                }
                .scrollIndicators(.never)
                .onDrop(of: [UTType.fileURL.identifier], isTargeted: nil) {
                    controller.acceptDropped($0, asReference: true)
                }
            }
        }
        .padding(15)
        .frame(maxWidth: .infinity)
        .frame(height: 150)
        .background(RoundedRectangle(cornerRadius: 14).fill(panel))
        .overlay(RoundedRectangle(cornerRadius: 14)
            .stroke(controller.selectedSinger == nil ? accent.opacity(0.4) : teal.opacity(0.55), lineWidth: 1))
    }

    private func singerCard(_ singer: VocalSingerProfile) -> some View {
        let selected = controller.selectedSingerID == singer.id
        return Button { controller.selectSinger(singer) } label: {
            VStack(spacing: 6) {
                ZStack {
                    Circle().fill(selected ? teal.opacity(0.22) : accent.opacity(0.18))
                    Image(systemName: selected ? "checkmark" : "person.wave.2")
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(selected ? teal : accent)
                }
                .frame(width: 34, height: 34)
                Text(singer.name)
                    .font(.system(size: 9.5, weight: selected ? .bold : .medium))
                    .foregroundStyle(selected ? .white : .secondary)
                    .lineLimit(1)
                Text("\(Int(singer.identityStrength * 100))% · \(singer.semitoneShift > 0 ? "+" : "")\(singer.semitoneShift)st")
                    .font(.system(size: 7.5, design: .monospaced))
                    .foregroundStyle(.secondary)
                if selected && controller.selectedSingerHasModel {
                    Text("TRAINED").font(.system(size: 6.5, weight: .bold, design: .monospaced))
                        .foregroundStyle(teal)
                }
            }
            .frame(width: 88, height: 82)
            .background(RoundedRectangle(cornerRadius: 11)
                .fill(selected ? teal.opacity(0.12) : background.opacity(0.65)))
            .overlay(RoundedRectangle(cornerRadius: 11)
                .stroke(selected ? teal.opacity(0.75) : Color.white.opacity(0.06), lineWidth: 1))
        }
        .buttonStyle(.plain)
        .contextMenu {
            Button("레퍼런스 보컬 추가") {
                controller.selectSinger(singer)
                controller.addReferencesToSelectedSinger()
            }
            Button("학습용 보컬 추가") {
                controller.selectSinger(singer)
                controller.addTrainingAudio()
            }
            Button("이름 변경") { controller.renameSinger(singer) }
            Button("기본 설정 저장") {
                controller.selectSinger(singer)
                controller.saveCurrentSettingsToSinger()
            }
            Divider()
            Button("삭제", role: .destructive) { controller.deleteSinger(singer) }
        }
    }

    private var trainingPanel: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                VStack(alignment: .leading, spacing: 3) {
                    Text("가수 전용 음색 학습").font(.system(size: 12, weight: .bold))
                    Text("깨끗한 단독 보컬을 자동 분할해 이 가수만의 Seed‑VC 모델을 만듭니다.")
                        .font(.system(size: 9)).foregroundStyle(.secondary)
                }
                Spacer()
                if controller.selectedSingerHasModel {
                    Label("전용 모델 사용 중", systemImage: "checkmark.seal.fill")
                        .font(.system(size: 9, weight: .semibold)).foregroundStyle(teal)
                }
            }
            HStack(spacing: 10) {
                Button {
                    controller.addReferencesToSelectedSinger()
                } label: {
                    Label("레퍼런스 추가", systemImage: "person.2.wave.2")
                }
                .buttonStyle(.bordered)
                Button {
                    controller.addTrainingAudio()
                } label: {
                    Label("학습 보컬 추가", systemImage: "waveform.badge.plus")
                }
                .buttonStyle(.bordered)
                Text("레퍼런스 \(controller.selectedSingerReferenceCount)개 · 학습 파일 \(controller.trainingSourceCount)개")
                    .font(.system(size: 9, design: .monospaced)).foregroundStyle(.secondary)
                Spacer()
                Picker("학습량", selection: $controller.trainingSteps) {
                    Text("빠른 100").tag(100)
                    Text("권장 300").tag(300)
                    Text("정밀 1000").tag(1000)
                }
                .pickerStyle(.menu)
                .frame(width: 115)
                Button {
                    controller.trainSelectedSinger()
                } label: {
                    Label(controller.selectedSingerHasModel ? "다시 학습" : "전용 모델 학습",
                          systemImage: "brain.head.profile")
                }
                .buttonStyle(.borderedProminent)
                .tint(accent)
                .disabled(controller.isBusy)
            }
            Text("여러 레퍼런스는 변환 시 음색 프롬프트로 혼합되며, 전용 학습에도 함께 사용됩니다.")
                .font(.system(size: 8.5)).foregroundStyle(.secondary)
        }
        .padding(15)
        .background(RoundedRectangle(cornerRadius: 14).fill(panel))
    }

    private var header: some View {
        HStack(spacing: 12) {
            ZStack {
                RoundedRectangle(cornerRadius: 10).fill(accent.opacity(0.22))
                Image(systemName: "waveform.and.mic").font(.system(size: 22, weight: .semibold)).foregroundStyle(accent)
            }
            .frame(width: 44, height: 44)
            VStack(alignment: .leading, spacing: 3) {
                Text("NEURACOUST VOCAL CONVERTER").font(.system(size: 14, weight: .bold, design: .rounded))
                Text("Seed-VC · Singing Voice Conversion · Apple Silicon").font(.system(size: 10, design: .monospaced)).foregroundStyle(.secondary)
            }
            Spacer()
            HStack(spacing: 7) {
                Circle().fill(controller.runtimeReady ? Color.green : Color.orange).frame(width: 7, height: 7)
                Text(controller.runtimeReady ? "엔진 준비됨" : "엔진 설치 필요")
                    .font(.system(size: 10, weight: .medium))
            }
        }
        .padding(.horizontal, 22)
        .frame(height: 70)
        .background(panel)
    }

    private func dropCard(
        title: String, subtitle: String, url: URL?, icon: String,
        isReference: Bool, choose: @escaping () -> Void
    ) -> some View {
        Button(action: choose) {
            VStack(spacing: 12) {
                Image(systemName: url == nil ? icon : "checkmark.circle.fill")
                    .font(.system(size: 28, weight: .medium))
                    .foregroundStyle(url == nil ? accent : teal)
                VStack(spacing: 5) {
                    Text(title).font(.system(size: 13, weight: .semibold))
                    Text(url?.lastPathComponent ?? subtitle)
                        .font(.system(size: 10))
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
                Text(url == nil ? "클릭하거나 파일을 드롭하세요" : "클릭해서 변경")
                    .font(.system(size: 9, design: .monospaced))
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity)
            .frame(height: 150)
            .background(RoundedRectangle(cornerRadius: 14).fill(panel))
            .overlay(RoundedRectangle(cornerRadius: 14).stroke(url == nil ? accent.opacity(0.45) : teal.opacity(0.55), style: StrokeStyle(lineWidth: 1, dash: [6, 5])))
        }
        .buttonStyle(.plain)
        .onDrop(of: [UTType.fileURL.identifier], isTargeted: nil) {
            controller.acceptDropped($0, asReference: isReference)
        }
    }

    private var settings: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("변환 설정").font(.system(size: 12, weight: .bold))
            HStack(spacing: 18) {
                VStack(alignment: .leading, spacing: 6) {
                    Text("품질").font(.caption).foregroundStyle(.secondary)
                    Picker("품질", selection: $controller.quality) {
                        ForEach(VocalConverterController.Quality.allCases) { Text($0.title).tag($0) }
                    }
                    .pickerStyle(.segmented)
                }
                VStack(alignment: .leading, spacing: 6) {
                    Text("음역 이동 · \(controller.semitoneShift > 0 ? "+" : "")\(controller.semitoneShift) st")
                        .font(.caption).foregroundStyle(.secondary)
                    Stepper("", value: $controller.semitoneShift, in: -12...12).labelsHidden()
                }
            }
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text("새 가수 정체성").font(.caption).foregroundStyle(.secondary)
                    Spacer()
                    Text("\(Int(controller.identityStrength * 100))%").font(.caption.monospacedDigit()).foregroundStyle(teal)
                }
                Slider(value: $controller.identityStrength, in: 0.45...0.90, step: 0.01).tint(accent)
                Text("높을수록 참조 음색이 강해지지만 고음과 빠른 자음에서 인공적인 흔적이 늘 수 있습니다.")
                    .font(.system(size: 9)).foregroundStyle(.secondary)
            }
        }
        .padding(17)
        .background(RoundedRectangle(cornerRadius: 14).fill(panel))
    }

    private var renderSection: some View {
        VStack(spacing: 12) {
            HStack {
                if !controller.runtimeReady {
                    Button {
                        controller.installRuntime()
                    } label: {
                        Label("Seed-VC 엔진 한 번 설치", systemImage: "arrow.down.circle")
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(accent)
                    .disabled(controller.isBusy)
                } else {
                    Button {
                        controller.convert()
                    } label: {
                        Label("새 보컬 렌더링", systemImage: "sparkles")
                            .frame(minWidth: 190)
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(accent)
                    .disabled(controller.isBusy || controller.sourceURL == nil || controller.referenceURL == nil)
                }
                if controller.isBusy {
                    ProgressView().controlSize(.small)
                    Button("취소") { controller.cancel() }.buttonStyle(.bordered)
                }
                Spacer()
            }
            HStack {
                Image(systemName: controller.isBusy ? "gearshape.2" : "info.circle")
                    .foregroundStyle(controller.isBusy ? accent : .secondary)
                Text(controller.status).font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(2)
                Spacer()
            }
        }
        .padding(17)
        .background(RoundedRectangle(cornerRadius: 14).fill(panel))
    }

    private var audition: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("A/B 확인").font(.system(size: 12, weight: .bold))
            HStack(spacing: 10) {
                playButton("A · 원본 SUNO 보컬", url: controller.sourceURL, color: .gray)
                playButton("B · 새 보컬", url: controller.resultURL, color: teal)
                Spacer()
                Button("Finder에서 보기") { controller.revealResult() }
                    .buttonStyle(.bordered)
            }
        }
        .padding(17)
        .background(RoundedRectangle(cornerRadius: 14).fill(panel))
    }

    private func playButton(_ title: String, url: URL?, color: Color) -> some View {
        Button {
            controller.play(url)
        } label: {
            Label(title, systemImage: controller.playing == url ? "stop.fill" : "play.fill")
        }
        .buttonStyle(.borderedProminent)
        .tint(color)
        .disabled(url == nil)
    }

    private var logPanel: some View {
        DisclosureGroup("설치·렌더링 로그") {
            ScrollView {
                Text(controller.logText)
                    .font(.system(size: 9, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.top, 8)
            }
            .frame(maxHeight: 150)
        }
        .font(.system(size: 10))
        .padding(14)
        .background(RoundedRectangle(cornerRadius: 12).fill(panel))
    }

    private var footer: some View {
        HStack {
            Text("모든 음성은 이 Mac에서 처리됩니다. 권리가 있는 음성만 사용하세요.")
            Spacer()
            Text("v\(Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "DEV")  ·  (C) 2026 Neuracoust")
        }
        .font(.system(size: 8.5, design: .monospaced))
        .foregroundStyle(.secondary)
        .padding(.horizontal, 20)
        .frame(height: 34)
        .background(panel)
    }
}
