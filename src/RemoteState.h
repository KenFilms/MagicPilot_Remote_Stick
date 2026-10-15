#pragma once

#include <stdint.h>
#include <stdio.h>

namespace MagicPilot {

static volatile bool cameraConnected = false;
static volatile bool connecting = false;
static volatile bool pinRequested = false;
static volatile bool pinReady = false;
static volatile uint32_t enteredPin = 0;

static volatile bool redrawAll = true;
static volatile bool statusDirty = false;
static volatile bool transportDirty = false;
static volatile bool timecodeDirty = false;
static volatile bool sourceDirty = false;

static volatile int16_t fileFps = 0;
static volatile uint8_t transportMode = 0;  // 0 = standby, 1 = play, 2 = record
static volatile uint8_t timecodeBytes[4] = {0, 0, 0, 0};  // BCD: frames, seconds, minutes, hours
static volatile uint8_t timecodeSource = 0;               // 0 = timecode, 1 = clip counter
static volatile bool haveTimecode = false;
static bool clipLive = false;          // a timecode packet has arrived since the last transport change

static char statusMessage[24] = "Press A to connect";
static uint8_t pinValues[6] = {};
static uint8_t pinPosition = 0;

static void setStatus(const char* message) {
    snprintf(statusMessage, sizeof(statusMessage), "%s", message);
    statusDirty = true;
}

}