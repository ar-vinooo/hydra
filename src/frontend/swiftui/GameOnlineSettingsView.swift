import SwiftUI

// A title's online settings: <app data>/online/<title ID>.toml, read by the
// core when the title boots
struct GameOnlineSettings {
    var enabled = false
    var server = ""
    var httpPort = 8080
    var nickname = ""
    var redirectHosts = ["*.demonware.net"]
    var demonwareKey = ""

    // RyuNET, the server of OCTR (github.com/Ryu7w7/OCTR-NitroFueled-PC), and
    // the RSA key it signs Demonware auth replies with (its
    // patches/CrashTeamRacingOnline.xml)
    static let ryuNetServer = "octr.ryu7w7.xyz"
    static let ryuNetKey =
        "F8C16F376D12D286BCD3023E7B0657B5049122C1814D33121064736F963BB2B52BA096763D5E846ECB38020D6B711EC631E7541410D490609E633EA7BDD80F571384F059EE75096E459308423F0DD40C7392F1F95528C791FC89D6D8EA4D38962938AC09187F79E05995958B178628E4DA8A6A521F8435426ABD692E56214AFD0F8AE5BD5B6DA64E7B597F57162000AFDCB7CA52C2D57222734A79FEAC6554A8600607FA4E1B33F31F523D1C40F5642D4171988E65C12BA1979B15FAF70CE2B6B6741442B673CF872EF2A9E51FD3E9E6EB5E161B27095BB6806B3DEE4EA5D214879CA9C097814688633B1E2830E5F1A80C4FFA5BEDD387DC724922816EC13C51"

    static func url(titleId: UInt64) -> URL {
        URL(fileURLWithPath: configGetAppDataPath())
            .appendingPathComponent("online")
            .appendingPathComponent(String(format: "%016llx.toml", titleId))
    }

    static func load(titleId: UInt64) -> GameOnlineSettings {
        var settings = GameOnlineSettings()
        guard let text = try? String(contentsOf: url(titleId: titleId), encoding: .utf8) else {
            return settings
        }

        for line in text.split(whereSeparator: \.isNewline) {
            let parts = line.split(separator: "=", maxSplits: 1)
            guard parts.count == 2 else { continue }
            let key = parts[0].trimmingCharacters(in: .whitespaces)
            let value = parts[1].trimmingCharacters(in: .whitespaces)
            switch key {
            case "enabled":
                settings.enabled = value == "true"
            case "server":
                settings.server = unquote(value)
            case "http_port":
                settings.httpPort = Int(value) ?? 8080
            case "nickname":
                settings.nickname = unquote(value)
            case "redirect_hosts":
                settings.redirectHosts =
                    value
                    .trimmingCharacters(in: CharacterSet(charactersIn: "[]"))
                    .split(separator: ",")
                    .map { unquote($0.trimmingCharacters(in: .whitespaces)) }
                    .filter { !$0.isEmpty }
            case "demonware_key":
                settings.demonwareKey = unquote(value)
            default:
                break
            }
        }

        return settings
    }

    func save(titleId: UInt64) throws {
        let url = Self.url(titleId: titleId)
        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(), withIntermediateDirectories: true)

        let hosts = redirectHosts.map { quote($0) }.joined(separator: ", ")
        let text = """
            # Online settings, read when the title boots
            enabled = \(enabled)
            server = \(quote(server))
            http_port = \(httpPort)
            nickname = \(quote(nickname))
            redirect_hosts = [\(hosts)]
            demonware_key = \(quote(demonwareKey))

            """
        try text.write(to: url, atomically: true, encoding: .utf8)
    }

    // What is wrong with the settings, nil when they can be saved
    var problem: String? {
        guard enabled else { return nil }
        if server.isEmpty {
            return "Enter the server."
        }
        if !(1...65535).contains(httpPort) {
            return "The HTTP port is 1 to 65535."
        }
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "_-"))
        if !(3...16).contains(nickname.count)
            || nickname.unicodeScalars.contains(where: { !$0.isASCII || !allowed.contains($0) })
        {
            return "The nickname is 3 to 16 letters, digits, _ or -."
        }
        if !demonwareKey.isEmpty
            && (demonwareKey.count != 512 || !demonwareKey.allSatisfy(\.isHexDigit))
        {
            return "The Demonware key is 512 hex digits (an RSA-2048 modulus)."
        }
        return nil
    }
}

private func quote(_ str: String) -> String {
    let escaped =
        str
        .replacingOccurrences(of: "\\", with: "\\\\")
        .replacingOccurrences(of: "\"", with: "\\\"")
    return "\"\(escaped)\""
}

private func unquote<S: StringProtocol>(_ str: S) -> String {
    var unquoted = String(str)
    if unquoted.count >= 2 && unquoted.hasPrefix("\"") && unquoted.hasSuffix("\"") {
        unquoted = String(unquoted.dropFirst().dropLast())
    }
    return
        unquoted
        .replacingOccurrences(of: "\\\"", with: "\"")
        .replacingOccurrences(of: "\\\\", with: "\\")
}

struct GameOnlineSettingsView: View {
    let game: Game
    @Binding var isPresented: Bool

    @State private var settings = GameOnlineSettings()
    @State private var errorMessage: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("\(self.game.name): Online")
                .font(.headline)

            Toggle("Play online", isOn: self.$settings.enabled)

            Form {
                TextField("Server", text: self.$settings.server)
                TextField(
                    "HTTP port", value: self.$settings.httpPort,
                    format: .number.grouping(.never))
                TextField("Nickname", text: self.$settings.nickname)
                TextField(
                    "Redirected hosts",
                    text: Binding(
                        get: { self.settings.redirectHosts.joined(separator: ", ") },
                        set: {
                            self.settings.redirectHosts =
                                $0
                                .split(separator: ",")
                                .map { $0.trimmingCharacters(in: .whitespaces) }
                                .filter { !$0.isEmpty }
                        }))
                TextField("Demonware key (hex)", text: self.$settings.demonwareKey)
            }
            .disabled(!self.settings.enabled)

            Text(
                "The redirected hosts resolve to the server, and TLS to the server's ports 80 and 443 reaches its HTTP port without encryption. The settings apply when the game boots."
            )
            .font(.caption)
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)

            if let errorMessage = self.errorMessage ?? self.settings.problem {
                Text(errorMessage)
                    .foregroundStyle(.red)
            }

            HStack {
                Button("RyuNET (OCTR)") {
                    self.settings.enabled = true
                    self.settings.server = GameOnlineSettings.ryuNetServer
                    self.settings.httpPort = 8080
                    self.settings.redirectHosts = ["*.demonware.net"]
                    self.settings.demonwareKey = GameOnlineSettings.ryuNetKey
                }

                Spacer()

                Button("Cancel") {
                    self.isPresented = false
                }
                .keyboardShortcut(.cancelAction)

                Button("Save") {
                    do {
                        try self.settings.save(titleId: self.game.loader.titleId)
                        self.isPresented = false
                    } catch {
                        self.errorMessage = "Failed to save: \(error.localizedDescription)"
                    }
                }
                .keyboardShortcut(.defaultAction)
                .disabled(self.settings.problem != nil)
            }
        }
        .padding()
        .frame(minWidth: 460)
        .onAppear {
            self.settings = GameOnlineSettings.load(titleId: self.game.loader.titleId)
        }
    }
}
