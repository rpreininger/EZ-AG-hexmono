// Der Web-Editor der Box, unverändert (Firmware/web/index.html), im Fenster des Plug-ins.
//
// Wie in der Mac-App: die Seite kommt aus dem Plug-in selbst (JUCE-Resource-Provider für
// /, /api/schema und /api/backup), den WebSocket ersetzt eine Brücke mit denselben
// Textzeilen. Dazu, weil JUCEs WebView keine eigenen Dialoge zeigt: Rückfragen über
// sidAsk(), „Presets aus Datei laden" und „Alle Presets als Datei sichern" über
// Dateidialoge des Plug-ins.

#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "SIDSynthProcessor.h"

class SIDSynthEditor : public juce::AudioProcessorEditor {
public:
    explicit SIDSynthEditor(SIDSynthProcessor& processor);
    ~SIDSynthEditor() override;

    void resized() override;

    /// Für EditorTest.cpp: JavaScript in der Seite ausführen.
    juce::WebBrowserComponent& browser() { return *browser_; }

private:
    juce::WebBrowserComponent::Options options();
    std::optional<juce::WebBrowserComponent::Resource> resource(const juce::String& url);
    void ask(const juce::var& request);
    void saveBackup();

    SIDSynthProcessor& processor_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::unique_ptr<juce::WebBrowserComponent> browser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SIDSynthEditor)
};
