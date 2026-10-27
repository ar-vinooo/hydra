import SwiftUI
import UniformTypeIdentifiers

struct GeneralSettingsView: View {
    @EnvironmentObject var globalState: GlobalState

    @State private var patchPaths: [String] = []

    private let switchType = UTType(exportedAs: "com.samoz256.switch-document", conformingTo: .data)
    private let xciType = UTType(filenameExtension: "xci", conformingTo: .data) ?? .data
    private let xczType = UTType(filenameExtension: "xcz", conformingTo: .data) ?? .data
    private let hatchType = UTType(exportedAs: "com.samoz256.hatch-document", conformingTo: .data)

    var body: some View {
        Spacer()
        HStack {
            Spacer()
            Form {
                Section("Game Paths") {
                    EditablePathList(
                        allowedContentTypes: [.folder, self.switchType, self.xciType, self.xczType],
                        items: $globalState.gamePaths
                    )
                    .onChange(of: globalState.gamePaths) { _, newValue in
                        let gamePathsOption = configGetGamePaths()
                        gamePathsOption.resize(to: newValue.count)
                        for i in 0..<newValue.count {
                            gamePathsOption.set(at: i, value: newValue[i])
                        }
                    }
                }

                LoaderPluginListView()

                Section("Gamecard Keys") {
                    KeysImportButton()
                    Text("Import prod.keys from your console to open XCI files.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                Section("Patch paths") {
                    EditablePathList(
                        allowedContentTypes: [.folder, self.hatchType], items: self.$patchPaths
                    )
                    .onChange(of: self.patchPaths) { _, newValue in
                        let patchPathsOption = configGetPatchPaths()
                        patchPathsOption.resize(to: newValue.count)
                        for i in 0..<newValue.count {
                            patchPathsOption.set(at: i, value: newValue[i])
                        }
                    }
                }
            }
            .onAppear {
                let patchPathsOption = configGetPatchPaths()
                self.patchPaths = []
                for i in 0..<patchPathsOption.count {
                    self.patchPaths.append(patchPathsOption.get(at: i))
                }
            }
            .formStyle(.grouped)
            Spacer()
        }
        Spacer()
    }
}
