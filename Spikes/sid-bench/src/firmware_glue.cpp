// Bindet den USB-Host, den Parser und den Filter der eigentlichen Firmware ein, ohne sie
// zu kopieren — der Test soll genau den Code prüfen, der in der Box läuft.

#include "../../../Firmware/src/USBConnection.cpp"
#include "../../../Firmware/src/midi_event.cpp"
#include "../../../Firmware/src/midi_filter.cpp"
