#include "CameraProtocol.h"
#include <assert.h>
#include <stdio.h>

using namespace MagicPilot;

static void resetProtocol() {
    fileFps = 0;
    transportMode = 0;
    timecodeSource = 0;
    haveTimecode = false;
    clipLive = false;
    pendingTimecode = false;
    pendingTimecodeCount = 0;
    transportDirty = timecodeDirty = sourceDirty = false;
    for (uint8_t index = 0; index < 4; ++index) timecodeBytes[index] = 0;
}

static void testIntegerDecoding() {
    const uint8_t shortValue[] = {0xFE, 0xFF};
    const uint8_t longValue[] = {0xFC, 0xFF, 0xFF, 0xFF};
    assert(readInt16(shortValue) == -2);
    assert(readInt32(longValue) == -4);
    assert(activeFps() == 30);
    fileFps = 24;
    assert(activeFps() == 24);
}

static void testPacketPadding(bool padded) {
    resetProtocol();
    const uint8_t packets[] = {
        0xFF, 6, 0, 0, 1, 9, 2, 0, 24, 0,
        0, 0,
        0xFF, 5, 0, 0, 10, 1, 1, 0, 2
    };
    if (padded) {
        parseControlPackets(packets, sizeof(packets));
    } else {
        uint8_t unpadded[sizeof(packets) - 2];
        for (size_t index = 0; index < 10; ++index) unpadded[index] = packets[index];
        for (size_t index = 12; index < sizeof(packets); ++index) unpadded[index - 2] = packets[index];
        parseControlPackets(unpadded, sizeof(unpadded));
    }
    assert(fileFps == 24);
    assert(transportMode == 2);
    assert(transportDirty);
}

static void testSourceAndTruncatedPackets() {
    resetProtocol();
    const uint8_t source[] = {0xFF, 5, 0, 0, 4, 7, 1, 0, 1};
    parseControlPackets(source, sizeof(source) - 1);
    assert(timecodeSource == 0);
    assert(!sourceDirty);
    parseControlPackets(source, sizeof(source));
    assert(timecodeSource == 1);
    assert(sourceDirty);
}

static void testTimecodeFiltering() {
    resetProtocol();
    const uint8_t initial[] = {0, 0, 0, 1};
    const uint8_t invalid[] = {0, 0x6A, 0, 1};
    const uint8_t jump[] = {0, 0, 0, 2};
    const uint8_t confirmed[] = {2, 0, 0, 2};
    setTimecode(initial);
    assert(haveTimecode && timecodeDirty);
    assert(timecodeFrameCount(initial) == 108000);
    timecodeDirty = false;
    setTimecode(invalid);
    assert(!timecodeDirty);
    setTimecode(jump);
    assert(pendingTimecode && timecodeBytes[3] == 1);
    transportMode = 2;
    setTimecode(confirmed);
    assert(!pendingTimecode && timecodeBytes[3] == 2);
    assert(timecodeBytes[0] == 2 && clipLive);

    const uint8_t stopped[] = {0xFF, 5, 0, 0, 10, 1, 1, 0, 0};
    parseControlPackets(stopped, sizeof(stopped));
    assert(transportMode == 0 && !clipLive);
}

int main() {
    testIntegerDecoding();
    testPacketPadding(false);
    testPacketPadding(true);
    testSourceAndTruncatedPackets();
    testTimecodeFiltering();
    puts("Camera protocol: all checks passed");
}