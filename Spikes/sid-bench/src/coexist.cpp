// Testfirmware: hält der Klang, wenn alles gleichzeitig läuft?
//
//   Kern 1   reSID (6581, FAST, 44,1 kHz) → I2S an GPIO 12/13/14
//   Kern 0   WLAN-Zugangspunkt „SID-Test", Webserver mit Reglern über WebSocket,
//            USB-Host für den C2MIDI Pro mit dem echten Gitarrenfilter
//   loop()   schreibt alle 15 s ein Preset in den Flash (und auf Knopfdruck im Browser)
//
// Gezählt werden Pufferunterläufe der I2S-Ausgabe — jeder davon wäre ein hörbarer
// Aussetzer —, die längste Lücke zwischen zwei berechneten Blöcken und die Latenz vom
// Browser zum Gerät und zurück.
//
// Solange kein DAC angeschlossen ist, kann der Browser den Klang mithören: halbe
// Abtastrate über den WebSocket, gut 150 ms verzögert — zum Hören, nicht zum Spielen.
//
// Browser: mit „SID-Test" verbinden (Passwort sid12345), dann http://192.168.4.1/

#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <driver/i2s.h>
#include <esp_partition.h>

#include <atomic>

#include "../../../Firmware/src/USBConnection.h"
#include "../../../Firmware/src/midi_event.h"
#include "../../../Firmware/src/midi_filter.h"
#include "sid.h"

namespace {

// MARK: - Verdrahtung und Einstellungen

constexpr int kI2sBckPin = 12;
constexpr int kI2sWsPin = 13;
constexpr int kI2sDoutPin = 14;

constexpr uint32_t kSampleRate = 44100;
constexpr int kBlock = 128;
constexpr int kDmaBuffers = 6;  // 6 × 128 Samples = 17 ms Vorrat
constexpr double kSidClockPal = 985248.0;

constexpr char kApSsid[] = "SID-Test";
constexpr char kApPassword[] = "sid12345";
constexpr uint32_t kSaveIntervalMs = 15000;

constexpr size_t kPreviewRing = 8192;   // Samples mit halber Rate, gut 370 ms
constexpr size_t kPreviewChunk = 512;   // je WebSocket-Nachricht, 23 ms

// MARK: - Gemeinsamer Zustand

// Vom Browser (Kern 0) und von MIDI (loop) geschrieben, von der Audio-Aufgabe gelesen.
std::atomic<uint16_t> g_cutoff{900};
std::atomic<uint8_t> g_resonance{12};
std::atomic<uint8_t> g_volume{12};
std::atomic<uint8_t> g_filter_mode{0x30};  // Tief- und Bandpass
std::atomic<bool> g_arpeggio{true};
std::atomic<uint8_t> g_note{45};
std::atomic<bool> g_gate{false};
std::atomic<uint32_t> g_triggers{0};
std::atomic<bool> g_save_requested{false};

// Von der Audio-Aufgabe einmal pro Sekunde veröffentlicht.
struct AudioStats {
    float load_avg = 0;
    float load_max = 0;
    uint32_t max_gap_us = 0;
};
portMUX_TYPE g_stats_mux = portMUX_INITIALIZER_UNLOCKED;
AudioStats g_stats;
std::atomic<uint32_t> g_underruns{0};
std::atomic<uint32_t> g_worst_gap_us{0};  // seit dem Start
std::atomic<uint32_t> g_gap_probe_us{0};  // längste Lücke seit dem letzten Zurücksetzen

// Mithören im Browser: die Audio-Aufgabe schreibt, preview_task() liest und verschickt.
int16_t g_preview[kPreviewRing];
std::atomic<uint32_t> g_preview_write{0};
std::atomic<uint32_t> g_listener{0};  // WebSocket-Client, der zuhört; 0 = niemand
std::atomic<uint32_t> g_preview_sent{0};
std::atomic<uint32_t> g_preview_dropped{0};
std::atomic<uint32_t> g_browser_gaps{0};  // vom Browser gemeldet: Puffer leergelaufen
std::atomic<uint32_t> g_ws_controls{0};   // Reglerbefehle seit dem letzten Status

reSID::SID g_sid;
uint16_t g_note_freq[128];
QueueHandle_t g_i2s_events = nullptr;

// MARK: - SID

namespace reg {
constexpr uint8_t kFreqLo = 0x00, kFreqHi = 0x01, kPwLo = 0x02, kPwHi = 0x03;
constexpr uint8_t kControl = 0x04, kAttackDecay = 0x05, kSustainRelease = 0x06;
constexpr uint8_t kFcLo = 0x15, kFcHi = 0x16, kResFilt = 0x17, kModeVol = 0x18;
}  // namespace reg

constexpr uint8_t kGate = 0x01, kSaw = 0x20, kPulse = 0x40;

void write_voice(int voice, uint16_t freq, uint16_t pw, uint8_t control) {
    const uint8_t base = static_cast<uint8_t>(voice * 7);
    g_sid.write(base + reg::kFreqLo, freq & 0xFF);
    g_sid.write(base + reg::kFreqHi, freq >> 8);
    g_sid.write(base + reg::kPwLo, pw & 0xFF);
    g_sid.write(base + reg::kPwHi, (pw >> 8) & 0x0F);
    g_sid.write(base + reg::kControl, control);
}

void sid_begin() {
    g_sid.set_chip_model(reSID::MOS6581);
    g_sid.set_sampling_parameters(kSidClockPal, reSID::SAMPLE_FAST, kSampleRate);
    g_sid.reset();
    for (int voice = 0; voice < 3; ++voice) {
        const uint8_t base = static_cast<uint8_t>(voice * 7);
        g_sid.write(base + reg::kAttackDecay, 0x09);
        g_sid.write(base + reg::kSustainRelease, 0xA6);
    }
}

/// Registerwerte für den nächsten Block, etwa 340 mal pro Sekunde.
void update_registers(uint32_t sample_pos) {
    static uint32_t seen_triggers = 0;

    const float t = static_cast<float>(sample_pos) / kSampleRate;
    const uint16_t pw = static_cast<uint16_t>(2048 + 1600 * sinf(t * 2.0f * PI * 0.7f));

    uint8_t note;
    bool gate;
    if (g_arpeggio.load()) {
        static constexpr uint8_t kPattern[] = {45, 52, 57, 60, 64, 60, 57, 52};
        const uint32_t step_samples = kSampleRate / 8;
        note = kPattern[(sample_pos / step_samples) % sizeof(kPattern)];
        gate = sample_pos % step_samples < step_samples * 3 / 4;
    } else {
        note = g_note.load();
        gate = g_gate.load();
        // Neuer Anschlag: einen Block lang Gate aus, damit die Hüllkurve neu startet.
        const uint32_t triggers = g_triggers.load();
        if (triggers != seen_triggers) {
            seen_triggers = triggers;
            gate = false;
        }
    }

    const uint8_t g = gate ? kGate : 0;
    const uint8_t low = note >= 12 ? note - 12 : note;
    write_voice(0, g_note_freq[note], pw, kSaw | g);
    write_voice(1, g_note_freq[note] + 4, pw, kPulse | g);
    write_voice(2, g_note_freq[low], 4095 - pw, kSaw | g);

    const uint16_t cutoff = g_cutoff.load();
    g_sid.write(reg::kFcLo, cutoff & 0x07);
    g_sid.write(reg::kFcHi, (cutoff >> 3) & 0xFF);
    g_sid.write(reg::kResFilt, static_cast<uint8_t>((g_resonance.load() << 4) | 0x07));
    g_sid.write(reg::kModeVol, static_cast<uint8_t>(g_filter_mode.load() | g_volume.load()));
}

// MARK: - Audio-Aufgabe (Kern 1)

void i2s_begin() {
    i2s_config_t config = {};
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
    config.sample_rate = kSampleRate;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    config.dma_buf_count = kDmaBuffers;
    config.dma_buf_len = kBlock;
    config.use_apll = false;
    config.tx_desc_auto_clear = true;
    i2s_driver_install(I2S_NUM_0, &config, 32, &g_i2s_events);

    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = kI2sBckPin;
    pins.ws_io_num = kI2sWsPin;
    pins.data_out_num = kI2sDoutPin;
    pins.data_in_num = I2S_PIN_NO_CHANGE;
    i2s_set_pin(I2S_NUM_0, &pins);
}

void audio_task(void*) {
    static short mono[kBlock];
    static int16_t frame[kBlock * 2];

    const double cpu_hz = getCpuFrequencyMhz() * 1e6;
    const double block_cycles = cpu_hz * kBlock / kSampleRate;
    const uint32_t blocks_per_second = kSampleRate / kBlock;
    const uint32_t start_ms = millis();
    bool settled = false;  // Unterläufe vor dem ersten vollen Puffer zählen nicht

    uint32_t sample_pos = 0;
    uint32_t blocks = 0;
    uint64_t cycles_sum = 0;
    uint32_t cycles_max = 0;
    uint32_t gap_max = 0;
    uint32_t last_block_us = micros();

    for (;;) {
        const uint32_t now_us = micros();
        const uint32_t gap = now_us - last_block_us;
        last_block_us = now_us;
        if (settled && gap > gap_max) gap_max = gap;
        if (settled && gap > g_gap_probe_us.load()) g_gap_probe_us.store(gap);

        const uint32_t t0 = ESP.getCycleCount();
        update_registers(sample_pos);
        reSID::cycle_count delta = kBlock * 32;
        g_sid.clock(delta, mono, kBlock);
        for (int s = 0; s < kBlock; ++s) {
            frame[2 * s] = mono[s];
            frame[2 * s + 1] = mono[s];
        }
        const uint32_t cycles = ESP.getCycleCount() - t0;
        sample_pos += kBlock;

        if (g_listener.load() != 0) {
            uint32_t w = g_preview_write.load(std::memory_order_relaxed);
            for (int s = 0; s < kBlock; s += 2) {
                g_preview[w++ % kPreviewRing] = static_cast<int16_t>((mono[s] + mono[s + 1]) / 2);
            }
            g_preview_write.store(w, std::memory_order_release);
        }

        size_t written = 0;
        i2s_write(I2S_NUM_0, frame, sizeof(frame), &written, portMAX_DELAY);

        i2s_event_t event;
        while (xQueueReceive(g_i2s_events, &event, 0) == pdTRUE) {
            if (event.type == I2S_EVENT_TX_Q_OVF && settled) g_underruns.fetch_add(1);
        }
        if (!settled && millis() - start_ms > 1000) settled = true;

        ++blocks;
        cycles_sum += cycles;
        if (cycles > cycles_max) cycles_max = cycles;
        if (blocks >= blocks_per_second) {
            AudioStats stats;
            stats.load_avg = static_cast<float>(100.0 * cycles_sum / blocks / block_cycles);
            stats.load_max = static_cast<float>(100.0 * cycles_max / block_cycles);
            stats.max_gap_us = gap_max;
            portENTER_CRITICAL(&g_stats_mux);
            g_stats = stats;
            portEXIT_CRITICAL(&g_stats_mux);
            if (gap_max > g_worst_gap_us.load()) g_worst_gap_us.store(gap_max);
            blocks = 0;
            cycles_sum = 0;
            cycles_max = 0;
            gap_max = 0;
        }
    }
}

AsyncWebSocket g_ws("/ws");

/// Kern 0, eigene Aufgabe, damit das Speichern in loop() den Strom nicht aufhält.
void preview_task(void*) {
    static int16_t chunk[kPreviewChunk];
    uint32_t read = 0;
    for (;;) {
        const uint32_t listener = g_listener.load();
        const uint32_t write = g_preview_write.load(std::memory_order_acquire);
        if (listener == 0) {
            read = write;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (write - read > kPreviewRing - kPreviewChunk) {  // zu weit zurück: aufholen
            read = write - kPreviewChunk * 2;
            g_preview_dropped.fetch_add(1);
        }
        if (write - read < kPreviewChunk) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        for (size_t i = 0; i < kPreviewChunk; ++i) chunk[i] = g_preview[(read + i) % kPreviewRing];
        read += kPreviewChunk;
        if (g_ws.availableForWrite(listener) &&
            g_ws.binary(listener, reinterpret_cast<const uint8_t*>(chunk), sizeof(chunk))) {
            g_preview_sent.fetch_add(1);
        } else {
            g_preview_dropped.fetch_add(1);
        }
    }
}

AudioStats read_stats() {
    portENTER_CRITICAL(&g_stats_mux);
    const AudioStats stats = g_stats;
    portEXIT_CRITICAL(&g_stats_mux);
    return stats;
}

// MARK: - USB-MIDI mit dem echten Gitarrenfilter

midifilter::Config ezag_config() {
    // Wie in Firmware/src/main.cpp.
    midifilter::Config config;
    config.lowest_input_channel = 0;
    config.highest_input_channel = 5;
    config.output_channel = 0;
    config.monophonic = true;
    config.priority = midifilter::NotePriority::Last;
    config.legato = true;
    config.retrigger_same_note = true;
    config.fallback_to_held_note = false;
    config.fret_mode = midifilter::FretMode::Velocity;
    config.velocity_threshold = 20;
    config.suppress_hammer_ons = true;
    return config;
}

USBConnection g_usb;
midifilter::Parser g_parser;
midifilter::Filter g_filter(ezag_config());
uint32_t g_midi_in = 0;
uint32_t g_notes_played = 0;
bool g_usb_ok = false;

uint8_t midi_length_for_cin(uint8_t cin) {
    switch (cin) {
        case 0x5: case 0xF: return 1;
        case 0x2: case 0x6: case 0xC: case 0xD: return 2;
        case 0x3: case 0x4: case 0x7: case 0x8: case 0x9: case 0xA: case 0xB: case 0xE: return 3;
        default: return 0;
    }
}

void play(const midifilter::Result& result) {
    for (uint8_t i = 0; i < result.count; ++i) {
        const midifilter::Event& event = result.events[i];
        if (event.is_note_on()) {
            g_arpeggio.store(false);
            g_note.store(event.note());
            g_gate.store(true);
            g_triggers.fetch_add(1);
            ++g_notes_played;
            Serial.printf("MIDI  Note On  %3u  v%3u\n", event.note(), event.velocity());
        } else if (event.is_note_off()) {
            if (event.note() == g_note.load()) g_gate.store(false);
        }
    }
}

/// Läuft in loop(), aufgerufen aus g_usb.task().
void on_midi(void*, const uint8_t* data, size_t length) {
    if (length < 2) return;
    uint8_t midi_length = midi_length_for_cin(data[0] & 0x0F);
    if (midi_length == 0) return;
    if (midi_length > length - 1) midi_length = static_cast<uint8_t>(length - 1);
    ++g_midi_in;

    const uint32_t now = micros();
    midifilter::Event event;
    for (uint8_t i = 0; i < midi_length; ++i) {
        if (g_parser.feed(data[i + 1], now, event)) play(g_filter.process(event));
    }
}

void on_usb_connect(void*) { Serial.println("USB   Geraet gefunden"); }
void on_usb_disconnect(void*) { Serial.println("USB   Geraet weg"); }

// MARK: - Flash

struct SaveResult {
    uint32_t duration_us = 0;
    uint32_t underruns = 0;
    uint32_t gap_us = 0;
    bool ok = false;
};
SaveResult g_last_save;
SaveResult g_last_erase;
uint32_t g_saves = 0;

/// Der ungünstigste Fall: einen beschriebenen 4-KB-Sektor löschen. LittleFS muss das
/// bei jedem Speichern früher oder später tun; auf einem frischen, leeren Flash fällt
/// es nur noch nicht auf. Benutzt die Coredump-Partition, die nichts sonst verwendet.
void erase_worst_case_test() {
    const esp_partition_t* scratch = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    if (!scratch) {
        Serial.println("FLASH keine Coredump-Partition fuer den Loeschtest");
        return;
    }
    static uint8_t junk[4096];
    for (size_t i = 0; i < sizeof(junk); ++i) junk[i] = static_cast<uint8_t>(i * 7 + 1);
    esp_partition_erase_range(scratch, 0, sizeof(junk));
    esp_partition_write(scratch, 0, junk, sizeof(junk));
    delay(50);

    const uint32_t underruns_before = g_underruns.load();
    g_gap_probe_us.store(0);
    const uint32_t t0 = micros();
    const bool ok = esp_partition_erase_range(scratch, 0, sizeof(junk)) == ESP_OK;
    const uint32_t duration = micros() - t0;
    delay(60);

    g_last_erase.duration_us = duration;
    g_last_erase.underruns = g_underruns.load() - underruns_before;
    g_last_erase.gap_us = g_gap_probe_us.load();
    g_last_erase.ok = ok;
    Serial.printf("FLASH Sektor geloescht in %.1f ms, laengste Audio-Luecke %.1f ms, "
                  "Aussetzer dabei: %u\n",
                  duration / 1000.0, g_last_erase.gap_us / 1000.0,
                  static_cast<unsigned>(g_last_erase.underruns));
}

/// Schreibt ein Preset so, wie es die echte Firmware tun würde: erst in eine
/// Hilfsdatei, dann umbenennen — das Umbenennen ist bei LittleFS atomar.
void save_preset_test(bool by_button) {
    char body[2048];
    int length = snprintf(body, sizeof(body),
                          "{\"fmt\":1,\"name\":\"Test %lu\",\"p\":{",
                          static_cast<unsigned long>(g_saves));
    for (int i = 0; i < 120 && length < static_cast<int>(sizeof(body)) - 32; ++i) {
        length += snprintf(body + length, sizeof(body) - length, "\"p%03d\":%d,", i, i * 17);
    }
    length += snprintf(body + length, sizeof(body) - length, "\"end\":0}}");

    const uint32_t underruns_before = g_underruns.load();
    g_gap_probe_us.store(0);
    const uint32_t t0 = micros();
    bool ok = false;
    File file = LittleFS.open("/preset-007.tmp", "w");
    if (file) {
        ok = file.write(reinterpret_cast<const uint8_t*>(body), length) ==
             static_cast<size_t>(length);
        file.close();
        ok = ok && LittleFS.rename("/preset-007.tmp", "/preset-007.json");
    }
    const uint32_t duration = micros() - t0;
    delay(60);  // Unterläufe werden erst gemeldet, wenn die Audio-Aufgabe wieder läuft

    g_last_save.duration_us = duration;
    g_last_save.underruns = g_underruns.load() - underruns_before;
    g_last_save.gap_us = g_gap_probe_us.load();
    g_last_save.ok = ok;
    ++g_saves;
    Serial.printf("FLASH Preset gespeichert (%s) in %.1f ms, %s, laengste Audio-Luecke %.1f ms, "
                  "Aussetzer dabei: %u\n",
                  by_button ? "Knopf" : "auto", duration / 1000.0, ok ? "ok" : "FEHLER",
                  g_last_save.gap_us / 1000.0,
                  static_cast<unsigned>(g_last_save.underruns));
    erase_worst_case_test();
}

// MARK: - Web

AsyncWebServer g_server(80);

const char kPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="de"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SID-Test</title>
<style>
body{font:16px system-ui,sans-serif;margin:16px;max-width:520px}
label{display:block;margin:16px 0 4px}input[type=range]{width:100%}
#s,#l,#t{font:13px ui-monospace,monospace;white-space:pre-wrap;background:#eee;padding:8px;border-radius:6px;margin-bottom:8px}
button{font-size:15px;padding:8px 12px;margin:4px 4px 4px 0}
</style>
<h1>SID-Test</h1>
<p><button id="snd">Ton im Browser an</button> (vorher PC-Lautst&auml;rke leise stellen)</p>
<div id="t">Ton: aus</div>
<label>Cutoff <b id="cv">900</b></label><input id="c" type="range" min="0" max="2047" value="900">
<label>Resonanz <b id="rv">12</b></label><input id="r" type="range" min="0" max="15" value="12">
<label>Lautst&auml;rke <b id="vv">12</b></label><input id="v" type="range" min="0" max="15" value="12">
<label>Filter</label>
<select id="m"><option value="16">Tiefpass<option value="32">Bandpass
<option value="48" selected>Tief + Band<option value="64">Hochpass<option value="0">aus</select>
<label><input id="a" type="checkbox" checked> Arpeggio (geht aus, sobald die Gitarre spielt)</label>
<p><button id="save">Speichertest</button><button id="ping">Latenz messen</button></p>
<div id="l">Latenz: noch nicht gemessen</div>
<div id="s">verbinde&hellip;</div>
<script>
const $=id=>document.getElementById(id);let ws,pend={},raf=0,pings=[],pingLeft=0;
let ac=null,next=0,gaps=0,late=0;
function conn(){
 ws=new WebSocket(`ws://${location.host}/ws`);ws.binaryType='arraybuffer';
 ws.onopen=()=>{$('s').textContent='verbunden';if(hearing())ws.send('audio=1')};
 ws.onclose=()=>{$('s').textContent='getrennt, neuer Versuch ...';setTimeout(conn,1000)};
 ws.onmessage=e=>{if(typeof e.data!='string'){pcm(e.data);return}
  const i=e.data.indexOf('='),k=e.data.slice(0,i),v=e.data.slice(i+1);
  if(k=='stat')$('s').textContent=v;
  else if(k=='pong')gotPong(+v);
  else if(k=='state'){const [c,r,vol,m,a]=v.split(',');
   // Den Regler, den man gerade zieht, nicht mit dem eigenen Echo zurücksetzen.
   for(const [id,x] of [['c',c],['r',r],['v',vol]])if(document.activeElement!=$(id)){$(id).value=x;$(id+'v').textContent=x}
   $('m').value=m;$('a').checked=a=='1'}};
}
function hearing(){return ac&&ac.state=='running'}
$('snd').onclick=async()=>{
 if(hearing()){await ac.suspend();ws.send('audio=0');$('snd').textContent='Ton im Browser an';$('t').textContent='Ton: aus';return}
 // iPhone: sonst schaltet der Stummschalter den Browser-Ton ab.
 if(navigator.audioSession)navigator.audioSession.type='playback';
 if(!ac)ac=new AudioContext();await ac.resume();next=0;ws.send('audio=1');$('snd').textContent='Ton im Browser aus'};
// Jede Nachricht: 512 Samples, 16 Bit, 22050 Hz. 120 ms Vorlauf gegen WLAN-Schwankungen.
function pcm(buf){if(!hearing())return;
 const d=new Int16Array(buf),b=ac.createBuffer(1,d.length,22050),ch=b.getChannelData(0);
 for(let i=0;i<d.length;i++)ch[i]=d[i]/32768;
 const now=ac.currentTime;
 if(next<now+0.01){if(next){gaps++;ws.send('tonl='+gaps)}next=now+0.12}
 else if(next>now+0.5){late++;return}
 const s=ac.createBufferSource();s.buffer=b;s.connect(ac.destination);s.start(next);next+=b.duration;
 $('t').textContent=`Ton: an, Vorlauf ${((next-now)*1000).toFixed(0)} ms, ${gaps} x leergelaufen, ${late} Bloecke zu spaet`}
function send(k,v){pend[k]=v;if(!raf)raf=requestAnimationFrame(()=>{raf=0;
 for(const k in pend)if(ws.readyState==1)ws.send(k+'='+pend[k]);pend={}})}
for(const id of ['c','r','v'])$(id).oninput=e=>{$(id+'v').textContent=e.target.value;send(id,e.target.value)};
$('m').onchange=e=>send('m',e.target.value);
$('a').onchange=e=>send('a',e.target.checked?1:0);
$('save').onclick=()=>ws.send('save=1');
$('ping').onclick=()=>{pings=[];pingLeft=30;sendPing()};
function sendPing(){ws.send('ping='+performance.now())}
function gotPong(t){pings.push(performance.now()-t);
 if(--pingLeft>0){setTimeout(sendPing,30);return}
 const s=[...pings].sort((a,b)=>a-b),avg=s.reduce((a,b)=>a+b,0)/s.length;
 const f=x=>x.toFixed(1),med=s[s.length>>1];
 $('l').textContent=`Latenz Browser -> Geraet -> Browser (${s.length} Messungen, Ton ${hearing()?'an':'aus'})\n`+
  `min ${f(s[0])} ms   Median ${f(med)} ms   max ${f(s[s.length-1])} ms   Mittel ${f(avg)} ms`;
 ws.send(`lat=${f(s[0])},${f(med)},${f(s[s.length-1])},${f(avg)},${s.length},${hearing()?1:0}`)}
conn();
</script>
)HTML";

String state_message() {
    char text[64];
    snprintf(text, sizeof(text), "state=%u,%u,%u,%u,%u", g_cutoff.load(), g_resonance.load(),
             g_volume.load(), g_filter_mode.load(), g_arpeggio.load() ? 1 : 0);
    return String(text);
}

void on_ws_event(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void* arg,
                 uint8_t* data, size_t length) {
    if (type == WS_EVT_CONNECT) {
        client->client()->setNoDelay(true);
        client->text(state_message());
        return;
    }
    if (type == WS_EVT_DISCONNECT) {
        uint32_t id = client->id();
        g_listener.compare_exchange_strong(id, 0);
        return;
    }
    if (type != WS_EVT_DATA) return;
    const auto* info = static_cast<AwsFrameInfo*>(arg);
    if (!info->final || info->index != 0 || info->len != length || info->opcode != WS_TEXT) return;

    char text[48];
    const size_t n = length < sizeof(text) - 1 ? length : sizeof(text) - 1;
    memcpy(text, data, n);
    text[n] = '\0';
    char* eq = strchr(text, '=');
    if (!eq) return;
    *eq = '\0';
    const char* key = text;
    const char* value = eq + 1;

    if (strcmp(key, "ping") == 0) {
        client->text(String("pong=") + value);
        return;
    }
    if (strcmp(key, "lat") == 0) {
        float lo = 0, median = 0, hi = 0, mean = 0;
        int count = 0, hearing = 0;
        sscanf(value, "%f,%f,%f,%f,%d,%d", &lo, &median, &hi, &mean, &count, &hearing);
        Serial.printf("WEB   Latenz (%d Messungen, Ton %s): min %.1f  Median %.1f  max %.1f  "
                      "Mittel %.1f ms\n",
                      count, hearing ? "an" : "aus", lo, median, hi, mean);
        return;
    }
    if (strcmp(key, "tonl") == 0) {
        g_browser_gaps.store(static_cast<uint32_t>(strtoul(value, nullptr, 10)));
        return;
    }
    if (strcmp(key, "audio") == 0) {
        const bool on = value[0] == '1';
        if (on) {
            g_listener.store(client->id());
        } else {
            uint32_t id = client->id();
            g_listener.compare_exchange_strong(id, 0);
        }
        Serial.printf("WEB   Ton im Browser %s\n", on ? "an" : "aus");
        return;
    }
    const long v = strtol(value, nullptr, 10);
    if (strcmp(key, "c") == 0) g_cutoff.store(static_cast<uint16_t>(constrain(v, 0, 2047)));
    else if (strcmp(key, "r") == 0) g_resonance.store(static_cast<uint8_t>(constrain(v, 0, 15)));
    else if (strcmp(key, "v") == 0) g_volume.store(static_cast<uint8_t>(constrain(v, 0, 15)));
    else if (strcmp(key, "m") == 0) g_filter_mode.store(static_cast<uint8_t>(v & 0x70));
    else if (strcmp(key, "a") == 0) g_arpeggio.store(v != 0);
    else if (strcmp(key, "save") == 0) g_save_requested.store(true);
    else return;
    if (strcmp(key, "save") != 0) g_ws_controls.fetch_add(1);

    // Alle anderen offenen Browser auf denselben Stand bringen.
    g_ws.textAll(state_message());
}

String status_text() {
    const AudioStats stats = read_stats();
    char text[768];
    snprintf(text, sizeof(text),
             "Audio   Last %.1f %% (Spitze %.1f %%), laengste Luecke %.1f ms (seit Start %.1f)\n"
             "        Aussetzer seit Start: %u\n"
             "WLAN    %u Geraet(e) verbunden, %u Browser offen, %u Reglerbefehle in 2 s\n"
             "Ton     %s, %u Pakete gesendet, %u verworfen, Browser %u x leergelaufen\n"
             "USB     %s, %u MIDI-Nachrichten, %u Noten gespielt\n"
             "Flash   %u x gespeichert, zuletzt %.1f ms, Aussetzer dabei %u; "
             "Sektor loeschen %.1f ms, Aussetzer dabei %u\n"
             "Heap    intern frei %u KB, Laufzeit %lu s",
             stats.load_avg, stats.load_max, stats.max_gap_us / 1000.0,
             g_worst_gap_us.load() / 1000.0, static_cast<unsigned>(g_underruns.load()),
             static_cast<unsigned>(WiFi.softAPgetStationNum()),
             static_cast<unsigned>(g_ws.count()), static_cast<unsigned>(g_ws_controls.exchange(0)),
             g_listener.load() != 0 ? "Browser hoert zu" : "aus",
             static_cast<unsigned>(g_preview_sent.load()),
             static_cast<unsigned>(g_preview_dropped.load()),
             static_cast<unsigned>(g_browser_gaps.load()),
             !g_usb_ok ? "Host-Start FEHLGESCHLAGEN"
                       : (g_usb.isConnected() ? "C2MIDI verbunden" : "kein Geraet"),
             static_cast<unsigned>(g_midi_in), static_cast<unsigned>(g_notes_played),
             static_cast<unsigned>(g_saves), g_last_save.duration_us / 1000.0,
             static_cast<unsigned>(g_last_save.underruns), g_last_erase.duration_us / 1000.0,
             static_cast<unsigned>(g_last_erase.underruns),
             static_cast<unsigned>(ESP.getFreeHeap() / 1024),
             static_cast<unsigned long>(millis() / 1000));
    return String(text);
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("=== SID-Zusammenspieltest ===");

    for (int note = 0; note < 128; ++note) {
        const double value = 440.0 * pow(2.0, (note - 69) / 12.0) * 16777216.0 / kSidClockPal;
        g_note_freq[note] = value > 65535.0 ? 65535 : static_cast<uint16_t>(value);
    }

    if (!LittleFS.begin(true)) Serial.println("FLASH LittleFS liess sich nicht einbinden");

    sid_begin();
    i2s_begin();
    xTaskCreatePinnedToCore(audio_task, "audio", 8192, nullptr, 18, nullptr, 1);

    g_usb.setMidiCallback(&on_midi, nullptr);
    g_usb.setConnectionCallbacks(&on_usb_connect, &on_usb_disconnect, nullptr);
    g_usb_ok = g_usb.begin();
    Serial.printf("USB   Host %s %s\n", g_usb_ok ? "laeuft" : "FEHLGESCHLAGEN",
                  g_usb.getLastError().c_str());

    WiFi.mode(WIFI_AP);
    WiFi.softAP(kApSsid, kApPassword);
    Serial.printf("WLAN  \"%s\" / %s  ->  http://%s/\n", kApSsid, kApPassword,
                  WiFi.softAPIP().toString().c_str());

    g_ws.onEvent(on_ws_event);
    g_server.addHandler(&g_ws);
    g_server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send_P(200, "text/html", kPage);
    });
    g_server.begin();
    xTaskCreatePinnedToCore(preview_task, "preview", 4096, nullptr, 2, nullptr, 0);
}

void loop() {
    g_usb.task();

    static uint32_t next_status_ms = 2000;
    static uint32_t next_save_ms = 10000;
    const uint32_t now = millis();

    const bool by_button = g_save_requested.exchange(false);
    if (by_button || static_cast<int32_t>(now - next_save_ms) >= 0) {
        next_save_ms = now + kSaveIntervalMs;
        save_preset_test(by_button);
    }

    if (static_cast<int32_t>(now - next_status_ms) >= 0) {
        next_status_ms = now + 2000;
        const String text = status_text();
        Serial.println(text);
        Serial.println();
        g_ws.textAll(String("stat=") + text);
        g_ws.cleanupClients();
    }
    delay(1);
}
