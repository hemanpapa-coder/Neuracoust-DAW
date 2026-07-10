import AppKit
import Darwin
import Foundation
import SwiftUI

struct ExternalDisk: Identifiable, Hashable {
    let id: String
    let title: String
}

private let supportedUsbMinBytes: Int64 = 4_000_000_000
private let supportedUsbMaxBytes: Int64 = 32_000_000_000

@main
struct NeuracoustDspUsbMakerApp: App {
    init() {
        Self.relaunchFromLocalCopyIfNeeded()
    }

    var body: some Scene {
        WindowGroup {
            UsbMakerView()
                .frame(minWidth: 720, minHeight: 520)
        }
    }

    private static func relaunchFromLocalCopyIfNeeded() {
        let fileManager = FileManager.default
        let currentBundle = Bundle.main.bundleURL
        let currentPath = currentBundle.path
        guard currentPath.hasPrefix("/Volumes/") else { return }

        let home = fileManager.homeDirectoryForCurrentUser
        let installDir = home
            .appendingPathComponent("Applications", isDirectory: true)
            .appendingPathComponent("Neuracoust", isDirectory: true)
        let targetBundle = installDir.appendingPathComponent("Neuracoust DSP USB Maker.app", isDirectory: true)

        do {
            try fileManager.createDirectory(at: installDir, withIntermediateDirectories: true)
            if fileManager.fileExists(atPath: targetBundle.path) {
                try fileManager.removeItem(at: targetBundle)
            }
            try fileManager.copyItem(at: currentBundle, to: targetBundle)

            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/open")
            process.arguments = ["-n", targetBundle.path]
            try process.run()
            exit(0)
        } catch {
            NSLog("Neuracoust DSP USB Maker could not relaunch from a local copy: \(error)")
        }
    }
}

struct UsbMakerView: View {
    @State private var mode = 0
    @State private var imagePath = ""
    @State private var disks: [ExternalDisk] = []
    @State private var selectedDisk = ""
    @State private var windowsHost = "windows11-server"
    @State private var windowsDiskNumber = ""
    @State private var confirmedErase = false
    @State private var isRunning = false
    @State private var status = "대기 중"
    @State private var logText = ""
    @State private var lastLogPath = ""
    @State private var progressValue = 0.0
    @State private var progressLabel = "대기 중"

    private var version: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "260703.0000"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                VStack(alignment: .leading, spacing: 3) {
                    Text("Neuracoust DSP Server USB Maker")
                        .font(.system(size: 22, weight: .semibold))
                    Text(status)
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Picker("", selection: $mode) {
                    Text("Mac USB").tag(0)
                    Text("Windows SSH").tag(1)
                }
                .pickerStyle(.segmented)
                .frame(width: 240)
            }

            HStack(spacing: 8) {
                TextField("Neuracoust DSP Server appliance image (.img)", text: $imagePath)
                Button("최신 이미지") {
                    loadLatestImage(showLog: true)
                }
                Button("이미지 선택") {
                    chooseImage()
                }
            }

            if mode == 0 {
                HStack(spacing: 8) {
                    Picker("USB 디스크", selection: $selectedDisk) {
                        Text("선택 안 됨").tag("")
                        ForEach(disks) { disk in
                            Text(disk.title).tag(disk.id)
                        }
                    }
                    Button("새로고침") {
                        refreshDisks()
                    }
                }
            } else {
                HStack(spacing: 8) {
                    TextField("Windows SSH 호스트", text: $windowsHost)
                    TextField("USB Disk Number", text: $windowsDiskNumber)
                        .frame(width: 150)
                }
            }

            Toggle("선택한 USB를 지우는 작업임을 확인합니다.", isOn: $confirmedErase)

            VStack(alignment: .leading, spacing: 6) {
                ProgressView(value: progressValue, total: 1.0)
                    .progressViewStyle(.linear)
                Text(progressLabel)
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }

            HStack {
                Button(isRunning ? "작업 중" : "USB 만들기") {
                    startWrite()
                }
                .disabled(isRunning || !confirmedErase || imagePath.isEmpty || (mode == 0 && selectedDisk.isEmpty) || (mode == 1 && (windowsHost.isEmpty || windowsDiskNumber.isEmpty)))

                Button("로그 지우기") {
                    logText = ""
                }
                .disabled(isRunning)

                Button("로그 열기") {
                    openLastLog()
                }
                .disabled(lastLogPath.isEmpty)

                Button("로그 복사") {
                    copyCurrentLog()
                }
                .disabled(logText.isEmpty && lastLogPath.isEmpty)

                Spacer()
                Text("v\(version)  (C) 2026 Neuracoust")
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
            }

            ScrollView {
                Text(logText.isEmpty ? "작업 로그가 여기에 표시됩니다." : logText)
                    .font(.system(.body, design: .monospaced))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
                    .padding(10)
            }
            .background(Color(nsColor: .textBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 6))
        }
        .padding(18)
        .onAppear {
            loadLatestImage(showLog: false)
            refreshDisks()
        }
        .onReceive(Timer.publish(every: 1.0, on: .main, in: .common).autoconnect()) { _ in
            refreshProgressFromLog()
        }
    }

    private func loadLatestImage(showLog: Bool) {
        if let latest = Self.findLatestApplianceImage() {
            imagePath = latest
            status = "최신 DSP 서버 이미지 선택됨"
            if showLog {
                appendLog("Latest appliance image: \(latest)")
            }
        } else if showLog {
            appendLog("No latest DSP server appliance image was found. Use 이미지 선택 after building the .img/.iso.")
        }
    }

    private func chooseImage() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url {
            imagePath = url.path
        }
    }

    private func refreshDisks() {
        status = "USB 디스크 확인 중"
        DispatchQueue.global(qos: .userInitiated).async {
            let found = Self.loadExternalDisks()
            DispatchQueue.main.async {
                disks = found
                if !found.contains(where: { $0.id == selectedDisk }) {
                    selectedDisk = found.first?.id ?? ""
                }
                status = found.isEmpty ? "지원 USB 없음 (4GB-32GB)" : "지원 USB \(found.count)개 확인"
            }
        }
    }

    private func startWrite() {
        isRunning = true
        status = "USB 제작 시작"
        lastLogPath = Self.makeLogPath()
        progressValue = 0
        progressLabel = "준비 중"
        appendLog("Starting Neuracoust DSP Server USB write")
        appendLog("Detailed log: \(lastLogPath)")

        DispatchQueue.global(qos: .userInitiated).async {
            let result: String
            if mode == 0 {
                result = runMacWriter()
            } else {
                result = runWindowsWriter()
            }
            DispatchQueue.main.async {
                appendLog(result)
                status = result.contains("complete") || result.contains("완료") ? "완료" : "확인 필요"
                refreshProgressFromLog()
                if status == "완료" {
                    progressValue = 1
                    progressLabel = "완료 - USB를 빼도 됩니다"
                } else if progressValue < 1 {
                    progressLabel = "실패 또는 확인 필요"
                }
                isRunning = false
            }
        }
    }

    private func runMacWriter() -> String {
        guard let resources = Bundle.main.resourcePath else { return "Resource path not found." }
        let script = "\(resources)/scripts/make-dsp-server-usb-macos.command"
        let logPath = lastLogPath.isEmpty ? Self.makeLogPath() : lastLogPath
        let command = "cd \(Self.shellQuote(resources)) && NEURACOUST_CONFIRM_ERASE=ERASE NEURACOUST_USB_LOG=\(Self.shellQuote(logPath)) \(Self.shellQuote(script)) --disk \(Self.shellQuote(selectedDisk)) --image \(Self.shellQuote(imagePath))"
        let appleScript = "do shell script \(Self.appleScriptString(command)) with administrator privileges"
        var error: NSDictionary?
        let output = NSAppleScript(source: appleScript)?.executeAndReturnError(&error).stringValue ?? ""
        if let error {
            return "Mac USB write failed: \(error)\nDetailed log: \(logPath)\n\(Self.tailFile(logPath, maxBytes: 20000))"
        }
        if let partition = Self.copyFallbackPartition(from: output) {
            return Self.runUserCopyFallback(partition: partition, imagePath: imagePath, logPath: logPath)
        }
        let body = output.isEmpty ? "Mac USB write complete." : output
        return "\(body)\nDetailed log: \(logPath)"
    }

    private func runWindowsWriter() -> String {
        guard let resources = Bundle.main.resourcePath else { return "Resource path not found." }
        let script = "\(resources)/scripts/make-dsp-server-usb-windows-remote.command"
        return Self.runProcess(
            executable: "/bin/zsh",
            arguments: [
                script,
                "--host", windowsHost,
                "--disk-number", windowsDiskNumber,
                "--image", imagePath
            ],
            currentDirectory: resources,
            environment: ["NEURACOUST_CONFIRM_ERASE": "ERASE"]
        )
    }

    private func appendLog(_ text: String) {
        if !logText.isEmpty {
            logText += "\n"
        }
        logText += text
    }

    private func openLastLog() {
        guard !lastLogPath.isEmpty else { return }
        NSWorkspace.shared.open(URL(fileURLWithPath: lastLogPath))
    }

    private func copyCurrentLog() {
        var text = logText
        if !lastLogPath.isEmpty,
           let fileText = try? String(contentsOfFile: lastLogPath, encoding: .utf8),
           !fileText.isEmpty {
            text += text.isEmpty ? fileText : "\n\n----- Detailed log file -----\n\(fileText)"
        }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
        status = "로그 복사됨"
    }

    private func refreshProgressFromLog() {
        guard !lastLogPath.isEmpty,
              let fileText = try? String(contentsOfFile: lastLogPath, encoding: .utf8),
              !fileText.isEmpty else {
            return
        }
        let parsed = Self.parseProgress(from: fileText)
        if parsed.totalBytes > 0 {
            var value = min(max(Double(parsed.writtenBytes) / Double(parsed.totalBytes), 0), 1)
            if fileText.contains("--- app/user file-copy fallback ---"),
               !fileText.contains("Neuracoust DSP Server USB file-copy fallback complete and ejected.") {
                value = min(value, 0.95)
            }
            progressValue = value
        }
        if let message = parsed.message {
            progressLabel = message
        } else if isRunning {
            progressLabel = "작업 진행 중"
        }
    }

    private static func loadExternalDisks() -> [ExternalDisk] {
        let output = runProcess(executable: "/usr/sbin/diskutil", arguments: ["list", "-plist", "external", "physical"])
        guard let data = output.data(using: .utf8),
              let plist = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil),
              let dict = plist as? [String: Any],
              let diskIds = dict["WholeDisks"] as? [String] else {
            return []
        }

        return diskIds.compactMap { diskId in
            let path = "/dev/\(diskId)"
            let info = runProcess(executable: "/usr/sbin/diskutil", arguments: ["info", "-plist", path])
            guard let data = info.data(using: .utf8),
                  let plist = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil),
                  let dict = plist as? [String: Any] else {
                return ExternalDisk(id: path, title: path)
            }
            let internalFlag = plistBool(dict["Internal"], defaultValue: true)
            let osInternal = plistBool(dict["OSInternalMedia"], defaultValue: false)
            let systemImage = plistBool(dict["SystemImage"], defaultValue: false)
            let wholeDisk = plistBool(dict["WholeDisk"], defaultValue: false)
            let writable = plistBool(dict["WritableMedia"], defaultValue: false)
            let busProtocol = dict["BusProtocol"] as? String ?? ""
            if internalFlag || osInternal || systemImage { return nil }
            if !wholeDisk || !writable { return nil }
            if busProtocol != "USB" { return nil }
            let name = dict["MediaName"] as? String ?? diskId
            let size = plistInt64(dict["TotalSize"])
            if size < supportedUsbMinBytes || size > supportedUsbMaxBytes { return nil }
            let gb = Double(size) / 1_000_000_000.0
            return ExternalDisk(id: path, title: "\(path)  \(name)  \(String(format: "%.1f", gb)) GB")
        }
    }

    private static func plistBool(_ value: Any?, defaultValue: Bool) -> Bool {
        if let boolValue = value as? Bool {
            return boolValue
        }
        if let numberValue = value as? NSNumber {
            return numberValue.boolValue
        }
        return defaultValue
    }

    private static func plistInt64(_ value: Any?) -> Int64 {
        if let int64Value = value as? Int64 {
            return int64Value
        }
        if let intValue = value as? Int {
            return Int64(intValue)
        }
        if let numberValue = value as? NSNumber {
            return numberValue.int64Value
        }
        return 0
    }

    private static func findLatestApplianceImage() -> String? {
        let fileManager = FileManager.default
        var latestRefs: [String] = []

        if let resources = Bundle.main.resourcePath {
            latestRefs.append("\(resources)/appliance-images/latest-dsp-server-appliance-image.txt")
        }
        latestRefs.append("/Volumes/Program Dev/DAW/dist/latest-dsp-server-appliance-image.txt")

        for ref in latestRefs {
            guard let text = try? String(contentsOfFile: ref, encoding: .utf8) else { continue }
            let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !trimmed.isEmpty else { continue }
            if trimmed.hasPrefix("/") && fileManager.fileExists(atPath: trimmed) {
                return trimmed
            }
            let base = URL(fileURLWithPath: ref).deletingLastPathComponent().path
            let relativePath = "\(base)/\(trimmed)"
            if fileManager.fileExists(atPath: relativePath) {
                return relativePath
            }
        }

        let searchDirs = [
            Bundle.main.resourcePath.map { "\($0)/appliance-images" },
            Optional("/Volumes/Program Dev/DAW/dist")
        ].compactMap { $0 }

        var candidates: [(path: String, date: Date)] = []
        for dir in searchDirs {
            guard let urls = try? fileManager.contentsOfDirectory(
                at: URL(fileURLWithPath: dir),
                includingPropertiesForKeys: [.contentModificationDateKey],
                options: [.skipsHiddenFiles]
            ) else { continue }
            for url in urls {
                let lower = url.lastPathComponent.lowercased()
                let looksLikeImage = lower.hasSuffix(".img") || lower.hasSuffix(".iso")
                let looksLikeDsp = lower.contains("dsp") || lower.contains("appliance") || lower.contains("neuracoust")
                guard looksLikeImage && looksLikeDsp else { continue }
                let values = try? url.resourceValues(forKeys: [.contentModificationDateKey])
                candidates.append((url.path, values?.contentModificationDate ?? .distantPast))
            }
        }
        return candidates.sorted { $0.date > $1.date }.first?.path
    }

    private static func runProcess(executable: String, arguments: [String], currentDirectory: String? = nil, environment: [String: String] = [:]) -> String {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        if let currentDirectory {
            process.currentDirectoryURL = URL(fileURLWithPath: currentDirectory)
        }
        if !environment.isEmpty {
            var env = ProcessInfo.processInfo.environment
            for (key, value) in environment {
                env[key] = value
            }
            process.environment = env
        }
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        do {
            try process.run()
            process.waitUntilExit()
        } catch {
            return "Failed to run \(executable): \(error)"
        }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        return String(data: data, encoding: .utf8) ?? ""
    }

    private static func copyFallbackPartition(from output: String) -> String? {
        guard let regex = try? NSRegularExpression(pattern: #"NEURACOUST_COPY_FALLBACK_READY partition=([^ ]+)"#) else {
            return nil
        }
        let range = NSRange(output.startIndex..<output.endIndex, in: output)
        guard let match = regex.firstMatch(in: output, range: range),
              match.numberOfRanges > 1,
              let partitionRange = Range(match.range(at: 1), in: output) else {
            return nil
        }
        return String(output[partitionRange])
    }

    private static func runUserCopyFallback(partition: String, imagePath: String, logPath: String) -> String {
        let imageBytes = (try? FileManager.default.attributesOfItem(atPath: imagePath)[.size] as? NSNumber)?.int64Value ?? 0
        let command = """
        set -euo pipefail
        exec >> \(shellQuote(logPath)) 2>&1
        echo "--- app/user file-copy fallback ---"
        echo "Partition: \(partition)"
        echo "Image: \(imagePath)"
        echo "Image bytes: \(imageBytes)"
        /usr/sbin/diskutil mount \(shellQuote(partition)) || true
        mount_point=""
        for i in {1..20}; do
          mount_point="$(/usr/sbin/diskutil info -plist \(shellQuote(partition)) 2>/dev/null | /usr/bin/plutil -extract MountPoint raw - 2>/dev/null || true)"
          if [[ -n "$mount_point" && -d "$mount_point" ]]; then
            break
          fi
          /bin/sleep 1
        done
        if [[ -z "$mount_point" || ! -d "$mount_point" ]]; then
          echo "Could not mount FAT32 USB volume for user copy."
          exit 1
        fi
        echo "FAT32 USB mounted at: $mount_point"
        echo "--- mounted volume permissions ---"
        /bin/ls -ldOe "$mount_point" || true
        /sbin/mount | /usr/bin/grep "$mount_point" || true
        /bin/mkdir -p "$mount_point/.neuracoust-write-test"
        /bin/rmdir "$mount_point/.neuracoust-write-test"
        /usr/bin/touch "$mount_point/.metadata_never_index" || true
        echo "--- extract ISO contents with bsdtar as app user ---"
        (
          while true; do
            kb="$(/usr/bin/du -sk "$mount_point" 2>/dev/null | /usr/bin/awk '{print $1}' || echo 0)"
            kb="${kb//[^0-9]/}"
            if [[ -z "$kb" ]]; then
              kb=0
            fi
            copied=$(( kb * 1024 ))
            echo "COPY_PROGRESS_BYTES=$copied TOTAL_BYTES=\(imageBytes)"
            /bin/sleep 1
          done
        ) &
        monitor_pid=$!
        set +e
        /usr/bin/bsdtar -xf \(shellQuote(imagePath)) -C "$mount_point" --no-same-owner --no-same-permissions
        extract_status=$?
        kill "$monitor_pid" >/dev/null 2>&1 || true
        wait "$monitor_pid" >/dev/null 2>&1 || true
        set -e
        if [[ "$extract_status" != "0" ]]; then
          echo "bsdtar reported warnings or errors while extracting. Continuing with boot file verification."
        fi
        echo "--- repair FAT32-incompatible live symlinks ---"
        if [[ -f "$mount_point/live/vmlinuz" ]]; then
          /usr/bin/grep -Eoh '/live/vmlinuz-[^ ]+' "$mount_point/boot/grub/"*.cfg 2>/dev/null | /usr/bin/sort -u | while read -r kernel_ref; do
            kernel_path="$mount_point${kernel_ref}"
            if [[ ! -f "$kernel_path" ]]; then
              echo "Creating FAT32 kernel copy: $kernel_ref"
              /bin/cp "$mount_point/live/vmlinuz" "$kernel_path"
            fi
          done
        fi
        if [[ -f "$mount_point/live/initrd.img" ]]; then
          /usr/bin/grep -Eoh '/live/initrd\\.img-[^ ]+' "$mount_point/boot/grub/"*.cfg 2>/dev/null | /usr/bin/sort -u | while read -r initrd_ref; do
            initrd_path="$mount_point${initrd_ref}"
            if [[ ! -f "$initrd_path" ]]; then
              echo "Creating FAT32 initrd copy: $initrd_ref"
              /bin/cp "$mount_point/live/initrd.img" "$initrd_path"
            fi
          done
        fi
        echo "--- verify copied boot files ---"
        /bin/ls -la "$mount_point/EFI/boot" "$mount_point/boot/grub" "$mount_point/live" 2>/dev/null || true
        if [[ ! -f "$mount_point/EFI/boot/bootx64.efi" && ! -f "$mount_point/EFI/BOOT/BOOTX64.EFI" ]]; then
          echo "Copied USB is missing EFI boot loader files."
          exit 1
        fi
        if [[ ! -f "$mount_point/live/filesystem.squashfs" ]]; then
          echo "Copied USB is missing Debian live filesystem.squashfs."
          exit 1
        fi
        if [[ ! -f "$mount_point/live/vmlinuz" || ! -f "$mount_point/live/initrd.img" ]]; then
          echo "Copied USB is missing Debian live kernel or initrd."
          exit 1
        fi
        missing_refs=0
        while read -r live_ref; do
          [[ -n "$live_ref" ]] || continue
          if [[ ! -f "$mount_point${live_ref}" ]]; then
            echo "Copied USB is missing GRUB referenced live file: $live_ref"
            missing_refs=1
          fi
        done < <(/usr/bin/grep -Eoh '/live/(vmlinuz|initrd\\.img)-[^ ]+' "$mount_point/boot/grub/"*.cfg 2>/dev/null | /usr/bin/sort -u)
        if [[ "$missing_refs" != "0" ]]; then
          exit 1
        fi
        echo "COPY_PROGRESS_BYTES=\(imageBytes) TOTAL_BYTES=\(imageBytes)"
        echo "--- eject copied USB ---"
        /usr/bin/mdutil -i off "$mount_point" >/dev/null 2>&1 || true
        /usr/bin/mdutil -E "$mount_point" >/dev/null 2>&1 || true
        parent_disk="$(/usr/sbin/diskutil info -plist \(shellQuote(partition)) 2>/dev/null | /usr/bin/plutil -extract ParentWholeDisk raw - 2>/dev/null || true)"
        if [[ -n "$parent_disk" ]]; then
          /usr/sbin/diskutil eject "/dev/$parent_disk" || {
            echo "Initial eject failed. Retrying forced unmount/eject for /dev/$parent_disk..."
            /usr/sbin/diskutil unmountDisk force "/dev/$parent_disk" || true
            /bin/sleep 1
            /usr/sbin/diskutil eject "/dev/$parent_disk"
          }
        else
          fallback_disk=\(shellQuote(partition.replacingOccurrences(of: #"s[0-9]+$"#, with: "", options: .regularExpression)))
          /usr/sbin/diskutil eject \(shellQuote(partition)) || {
            echo "Initial partition eject failed. Retrying forced unmount/eject for $fallback_disk..."
            /usr/sbin/diskutil unmountDisk force "$fallback_disk" || true
            /bin/sleep 1
            /usr/sbin/diskutil eject "$fallback_disk"
          }
        fi
        echo "Neuracoust DSP Server USB file-copy fallback complete and ejected."
        """
        let output = runProcess(executable: "/bin/zsh", arguments: ["-c", command])
        let logTail = tailFile(logPath, maxBytes: 30000)
        return "\(output)\nDetailed log: \(logPath)\n\(logTail)"
    }

    private static func makeLogPath() -> String {
        let fileManager = FileManager.default
        let logDir = fileManager.homeDirectoryForCurrentUser
            .appendingPathComponent("Library", isDirectory: true)
            .appendingPathComponent("Logs", isDirectory: true)
            .appendingPathComponent("Neuracoust", isDirectory: true)
            .appendingPathComponent("DSPUSBMaker", isDirectory: true)
        try? fileManager.createDirectory(at: logDir, withIntermediateDirectories: true)
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        return logDir.appendingPathComponent("write-\(formatter.string(from: Date())).log").path
    }

    private static func tailFile(_ path: String, maxBytes: Int) -> String {
        guard let handle = FileHandle(forReadingAtPath: path) else { return "Detailed log could not be opened yet." }
        defer { try? handle.close() }
        let size = (try? handle.seekToEnd()) ?? 0
        let offset = size > UInt64(maxBytes) ? size - UInt64(maxBytes) : 0
        try? handle.seek(toOffset: offset)
        let data = handle.readDataToEndOfFile()
        return String(data: data, encoding: .utf8) ?? ""
    }

    private static func parseProgress(from log: String) -> (writtenBytes: Int64, totalBytes: Int64, message: String?) {
        var totalBytes: Int64 = 0
        if let range = log.range(of: #"Image bytes: ([0-9]+)"#, options: .regularExpression) {
            let match = String(log[range])
            totalBytes = Int64(match.replacingOccurrences(of: "Image bytes: ", with: "")) ?? 0
        }

        var writtenBytes: Int64 = 0
        if let regex = try? NSRegularExpression(pattern: #"COPY_PROGRESS_BYTES=([0-9]+) TOTAL_BYTES=([0-9]+)"#) {
            let nsRange = NSRange(log.startIndex..<log.endIndex, in: log)
            let matches = regex.matches(in: log, range: nsRange)
            for match in matches {
                guard match.numberOfRanges > 2,
                      let writtenRange = Range(match.range(at: 1), in: log),
                      let totalRange = Range(match.range(at: 2), in: log),
                      let written = Int64(log[writtenRange]),
                      let total = Int64(log[totalRange]) else { continue }
                writtenBytes = max(writtenBytes, written)
                totalBytes = max(totalBytes, total)
            }
        }

        let patterns = [
            #"([0-9]+) bytes transferred"#,
            #"([0-9]+) bytes copied"#
        ]
        for pattern in patterns {
            if let regex = try? NSRegularExpression(pattern: pattern) {
                let nsRange = NSRange(log.startIndex..<log.endIndex, in: log)
                let matches = regex.matches(in: log, range: nsRange)
                for match in matches {
                    guard match.numberOfRanges > 1,
                          let range = Range(match.range(at: 1), in: log),
                          let value = Int64(log[range]) else { continue }
                    if value != totalBytes {
                        writtenBytes = max(writtenBytes, value)
                    }
                }
            }
        }

        let message: String?
        if log.contains("Neuracoust DSP Server USB write complete") || log.contains("Neuracoust DSP Server USB file-copy fallback complete") {
            message = "완료 - USB를 빼도 됩니다"
            writtenBytes = max(writtenBytes, totalBytes)
        } else if log.contains("Operation not permitted") {
            message = log.contains("--- extract ISO contents with bsdtar ---") ? "ISO 파일 복사 중" : "권한 오류: USB 장치 쓰기 차단"
        } else if log.contains("--- extract ISO contents with bsdtar ---") {
            message = "ISO 파일 복사 중"
        } else if log.contains("--- file-copy fallback: format USB as single MBR/FAT32 ---") || log.contains("--- file-copy fallback: format USB as FAT32 ---") {
            message = "USB 포맷 후 파일 복사 준비"
        } else if log.contains("--- write with dd using block device ---") {
            message = "block 장치로 쓰기 시도 중"
        } else if log.contains("--- write with dd ---") {
            message = "USB 이미지 쓰는 중"
        } else if log.contains("--- unmount target ---") {
            message = "USB 준비 중"
        } else if log.contains("Validated target:") {
            message = "USB 검증 완료"
        } else {
            message = nil
        }

        return (writtenBytes, totalBytes, message)
    }

    private static func shellQuote(_ value: String) -> String {
        return "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    private static func appleScriptString(_ value: String) -> String {
        return "\"" + value.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
    }
}
