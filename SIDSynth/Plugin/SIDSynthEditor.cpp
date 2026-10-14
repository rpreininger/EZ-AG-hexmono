#include "SIDSynthEditor.h"

#include "BinaryData.h"
#include "schema.h"

namespace {

/// Vor der Seite geladen. Ersetzt den WebSocket, confirm(), den POST auf /api/restore und
/// den Download von /api/backup — alles über die Brücke von JUCE.
constexpr const char* kBridge = R"JS(
(() => {
  const backend = window.__JUCE__.backend;
  const post = text => backend.emitEvent('sidCommand', String(text));

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
  backend.addEventListener('sidLine', text => {
    const s = window.__sidSocket;
    if (s && s.onmessage) s.onmessage({ data: text });
  });

  // Rückfragen: der Editor wartet auf ein Promise, das Plug-in zeigt den Dialog.
  let asked = 0;
  const waiting = new Map();
  window.sidAsk = text => new Promise(resolve => {
    const id = ++asked;
    waiting.set(id, resolve);
    backend.emitEvent('sidAsk', { id, text });
  });
  backend.addEventListener('sidAnswer', a => {
    const resolve = waiting.get(a.id);
    waiting.delete(a.id);
    if (resolve) resolve(!!a.ok);
  });

  // „Presets aus Datei laden": der Inhalt geht über die Brücke statt per POST.
  const fetchOriginal = window.fetch.bind(window);
  window.fetch = (url, init) => {
    if (String(url).endsWith('/api/restore') && init && init.method === 'POST') {
      backend.emitEvent('sidRestore', String(init.body));
      return Promise.resolve(new Response('Presets werden geladen …', { status: 202 }));
    }
    return fetchOriginal(url, init);
  };

  // „Alle Presets als Datei sichern": das Plug-in fragt nach dem Ort.
  document.addEventListener('click', e => {
    const link = e.target.closest && e.target.closest('a[href="/api/backup"]');
    if (!link) return;
    e.preventDefault();
    backend.emitEvent('sidBackup', '');
  }, true);

  // Was es im Plug-in nicht gibt: Ton im Browser, MIDI OUT, EZ-AG-Filter, Encoder-Board,
  // Startplatz der Box.
  document.addEventListener('DOMContentLoaded', () => {
    const style = document.createElement('style');
    style.textContent = '#snd, #enc, .row:has(#thru), .row:has(#ezag), .row:has(#start),'
      + ' .row:has(#knob1), .row:has(#knob2), .row:has(#knob3) { display: none !important; }';
    document.head.appendChild(style);
  });
})();
)JS";

/// Dasselbe Schema wie /api/schema der Box, aus schema.h.
juce::String schemaJson() {
    struct Out {
        std::string text;
        void print(const char* s) { text += s; }
        void print(char c) { text += c; }
    } out;
    synth::write_schema(out);
    return juce::String::fromUTF8(out.text.c_str());
}

std::vector<std::byte> bytes(const void* data, size_t size) {
    const auto* p = static_cast<const std::byte*>(data);
    return {p, p + size};
}

std::vector<std::byte> bytes(const juce::String& text) {
    return bytes(text.toRawUTF8(), text.getNumBytesAsUTF8());
}

}  // namespace

SIDSynthEditor::SIDSynthEditor(SIDSynthProcessor& processor) : AudioProcessorEditor(processor), processor_(processor) {
    browser_ = std::make_unique<juce::WebBrowserComponent>(options());
    addAndMakeVisible(*browser_);
    processor_.setEditorSender([safe = juce::Component::SafePointer<SIDSynthEditor>(this)](const juce::String& line) {
        if (safe != nullptr) safe->browser_->emitEventIfBrowserIsVisible("sidLine", line);
    });
    browser_->goToURL(juce::WebBrowserComponent::getResourceProviderRoot());

    setResizable(true, true);
    setResizeLimits(420, 480, 2400, 1800);
    setSize(1000, 760);
}

SIDSynthEditor::~SIDSynthEditor() { processor_.setEditorSender(nullptr); }

void SIDSynthEditor::resized() { browser_->setBounds(getLocalBounds()); }

juce::WebBrowserComponent::Options SIDSynthEditor::options() {
    return juce::WebBrowserComponent::Options{}
        .withBackend(juce::WebBrowserComponent::Options::Backend::defaultBackend)
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withUserScript(kBridge)
        .withResourceProvider([this](const juce::String& url) { return resource(url); })
        .withEventListener("sidCommand", [this](const juce::var& line) { processor_.handleCommand(line.toString()); })
        .withEventListener("sidAsk", [this](const juce::var& request) { ask(request); })
        .withEventListener("sidRestore", [this](const juce::var& text) { processor_.restorePresets(text.toString()); })
        .withEventListener("sidBackup", [this](const juce::var&) { saveBackup(); });
}

/// Die Adressen des Webservers der Box.
std::optional<juce::WebBrowserComponent::Resource> SIDSynthEditor::resource(const juce::String& url) {
    const juce::String path = url.upToFirstOccurrenceOf("?", false, false);
    if (path == "/" || path == "/index.html") {
        return juce::WebBrowserComponent::Resource{bytes(BinaryData::index_html, BinaryData::index_htmlSize),
                                                   "text/html"};
    }
    if (path == "/api/schema") {
        return juce::WebBrowserComponent::Resource{bytes(schemaJson()), "application/json"};
    }
    if (path == "/api/backup") {
        return juce::WebBrowserComponent::Resource{bytes(processor_.backupJson()), "application/json"};
    }
    return std::nullopt;
}

void SIDSynthEditor::ask(const juce::var& request) {
    const juce::var id = request["id"];
    auto safe = juce::Component::SafePointer<SIDSynthEditor>(this);
    juce::AlertWindow::showOkCancelBox(
        juce::MessageBoxIconType::QuestionIcon, "SID-Synth", request["text"].toString(), "OK", "Abbrechen", this,
        juce::ModalCallbackFunction::create([safe, id](int result) {
            if (safe == nullptr) return;
            auto* answer = new juce::DynamicObject();
            answer->setProperty("id", id);
            answer->setProperty("ok", result != 0);
            safe->browser_->emitEventIfBrowserIsVisible("sidAnswer", juce::var(answer));
        }));
}

void SIDSynthEditor::saveBackup() {
    chooser_ = std::make_unique<juce::FileChooser>(
        "Alle Presets sichern", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("sid-presets.json"),
        "*.json");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this](const juce::FileChooser& chooser) {
                              const juce::File file = chooser.getResult();
                              if (file != juce::File{}) file.replaceWithText(processor_.backupJson());
                          });
}
