import SwiftUI
import UniformTypeIdentifiers

private let switchType = UTType(exportedAs: "com.samoz256.switch-document", conformingTo: .data)
private let nspType = UTType(filenameExtension: "nsp", conformingTo: .data) ?? .data
private let xciType = UTType(filenameExtension: "xci", conformingTo: .data) ?? .data
private let nszType = UTType(filenameExtension: "nsz", conformingTo: .data) ?? .data
private let xczType = UTType(filenameExtension: "xcz", conformingTo: .data) ?? .data
#if os(macOS)
    private let allowedContentTypes: [UTType] = [.folder, switchType, xciType, nspType, xczType, nszType]

    private let installFirmwarePlacement = ToolbarItemPlacement.confirmationAction
    private let addGamePlacement = ToolbarItemPlacement.confirmationAction
#else
    private let allowedContentTypes: [UTType] = [switchType, xciType, nspType, xczType, nszType]  // No game folders on iOS

    private let installFirmwarePlacement = ToolbarItemPlacement.topBarLeading
    private let addGamePlacement = ToolbarItemPlacement.topBarTrailing
#endif

struct GameListToolbarItems: ToolbarContent {
    @EnvironmentObject var globalState: GlobalState

    @Binding var viewMode: ViewMode

    @State private var isFirmwareFilePickerPresented = false
    @State private var isGameFilePickerPresented = false
    @State private var showingFirmwareImportError = false
    @State private var isContentPickerPresented = false
    @State private var isInstallingContent = false
    @State private var showingContentInstallResult = false
    @State private var contentInstallSucceeded = false
    @State private var contentInstallMessage = ""

    var body: some ToolbarContent {
        #if os(macOS)
            ToolbarItemGroup(placement: .principal) {
                Button("List View", systemImage: "list.bullet") {
                    viewMode = .list
                }
                .disabled(viewMode == .list)

                Button("Grid View", systemImage: "rectangle.grid.3x2.fill") {
                    viewMode = .grid
                }
                .disabled(viewMode == .grid)
            }
        #endif

        let firmwarePath = configGetFirmwarePath()
        if firmwarePath.isEmpty {
            ToolbarItem(placement: installFirmwarePlacement) {
                Button("Install Firmware") {
                    isFirmwareFilePickerPresented = true
                }
                .fileImporter(
                    isPresented: $isFirmwareFilePickerPresented,
                    allowedContentTypes: [.folder],
                    allowsMultipleSelection: false
                ) { result in
                    switch result {
                    case .success(let fileURLs):
                        for fileURL in fileURLs {
                            guard fileURL.startAccessingSecurityScopedResource() else {
                                showingFirmwareImportError = true
                                return
                            }

                            defer { fileURL.stopAccessingSecurityScopedResource() }

                            #if os(macOS)
                                configSetFirmwarePath(fileURL.path(percentEncoded: false))
                            #else
                                let path = "\(configGetAppDataPath())/firmware"
                                do {
                                    try FileManager.default.copyItem(
                                        atPath: fileURL.path(percentEncoded: false),
                                        toPath: firmwarePath)
                                } catch {
                                    showingFirmwareImportError = true
                                }

                                configSetFirmwarePath(path)
                            #endif

                            configSerialize()
                        }
                    case .failure(let error):
                        print(error)
                    }
                }
                .alert("Failed to import firmware", isPresented: $showingFirmwareImportError) {
                    Button("OK", role: .cancel) {}
                }
            }
        }

        ToolbarItem(placement: addGamePlacement) {
            Button("Refresh games", systemImage: "arrow.clockwise") {
                globalState.loaderPluginRefreshID += 1
            }
        }

        ToolbarItem(placement: addGamePlacement) {
            KeysImportButton()
        }

        ToolbarItem(placement: addGamePlacement) {
            Button(isInstallingContent ? "Installing…" : "Install Update / DLC", systemImage: "shippingbox") {
                isContentPickerPresented = true
            }
            .disabled(isInstallingContent || globalState.system != nil)
            .help("Install an NSP or XCI containing game updates or DLC")
            .fileImporter(isPresented: $isContentPickerPresented,
                          allowedContentTypes: [nspType, xciType],
                          allowsMultipleSelection: false) { result in
                switch result {
                case .success(let urls):
                    guard let url = urls.first else { return }
                    isInstallingContent = true
                    Task {
                        let outcome = await Task.detached(priority: .userInitiated) { () -> (Bool, String) in
                            let access = url.startAccessingSecurityScopedResource()
                            defer { if access { url.stopAccessingSecurityScopedResource() } }
                            let success = url.path(percentEncoded: false).withHydraString { hydraInstallContentPackage($0) }
                            // Copy the thread-local result before leaving this synchronous call.
                            return (success, String(withHydraString: hydraContentInstallGetResult()))
                        }.value
                        contentInstallSucceeded = outcome.0
                        contentInstallMessage = outcome.1
                        isInstallingContent = false
                        showingContentInstallResult = true
                        if outcome.0 { globalState.loaderPluginRefreshID += 1 }
                    }
                case .failure(let error):
                    contentInstallSucceeded = false
                    contentInstallMessage = error.localizedDescription
                    showingContentInstallResult = true
                }
            }
            .alert(contentInstallSucceeded ? "Content installed" : "Installation failed",
                   isPresented: $showingContentInstallResult) {
                Button("OK", role: .cancel) {}
            } message: {
                Text(contentInstallMessage)
            }
        }

        ToolbarItem(placement: addGamePlacement) {
            Button("Add Game", systemImage: "plus") {
                isGameFilePickerPresented = true
            }
            .fileImporter(
                isPresented: $isGameFilePickerPresented,
                allowedContentTypes: allowedContentTypes,
                allowsMultipleSelection: true
            ) { result in
                switch result {
                case .success(let fileURLs):
                    for fileURL in fileURLs {
                        do {
                            try registerUrl(fileURL)
                        } catch {
                            print("Failed to register URL \(fileURL.path(percentEncoded: false))")
                            continue
                        }

                        // TODO: isn't there a better way?
                        globalState.gamePaths.append(fileURL.path(percentEncoded: false))
                        let gamePathsOption = configGetGamePaths()
                        gamePathsOption.append(
                            value: fileURL.path(percentEncoded: false))
                        configSerialize()
                    }
                case .failure(let error):
                    print(error)
                }
            }
        }

        #if os(iOS)
            ToolbarItem(placement: .topBarTrailing) {
                NavigationLink(destination: SettingsView()) {
                    Image(systemName: "gear")
                }
            }
        #endif
    }

}
