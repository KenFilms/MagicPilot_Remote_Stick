/*
 * MagicPilot Remote Stick
 *
 * Ken Friedl, 2026-10-06 initial version.
 * 
 * Timecode display and record start/stop for Blackmagic cameras on the
 * M5StickC and StickS3. Talks to the camera over Bluetooth LE with
 * the Blackmagic Camera Control protocol, using the ESP-IDF Bluedroid GATT client.
 * The BLE connection code follows https://github.com/KenFilms/MagicPilot_Remote
 *
 * Buttons
 *   Main screen:  A = connect, or start/stop recording once connected
 *                 B = switch between timecode and clip counter
 *   PIN screen:   B = next digit value, A = accept digit / submit after the 6th,
 *                 hold B = back one digit
 */

#include "Board.h"
#include "CameraBluetooth.h"
#include "RemoteUI.h"

void setup() {
    MagicPilot::initializeHardware();
    Serial.begin(115200);
    MagicPilot::initializeBluetooth();
    MagicPilot::drawScreen();
    Serial.println("[BOOT] MagicPilot Remote Stick ready; press A to scan");
}

void loop() {
    M5.update();
    MagicPilot::processPendingPin();
    const bool enteringPin = MagicPilot::updateUserInterface();
    delay(enteringPin ? 10 : 5);
}
