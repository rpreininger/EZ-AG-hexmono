import SwiftUI
import UniformTypeIdentifiers
import WebKit
#if os(macOS)
import AppKit
#endif

/// Der Web-Editor der Box, unverändert (Firmware/web/index.html), in einer WKWebView.
///
/// Die Seite wird unter `sid://box/` geladen; `/api/schema`, `/api/backup` und
/// `/api/restore` beantwortet der Scheme-Handler, so wie der Webserver der Box. Statt des
/// WebSockets bekommt die Seite eine kleine Brücke nach Swift mit denselben Textzeilen.
/// Am Editor selbst ist dafür nichts geändert.
struct EditorView {
    let app: AppModel

    @MainActor
    private func makeWebView(_ coordinator: Coordinator) -> WKWebView {
        let bridge = EditorBridge(app: app)
        let configuration = WKWebViewConfiguration()
        configuration.setURLSchemeHandler(bridge, forURLScheme: "sid")
        let content = configuration.userContentController
        content.addUserScript(WKUserScript(source: EditorBridge.socketShim,
                                           injectionTime: .atDocumentStart, forMainFrameOnly: true))
        content.addUserScript(WKUserScript(source: EditorBridge.appStyle,
                                           injectionTime: .atDocumentEnd, forMainFrameOnly: true))
        content.add(bridge, name: "sid")

        let webView = WKWebView(frame: .zero, configuration: configuration)
        webView.navigationDelegate = bridge
        webView.uiDelegate = bridge
        #if DEBUG
        webView.isInspectable = true  // Safari → Entwickler, zum Nachsehen im Editor
        #endif
        #if os(iOS)
        // Die Seite rückt selbst ab (env(safe-area-inset-…)), nicht noch einmal die WebView.
        webView.scrollView.contentInsetAdjustmentBehavior = .never
        webView.scrollView.minimumZoomScale = 1
        webView.scrollView.maximumZoomScale = 1
        #endif
        bridge.webView = webView
        coordinator.bridge = bridge
        webView.load(URLRequest(url: URL(string: "sid://box/")!))
        return webView
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    final class Coordinator {
        var bridge: EditorBridge?
    }
}

#if os(macOS)
extension EditorView: NSViewRepresentable {
    func makeNSView(context: Context) -> WKWebView { makeWebView(context.coordinator) }
    func updateNSView(_ nsView: WKWebView, context: Context) {}
}
#else
extension EditorView: UIViewRepresentable {
    func makeUIView(context: Context) -> WKWebView { makeWebView(context.coordinator) }
    func updateUIView(_ uiView: WKWebView, context: Context) {}
}
#endif

@MainActor
final class EditorBridge: NSObject, WKURLSchemeHandler, WKScriptMessageHandler, WKNavigationDelegate,
    WKUIDelegate {
    let app: AppModel
    weak var webView: WKWebView?

    init(app: AppModel) {
        self.app = app
        super.init()
        app.controller.send = { [weak self] line in self?.deliver(line) }
    }

    /// Ersetzt den WebSocket der Seite. Gleich nach dem Öffnen fragt sie wie auf der Box
    /// den Zustand ab.
    static let socketShim = """
    (() => {
      const post = t => window.webkit.messageHandlers.sid.postMessage(String(t));
      class BridgeSocket {
        constructor() {
          this.readyState = 0;
          window.__sidSocket = this;
          setTimeout(() => { this.readyState = 1; if (this.onopen) this.onopen(); post('state'); }, 0);
        }
        send(text) { if (this.readyState === 1) post(text); }
        close() {}
      }
      window.WebSocket = BridgeSocket;
      window.__sidReceive = text => {
        const s = window.__sidSocket;
        if (s && s.onmessage) s.onmessage({ data: text });
      };
    })();
    """

    /// Dazu: kein Zoom. „Ton im Browser" braucht es nicht, der Ton kommt direkt aus dem Gerät. Das
    /// Encoder-Board gibt es hier auch nicht, und die App sendet kein MIDI — also auch
    /// kein „Noten auch an MIDI OUT".
    static let appStyle = """
    (() => {
      const style = document.createElement('style');
      style.textContent = '#snd, #enc, .row:has(#thru) { display: none !important; }'
        + ' html { touch-action: manipulation; }';
      document.head.appendChild(style);
      // Kein Zoom in der App: iOS zoomt sonst beim Antippen jedes Felds heran (Schrift
      // unter 16 px) und beim Doppeltipp.
      const viewport = document.querySelector('meta[name=viewport]');
      if (viewport) viewport.content += ',maximum-scale=1,user-scalable=no';
    })();
    """

    private func deliver(_ line: String) {
        guard let webView else { return }
        webView.evaluateJavaScript("window.__sidReceive(\(Patch.quoted(line)))", completionHandler: nil)
    }

    // MARK: Befehle aus der Seite

    func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
        guard let line = message.body as? String else { return }
        if line.hasPrefix("ping ") {
            deliver("pong " + line.dropFirst(5))
        } else if line.hasPrefix("audio ") {
            // nur für das Vorhören im Browser der Box
        } else {
            app.controller.handle(line)
        }
    }

    // MARK: Die Adressen des Webservers der Box

    func webView(_ webView: WKWebView, start task: WKURLSchemeTask) {
        let url = task.request.url ?? URL(string: "sid://box/")!
        let body: Data
        let type: String
        var status = 200
        var afterwards: (() -> Void)?
        switch url.path {
        case "", "/", "/index.html":
            guard let file = Bundle.main.url(forResource: "index", withExtension: "html"),
                  let data = try? Data(contentsOf: file) else {
                task.didFailWithError(URLError(.fileDoesNotExist))
                return
            }
            body = data
            type = "text/html; charset=utf-8"
        case "/api/schema":
            body = Data(String(cString: sid_schema_json()).utf8)
            type = "application/json"
        case "/api/backup":
            body = Data(app.controller.store.backupJSON().utf8)
            type = "application/json"
        case "/api/restore" where task.request.httpMethod == "POST":
            // Wie on_restore() der Box: gleich antworten, das Ergebnis kommt als `msg`.
            let upload = Self.body(of: task.request)
            type = "text/plain; charset=utf-8"
            if upload.isEmpty || upload.count > 64 * 1024 {
                status = 413
                body = Data("Datei leer oder größer als 64 KB".utf8)
            } else {
                status = 202
                body = Data("Presets werden geladen …".utf8)
                afterwards = { [app] in app.controller.restorePresets(upload) }
            }
        default:
            task.didFailWithError(URLError(.fileDoesNotExist))
            return
        }
        let response = HTTPURLResponse(url: url, statusCode: status, httpVersion: "HTTP/1.1",
                                       headerFields: ["Content-Type": type, "Cache-Control": "no-cache"])!
        task.didReceive(response)
        task.didReceive(body)
        task.didFinish()
        if let afterwards { DispatchQueue.main.async(execute: afterwards) }
    }

    /// Der Rumpf eines POST; WebKit liefert ihn je nach Größe als Daten oder als Stream.
    private static func body(of request: URLRequest) -> Data {
        if let data = request.httpBody { return data }
        guard let stream = request.httpBodyStream else { return Data() }
        stream.open()
        defer { stream.close() }
        var data = Data()
        var buffer = [UInt8](repeating: 0, count: 16 * 1024)
        while stream.hasBytesAvailable {
            let n = stream.read(&buffer, maxLength: buffer.count)
            if n <= 0 { break }
            data.append(buffer, count: n)
            if data.count > 64 * 1024 { break }
        }
        return data
    }

    func webView(_ webView: WKWebView, stop task: WKURLSchemeTask) {}

    /// „Alle Presets als Datei sichern" fragt hier nach dem Ort, statt herunterzuladen.
    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
                 decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        if action.request.url?.path == "/api/backup" {
            decisionHandler(.cancel)
            app.exportBackup()
            return
        }
        decisionHandler(.allow)
    }

    #if os(macOS)
    /// Sonst landen Tasten (A–K spielt die Klaviatur) nicht in der Seite, bis man einmal
    /// hineinklickt.
    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        webView.window?.makeFirstResponder(webView)
    }

    /// Für „Presets aus Datei laden": ohne diese Methode öffnet `<input type=file>` unter
    /// macOS keinen Dialog. iOS bringt seinen eigenen mit.
    func webView(_ webView: WKWebView, runOpenPanelWith parameters: WKOpenPanelParameters,
                 initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping ([URL]?) -> Void) {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.json]
        panel.allowsMultipleSelection = parameters.allowsMultipleSelection
        completionHandler(panel.runModal() == .OK ? panel.urls : nil)
    }
    #endif

    // MARK: confirm() der Seite

    func webView(_ webView: WKWebView, runJavaScriptConfirmPanelWithMessage message: String,
                 initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping (Bool) -> Void) {
        Task { @MainActor in completionHandler(await Dialogs.confirm(message, "")) }
    }

    func webView(_ webView: WKWebView, runJavaScriptAlertPanelWithMessage message: String,
                 initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping () -> Void) {
        Task { @MainActor in
            await Dialogs.show(message, "")
            completionHandler()
        }
    }
}
