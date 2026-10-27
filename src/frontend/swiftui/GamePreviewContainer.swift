import SwiftUI

protocol GamePreview: View {
    init(game: Game)
}

struct GamePreviewContainer<T: GamePreview>: View {
    @EnvironmentObject var globalState: GlobalState

    let game: Game

    @State private var showAlert = false
    @State private var alertMessage = ""
    @State private var showOnlineSettings = false

    var body: some View {
        ClickableListItem(onClick: {
            globalState.activeGame = self.game
        }) {
            T(game: self.game)
        }
        .contextMenu {
            Button {
                globalState.activeGame = self.game
            } label: {
                Label("Play", systemImage: "play")
            }

            Divider()

            Menu("Extract...") {
                GameExtractButton(
                    game: self.game, content: .icon, showAlert: self.$showAlert,
                    alertMessage: self.$alertMessage)
                GameExtractButton(
                    game: self.game, content: .exefs, showAlert: self.$showAlert,
                    alertMessage: self.$alertMessage)
                GameExtractButton(
                    game: self.game, content: .romfs, showAlert: self.$showAlert,
                    alertMessage: self.$alertMessage)
            }
            .menuOrder(.fixed)

            Button {
                self.showOnlineSettings = true
            } label: {
                Label("Configure Online...", systemImage: "network")
            }
        }
        .alert(self.alertMessage, isPresented: self.$showAlert) {
            Button("OK", role: .cancel) {}
        }
        .sheet(isPresented: self.$showOnlineSettings) {
            GameOnlineSettingsView(game: self.game, isPresented: self.$showOnlineSettings)
        }
    }
}
