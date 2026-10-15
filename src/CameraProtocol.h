#pragma once

#include "RemoteState.h"
#include <stddef.h>

namespace MagicPilot {

// Packet layout: dest, length, command, reserved, category, parameter, type, operation, then the value.
static const uint8_t TRANSPORT_PACKET[] = {0xFF, 0x05, 0x00, 0x00, 0x0A, 0x01, 0x01, 0x00, 0, 0, 0, 0};
static const uint8_t TIMECODE_SOURCE_PACKET[] = {0xFF, 0x05, 0x00, 0x00, 0x04, 0x07, 0x01, 0x00, 0, 0, 0, 0};

static uint32_t pendingTimecodeCount = 0;
static bool pendingTimecode = false;

static int32_t readInt32(const uint8_t* data) {
    return (int32_t)((uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24));
}

static int16_t readInt16(const uint8_t* data) {
    return (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static size_t alignedPacketLength(uint8_t dataLength) {
    return 4 + ((dataLength + 3) & ~((size_t)3));
}

static bool startsControlPacket(const uint8_t* bytes, size_t length, size_t offset) {
    return offset + 4 <= length && bytes[offset] == 0xFF && bytes[offset + 2] == 0;
}

static bool validBcd(uint8_t value, uint8_t limit) {
    const uint8_t high = value >> 4;
    const uint8_t low = value & 0x0F;
    return high <= 9 && low <= 9 && high * 10 + low < limit;
}

static uint8_t bcdToInt(uint8_t value) {
    return (uint8_t)((value >> 4) * 10 + (value & 0x0F));
}

// Assumes 30 fps while the rate is unknown.
static uint32_t activeFps() {
    return fileFps > 0 && fileFps <= 60 ? fileFps : 30;
}

static uint32_t timecodeFrameCount(const uint8_t* bytes) {
    const uint32_t hours = bcdToInt(bytes[3]);
    const uint32_t minutes = bcdToInt(bytes[2]);
    const uint32_t seconds = bcdToInt(bytes[1]);
    const uint32_t frames = bcdToInt(bytes[0]);
    return (((hours * 60 + minutes) * 60 + seconds) * activeFps()) + frames;
}

// Rejects invalid values and one-off jumps (packets arrive every ~2 frames).
static void setTimecode(const uint8_t* bytes) {
    if (!validBcd(bytes[0], 60) || !validBcd(bytes[1], 60) || !validBcd(bytes[2], 60) || !validBcd(bytes[3], 24)) return;
    const uint32_t frameCount = timecodeFrameCount(bytes);
    if (haveTimecode) {
        const uint32_t previous = timecodeFrameCount((const uint8_t*)timecodeBytes);
        const uint32_t fps = activeFps();
        if (frameCount > previous + fps * 3 || previous > frameCount + fps * 3) {
            if (!pendingTimecode || frameCount <= pendingTimecodeCount || frameCount > pendingTimecodeCount + 4) {
                pendingTimecode = true;
                pendingTimecodeCount = frameCount;
                return;
            }
        }
        pendingTimecode = false;
    }
    for (uint8_t i = 0; i < 4; ++i) timecodeBytes[i] = bytes[i];
    haveTimecode = true;
    timecodeDirty = true;
    if (transportMode == 2) clipLive = true;
}

static void parseControlPackets(const uint8_t* bytes, size_t length) {
    size_t offset = 0;
    while (offset + 4 <= length) {
        const uint8_t* packet = bytes + offset;
        const uint8_t payloadLength = packet[1];
        const size_t rawPacketLength = 4 + payloadLength;
        const size_t paddedPacketLength = alignedPacketLength(payloadLength);
        const size_t remainingLength = length - offset;
        if (rawPacketLength > remainingLength || packet[2] != 0) break;

        // The camera may or may not pad packets to 4 bytes, so decide by where the next packet starts.
        size_t packetLength = rawPacketLength;
        if (paddedPacketLength > rawPacketLength && paddedPacketLength <= remainingLength) {
            const bool rawNext = startsControlPacket(bytes, length, offset + rawPacketLength);
            const bool paddedNext = startsControlPacket(bytes, length, offset + paddedPacketLength);
            if (paddedNext && !rawNext) packetLength = paddedPacketLength;
            else if (remainingLength == paddedPacketLength) packetLength = paddedPacketLength;
        }
        if (payloadLength >= 4) {
            const uint8_t category = packet[4];
            const uint8_t parameter = packet[5];
            const uint8_t type = packet[6];
            const uint8_t* data = packet + 8;
            const size_t valueLength = payloadLength - 4;
            if (category == 1 && parameter == 9 && type == 2 && valueLength >= 2) {
                fileFps = readInt16(data);
            } else if (category == 10 && parameter == 1 && valueLength >= 1) {
                const uint8_t previousMode = transportMode;
                transportMode = data[0];
                // The clip counter is only valid once a timecode packet arrives after the change.
                if (transportMode != previousMode) clipLive = false;
                transportDirty = true;
            } else if (category == 4 && parameter == 7 && valueLength >= 1) {
                timecodeSource = data[0];
                sourceDirty = true;
            } else if (category == 9 && parameter == 4 && valueLength >= 4) {
                setTimecode(data);
            }
        }
        offset += packetLength;
    }
}

}