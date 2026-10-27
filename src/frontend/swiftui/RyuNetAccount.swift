import CryptoKit
import Foundation
import Network

#if os(macOS)
    import AppKit
#endif

// The nickname account on a RyuNET server, as OCTR's launcher manages it
// (launcher/ryunet_http.py, launcher/discord_oauth.py): the nickname's status
// and its Discord link. Requests are signed with this installation's key, the
// one the core uses in game (<app data>/online/ryunet.key).
struct RyuNetAccount {
    struct Status {
        // free, yours, taken, reserved or invalid
        var status: String
        var linked: Bool
        var discord: String?
    }

    enum AccountError: LocalizedError {
        case noKey
        case unreachable
        case badAnswer
        case discord(String)

        var errorDescription: String? {
            switch self {
            case .noKey: return "The installation key could not be read or created."
            case .unreachable: return "The server did not answer."
            case .badAnswer: return "The server's answer was not understood."
            case .discord(let error): return "Discord authorization failed: \(error)."
            }
        }
    }

    // OCTR's Discord application; its redirect URIs are these loopback ports
    static let discordAppId = "1560408801704943708"
    static let discordPorts: [UInt16] = [53134, 53135, 53136]

    let host: String
    let port: Int

    // Seconds to add to this Mac's clock to get the server's: signatures are
    // only accepted within minutes of the server's time
    private static var clockOffset = 0

    // MARK: Key

    private static var keyURL: URL {
        URL(fileURLWithPath: configGetAppDataPath())
            .appendingPathComponent("online")
            .appendingPathComponent("ryunet.key")
    }

    // The installation key, created like the core does when there is none
    private static func installationKey() throws -> Data {
        if let text = try? String(contentsOf: keyURL, encoding: .ascii),
            let key = Data(hex: text.trimmingCharacters(in: .whitespacesAndNewlines)),
            key.count == 32
        {
            return key
        }
        var bytes = [UInt8](repeating: 0, count: 32)
        guard SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes) == errSecSuccess
        else { throw AccountError.noKey }
        let key = Data(bytes)
        do {
            try FileManager.default.createDirectory(
                at: keyURL.deletingLastPathComponent(), withIntermediateDirectories: true)
            try (key.hex + "\n").write(to: keyURL, atomically: true, encoding: .ascii)
        } catch {
            throw AccountError.noKey
        }
        return key
    }

    // What the server stores of the key: SHA-256 of HMAC-SHA256(key, host),
    // both as lowercase hex
    private func keyHash() throws -> String {
        let key = SymmetricKey(data: try Self.installationKey())
        let serverKey = Data(
            HMAC<SHA256>.authenticationCode(
                for: Data(host.lowercased().utf8), using: key)
        ).hex
        return Data(SHA256.hash(data: Data(serverKey.utf8))).hex
    }

    // X-RyuNET-Proof: good once, for a few minutes of the server's time
    private func proof(keyHash: String, method: String, target: String) -> String {
        let timestamp = String(Int(Date().timeIntervalSince1970) + Self.clockOffset)
        var nonceBytes = [UInt8](repeating: 0, count: 16)
        _ = SecRandomCopyBytes(kSecRandomDefault, nonceBytes.count, &nonceBytes)
        let nonce = Data(nonceBytes).hex
        let mac = Data(
            HMAC<SHA256>.authenticationCode(
                for: Data("\(timestamp)\n\(nonce)\n\(method)\n\(target)".utf8),
                using: SymmetricKey(data: Data(keyHash.utf8)))
        ).hex
        return "1.\(timestamp).\(nonce).\(mac)"
    }

    // MARK: Requests

    private func request(
        _ target: String, form: [String: String]? = nil, register: Bool = false,
        timeout: TimeInterval = 10
    ) async throws -> [String: Any] {
        let keyHash = try keyHash()
        let method = form == nil ? "GET" : "POST"
        var body = Data()
        var headers = [
            "Host: \(host):\(port)",
            "User-Agent: SacredFire launcher",
            "X-RyuNET-Proof: \(proof(keyHash: keyHash, method: method, target: target))",
            "Connection: close",
        ]
        if register {
            headers.append("X-RyuNET-Key-Hash: \(keyHash)")
        }
        if let form {
            var components = URLComponents()
            components.queryItems = form.map { URLQueryItem(name: $0.key, value: $0.value) }
            body = Data((components.percentEncodedQuery ?? "").utf8)
            headers.append("Content-Type: application/x-www-form-urlencoded")
        }
        if method == "POST" {
            headers.append("Content-Length: \(body.count)")
        }
        let head = "\(method) \(target) HTTP/1.1\r\n" + headers.joined(separator: "\r\n") + "\r\n\r\n"

        // Plain HTTP over a TCP connection of our own: App Transport Security
        // keeps URLSession from plain HTTP to arbitrary servers, and the core
        // reaches the same port the same way
        let reply = try await Self.exchange(
            host: host, port: port, data: Data(head.utf8) + body, timeout: timeout)
        guard let split = reply.range(of: Data("\r\n\r\n".utf8)) else {
            throw AccountError.badAnswer
        }
        let headerText = String(decoding: reply[..<split.lowerBound], as: UTF8.self)
        var content = Data(reply[split.upperBound...])
        var chunked = false
        for line in headerText.components(separatedBy: "\r\n").dropFirst() {
            let parts = line.split(separator: ":", maxSplits: 1)
            guard parts.count == 2 else { continue }
            let name = parts[0].trimmingCharacters(in: .whitespaces).lowercased()
            let value = parts[1].trimmingCharacters(in: .whitespaces)
            if name == "date", let serverTime = Self.httpDateFormatter.date(from: value) {
                let offset = Int(serverTime.timeIntervalSinceNow.rounded())
                Self.clockOffset = abs(offset) > 5 ? offset : 0
            } else if name == "transfer-encoding", value.lowercased().contains("chunked") {
                chunked = true
            }
        }
        if chunked {
            content = Self.dechunk(content)
        }
        guard let answer = try? JSONSerialization.jsonObject(with: content) as? [String: Any]
        else { throw AccountError.badAnswer }
        return answer
    }

    // Sends the request and reads the reply until the server closes the
    // connection (Connection: close)
    private static func exchange(host: String, port: Int, data: Data, timeout: TimeInterval)
        async throws -> Data
    {
        guard let nwPort = NWEndpoint.Port(rawValue: UInt16(clamping: port)) else {
            throw AccountError.unreachable
        }
        let connection = NWConnection(host: NWEndpoint.Host(host), port: nwPort, using: .tcp)
        let exchange = HttpExchange(connection: connection, request: data)
        return try await exchange.run(timeout: timeout)
    }

    private static func dechunk(_ data: Data) -> Data {
        var result = Data()
        var rest = data[...]
        while let lineEnd = rest.range(of: Data("\r\n".utf8)) {
            let sizeText = String(decoding: rest[..<lineEnd.lowerBound], as: UTF8.self)
                .split(separator: ";").first.map(String.init) ?? ""
            guard let size = Int(sizeText.trimmingCharacters(in: .whitespaces), radix: 16),
                size > 0
            else { break }
            let start = lineEnd.upperBound
            guard let end = rest.index(start, offsetBy: size, limitedBy: rest.endIndex) else {
                result.append(rest[start...])
                break
            }
            result.append(rest[start..<end])
            rest = rest[min(rest.index(end, offsetBy: 2, limitedBy: rest.endIndex) ?? rest.endIndex, rest.endIndex)...]
        }
        return result
    }

    private static let httpDateFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(identifier: "GMT")
        formatter.dateFormat = "EEE, dd MMM yyyy HH:mm:ss zzz"
        return formatter
    }()

    private static func query(name: String) -> String {
        var components = URLComponents()
        components.queryItems = [URLQueryItem(name: "name", value: name)]
        return components.percentEncodedQuery ?? ""
    }

    func status(nickname: String) async throws -> Status {
        let answer = try await request("/ryunet/nick?" + Self.query(name: nickname))
        guard let status = answer["status"] as? String else { throw AccountError.badAnswer }
        return Status(
            status: status, linked: answer["linked"] as? Bool ?? false,
            discord: answer["discord"] as? String)
    }

    // Links the nickname to the Discord account the player authorizes in the
    // browser, or with an empty nickname recovers the account's nickname for
    // this installation. Returns the server's answer ("status", "nick", ...).
    func linkDiscord(nickname: String) async throws -> [String: Any] {
        let (code, redirectUri) = try await Self.authorizeDiscord()
        return try await request(
            "/ryunet/discord/link",
            form: ["nick": nickname, "code": code, "redirect_uri": redirectUri],
            register: true, timeout: 25)
    }

    // MARK: Discord authorization

    // Opens Discord's authorization page and waits for the browser to come
    // back to the loopback redirect URI with a code. The code alone is
    // useless: the server trades it for the Discord account.
    private static func authorizeDiscord() async throws -> (code: String, redirectUri: String) {
        var stateBytes = [UInt8](repeating: 0, count: 16)
        _ = SecRandomCopyBytes(kSecRandomDefault, stateBytes.count, &stateBytes)
        let state = Data(stateBytes).hex

        for port in discordPorts {
            guard let nwPort = NWEndpoint.Port(rawValue: port) else { continue }
            let parameters = NWParameters.tcp
            parameters.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: nwPort)
            parameters.allowLocalEndpointReuse = true
            guard let listener = try? NWListener(using: parameters) else { continue }
            let redirectUri = "http://127.0.0.1:\(port)/discord"
            let callback = DiscordCallback(state: state)
            guard await callback.start(listener: listener) else { continue }

            var components = URLComponents(string: "https://discord.com/oauth2/authorize")!
            components.queryItems = [
                URLQueryItem(name: "client_id", value: discordAppId),
                URLQueryItem(name: "response_type", value: "code"),
                URLQueryItem(name: "scope", value: "identify"),
                URLQueryItem(name: "prompt", value: "none"),
                URLQueryItem(name: "redirect_uri", value: redirectUri),
                URLQueryItem(name: "state", value: state),
            ]
            #if os(macOS)
                if let url = components.url {
                    _ = await MainActor.run { NSWorkspace.shared.open(url) }
                }
            #endif

            let result = await callback.wait(timeout: 300)
            listener.cancel()
            switch result {
            case .code(let code): return (code, redirectUri)
            case .error(let error): throw AccountError.discord(error)
            }
        }
        throw AccountError.discord("the loopback ports are busy")
    }
}

// One HTTP request and its reply on a TCP connection
private final class HttpExchange: @unchecked Sendable {
    private let connection: NWConnection
    private let request: Data
    private let queue = DispatchQueue(label: "hydra.ryunet-http")
    private var reply = Data()
    private var continuation: CheckedContinuation<Data, Error>?

    init(connection: NWConnection, request: Data) {
        self.connection = connection
        self.request = request
    }

    func run(timeout: TimeInterval) async throws -> Data {
        try await withCheckedThrowingContinuation { continuation in
            queue.async {
                self.continuation = continuation
                self.connection.stateUpdateHandler = { [self] state in
                    switch state {
                    case .ready:
                        self.connection.send(
                            content: self.request,
                            completion: .contentProcessed { [self] error in
                                if error != nil {
                                    self.finish(.failure(RyuNetAccount.AccountError.unreachable))
                                } else {
                                    self.receive()
                                }
                            })
                    case .failed, .waiting:
                        self.finish(.failure(RyuNetAccount.AccountError.unreachable))
                    default:
                        break
                    }
                }
                self.connection.start(queue: self.queue)
                self.queue.asyncAfter(deadline: .now() + timeout) {
                    self.finish(.failure(RyuNetAccount.AccountError.unreachable))
                }
            }
        }
    }

    private func receive() {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 65536) {
            [self] data, _, complete, error in
            if let data {
                self.reply.append(data)
            }
            if complete || error != nil || self.reply.count > 1 << 20 {
                self.finish(
                    self.reply.isEmpty
                        ? .failure(RyuNetAccount.AccountError.unreachable)
                        : .success(self.reply))
            } else {
                self.receive()
            }
        }
    }

    private func finish(_ result: Swift.Result<Data, Error>) {
        guard let continuation else { return }
        self.continuation = nil
        connection.cancel()
        continuation.resume(with: result)
    }
}

// The loopback server Discord redirects the browser to
private final class DiscordCallback: @unchecked Sendable {
    enum Result {
        case code(String)
        case error(String)
    }

    private let state: String
    private let queue = DispatchQueue(label: "hydra.discord-callback")
    private var continuation: CheckedContinuation<Result, Never>?
    private var result: Result?
    private var started = false

    init(state: String) { self.state = state }

    // Whether the listener could take its port
    func start(listener: NWListener) async -> Bool {
        await withCheckedContinuation { (ready: CheckedContinuation<Bool, Never>) in
            // The handler runs on `queue`, as does everything touching `started`
            listener.stateUpdateHandler = { [self] state in
                guard !self.started else { return }
                switch state {
                case .ready:
                    self.started = true
                    ready.resume(returning: true)
                case .failed, .cancelled:
                    self.started = true
                    ready.resume(returning: false)
                default:
                    break
                }
            }
            listener.newConnectionHandler = { [weak self] connection in
                self?.handle(connection)
            }
            listener.start(queue: queue)
        }
    }

    func wait(timeout: TimeInterval) async -> Result {
        await withCheckedContinuation { continuation in
            queue.async {
                if let result = self.result {
                    continuation.resume(returning: result)
                    return
                }
                self.continuation = continuation
                self.queue.asyncAfter(deadline: .now() + timeout) {
                    self.finish(.error("timeout"))
                }
            }
        }
    }

    private func finish(_ result: Result) {
        guard self.result == nil else { return }
        self.result = result
        continuation?.resume(returning: result)
        continuation = nil
    }

    private func handle(_ connection: NWConnection) {
        connection.start(queue: queue)
        connection.receive(minimumIncompleteLength: 1, maximumLength: 16384) {
            [weak self] data, _, _, _ in
            guard let self else { return }
            let request = data.flatMap { String(data: $0, encoding: .isoLatin1) } ?? ""
            let line = request.split(separator: "\r\n", maxSplits: 1).first ?? ""
            let parts = line.split(separator: " ")
            let target = parts.count >= 2 && parts[0] == "GET" ? String(parts[1]) : ""
            let components = URLComponents(string: "http://127.0.0.1" + target)
            let items = components?.queryItems ?? []
            let value = { (name: String) in items.first { $0.name == name }?.value ?? "" }

            var page = "Discord account authorized. You can close this tab and go back to Hydra."
            var status = "200 OK"
            if components?.path != "/discord" || value("state") != self.state {
                // Not this request (an old tab, another program): keep waiting
                page = "This authorization is not Hydra's."
                status = "400 Bad Request"
            } else if !value("error").isEmpty || value("code").isEmpty {
                let error = value("error")
                self.finish(.error(error.isEmpty || error == "access_denied" ? "denied" : error))
                page = "Authorization was not granted."
            } else {
                self.finish(.code(value("code")))
            }
            let body = Data(page.utf8)
            let head =
                "HTTP/1.1 \(status)\r\nContent-Type: text/plain; charset=utf-8\r\n"
                + "Connection: close\r\nContent-Length: \(body.count)\r\n\r\n"
            connection.send(
                content: Data(head.utf8) + body,
                completion: .contentProcessed { _ in connection.cancel() })
        }
    }
}

extension Data {
    fileprivate init?(hex: String) {
        guard hex.count % 2 == 0 else { return nil }
        var bytes = [UInt8]()
        bytes.reserveCapacity(hex.count / 2)
        var index = hex.startIndex
        while index < hex.endIndex {
            let next = hex.index(index, offsetBy: 2)
            guard let byte = UInt8(hex[index..<next], radix: 16) else { return nil }
            bytes.append(byte)
            index = next
        }
        self.init(bytes)
    }

    fileprivate var hex: String {
        map { String(format: "%02x", $0) }.joined()
    }
}
