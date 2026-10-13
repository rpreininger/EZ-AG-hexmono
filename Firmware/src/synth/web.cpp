#include "web.h"

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include <atomic>

#include "engine.h"
#include "params.h"
#include "presets.h"
#include "web_page.h"

namespace synth {
namespace web {
namespace {

constexpr size_t kLineLength = 112;
constexpr size_t kPreviewChunk = 512;  // je Nachricht 23 ms bei 22,05 kHz

struct Line {
    char text[kLineLength];
};

AsyncWebServer g_server(80);
AsyncWebSocket g_ws("/ws");
QueueHandle_t g_inbox = nullptr;
CommandHandler g_handler = nullptr;
RestoreHandler g_restore = nullptr;
std::atomic<uint32_t> g_listener{0};  // WebSocket-Client, der den Ton hören will; 0 = keiner
std::atomic<char*> g_upload{nullptr};  // hochgeladene Sicherung auf dem Weg zu loop()

void enqueue(const char* text, size_t length) {
    Line line;
    const size_t n = length < kLineLength - 1 ? length : kLineLength - 1;
    memcpy(line.text, text, n);
    line.text[n] = '\0';
    xQueueSend(g_inbox, &line, 0);
}

void stop_listening(uint32_t client) {
    if (g_listener.compare_exchange_strong(client, 0)) engine::preview().set_enabled(false);
}

void on_ws_event(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void* arg,
                 uint8_t* data, size_t length) {
    if (type == WS_EVT_CONNECT) {
        client->client()->setNoDelay(true);
        enqueue("state", 5);  // loop() schickt allen den aktuellen Stand
        return;
    }
    if (type == WS_EVT_DISCONNECT) {
        stop_listening(client->id());
        return;
    }
    if (type != WS_EVT_DATA) return;
    const auto* info = static_cast<AwsFrameInfo*>(arg);
    if (!info->final || info->index != 0 || info->len != length || info->opcode != WS_TEXT) return;
    const char* text = reinterpret_cast<const char*>(data);

    if (length > 5 && length < 40 && memcmp(text, "ping ", 5) == 0) {
        char reply[48];
        snprintf(reply, sizeof(reply), "pong %.*s", static_cast<int>(length - 5), text + 5);
        client->text(reply);
        return;
    }
    if (length == 7 && memcmp(text, "audio ", 6) == 0) {
        if (text[6] == '1') {
            g_listener.store(client->id());
            engine::preview().set_enabled(true);
        } else {
            stop_listening(client->id());
        }
        return;
    }
    enqueue(text, length);
}

/// Kern 0, eigene Aufgabe: liest den Ringpuffer der Engine und schickt ihn an den
/// Browser, der zuhört. Hängt der Browser hinterher, wird übersprungen statt gestaut.
void preview_task(void*) {
    static int16_t chunk[kPreviewChunk];
    PreviewRing& ring = engine::preview();
    uint32_t read = ring.write_position();
    for (;;) {
        const uint32_t listener = g_listener.load();
        const uint32_t write = ring.write_position();
        if (listener == 0) {
            read = write;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (write - read > PreviewRing::kSize - kPreviewChunk) read = write - kPreviewChunk * 2;
        if (write - read < kPreviewChunk) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        for (size_t i = 0; i < kPreviewChunk; ++i) chunk[i] = ring.at(read + i);
        read += kPreviewChunk;
        if (g_ws.availableForWrite(listener)) {
            g_ws.binary(listener, reinterpret_cast<const uint8_t*>(chunk), sizeof(chunk));
        }
    }
}

// MARK: - Sicherung hochladen

/// Der Rumpf kommt in Stücken. Gesammelt wird im PSRAM; den Puffer gibt der Webserver
/// mit der Anfrage wieder frei, es sei denn, on_restore() hat ihn an loop() weitergereicht.
void on_restore_body(AsyncWebServerRequest* request, uint8_t* data, size_t length, size_t index,
                     size_t total) {
    if (index == 0 && total > 0 && total <= presets::kMaxBackupSize) {
        request->_tempObject = ps_calloc(total + 1, 1);  // mit Nullbyte am Ende
    }
    if (request->_tempObject != nullptr && index + length <= total) {
        memcpy(static_cast<char*>(request->_tempObject) + index, data, length);
    }
}

/// Antwortet gleich; das Ergebnis meldet loop() über den WebSocket als `msg`.
void on_restore(AsyncWebServerRequest* request) {
    char* text = static_cast<char*>(request->_tempObject);
    if (text == nullptr) {
        request->send(413, "text/plain", "Datei leer oder größer als 64 KB");
        return;
    }
    char* idle = nullptr;
    if (!g_upload.compare_exchange_strong(idle, text)) {
        request->send(409, "text/plain", "Es wird gerade schon eine Datei geladen");
        return;
    }
    request->_tempObject = nullptr;  // gehört jetzt loop()
    request->send(202, "text/plain", "Presets werden geladen …");
}

// MARK: - Schema

void print_string(Print& out, const char* text) {
    out.print('"');
    for (const char* c = text; *c != '\0'; ++c) {
        if (*c == '"' || *c == '\\') out.print('\\');
        out.print(*c);
    }
    out.print('"');
}

const char* kind_name(Kind kind) {
    switch (kind) {
        case Kind::Range: return "range";
        case Kind::Choice: return "choice";
        case Kind::Toggle: return "toggle";
    }
    return "range";
}

}  // namespace

void write_schema(Print& out) {
    out.printf("{\"slots\":%d,\"params\":[", presets::kSlots);
    for (size_t i = 0; i < kParamCount; ++i) {
        const Param& p = kParams[i];
        if (i > 0) out.print(',');
        out.print("{\"key\":");
        print_string(out, p.key);
        out.print(",\"label\":");
        print_string(out, p.label);
        out.print(",\"group\":");
        print_string(out, p.group);
        out.printf(",\"kind\":\"%s\",\"min\":%d,\"max\":%d,\"def\":%d,\"fmt\":\"%s\"",
                   kind_name(p.kind), p.min, p.max, p.def, format_name(p.format));
        if (p.kind == Kind::Choice) {
            out.print(",\"choices\":[\"");
            for (const char* c = p.choices; *c != '\0'; ++c) {
                if (*c == '|') {
                    out.print("\",\"");
                } else {
                    out.print(*c);
                }
            }
            out.print("\"]");
        }
        if (p.format == Format::Exp || p.format == Format::Exp0) {
            out.printf(",\"lo\":%g,\"hi\":%g,\"unit\":\"%s\"", p.lo, p.hi, p.unit);
        }
        out.print('}');
    }
    out.print("]}");
}

void begin(CommandHandler handler, RestoreHandler restore) {
    g_handler = handler;
    g_restore = restore;
    g_inbox = xQueueCreate(32, sizeof(Line));

    WiFi.mode(WIFI_AP);
    WiFi.softAP(kSsid, kPassword);
    if (MDNS.begin(kHostname)) MDNS.addService("http", "tcp", 80);

    g_ws.onEvent(on_ws_event);
    g_server.addHandler(&g_ws);
    g_server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response =
            request->beginResponse(200, "text/html", kIndexHtmlGz, kIndexHtmlGzSize);
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "no-cache");
        request->send(response);
    });
    g_server.on("/api/schema", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncResponseStream* out = request->beginResponseStream("application/json");
        write_schema(*out);
        request->send(out);
    });
    g_server.on("/api/backup", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncResponseStream* out = request->beginResponseStream("application/json");
        out->addHeader("Content-Disposition", "attachment; filename=\"sid-presets.json\"");
        presets::write_backup(*out);
        request->send(out);
    });
    g_server.on("/api/restore", HTTP_POST, on_restore, nullptr, on_restore_body);
    // Handys fragen beim Verbinden allerlei Adressen ab — alles auf den Editor lenken.
    g_server.onNotFound([](AsyncWebServerRequest* request) { request->redirect("/"); });
    g_server.begin();

    xTaskCreatePinnedToCore(preview_task, "preview", 4096, nullptr, 2, nullptr, 0);
}

void poll() {
    Line line;
    while (xQueueReceive(g_inbox, &line, 0) == pdTRUE) g_handler(line.text);

    char* upload = g_upload.load();
    if (upload != nullptr) {
        g_restore(upload, strlen(upload));
        g_upload.store(nullptr);  // erst jetzt darf die nächste Datei kommen
        free(upload);
    }

    static uint32_t next_cleanup_ms = 0;
    if (static_cast<int32_t>(millis() - next_cleanup_ms) >= 0) {
        next_cleanup_ms = millis() + 1000;
        g_ws.cleanupClients();
    }
}

void broadcast(const String& text) {
    if (g_ws.count() > 0) g_ws.textAll(text);
}

size_t clients() { return g_ws.count(); }

}  // namespace web
}  // namespace synth
