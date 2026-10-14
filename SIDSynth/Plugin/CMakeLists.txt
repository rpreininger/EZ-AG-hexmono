# SID-Synth als VST3-Plug-in für macOS — dieselbe Klangerzeugung wie Box und App.
#
#   make synth-vst3      bauen, prüfen und nach ~/Library/Audio/Plug-Ins/VST3 kopieren
#
# JUCE kommt als feste Version beim ersten Konfigurieren dazu. Lizenz: JUCE 8 steht
# unter der AGPLv3, reSID unter der GPL-2.0-or-later — das Plug-in ist damit AGPLv3.

cmake_minimum_required(VERSION 3.22)

# Vor project(): sonst gilt es nicht. Universell, damit es auch auf Intel-Macs läuft.
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
project(SIDSynthPlugin VERSION 0.1.0 LANGUAGES C CXX)


include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.14
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
file(GLOB RESID_SOURCES ${ROOT}/Firmware/lib/reSID/src/*.cc)
set(SYNTH_SOURCES
    ${RESID_SOURCES}
    ${ROOT}/Firmware/src/synth/params.cpp
    ${ROOT}/Firmware/src/synth/engine_core.cpp
    ${ROOT}/Firmware/src/synth/factory.cpp)
set(SYNTH_INCLUDES
    ${ROOT}/Firmware/lib/reSID/src
    ${ROOT}/Firmware/src/synth)

juce_add_plugin(SIDSynthPlugin
    PRODUCT_NAME "SID-Synth"
    COMPANY_NAME "Ralf Preininger"
    BUNDLE_ID de.ralfpreininger.SIDSynth.vst3
    PLUGIN_MANUFACTURER_CODE Rpre
    PLUGIN_CODE SidS
    FORMATS VST3
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Instrument Synth
    COPY_PLUGIN_AFTER_BUILD FALSE)

# Der Web-Editor der Box, unverändert, im Plug-in eingebettet.
juce_add_binary_data(SIDSynthWebEditor SOURCES ${ROOT}/Firmware/web/index.html)

target_sources(SIDSynthPlugin PRIVATE
    SIDSynthProcessor.cpp
    EditorProtocol.cpp
    SIDSynthEditor.cpp
    ParamText.cpp
    ${SYNTH_SOURCES})
target_include_directories(SIDSynthPlugin PRIVATE ${SYNTH_INCLUDES})
target_compile_features(SIDSynthPlugin PRIVATE cxx_std_17)
target_compile_definitions(SIDSynthPlugin PUBLIC
    RESID_ENABLE_INTERPOLATE
    JUCE_WEB_BROWSER=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(SIDSynthPlugin
    PRIVATE juce::juce_audio_utils juce::juce_gui_extra SIDSynthWebEditor
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags)

# Lädt das fertige VST3 wie eine DAW, spielt Noten und vergleicht mit der Klangerzeugung.
juce_add_console_app(SIDSynthPluginTest PRODUCT_NAME "SIDSynthPluginTest")
target_sources(SIDSynthPluginTest PRIVATE PluginTest.cpp ${SYNTH_SOURCES})
target_include_directories(SIDSynthPluginTest PRIVATE ${SYNTH_INCLUDES})
target_compile_features(SIDSynthPluginTest PRIVATE cxx_std_17)
target_compile_definitions(SIDSynthPluginTest PRIVATE
    RESID_ENABLE_INTERPOLATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(SIDSynthPluginTest
    PRIVATE juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags)

# Öffnet das Plug-in-Fenster mit dem Web-Editor und bedient ihn über dessen JavaScript.
juce_add_console_app(SIDSynthEditorTest PRODUCT_NAME "SIDSynthEditorTest")
target_sources(SIDSynthEditorTest PRIVATE
    EditorTest.cpp SIDSynthProcessor.cpp EditorProtocol.cpp SIDSynthEditor.cpp ParamText.cpp
    ${SYNTH_SOURCES})
target_include_directories(SIDSynthEditorTest PRIVATE ${SYNTH_INCLUDES})
target_compile_features(SIDSynthEditorTest PRIVATE cxx_std_17)
target_compile_definitions(SIDSynthEditorTest PRIVATE
    RESID_ENABLE_INTERPOLATE
    JucePlugin_Build_Standalone=0
    JUCE_WEB_BROWSER=1
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(SIDSynthEditorTest
    PRIVATE juce::juce_audio_utils juce::juce_gui_extra SIDSynthWebEditor
    PUBLIC juce::juce_recommended_config_flags)
