// Welche Hardware: das bisherige ESP32-S3-DevKitC oder das Guition JC4827W543.
//
// Die Umgebung `guition` in platformio.ini setzt BOARD_GUITION_JC4827W543, alles andere
// baut für das DevKitC. Pins und Fähigkeiten stehen nur hier.
//
// Guition JC4827W543 (4,3″, 480×272, NV3041A über QSPI, ESP32-S3 N4R8), Stecker laut
// Aufdruck auf der Platine:
//   P1  GND RXD TXD +5V     Konsole UART0 / Versorgung — frei, die Konsole läuft über USB-C
//   P2  IO46 IO9 IO14 IO5   frei (IO46 ist ein Boot-Pin, besser nicht)
//   P3  IO6 IO7 IO15 IO16   DAC (BCK, LRCK, DIN) und DIN-MIDI-Eingang
//   P4  GND 3.3V IO17 IO18  dieselben Pins wie P5 — nur GND/3,3 V für weitere Module
//   P5  GND 3.3V IO17 IO18  STEMMA-QT-Buchse: Encoder-Board (SDA 17, SCL 18)
// Intern belegt: Display CS 45, SCK 47, D0–D3 21/48/40/39, Hintergrundlicht 1.

#pragma once

#if defined(BOARD_GUITION_JC4827W543)

#define SYNTH_USB_HOST_MIDI 0  // DIN-MIDI an einer UART
#define SYNTH_HAS_DISPLAY 1
#define SYNTH_HAS_ACTIVITY_LED 0  // GPIO 48 ist hier eine Datenleitung des Displays

namespace synth {
namespace board {

constexpr int kI2sBckPin = 6;    // P3
constexpr int kI2sWsPin = 7;     // P3
constexpr int kI2sDoutPin = 15;  // P3
constexpr int kSdaPin = 17;      // P5
constexpr int kSclPin = 18;      // P5
constexpr int kMidiRxPin = 16;   // P3, hinter einem Optokoppler-Modul mit 3,3 V
constexpr int kMidiTxPin = -1;   // kein MIDI OUT

constexpr int kLcdCs = 45;
constexpr int kLcdSck = 47;
constexpr int kLcdD0 = 21;
constexpr int kLcdD1 = 48;
constexpr int kLcdD2 = 40;
constexpr int kLcdD3 = 39;
constexpr int kLcdBacklight = 1;
constexpr int kLcdWidth = 480;
constexpr int kLcdHeight = 272;

}  // namespace board
}  // namespace synth

#else  // ESP32-S3-DevKitC-1, N16R8-Nachbau

#define SYNTH_USB_HOST_MIDI 1  // C2MIDI Pro an der nativen USB-Buchse
#define SYNTH_HAS_DISPLAY 0
#define SYNTH_HAS_ACTIVITY_LED 1

namespace synth {
namespace board {

constexpr int kI2sBckPin = 12;
constexpr int kI2sWsPin = 13;
constexpr int kI2sDoutPin = 14;
constexpr int kSdaPin = 8;
constexpr int kSclPin = 9;

}  // namespace board
}  // namespace synth

#endif
