import Foundation
import UniformTypeIdentifiers
#if os(macOS)
import AppKit
#else
import UIKit
#endif

/// Meldungen, Rückfragen und Dateidialoge für Mac und iPad/iPhone. Auf dem Mac warten die
/// Dialoge modal, auf iOS werden sie über dem obersten Bildschirm gezeigt — deshalb ist
/// alles hier `async`.
@MainActor
enum Dialogs {
    static func show(_ title: String, _ text: String) async {
        _ = await ask(title, text, buttons: ["OK"])
    }

    /// true bei „OK".
    static func confirm(_ title: String, _ text: String) async -> Bool {
        await ask(title, text, buttons: ["OK", "Abbrechen"]) == 0
    }

    // MARK: Mac

    #if os(macOS)
    private static func ask(_ title: String, _ text: String, buttons: [String]) async -> Int {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = text
        buttons.forEach { alert.addButton(withTitle: $0) }
        return alert.runModal().rawValue - NSApplication.ModalResponse.alertFirstButtonReturn.rawValue
    }

    static func askText(_ title: String, _ text: String, value: String) async -> String? {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = text
        let field = NSTextField(string: value)
        field.frame = NSRect(x: 0, y: 0, width: 240, height: 24)
        alert.accessoryView = field
        alert.addButton(withTitle: "OK")
        alert.addButton(withTitle: "Abbrechen")
        return alert.runModal() == .alertFirstButtonReturn ? field.stringValue : nil
    }

    static func save(_ data: Data, name: String) async throws {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.json]
        panel.nameFieldStringValue = name
        guard panel.runModal() == .OK, let url = panel.url else { return }
        try data.write(to: url, options: .atomic)
    }

    /// Inhalt und Name der gewählten Datei, nil bei Abbruch.
    static func open() async throws -> (data: Data, name: String)? {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.json]
        guard panel.runModal() == .OK, let url = panel.url else { return nil }
        return (try Data(contentsOf: url), url.lastPathComponent)
    }

    // MARK: iPad und iPhone

    #else
    private static func ask(_ title: String, _ text: String, buttons: [String]) async -> Int {
        guard let top = topViewController() else { return buttons.count - 1 }
        return await withCheckedContinuation { continuation in
            let alert = UIAlertController(title: title, message: text.isEmpty ? nil : text,
                                          preferredStyle: .alert)
            for (index, title) in buttons.enumerated() {
                let style: UIAlertAction.Style = index > 0 && index == buttons.count - 1 ? .cancel : .default
                alert.addAction(UIAlertAction(title: title, style: style) { _ in continuation.resume(returning: index) })
            }
            top.present(alert, animated: true)
        }
    }

    static func askText(_ title: String, _ text: String, value: String) async -> String? {
        guard let top = topViewController() else { return nil }
        return await withCheckedContinuation { continuation in
            let alert = UIAlertController(title: title, message: text, preferredStyle: .alert)
            alert.addTextField { field in
                field.text = value
                field.autocapitalizationType = .none
                field.autocorrectionType = .no
                field.keyboardType = .URL
            }
            alert.addAction(UIAlertAction(title: "OK", style: .default) { [weak alert] _ in
                continuation.resume(returning: alert?.textFields?.first?.text)
            })
            alert.addAction(UIAlertAction(title: "Abbrechen", style: .cancel) { _ in
                continuation.resume(returning: nil)
            })
            top.present(alert, animated: true)
        }
    }

    static func save(_ data: Data, name: String) async throws {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(name)
        try data.write(to: url, options: .atomic)
        _ = await pick(UIDocumentPickerViewController(forExporting: [url], asCopy: true))
    }

    static func open() async throws -> (data: Data, name: String)? {
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.json], asCopy: true)
        guard let url = await pick(picker) else { return nil }
        return (try Data(contentsOf: url), url.lastPathComponent)
    }

    private static var pickerDelegate: PickerDelegate?

    private static func pick(_ picker: UIDocumentPickerViewController) async -> URL? {
        guard let top = topViewController() else { return nil }
        return await withCheckedContinuation { continuation in
            let delegate = PickerDelegate { url in
                pickerDelegate = nil
                continuation.resume(returning: url)
            }
            pickerDelegate = delegate
            picker.delegate = delegate
            top.present(picker, animated: true)
        }
    }

    private final class PickerDelegate: NSObject, UIDocumentPickerDelegate {
        let done: (URL?) -> Void
        init(done: @escaping (URL?) -> Void) { self.done = done }
        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
            done(urls.first)
        }
        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) { done(nil) }
    }

    private static func topViewController() -> UIViewController? {
        let window = UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .flatMap(\.windows)
            .first { $0.isKeyWindow }
        var top = window?.rootViewController
        while let presented = top?.presentedViewController { top = presented }
        return top
    }
    #endif
}
