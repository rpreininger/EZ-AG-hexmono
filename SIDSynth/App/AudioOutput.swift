import AVFoundation

/// Spielt die Klangerzeugung über AVAudioEngine.
///
/// Gerechnet wird wie auf der Box mit 44,1 kHz mono; auf die Rate der Hardware (meist
/// 48 kHz) rechnet AVAudioEngine selbst um. Beide Kanäle bekommen dasselbe Signal wie
/// beim PCM5102A der Box. Im Callback nur sid_render(): kein Lock, kein Speicher, kein
/// Swift-Objekt.
final class AudioOutput {
    private let engine = AVAudioEngine()
    private let source: AVAudioSourceNode
    private var observer: NSObjectProtocol?

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
        observer = NotificationCenter.default.addObserver(
            forName: .AVAudioEngineConfigurationChange, object: engine, queue: .main
        ) { [weak self] _ in
            try? self?.start()
        }
    }

    func start() throws {
        if !engine.isRunning { try engine.start() }
    }
}
