import Combine
import CoreMIDI
import Foundation
import MidiFilterCore

struct EndpointInfo: Identifiable, Hashable {
    let id: MIDIUniqueID
    let name: String
    let endpoint: MIDIEndpointRef
}

/// Owns the CoreMIDI client and everything the UI binds to.
@MainActor
final class MIDIEngine: ObservableObject {

    // Devices
    @Published private(set) var sources: [EndpointInfo] = []
    @Published private(set) var destinations: [EndpointInfo] = []
    @Published var selectedSourceIDs: Set<MIDIUniqueID> = [] {
        didSet { guard selectedSourceIDs != oldValue else { return }; connectSources(); persist() }
    }
    @Published var selectedDestinationIDs: Set<MIDIUniqueID> = [] {
        didSet { guard selectedDestinationIDs != oldValue else { return }; updateDestinations(); persist() }
    }

    // Filter
    @Published var config = FilterConfig() {
        didSet { guard config != oldValue else { return }; router.setConfig(config); updateTicker(); persist() }
    }

    // Monitor
    @Published private(set) var entries: [LogEntry] = []
    @Published var isPaused = false { didSet { router.log.isPaused = isPaused } }
    @Published private(set) var errorMessage: String?

    /// Newest entries first, capped so SwiftUI stays smooth. The full log is still
    /// available for export.
    private let visibleEntryLimit = 400

    private let sender = MIDISender()
    private lazy var router = MIDIRouter(sender: sender)

    private var client = MIDIClientRef()
    private var inputPort = MIDIPortRef()
    private var outputPort = MIDIPortRef()
    private var virtualSource = MIDIEndpointRef()
    private var virtualDestination = MIDIEndpointRef()
    private var connectedSources: [MIDIUniqueID: MIDIEndpointRef] = [:]

    private var ticker: DispatchSourceTimer?
    private var uiTimer: Timer?

    let virtualSourceName = "MidiFilter Out"
    let virtualDestinationName = "MidiFilter In"

    init() {
        loadPersisted()
    }

    // MARK: Lifecycle

    func start() {
        guard client == 0 else { return }

        var status = MIDIClientCreateWithBlock("MidiFilter" as CFString, &client) { [weak self] notification in
            guard notification.pointee.messageID == .msgSetupChanged else { return }
            Task { @MainActor in self?.refreshEndpoints() }
        }
        guard check(status, "MIDI-Client") else { return }

        let receive: MIDIReceiveBlock = { [router] listPtr, _ in
            router.receive(listPtr)
        }

        status = MIDIInputPortCreateWithProtocol(client, "MidiFilter Input" as CFString, ._1_0, &inputPort, receive)
        _ = check(status, "Eingangsport")

        status = MIDIOutputPortCreate(client, "MidiFilter Output" as CFString, &outputPort)
        _ = check(status, "Ausgangsport")

        // Virtual endpoints, so software synths and DAWs can be wired up without a cable.
        status = MIDISourceCreateWithProtocol(client, virtualSourceName as CFString, ._1_0, &virtualSource)
        _ = check(status, "virtueller Ausgang")

        status = MIDIDestinationCreateWithProtocol(client, virtualDestinationName as CFString, ._1_0,
                                                   &virtualDestination, receive)
        _ = check(status, "virtueller Eingang")

        // Without this CoreMIDI hands out a fresh ID on every launch, and anything that
        // remembers our ports by ID — a DAW, a software synth — loses the connection.
        Self.assignStableID(0x4D46_0001, to: virtualSource)
        Self.assignStableID(0x4D46_0002, to: virtualDestination)

        sender.configure(outputPort: outputPort, virtualSource: virtualSource)
        router.setConfig(config)

        refreshEndpoints()
        updateTicker()
        startUITimer()
    }

    func stop() {
        panic()
        uiTimer?.invalidate()
        uiTimer = nil
        ticker?.cancel()
        ticker = nil
        for (_, endpoint) in connectedSources {
            MIDIPortDisconnectSource(inputPort, endpoint)
        }
        connectedSources.removeAll()
        if virtualSource != 0 { MIDIEndpointDispose(virtualSource) }
        if virtualDestination != 0 { MIDIEndpointDispose(virtualDestination) }
        if client != 0 { MIDIClientDispose(client) }
        client = 0
    }

    // MARK: Devices

    func refreshEndpoints() {
        sources = Self.endpoints(count: MIDIGetNumberOfSources(), at: MIDIGetSource)
            .filter { $0.endpoint != virtualSource }
        destinations = Self.endpoints(count: MIDIGetNumberOfDestinations(), at: MIDIGetDestination)
            .filter { $0.endpoint != virtualDestination }
        connectSources()
        updateDestinations()
    }

    private static func endpoints(count: Int, at accessor: (Int) -> MIDIEndpointRef) -> [EndpointInfo] {
        (0..<count).compactMap { index in
            let endpoint = accessor(index)
            var uniqueID: MIDIUniqueID = 0
            guard MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyUniqueID, &uniqueID) == noErr else { return nil }
            return EndpointInfo(id: uniqueID, name: displayName(of: endpoint), endpoint: endpoint)
        }
    }

    /// Best effort — if something else already claimed the ID, CoreMIDI keeps the one it
    /// assigned and the port simply works as before.
    private static func assignStableID(_ id: MIDIUniqueID, to endpoint: MIDIEndpointRef) {
        guard endpoint != 0 else { return }
        MIDIObjectSetIntegerProperty(endpoint, kMIDIPropertyUniqueID, id)
    }

    private static func displayName(of endpoint: MIDIEndpointRef) -> String {
        var name: Unmanaged<CFString>?
        guard MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name) == noErr,
              let name else { return "Unbekannt" }
        return name.takeRetainedValue() as String
    }

    /// Connects exactly the selected sources, leaving already connected ones alone.
    private func connectSources() {
        guard inputPort != 0 else { return }
        let wanted = sources.filter { selectedSourceIDs.contains($0.id) }

        for (id, endpoint) in connectedSources where !selectedSourceIDs.contains(id) {
            MIDIPortDisconnectSource(inputPort, endpoint)
            connectedSources.removeValue(forKey: id)
        }
        for source in wanted where connectedSources[source.id] == nil {
            if check(MIDIPortConnectSource(inputPort, source.endpoint, nil), "Verbindung zu \(source.name)") {
                connectedSources[source.id] = source.endpoint
            }
        }
    }

    private func updateDestinations() {
        sender.setDestinations(destinations.filter { selectedDestinationIDs.contains($0.id) }.map(\.endpoint))
    }

    // MARK: Filter control

    func panic() {
        router.panic()
    }

    func clearLog() {
        router.log.clear()
        lastRenderedID = nil
        entries = []
    }

    func exportLog() -> String {
        router.log.exportText()
    }

    /// Blip mode and slide mode need a clock — one to time out held `Note On`s, the other
    /// to hand on the releases no fret note claimed. Keep it off otherwise.
    private func updateTicker() {
        let needed = config.fretMode == .blip || config.fretSlide
        if needed, ticker == nil {
            let timer = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "MidiFilter.tick",
                                                                           qos: .userInteractive))
            timer.schedule(deadline: .now(), repeating: .milliseconds(4), leeway: .milliseconds(1))
            timer.setEventHandler { [router] in router.tick() }
            timer.resume()
            ticker = timer
        } else if !needed {
            ticker?.cancel()
            ticker = nil
        }
    }

    private func startUITimer() {
        uiTimer?.invalidate()
        let timer = Timer(timeInterval: 1.0 / 20, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refreshEntries() }
        }
        RunLoop.main.add(timer, forMode: .common)
        uiTimer = timer
    }

    private var lastRenderedID: UInt64?

    private func refreshEntries() {
        let all = router.log.snapshot()
        guard all.last?.id != lastRenderedID else { return }
        lastRenderedID = all.last?.id
        entries = Array(all.suffix(visibleEntryLimit).reversed())
    }

    // MARK: Errors

    @discardableResult
    private func check(_ status: OSStatus, _ what: String) -> Bool {
        guard status != noErr else { return true }
        errorMessage = "\(what) fehlgeschlagen (OSStatus \(status))"
        return false
    }

    // MARK: Persistence

    private enum Key {
        static let config = "filterConfig"
        static let sources = "selectedSources"
        static let destinations = "selectedDestinations"
    }

    private var isLoading = false

    private func persist() {
        guard !isLoading else { return }
        let defaults = UserDefaults.standard
        if let data = try? JSONEncoder().encode(config) {
            defaults.set(data, forKey: Key.config)
        }
        defaults.set(selectedSourceIDs.map(Int.init), forKey: Key.sources)
        defaults.set(selectedDestinationIDs.map(Int.init), forKey: Key.destinations)
    }

    private func loadPersisted() {
        isLoading = true
        defer { isLoading = false }
        let defaults = UserDefaults.standard
        if let data = defaults.data(forKey: Key.config),
           let stored = try? JSONDecoder().decode(FilterConfig.self, from: data) {
            config = stored
        }
        if let stored = defaults.array(forKey: Key.sources) as? [Int] {
            selectedSourceIDs = Set(stored.map(MIDIUniqueID.init))
        }
        if let stored = defaults.array(forKey: Key.destinations) as? [Int] {
            selectedDestinationIDs = Set(stored.map(MIDIUniqueID.init))
        }
    }
}
