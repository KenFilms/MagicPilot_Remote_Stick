/*
 * MagicPilot Remote Stick
 *
 * Ken Friedl, 2026-10-06 initial version.
 * 
 * Timecode display and record start/stop for Blackmagic cameras on the original
 * M5StickC (80x160 screen, two buttons). Talks to the camera over Bluetooth LE with
 * the Blackmagic Camera Control protocol, using the ESP-IDF Bluedroid GATT client.
 * The BLE connection code follows https://github.com/KenFilms/MagicPilot_Remote
 *
 * Buttons
 *   Main screen:  A = connect, or start/stop recording once connected
 *                 B = switch between timecode and clip counter
 *   PIN screen:   B = next digit value, A = accept digit / submit after the 6th,
 *                 hold B = back one digit
 */

#include <M5StickC.h>
#include <esp32-hal-bt.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>
#include <esp_gatt_common_api.h>
#include <stdio.h>
#include <string.h>

static const char* CAMERA_SERVICE_UUID = "291D567A-6D75-11E6-8B77-86F30CA893D3";
static const char* OUTGOING_UUID = "5DD3465F-1AEE-4299-8493-D2ECA2F8E1BB";
static const char* INCOMING_UUID = "B864E140-76A0-416A-BF30-5876504537D9";
static const char* TIMECODE_UUID = "6D8F2110-86F1-41BF-9AFB-451D87E976C8";
static const char* STATUS_UUID = "7FE8691D-95DC-4FC5-8ABD-CA74339B51B9";
static const char* DEVICE_NAME_UUID = "FFAC0C52-C9FB-41A0-B063-CC76282EB89C";

static const uint16_t GREY = 0x8410;
static const uint16_t UI_BAR = 0x1082;
static const uint16_t UI_BT_BLUE = 0x2DFF;
static const uint16_t UI_SEL = 0xFD20;
static const uint16_t UI_GREEN = 0x07E0;

static const int SCREEN_W = 160, SCREEN_H = 80;

// Packet layout: dest, length, command, reserved, category, parameter, type, operation, then the value.
static const uint8_t TRANSPORT_PACKET[] = {0xFF, 0x05, 0x00, 0x00, 0x0A, 0x01, 0x01, 0x00, 0, 0, 0, 0};
static const uint8_t TIMECODE_SOURCE_PACKET[] = {0xFF, 0x05, 0x00, 0x00, 0x04, 0x07, 0x01, 0x00, 0, 0, 0, 0};

static esp_gatt_if_t gattInterface = ESP_GATT_IF_NONE;
static uint16_t gattConnectionId = 0;
static uint16_t serviceStartHandle = 0;
static uint16_t serviceEndHandle = 0;
static uint16_t outgoingHandle = 0;
static uint16_t incomingHandle = 0;
static uint16_t timecodeHandle = 0;
static uint16_t statusHandle = 0;
static uint16_t deviceNameHandle = 0;
static uint16_t pendingDescriptorHandle = 0;
static uint16_t notificationHandles[3] = {0, 0, 0};
static esp_gatt_char_prop_t notificationProperties[3] = {0, 0, 0};
static uint8_t notificationIndex = 0;
static esp_bd_addr_t cameraBda = {};
static esp_bd_addr_t pairingBda = {};
static esp_ble_addr_type_t cameraAddressType = BLE_ADDR_TYPE_PUBLIC;
static esp_ble_scan_params_t scanParameters = {};

static volatile bool cameraConnected = false;
static volatile bool connecting = false;
static volatile bool bleStackReady = false;
static volatile bool scanParametersReady = false;
static volatile bool cameraFound = false;
static volatile bool gattConnectionOpen = false;
static volatile bool securityReady = false;
static volatile bool serviceSearchStarted = false;
static volatile bool passkeyReplyPending = false;
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
static uint8_t clipHeldBytes[4] = {};  // last clip counter, kept after recording stops
static bool clipLive = false;          // a timecode packet has arrived since the last transport change
static uint32_t pendingTimecodeCount = 0;
static bool pendingTimecode = false;

static char statusMessage[24] = "Press A to connect";

static uint8_t pinValues[6] = {};
static uint8_t pinPosition = 0;
static bool pinScreenShown = false;
static bool pinBackHandled = false;
static const uint32_t PIN_BACK_HOLD_MS = 700;

static void setStatus(const char* message) {
    snprintf(statusMessage, sizeof(statusMessage), "%s", message);
    statusDirty = true;
}

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

static bool writeCameraPacket(const uint8_t* packet, size_t packetLength) {
    if (!cameraConnected || outgoingHandle == 0 || gattInterface == ESP_GATT_IF_NONE) return false;
    return esp_ble_gattc_write_char(gattInterface, gattConnectionId, outgoingHandle, (uint16_t)packetLength,
                                    (uint8_t*)packet, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_MITM) == ESP_OK;
}

static bool sendInt8Packet(const uint8_t* packetTemplate, uint8_t value) {
    uint8_t packet[sizeof(TRANSPORT_PACKET)];
    memcpy(packet, packetTemplate, sizeof(packet));
    packet[8] = value;
    return writeCameraPacket(packet, sizeof(packet));
}

static void markDisconnected() {
    cameraConnected = false;
    connecting = false;
    pinRequested = false;
    pinReady = false;
    passkeyReplyPending = false;
    gattConnectionId = 0;
    serviceStartHandle = serviceEndHandle = 0;
    outgoingHandle = incomingHandle = timecodeHandle = statusHandle = deviceNameHandle = 0;
    pendingDescriptorHandle = 0;
    notificationIndex = 0;
    notificationHandles[0] = notificationHandles[1] = notificationHandles[2] = 0;
    notificationProperties[0] = notificationProperties[1] = notificationProperties[2] = 0;
    gattConnectionOpen = false;
    securityReady = false;
    serviceSearchStarted = false;
    transportMode = 0;
    fileFps = 0;
    haveTimecode = false;
    pendingTimecode = false;
    clipLive = false;
    redrawAll = true;
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

static int hexNibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

// Converts a UUID string to the reversed byte order Bluedroid uses; len stays 0 if it is malformed.
static esp_bt_uuid_t uuid128FromString(const char* text) {
    esp_bt_uuid_t uuid = {};
    uint8_t bytes[16] = {};
    size_t byteCount = 0;
    for (size_t index = 0; text[index] != '\0' && byteCount < sizeof(bytes);) {
        if (text[index] == '-') { ++index; continue; }
        const int high = hexNibble(text[index]);
        const int low = hexNibble(text[index + 1]);
        if (high < 0 || low < 0) return uuid;
        bytes[byteCount++] = (uint8_t)((high << 4) | low);
        index += 2;
    }
    if (byteCount != sizeof(bytes)) return uuid;
    uuid.len = ESP_UUID_LEN_128;
    for (size_t index = 0; index < sizeof(bytes); ++index) uuid.uuid.uuid128[index] = bytes[15 - index];
    return uuid;
}

static bool uuidMatches(const esp_bt_uuid_t& candidate, const char* text) {
    const esp_bt_uuid_t expected = uuid128FromString(text);
    return candidate.len == ESP_UUID_LEN_128 && expected.len == ESP_UUID_LEN_128 &&
           memcmp(candidate.uuid.uuid128, expected.uuid.uuid128, ESP_UUID_LEN_128) == 0;
}

static bool advertisementHasCameraService(uint8_t* advertisement) {
    uint8_t length = 0;
    uint8_t* uuids = esp_ble_resolve_adv_data(advertisement, ESP_BLE_AD_TYPE_128SRV_CMPL, &length);
    if (uuids == nullptr || length < ESP_UUID_LEN_128) return false;
    const esp_bt_uuid_t service = uuid128FromString(CAMERA_SERVICE_UUID);
    for (uint8_t offset = 0; offset + ESP_UUID_LEN_128 <= length; offset += ESP_UUID_LEN_128) {
        if (memcmp(uuids + offset, service.uuid.uuid128, ESP_UUID_LEN_128) == 0) return true;
    }
    return false;
}

// Copies the advertised device name (complete or shortened) into name; empty if there is none.
static void advertisedName(uint8_t* advertisement, char* name, size_t size) {
    uint8_t nameLength = 0;
    uint8_t* nameData = esp_ble_resolve_adv_data(advertisement, ESP_BLE_AD_TYPE_NAME_CMPL, &nameLength);
    if (nameData == nullptr) nameData = esp_ble_resolve_adv_data(advertisement, ESP_BLE_AD_TYPE_NAME_SHORT, &nameLength);
    memset(name, 0, size);
    if (nameData != nullptr) memcpy(name, nameData, nameLength < size - 1 ? nameLength : size - 1);
}

// A camera advertises its name, the camera service UUID, or both.
static bool isCameraAdvertisement(uint8_t* advertisement) {
    char name[64];
    advertisedName(advertisement, name, sizeof(name));
    return strstr(name, "Blackmagic") != nullptr || strstr(name, "BMPCC") != nullptr ||
           advertisementHasCameraService(advertisement);
}

static void startCameraScan() {
    if (!connecting || !scanParametersReady) return;
    Serial.println("[BLE] scanning for Blackmagic camera (8 seconds)");
    if (esp_ble_gap_start_scanning(8) != ESP_OK) {
        connecting = false;
        setStatus("Scan failed");
    }
}

// Looks up a characteristic of the camera service; returns 0 if it is missing.
static uint16_t findCharacteristicHandle(const char* uuidText, esp_gatt_char_prop_t* properties) {
    esp_gattc_char_elem_t characteristic = {};
    uint16_t count = 1;
    const esp_bt_uuid_t uuid = uuid128FromString(uuidText);
    if (uuid.len != ESP_UUID_LEN_128 ||
        esp_ble_gattc_get_char_by_uuid(gattInterface, gattConnectionId, serviceStartHandle, serviceEndHandle, uuid,
                                       &characteristic, &count) != ESP_GATT_OK || count == 0) return 0;
    if (properties != nullptr) *properties = characteristic.properties;
    return characteristic.char_handle;
}

static void registerNextNotification();

static void discoverCameraCharacteristics() {
    esp_gatt_char_prop_t outgoingProperties = 0;
    incomingHandle = findCharacteristicHandle(INCOMING_UUID, &notificationProperties[0]);
    timecodeHandle = findCharacteristicHandle(TIMECODE_UUID, &notificationProperties[1]);
    statusHandle = findCharacteristicHandle(STATUS_UUID, &notificationProperties[2]);
    outgoingHandle = findCharacteristicHandle(OUTGOING_UUID, &outgoingProperties);
    deviceNameHandle = findCharacteristicHandle(DEVICE_NAME_UUID, nullptr);
    notificationHandles[0] = incomingHandle;
    notificationHandles[1] = timecodeHandle;
    notificationHandles[2] = statusHandle;

    Serial.printf("[BLE] characteristics: control-out=%d control-in=%d timecode=%d status=%d name=%d\n",
                  outgoingHandle != 0, incomingHandle != 0, timecodeHandle != 0, statusHandle != 0, deviceNameHandle != 0);
    if (deviceNameHandle != 0) {
        uint8_t deviceName[] = "MagicPilot Stick";
        esp_ble_gattc_write_char(gattInterface, gattConnectionId, deviceNameHandle, sizeof(deviceName) - 1, deviceName,
                                 ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_MITM);
    }
    cameraConnected = outgoingHandle != 0 && (outgoingProperties & ESP_GATT_CHAR_PROP_BIT_WRITE) != 0;
    connecting = false;
    redrawAll = true;
    setStatus(cameraConnected ? "Connected" : "Control unavailable");
    notificationIndex = 0;
    registerNextNotification();
}

// Enables notifications one characteristic at a time; each step continues from its GATT event.
static void registerNextNotification() {
    while (notificationIndex < 3) {
        const uint16_t handle = notificationHandles[notificationIndex];
        const uint8_t properties = notificationProperties[notificationIndex];
        if (handle == 0 || (properties & (ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_INDICATE)) == 0) {
            ++notificationIndex;
            continue;
        }
        if (esp_ble_gattc_register_for_notify(gattInterface, cameraBda, handle) == ESP_OK) return;
        Serial.printf("[BLE] notification registration failed for handle %u\n", handle);
        ++notificationIndex;
    }
    Serial.println("[BLE] notification setup complete");
}

// The service search needs both the open connection and finished pairing.
static void startServiceSearchIfReady() {
    if (!gattConnectionOpen || !securityReady || serviceSearchStarted) return;
    esp_bt_uuid_t service = uuid128FromString(CAMERA_SERVICE_UUID);
    serviceSearchStarted = true;
    if (esp_ble_gattc_search_service(gattInterface, gattConnectionId, &service) != ESP_OK) {
        serviceSearchStarted = false;
        connecting = false;
        setStatus("Service search failed");
    }
}

// Handles scan results and pairing (security request, PIN request, authentication result).
static void gapEventHandler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* parameter) {
    switch (event) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            scanParametersReady = parameter->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS;
            if (scanParametersReady) startCameraScan();
            else { connecting = false; setStatus("Scan setup failed"); }
            break;
        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            if (parameter->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT && connecting && !cameraFound &&
                isCameraAdvertisement(parameter->scan_rst.ble_adv)) {
                cameraFound = true;
                cameraAddressType = parameter->scan_rst.ble_addr_type;
                memcpy(cameraBda, parameter->scan_rst.bda, sizeof(cameraBda));
                Serial.printf("[BLE] selected camera %02X:%02X:%02X:%02X:%02X:%02X\n", cameraBda[0], cameraBda[1],
                              cameraBda[2], cameraBda[3], cameraBda[4], cameraBda[5]);
                setStatus("Camera found...");
                esp_ble_gap_stop_scanning();
                if (esp_ble_gattc_open(gattInterface, cameraBda, cameraAddressType, true) != ESP_OK) {
                    connecting = false;
                    setStatus("Connection failed");
                }
            } else if (parameter->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT && connecting && !cameraFound) {
                Serial.println("[BLE] scan ended without a camera; check camera pairing mode and advertising");
                connecting = false;
                setStatus("Camera not found");
            }
            break;
        case ESP_GAP_BLE_SEC_REQ_EVT:
            esp_ble_gap_security_rsp(parameter->ble_security.ble_req.bd_addr, true);
            break;
        case ESP_GAP_BLE_PASSKEY_REQ_EVT:
            Serial.println("[BLE] passkey requested; enter the camera PIN");
            memcpy(pairingBda, parameter->ble_security.ble_req.bd_addr, sizeof(pairingBda));
            pinPosition = 0;
            memset(pinValues, 0, sizeof(pinValues));
            pinReady = false;
            passkeyReplyPending = true;
            pinRequested = true;
            setStatus("Enter camera PIN");
            break;
        case ESP_GAP_BLE_AUTH_CMPL_EVT:
            if (parameter->ble_security.auth_cmpl.success) {
                Serial.println("[BLE] encrypted pairing complete");
                securityReady = true;
                startServiceSearchIfReady();
            } else {
                Serial.printf("[BLE] authentication failed: %u\n", parameter->ble_security.auth_cmpl.fail_reason);
                connecting = false;
                pinRequested = false;
                passkeyReplyPending = false;
                setStatus("Pairing failed, retry");
                esp_ble_gap_disconnect(parameter->ble_security.auth_cmpl.bd_addr);
            }
            break;
        default:
            break;
    }
}

// Drives the connection: open, pair, find the service, enable notifications, then receive data.
static void gattClientEventHandler(esp_gattc_cb_event_t event, esp_gatt_if_t interface, esp_ble_gattc_cb_param_t* parameter) {
    if (event == ESP_GATTC_REG_EVT) {
        if (parameter->reg.status != ESP_GATT_OK) {
            setStatus("GATT setup failed");
            return;
        }
        gattInterface = interface;
        scanParameters.scan_type = BLE_SCAN_TYPE_ACTIVE;
        scanParameters.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
        scanParameters.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
        scanParameters.scan_interval = 0x50;
        scanParameters.scan_window = 0x30;
        scanParameters.scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE;
        if (esp_ble_gap_set_scan_params(&scanParameters) != ESP_OK) setStatus("Scan setup failed");
        return;
    }
    if (interface != gattInterface) return;

    switch (event) {
        case ESP_GATTC_CONNECT_EVT:
            gattConnectionId = parameter->connect.conn_id;
            memcpy(cameraBda, parameter->connect.remote_bda, sizeof(cameraBda));
            Serial.println("[BLE] GATT connected; requesting encrypted pairing");
            if (esp_ble_set_encryption(cameraBda, ESP_BLE_SEC_ENCRYPT_MITM) != ESP_OK) {
                connecting = false;
                setStatus("Pairing failed, retry");
            }
            break;
        case ESP_GATTC_OPEN_EVT:
            if (parameter->open.status != ESP_GATT_OK) {
                connecting = false;
                setStatus("Connection failed");
                break;
            }
            gattConnectionId = parameter->open.conn_id;
            gattConnectionOpen = true;
            startServiceSearchIfReady();
            break;
        case ESP_GATTC_SEARCH_RES_EVT:
            if (uuidMatches(parameter->search_res.srvc_id.uuid, CAMERA_SERVICE_UUID)) {
                serviceStartHandle = parameter->search_res.start_handle;
                serviceEndHandle = parameter->search_res.end_handle;
            }
            break;
        case ESP_GATTC_SEARCH_CMPL_EVT:
            if (parameter->search_cmpl.status != ESP_GATT_OK || serviceStartHandle == 0) {
                connecting = false;
                setStatus("Camera service missing");
                break;
            }
            discoverCameraCharacteristics();
            break;
        case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
            if (parameter->reg_for_notify.status != ESP_GATT_OK || notificationIndex >= 3 ||
                parameter->reg_for_notify.handle != notificationHandles[notificationIndex]) {
                ++notificationIndex;
                registerNextNotification();
                break;
            }
            esp_bt_uuid_t cccdUuid = {};
            cccdUuid.len = ESP_UUID_LEN_16;
            cccdUuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
            esp_gattc_descr_elem_t descriptor = {};
            uint16_t count = 1;
            if (esp_ble_gattc_get_descr_by_char_handle(gattInterface, gattConnectionId, parameter->reg_for_notify.handle,
                                                       cccdUuid, &descriptor, &count) != ESP_GATT_OK || count == 0) {
                ++notificationIndex;
                registerNextNotification();
                break;
            }
            const uint8_t properties = notificationProperties[notificationIndex];
            // CCCD value: 1 = notifications, 2 = indications.
            uint8_t config[2] = {(uint8_t)((properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) ? 1 : 2), 0};
            pendingDescriptorHandle = descriptor.handle;
            if (esp_ble_gattc_write_char_descr(gattInterface, gattConnectionId, descriptor.handle, sizeof(config), config,
                                               ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_MITM) != ESP_OK) {
                pendingDescriptorHandle = 0;
                ++notificationIndex;
                registerNextNotification();
            }
            break;
        }
        case ESP_GATTC_WRITE_DESCR_EVT:
            if (pendingDescriptorHandle != 0 && parameter->write.handle == pendingDescriptorHandle) {
                if (parameter->write.status != ESP_GATT_OK) Serial.printf("[BLE] CCCD write failed: %u\n", parameter->write.status);
                pendingDescriptorHandle = 0;
                ++notificationIndex;
                registerNextNotification();
            }
            break;
        case ESP_GATTC_NOTIFY_EVT:
            parseControlPackets(parameter->notify.value, parameter->notify.value_len);
            break;
        case ESP_GATTC_DISCONNECT_EVT: {
            Serial.printf("[BLE] disconnected, reason 0x%02X\n", (unsigned)parameter->disconnect.reason);
            const bool wasConnected = cameraConnected, wasPairing = pinRequested;
            markDisconnected();
            // A pairing failure has already set its own message, so leave that on screen.
            if (wasConnected) setStatus("Disconnected");
            else if (wasPairing) setStatus("Pairing lost, retry");
            break;
        }
        default:
            break;
    }
}

static bool bluetoothInitFailure(const char* stage, esp_err_t error) {
    char message[sizeof(statusMessage)];
    snprintf(message, sizeof(message), "BT %s failed", stage);
    Serial.printf("[BLE] %s failed: %d\n", stage, error);
    setStatus(message);
    return false;
}

// Starts the BLE host and registers the GATT client; keyboard-only I/O makes the camera ask for a PIN.
static bool initializeBluetooth() {
    esp_err_t result = ESP_OK;
    if (!btStarted() && !btStart()) return bluetoothInitFailure("start", ESP_FAIL);
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        return bluetoothInitFailure("state", ESP_ERR_INVALID_STATE);
    }

    esp_bluedroid_status_t hostStatus = esp_bluedroid_get_status();
    if (hostStatus == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        result = esp_bluedroid_init();
        if (result != ESP_OK) return bluetoothInitFailure("host init", result);
        hostStatus = esp_bluedroid_get_status();
    }
    if (hostStatus == ESP_BLUEDROID_STATUS_INITIALIZED) {
        result = esp_bluedroid_enable();
        if (result != ESP_OK) return bluetoothInitFailure("host enable", result);
        hostStatus = esp_bluedroid_get_status();
    }
    if (hostStatus != ESP_BLUEDROID_STATUS_ENABLED) return bluetoothInitFailure("host", ESP_ERR_INVALID_STATE);

    esp_ble_auth_req_t authRequest = ESP_LE_AUTH_REQ_BOND_MITM;
    esp_ble_io_cap_t ioCapability = ESP_IO_CAP_IN;
    uint8_t keySize = 16;
    uint8_t keyMask = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    result = esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &authRequest, sizeof(authRequest));
    if (result != ESP_OK) return bluetoothInitFailure("auth config", result);
    result = esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &ioCapability, sizeof(ioCapability));
    if (result != ESP_OK) return bluetoothInitFailure("I/O config", result);
    result = esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &keySize, sizeof(keySize));
    if (result != ESP_OK) return bluetoothInitFailure("key size", result);
    result = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keyMask, sizeof(keyMask));
    if (result != ESP_OK) return bluetoothInitFailure("init key", result);
    result = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keyMask, sizeof(keyMask));
    if (result != ESP_OK) return bluetoothInitFailure("resp key", result);
    result = esp_ble_gatt_set_local_mtu(200);
    if (result != ESP_OK) return bluetoothInitFailure("MTU", result);

    result = esp_ble_gap_register_callback(gapEventHandler);
    if (result != ESP_OK) return bluetoothInitFailure("GAP callback", result);
    result = esp_ble_gattc_register_callback(gattClientEventHandler);
    if (result != ESP_OK) return bluetoothInitFailure("GATT callback", result);
    result = esp_ble_gattc_app_register(0);
    if (result != ESP_OK) return bluetoothInitFailure("GATT app", result);
    bleStackReady = true;
    return true;
}

// Starts scanning for the camera; the rest of the connection runs from the BLE events.
static void beginConnection() {
    if (connecting || cameraConnected) return;
    if (!bleStackReady) {
        setStatus("Bluetooth unavailable");
        return;
    }
    connecting = true;
    cameraFound = false;
    gattConnectionOpen = false;
    securityReady = false;
    serviceSearchStarted = false;
    serviceStartHandle = serviceEndHandle = 0;
    setStatus("Searching...");
    startCameraScan();
}

static void toggleTimecodeSource() {
    timecodeSource = timecodeSource == 0 ? 1 : 0;
    sourceDirty = true;
    sendInt8Packet(TIMECODE_SOURCE_PACKET, timecodeSource);
}

static void drawTopBar() {
    M5.Lcd.fillRect(0, 0, SCREEN_W, 14, UI_BAR);
    M5.Lcd.drawFastHLine(0, 13, SCREEN_W, GREY);
    M5.Lcd.setTextFont(1);
    M5.Lcd.setTextDatum(ML_DATUM);
    if (cameraConnected && transportMode == 2) {
        M5.Lcd.fillCircle(8, 7, 4, RED);
        M5.Lcd.setTextColor(RED, UI_BAR);
        M5.Lcd.drawString("REC", 16, 7, 1);
    } else {
        const char* mode = !cameraConnected ? "OFFLINE" : transportMode == 1 ? "PLAY" : "STBY";
        M5.Lcd.setTextColor(cameraConnected ? WHITE : GREY, UI_BAR);
        M5.Lcd.drawString(mode, 4, 7, 1);
    }
    if (cameraConnected) {
        M5.Lcd.setTextDatum(MR_DATUM);
        M5.Lcd.setTextColor(WHITE, UI_BAR);
        M5.Lcd.drawString(timecodeSource == 0 ? "TC" : "CLIP", 142, 7, 1);
    }
    M5.Lcd.fillCircle(151, 7, 4, cameraConnected ? UI_BT_BLUE : GREY);
}

// Fixed-width cells keep the colons in place while the digits change.
static const int TIMECODE_CHARS = 11, TIMECODE_CENTER_Y = 34;
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
    int digitCell = M5.Lcd.textWidth("0", font) + 1;
    int colonCell = M5.Lcd.textWidth(":", font) + 2;
    if (8 * digitCell + 3 * colonCell > SCREEN_W - 4) {
        font = 2;
        digitCell = M5.Lcd.textWidth("0", font) + 1;
        colonCell = M5.Lcd.textWidth(":", font) + 2;
    }
    const int cellHeight = M5.Lcd.fontHeight(font);

    if (!timecodeCacheValid || color != drawnTimecodeColor) {
        memset(drawnTimecode, 0, sizeof(drawnTimecode));
        drawnTimecodeColor = color;
        timecodeCacheValid = true;
    }
    M5.Lcd.setTextColor(color, BLACK);
    M5.Lcd.setTextDatum(MC_DATUM);
    int cellX = (SCREEN_W - (8 * digitCell + 3 * colonCell)) / 2;
    for (int index = 0; index < TIMECODE_CHARS && text[index] != '\0'; ++index) {
        const int cellWidth = text[index] == ':' ? colonCell : digitCell;
        if (text[index] != drawnTimecode[index]) {
            const char glyph[2] = {text[index], '\0'};
            M5.Lcd.fillRect(cellX, TIMECODE_CENTER_Y - cellHeight / 2, cellWidth, cellHeight, BLACK);
            M5.Lcd.drawString(glyph, cellX + cellWidth / 2, TIMECODE_CENTER_Y, font);
            drawnTimecode[index] = text[index];
        }
        cellX += cellWidth;
    }
    M5.Lcd.setTextFont(1);
}

// Status message and button hints.
static void drawBottom() {
    M5.Lcd.fillRect(0, 57, SCREEN_W, SCREEN_H - 57, BLACK);
    M5.Lcd.drawFastHLine(0, 56, SCREEN_W, GREY);
    M5.Lcd.setTextFont(1);
    M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.setTextColor(WHITE, BLACK);
    M5.Lcd.drawString(statusMessage, 4, 60, 1);
    M5.Lcd.setTextColor(GREY, BLACK);
    const char* hint = cameraConnected ? (transportMode == 2 ? "A:STOP  B:TC/CLIP" : "A:REC  B:TC/CLIP")
                                       : (connecting ? "Please wait" : "A:CONNECT");
    M5.Lcd.drawString(hint, 4, 70, 1);
}

static void drawScreen() {
    M5.Lcd.fillScreen(BLACK);
    timecodeCacheValid = false;
    drawTopBar();
    drawTimecode();
    drawBottom();
}

static void drawPinScreen() {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextColor(WHITE, BLACK);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.drawString("CAMERA PIN", SCREEN_W / 2, 9, 2);
    for (uint8_t i = 0; i < 6; ++i) {
        const int x = 8 + i * 24;
        const bool current = i == pinPosition;
        const uint16_t color = current ? UI_SEL : (i < pinPosition ? WHITE : GREY);
        M5.Lcd.drawRoundRect(x, 22, 22, 30, 3, color);
        if (current) M5.Lcd.drawRoundRect(x + 1, 23, 20, 28, 3, color);
        char digit[2] = {i <= pinPosition ? (char)('0' + pinValues[i]) : '-', '\0'};
        M5.Lcd.setTextColor(color, BLACK);
        M5.Lcd.drawString(digit, x + 11, 38, 4);
    }
    M5.Lcd.setTextColor(GREY, BLACK);
    M5.Lcd.drawString("B:+  A:OK  hold B:back", SCREEN_W / 2, 66, 1);
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

void setup() {
    M5.begin();
    M5.Lcd.setRotation(3);
    M5.Axp.ScreenBreath(11);
    Serial.begin(115200);
    initializeBluetooth();
    drawScreen();
    Serial.println("[BOOT] MagicPilot Remote Stick ready; press A to scan");
}

void loop() {
    M5.update();
    if (passkeyReplyPending && pinReady) {
        const esp_err_t result = esp_ble_passkey_reply(pairingBda, true, enteredPin);
        passkeyReplyPending = false;
        pinReady = false;
        pinRequested = false;
        if (result != ESP_OK) setStatus("Passkey reply failed");
    }
    if (pinRequested) {
        if (!pinScreenShown) {
            pinScreenShown = true;
            pinBackHandled = false;
            drawPinScreen();
        }
        handlePinButtons();
        delay(10);
        return;
    }
    if (pinScreenShown) {
        pinScreenShown = false;
        redrawAll = true;
    }
    handleMainButtons();
    updateDisplay();
    delay(5);
}
