#pragma once

#include <Arduino.h>
#if defined(MAGICPILOT_STICK_S3) || defined(CONFIG_IDF_TARGET_ESP32S3)
#include <M5Unified.h>
#else
#include <M5StickC.h>
#endif

namespace MagicPilot {

static int screenWidth = 160, screenHeight = 80;

static void initializeHardware() {
#if defined(MAGICPILOT_STICK_S3) || defined(CONFIG_IDF_TARGET_ESP32S3)
    auto config = M5.config();
    config.internal_imu = false;
    config.internal_mic = false;
    config.internal_spk = false;
    M5.begin(config);
    M5.Lcd.setBrightness(180);
#else
    M5.begin();
    M5.Axp.ScreenBreath(11);
#endif
    M5.Lcd.setRotation(3);
    screenWidth = M5.Lcd.width();
    screenHeight = M5.Lcd.height();
}

}