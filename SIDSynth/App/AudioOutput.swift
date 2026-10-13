import AVFoundation

/// Spielt die Klangerzeugung über AVAudioEngine.
///
/// Gerechnet wird wie auf der Box mit 44,1 kHz mono; auf die Rate der Hardware (meist
/// 48 kHz) rechnet AVAudioEngine selbst um. Beide Kanäle bekommen dasselbe Signal wie
/// beim PCM5102A der Box. Im Callback nur sid_render(): kein Lock, kein Speicher, kein
/// Swift-Objekt.
///
/// Auf iPad und iPhone kommt die Audio-Session dazu: Wiedergabe auch bei Stummschalter und
/// im Hintergrund, ein kleiner Puffer fürs Spielen über MIDI, und nach einem Anruf oder
/// Siri wieder anlaufen.
final class AudioOutput {
    private let engine = AVAudioEngine()
    private let source: AVAudioSourceNode
    private var observers: [NSObjectProtocol] = []

    init() {
        let format = AVAudioFormat(standardFormatWithSampleRate: 44100, channels: 2)!
        source = AVAudioSourceNode(format: format) { _, _, frameCount, bufferList -> OSStatus in
            let buffers = UnsafeMutableAudioBufferListPointer(bufferList)
            guard let left = buffers[0].mData?.assumingMemoryBound(to: Float.self) else { return noErr }
            sid_render(left, Int32(frameCount))
            for channel in 1..<buffers.count {
                buffers[channel].mData?.copyMemory(from: left, byteCount: Int(frameCount) * MemoryLayout<Float>.size)
            }
            return noErr
        }
        engine.attach(source)
        engine.connect(source, to: engine.mainMixerNode, format: format)

        // Kopfhörer ab, anderes Ausgabegerät: AVAudioEngine hält dann an.
        observers.append(NotificationCenter.default.addObserver(
            forName: .AVAudioEngineConfigurationChange, object: engine, queue: .main
        ) { [weak self] _ in
            try? self?.start()
        })
        #if os(iOS)
        observers.append(NotificationCenter.default.addObserver(
            forName: AVAudioSession.interruptionNotification, object: nil, queue: .main
        ) { [weak self] note in
            let type = (note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt)
                .flatMap(AVAudioSession.InterruptionType.init)
            if type == .ended { try? self?.start() }
        })
        observers.append(NotificationCenter.default.addObserver(
            forName: AVAudioSession.mediaServicesWereResetNotification, object: nil, queue: .main
        ) { [weak self] _ in
            try? self?.start()
        })
        #endif
    }

    func start() throws {
        #if os(iOS)
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.playback, mode: .default)
        try? session.setPreferredSampleRate(44100)
        try? session.setPreferredIOBufferDuration(0.005)  // ~5 ms, wie die Box mit ~3 ms Blöcken
        try session.setActive(true)
        #endif
        if !engine.isRunning { try engine.start() }
    }
}
