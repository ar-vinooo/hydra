import SwiftUI
import UniformTypeIdentifiers

struct KeysImportButton: View {
    @EnvironmentObject var globalState: GlobalState
    @State private var showingPicker = false
    @State private var showingError = false
    @State private var errorMessage = ""

    var body: some View {
        Button("Import Keys", systemImage: "key") {
            showingPicker = true
        }
        .help("Import prod.keys to open XCI gamecards")
        .fileImporter(
            isPresented: $showingPicker,
            allowedContentTypes: [.data],
            allowsMultipleSelection: false
        ) { result in
            do {
                guard let url = try result.get().first else { return }
                let scoped = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }

                let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
                guard let size = attributes[.size] as? NSNumber, size.intValue <= 1_048_576 else {
                    throw CocoaError(.fileReadCorruptFile)
                }
                let data = try Data(contentsOf: url)
                guard let text = String(data: data, encoding: .utf8),
                    text.split(whereSeparator: \.isNewline).contains(where: { line in
                        let parts = line.split(separator: "=", maxSplits: 1)
                        guard parts.count == 2,
                            parts[0].trimmingCharacters(in: .whitespaces) == "header_key"
                        else { return false }
                        let value = (parts[1].split(separator: "#", maxSplits: 1,
                            omittingEmptySubsequences: false).first ?? "")
                            .trimmingCharacters(in: .whitespaces)
                        return value.count == 64 && value.allSatisfy(\.isHexDigit)
                    })
                else {
                    errorMessage = "Choose a prod.keys file containing a valid header_key."
                    showingError = true
                    return
                }
                let directory = URL(fileURLWithPath: configGetAppDataPath())
                    .appendingPathComponent("keys", isDirectory: true)
                try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
                let destination = directory.appendingPathComponent("prod.keys")
                try data.write(to: destination, options: .atomic)
                try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: destination.path)
                // Retry any XCI entries that could not load before keys existed.
                globalState.loaderPluginRefreshID += 1
            } catch {
                errorMessage = error.localizedDescription
                showingError = true
            }
        }
        .alert("Unable to import keys", isPresented: $showingError) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(errorMessage)
        }
    }
}
