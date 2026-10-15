#pragma once

#include "Board.h"
#include "CameraBluetooth.h"
#include <string.h>

namespace MagicPilot {

static const uint16_t GREY = 0x8410;
static const uint16_t UI_BAR = 0x1082;
static const uint16_t UI_BT_BLUE = 0x2DFF;
static const uint16_t UI_SEL = 0xFD20;
static const uint16_t UI_GREEN = 0x07E0;

#if defined(MAGICPILOT_STICK_S3) || defined(CONFIG_IDF_TARGET_ESP32S3)
static const bool LARGE_UI = true;
#else
static const bool LARGE_UI = false;
#endif
static const uint8_t UI_TEXT_FONT = LARGE_UI ? 2 : 1;
static const int TOP_BAR_HEIGHT = LARGE_UI ? 24 : 14;
static const int BOTTOM_HEIGHT = LARGE_UI ? 40 : 23;
static const int UI_LINE_SPACING = LARGE_UI ? 18 : 10;
static const int PIN_CELL_WIDTH = LARGE_UI ? 34 : 24;
static const int PIN_CELL_HEIGHT = LARGE_UI ? 50 : 30;

static uint8_t clipHeldBytes[4] = {};  // last clip counter, kept after recording stops
static bool pinScreenShown = false;
static bool pinBackHandled = false;
static const uint32_t PIN_BACK_HOLD_MS = 700;

static void drawTopBar() {
    const int centerY = TOP_BAR_HEIGHT / 2;
    M5.Lcd.setTextSize(1);
    M5.Lcd.fillRect(0, 0, screenWidth, TOP_BAR_HEIGHT, UI_BAR);
    M5.Lcd.drawFastHLine(0, TOP_BAR_HEIGHT - 1, screenWidth, GREY);
    M5.Lcd.setTextFont(UI_TEXT_FONT);
    M5.Lcd.setTextDatum(ML_DATUM);
    if (cameraConnected && transportMode == 2) {
        M5.Lcd.fillCircle(8, centerY, 4, RED);
        M5.Lcd.setTextColor(RED, UI_BAR);
        M5.Lcd.drawString("REC", 16, centerY, UI_TEXT_FONT);
    } else {
        const char* mode = !cameraConnected ? "OFFLINE" : transportMode == 1 ? "PLAY" : "STBY";
        M5.Lcd.setTextColor(cameraConnected ? WHITE : GREY, UI_BAR);
        M5.Lcd.drawString(mode, 4, centerY, UI_TEXT_FONT);
    }
    if (cameraConnected) {
        M5.Lcd.setTextDatum(MR_DATUM);
        M5.Lcd.setTextColor(WHITE, UI_BAR);
        M5.Lcd.drawString(timecodeSource == 0 ? "TC" : "CLIP", screenWidth - 18, centerY, UI_TEXT_FONT);
    }
    M5.Lcd.fillCircle(screenWidth - 9, centerY, 4, cameraConnected ? UI_BT_BLUE : GREY);
}

// Fixed-width cells keep the colons in place while the digits change.
static const int TIMECODE_CHARS = 11;
static char drawnTimecode[TIMECODE_CHARS + 1] = {};
static uint16_t drawnTimecodeColor = 0;
static bool timecodeCacheValid = false;  // cleared whenever the screen is wiped

static void drawTimecode() {
    char text[20] = "--:--:--:--";
    if (cameraConnected && haveTimecode) {
        if (timecodeSource == 0) {
            snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X", timecodeBytes[3], timecodeBytes[2], timecodeBytes[1], timecodeBytes[0]);
        } else {
            // While recording, the camera sends its clip counter in the timecode data; keep the last value after stopping.
            if (transportMode == 2) {
                for (uint8_t i = 0; i < 4; ++i) clipHeldBytes[i] = clipLive ? timecodeBytes[i] : 0;
            }
            snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X", clipHeldBytes[3], clipHeldBytes[2], clipHeldBytes[1], clipHeldBytes[0]);
        }
    }
    const uint16_t color = !cameraConnected ? GREY : transportMode == 2 ? RED : WHITE;

    // Font 4 if eight digits and three colons fit the screen, else font 2.
    uint8_t font = 4;
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextFont(font);
    int digitCell = M5.Lcd.textWidth("0") + 1;
    int colonCell = M5.Lcd.textWidth(":") + 2;
#if defined(MAGICPILOT_STICK_S3) || defined(CONFIG_IDF_TARGET_ESP32S3)
    float scale = (screenWidth - 18.0f) / (8 * (digitCell - 1) + 3 * (colonCell - 2));
    if (scale > 1.5f) scale = 1.5f;
    M5.Lcd.setTextSize(scale);
    digitCell = M5.Lcd.textWidth("0") + 1;
    colonCell = M5.Lcd.textWidth(":") + 2;
#endif
    if (8 * digitCell + 3 * colonCell > screenWidth - 4) {
        font = 2;
        M5.Lcd.setTextFont(font);
        digitCell = M5.Lcd.textWidth("0") + 1;
        colonCell = M5.Lcd.textWidth(":") + 2;
    }
    const int cellHeight = M5.Lcd.fontHeight(font);

    if (!timecodeCacheValid || color != drawnTimecodeColor) {
        memset(drawnTimecode, 0, sizeof(drawnTimecode));
        drawnTimecodeColor = color;
        timecodeCacheValid = true;
    }
    M5.Lcd.setTextColor(color, BLACK);
    M5.Lcd.setTextDatum(MC_DATUM);
    const int centerY = LARGE_UI ? (TOP_BAR_HEIGHT + screenHeight - BOTTOM_HEIGHT) / 2
                                : (screenHeight - 12) / 2;
    int cellX = (screenWidth - (8 * digitCell + 3 * colonCell)) / 2;
    for (int index = 0; index < TIMECODE_CHARS && text[index] != '\0'; ++index) {
        const int cellWidth = text[index] == ':' ? colonCell : digitCell;
        if (text[index] != drawnTimecode[index]) {
            const char glyph[2] = {text[index], '\0'};
            M5.Lcd.fillRect(cellX, centerY - cellHeight / 2, cellWidth, cellHeight, BLACK);
            M5.Lcd.drawString(glyph, cellX + cellWidth / 2, centerY, font);
            drawnTimecode[index] = text[index];
        }
        cellX += cellWidth;
    }
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextFont(1);
}

// Status message and button hints.
static void drawBottom() {
    const int top = screenHeight - BOTTOM_HEIGHT;
    M5.Lcd.setTextSize(1);
    M5.Lcd.fillRect(0, top, screenWidth, BOTTOM_HEIGHT, BLACK);
    M5.Lcd.drawFastHLine(0, top - 1, screenWidth, GREY);
    M5.Lcd.setTextFont(UI_TEXT_FONT);
    M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.setTextColor(WHITE, BLACK);
    M5.Lcd.drawString(statusMessage, 4, top + 3, UI_TEXT_FONT);
    M5.Lcd.setTextColor(GREY, BLACK);
    const char* hint = cameraConnected ? (transportMode == 2 ? "A:STOP  B:TC/CLIP" : "A:REC  B:TC/CLIP")
                                       : (connecting ? "Please wait" : "A:CONNECT");
    M5.Lcd.drawString(hint, 4, top + 3 + UI_LINE_SPACING, UI_TEXT_FONT);
}

static void drawScreen() {
    M5.Lcd.fillScreen(BLACK);
    timecodeCacheValid = false;
    drawTopBar();
    drawTimecode();
    drawBottom();
}

static void drawPinScreen() {
    M5.Lcd.setTextSize(1);
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextColor(WHITE, BLACK);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.drawString("CAMERA PIN", screenWidth / 2, LARGE_UI ? 15 : 9, LARGE_UI ? 4 : 2);
    const int pinTop = (screenHeight - PIN_CELL_HEIGHT - 6) / 2;
    for (uint8_t i = 0; i < 6; ++i) {
        const int x = (screenWidth - 6 * PIN_CELL_WIDTH) / 2 + i * PIN_CELL_WIDTH;
        const bool current = i == pinPosition;
        const uint16_t color = current ? UI_SEL : (i < pinPosition ? WHITE : GREY);
        M5.Lcd.drawRoundRect(x, pinTop, PIN_CELL_WIDTH - 2, PIN_CELL_HEIGHT, 3, color);
        if (current) M5.Lcd.drawRoundRect(x + 1, pinTop + 1, PIN_CELL_WIDTH - 4, PIN_CELL_HEIGHT - 2, 3, color);
        char digit[2] = {i <= pinPosition ? (char)('0' + pinValues[i]) : '-', '\0'};
        M5.Lcd.setTextColor(color, BLACK);
        M5.Lcd.drawString(digit, x + (PIN_CELL_WIDTH - 2) / 2,
                  pinTop + PIN_CELL_HEIGHT / 2 + (LARGE_UI ? 0 : 1), LARGE_UI ? 6 : 4);
    }
    M5.Lcd.setTextColor(GREY, BLACK);
    M5.Lcd.drawString("B:+  A:OK  hold B:back", screenWidth / 2, screenHeight - 14, UI_TEXT_FONT);
}

static void handlePinButtons() {
    if (M5.BtnB.pressedFor(PIN_BACK_HOLD_MS) && !pinBackHandled) {
        pinBackHandled = true;
        if (pinPosition > 0) --pinPosition;
        drawPinScreen();
    }
    if (M5.BtnB.wasReleased()) {
        if (!pinBackHandled) {
            pinValues[pinPosition] = (pinValues[pinPosition] + 1) % 10;
            drawPinScreen();
        }
        pinBackHandled = false;
    }
    if (M5.BtnA.wasPressed()) {
        if (pinPosition < 5) {
            ++pinPosition;
            drawPinScreen();
        } else {
            uint32_t pin = 0;
            for (uint8_t i = 0; i < 6; ++i) pin = pin * 10 + pinValues[i];
            enteredPin = pin;
            pinReady = true;
            memset(pinValues, 0, sizeof(pinValues));
        }
    }
}

static void handleMainButtons() {
    if (M5.BtnA.wasPressed()) {
        if (!cameraConnected) beginConnection();
        else sendInt8Packet(TRANSPORT_PACKET, transportMode == 2 ? 0 : 2);
    }
    if (M5.BtnB.wasPressed() && cameraConnected) toggleTimecodeSource();
}

// Redraws only what changed since the last pass.
static void updateDisplay() {
    if (redrawAll) {
        redrawAll = false;
        statusDirty = transportDirty = timecodeDirty = sourceDirty = false;
        drawScreen();
        return;
    }
    if (transportDirty || sourceDirty || statusDirty) {
        transportDirty = sourceDirty = statusDirty = false;
        drawTopBar();
        drawBottom();
        drawTimecode();
        return;
    }
    if (timecodeDirty) {
        timecodeDirty = false;
        drawTimecode();
    }
}

static bool updateUserInterface() {
    if (pinRequested) {
        if (!pinScreenShown) {
            pinScreenShown = true;
            pinBackHandled = false;
            drawPinScreen();
        }
        handlePinButtons();
        return true;
    }
    if (pinScreenShown) {
        pinScreenShown = false;
        redrawAll = true;
    }
    handleMainButtons();
    updateDisplay();
    return false;
}

}