#include "gateway.h"
#include "config.h"
#include "protocol.h"
#include "frame_filter.h"
#include "sb_frame.h"
#include "button_controller.h"
#include "uart_log_capture.h"
#include "time_manager.h"
#include <WiFi.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <mqtt_client.h>
#include <esp_system.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <time.h>
#include <atomic>
#include <algorithm>

namespace
{
HardwareSerial uart(1);
Preferences prefs;
ButtonController button;
esp_mqtt_client_handle_t mqtt = nullptr;
String gatewayId, bootId, prefix, brokerUri, will;
String deviceName;
String tester, location, doorType, motorType, wifiSsid, wifiPassword;
bool hasSavedTestInfo()
{
    return !tester.isEmpty() && !location.isEmpty() &&
           !doorType.isEmpty() && !motorType.isEmpty();
}
uint32_t infoRev = 0, frameSeq = 0, lineSeq = 0, stateSeq = 0;
bool fsReady = false, started = false, online = false, ready = false;
bool localConfigPending = false;
bool useLocalBroker = false;
uint32_t brokerRestartAt = 0;
int localConfigMessageId = -1;
bool mqttStartAttempted = false;
esp_err_t mqttStartResult = ESP_OK;
std::atomic<int> mqttLastEvent{MQTT_EVENT_BEFORE_CONNECT};
std::atomic<int> mqttErrorType{MQTT_ERROR_TYPE_NONE};
std::atomic<int> mqttTlsEspError{0};
std::atomic<int> mqttTlsStackError{0};
std::atomic<int> mqttCertVerifyFlags{0};
std::atomic<int> mqttSocketError{0};
std::atomic<int> mqttConnectReturnCode{0};
void sendLocalConfig()
{
    if (!ready || !localConfigPending || localConfigMessageId >= 0) return;
    JsonDocument d;
    d["cycle_sec"] = button.cycleSeconds();
    d["limit"] = button.limit();
    String payload;
    serializeJson(d, payload);
    localConfigMessageId = esp_mqtt_client_enqueue(mqtt, (prefix + "up/config").c_str(),
                                                  payload.c_str(), payload.length(), 1, false, true);
}
uint32_t droppedFrames = 0, droppedLogs = 0, rxOverflow = 0;
uint32_t invalidStatusFrames = 0;
uint32_t invalidStoredStatusFrames = 0;
uint32_t filteredSbFrames = 0, filteredStoredSbFrames = 0;
uint32_t lastHeartbeat = 0, lastWifiRetry = 0, lastApCheck = 0;
uint32_t lastDiagnostic = 0;
String pendingPath;
int pendingId = -1, subscriptions = 0;
uint32_t lastPublishTry = 0;
uint32_t pendingSince = 0, stalePendingRetries = 0;
uint32_t liveFramesEnqueued = 0, frameFallbacks = 0, outboxFramesAcked = 0;
constexpr size_t FrameMqttBudget = 8192;
uint32_t maxLoopUs = 0;
uint32_t outboxScanLastUs = 0, outboxScanMaxUs = 0;
uint32_t frameReadLastUs = 0, frameReadMaxUs = 0;
uint32_t frameWriteLastUs = 0, frameWriteMaxUs = 0;
uint32_t mqttEnqueueLastUs = 0, mqttEnqueueMaxUs = 0;
uint32_t webLoopLastUs = 0, webLoopGapMaxUs = 0;
// Limit MQTT memory; use LittleFS when the live queue is full.
uint64_t fileSequence = 0;
uint16_t outboxCount = 0;
uint64_t outboxFiles[SMO_OUTBOX_LIMIT] = {};
bool outboxIndexReady = false;
String lastFrame = "{}", history = "[]";
uint32_t lastFrameAt = 0;
bool hasValidFrame = false;
bool lastRemoteDisabled = false, lastAutoCloserCounting = false;
smo::FrameFilter frameFilter(SMO_ERROR_CANCEL_MS, SMO_ERROR_CONFIRM_MS, SMO_FRAME_DUPLICATE_MS);
smo::FrameDeduplicator<264> otherFrameDeduplicator;
uint32_t otherDuplicateFrames = 0;
smo::UartLogCapture uartLogCapture;
void resetStatusFrameWindow();
void beginUartLogCapture(uint32_t now);
void flushLog();
bool uartLogCaptureActive()
{
    return uartLogCapture.active();
}
uint32_t unsupportedUartFrames = 0, uartBytes = 0, badFooters = 0, partialTimeouts = 0;
struct UartPreview { uint32_t seq = 0, at = 0; String direction, encoding, data; };
UartPreview uartPreview[40];
uint32_t previewSeq = 0, logQueued = 0, logQueueFailed = 0, mqttDisconnects = 0;
String lastUartCommand, lastUartAction, lastUartAck, lastUartError, lastUartTx;
// Keep UART logs in a separate bounded queue.
struct LogBatch { uint16_t length, count; char data[6144]; };
QueueHandle_t logBatches = nullptr;
JsonDocument logBatch;
String logBatchSession;
uint32_t logBatchStarted = 0;
std::atomic<bool> logLinkReady{false};
std::atomic<uint32_t> logSent{0}, logSendFailed{0};
void flushLogBatch()
{
    const size_t count = logBatch["entries"].size();
    if (!count) return;
    // Reuse one packet buffer on the loop task.
    static LogBatch packet;
    const size_t size = measureJson(logBatch);
    if (!logBatches || size >= sizeof(packet.data))
    {
        // Keep the batch intact if the sender is unavailable.
        ++logQueueFailed;
        return;
    }
    packet.length = serializeJson(logBatch, packet.data, sizeof(packet.data));
    packet.count = count;
    // Wait for a free queue slot instead of dropping logs.
    xQueueSend(logBatches, &packet, portMAX_DELAY);
    logQueued += count;
    logBatch.clear();
}
void uartLogSender(void *)
{
    // Send UART logs outside the Arduino loop.
    static LogBatch packet;
    const String topic = prefix + "up/uart_log";
    for (;;)
    {
        if (xQueueReceive(logBatches, &packet, portMAX_DELAY) != pdTRUE) continue;
        // Retry the same packet until MQTT accepts it.
        for (;;)
        {
            if (!logLinkReady.load())
            {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            int result = esp_mqtt_client_publish(mqtt, topic.c_str(), packet.data,
                                                 packet.length, 0, false);
            if (result >= 0)
            {
                logSent.fetch_add(packet.count);
                break;
            }
            logSendFailed.fetch_add(1);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        // Yield between packets.
        vTaskDelay(1);
    }
}

// MQTT callbacks pass bounded events to the Arduino loop.
struct Event
{
    int kind;
    int id;
    bool retained;
    char topic[160];
    char data[SMO_MAX_COMMAND + 1];
};
QueueHandle_t events = nullptr;
volatile bool eventOverflow = false;
Event incoming = {};
size_t incomingLength = 0;
bool incomingValid = false;

String json(const JsonDocument &d)
{
    String s;
    serializeJson(d, s);
    return s;
}
void base(JsonDocument &d)
{
    d["boot_id"] = bootId;
}
int publish(const char *suffix, const String &payload, int qos = 1, bool retain = false)
{
    if (!online || !mqtt)
        return -1;
    return esp_mqtt_client_enqueue(mqtt, (prefix + suffix).c_str(), payload.c_str(),
                                   payload.length(), qos, retain, true);
}
void previewUartLog(const char *direction, const char *encoding, const String &data)
{
    auto &row = uartPreview[previewSeq % 40];
    row.seq = ++previewSeq;
    row.at = millis();
    row.direction = direction;
    row.encoding = encoding;
    row.data = data;
}
void publishUartLog(const char *direction, const char *encoding, const String &data,
                    const char *commandId = nullptr)
{
    if (strcmp(direction, "TX") == 0) previewUartLog(direction, encoding, data);
    if (logBatch["entries"].size() && logBatchSession != button.sessionId()) flushLogBatch();
    if (!logBatch["entries"].size())
    {
        logBatchStarted = millis();
        logBatchSession = button.sessionId();
        logBatch["stream_id"] = "UART_MAIN";
        if (logBatchSession.isEmpty()) logBatch["session_id"] = nullptr;
        else logBatch["session_id"] = logBatchSession;
        logBatch["entries"].to<JsonArray>();
    }
    auto entry = logBatch["entries"].as<JsonArray>().add<JsonObject>();
    entry["line_seq"] = ++lineSeq;
    entry["direction"] = direction;
    entry["encoding"] = encoding;
    entry["data"] = data;
    entry["cmd_id"] = commandId;
    entry["ts"] = getUtcIsoTimestamp();
    if (logBatch["entries"].size() >= 4) flushLogBatch();
}
bool writeAtomic(const String &path, const String &payload)
{
    if (!fsReady)
        return false;
    String tmp = path + ".tmp";
    File f = LittleFS.open(tmp, "w");
    if (!f)
        return false;
    bool ok = f.print(payload) == payload.length();
    f.flush();
    f.close();
    if (!ok)
    {
        LittleFS.remove(tmp);
        return false;
    }
    return LittleFS.rename(tmp, path);
}
String readFile(const String &path)
{
    File f = LittleFS.open(path, "r");
    if (!f)
        return "";
    String s = f.readString();
    f.close();
    return s;
}
void scanOutbox()
{
    const uint32_t started = micros();
    outboxCount = 0;
    outboxIndexReady = false;
    File dir = LittleFS.open("/smo_outbox");
    if (!dir)
        return;
    File f = dir.openNextFile();
    while (f)
    {
        String name = f.name();
        if (name.endsWith(".json"))
        {
            outboxCount++;
            int slash = name.lastIndexOf('/');
            uint64_t value = strtoull(name.substring(slash + 1).c_str(), nullptr, 10);
            if (value > fileSequence)
                fileSequence = value;
            if (outboxCount <= SMO_OUTBOX_LIMIT)
                outboxFiles[outboxCount - 1] = value;
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
    if (outboxCount <= SMO_OUTBOX_LIMIT)
    {
        std::sort(outboxFiles, outboxFiles + outboxCount);
        outboxIndexReady = true;
    }
    outboxScanLastUs = micros() - started;
    if (outboxScanLastUs > outboxScanMaxUs) outboxScanMaxUs = outboxScanLastUs;
}
void clearOutboxOnce()
{
#ifndef SMO_CLEAR_OUTBOX_ON_BOOT
    return;
#else
    if (!fsReady) return;
    File dir = LittleFS.open("/smo_outbox");
    if (dir)
    {
        File file = dir.openNextFile();
        while (file)
        {
            String path = file.name();
            if (path.indexOf('/') < 0) path = "/smo_outbox/" + path;
            const bool jsonFile = path.endsWith(".json");
            file.close();
            file = dir.openNextFile();
            if (jsonFile) LittleFS.remove(path);
        }
        dir.close();
    }
#endif
}
String oldestPath()
{
    if (!outboxIndexReady || !outboxCount) return String();
    char path[64];
    snprintf(path, sizeof(path), "/smo_outbox/%020llu.json",
             (unsigned long long)outboxFiles[0]);
    return String(path);
}
void addIndexedOutbox(uint64_t sequence)
{
    if (!outboxIndexReady || outboxCount >= SMO_OUTBOX_LIMIT) return;
    uint16_t position = 0;
    while (position < outboxCount && outboxFiles[position] < sequence) ++position;
    memmove(outboxFiles + position + 1, outboxFiles + position,
            (outboxCount - position) * sizeof(outboxFiles[0]));
    outboxFiles[position] = sequence;
    ++outboxCount;
}
void removeIndexedOutbox(const String &path)
{
    if (!outboxIndexReady) return;
    for (uint16_t i = 0; i < outboxCount; ++i)
    {
        char candidate[64];
        snprintf(candidate, sizeof(candidate), "/smo_outbox/%020llu.json",
                 (unsigned long long)outboxFiles[i]);
        if (path == candidate)
        {
            --outboxCount;
            memmove(outboxFiles + i, outboxFiles + i + 1,
                    (outboxCount - i) * sizeof(outboxFiles[0]));
            return;
        }
    }
}
bool queueFrame(const String &payload)
{
    // Publish live frames first and use LittleFS as fallback.
    if (ready && mqtt && ESP.getFreeHeap() >= 32768 &&
        esp_mqtt_client_get_outbox_size(mqtt) + payload.length() + 256 <= FrameMqttBudget)
    {
        const uint32_t started = micros();
        const int id = publish("up/frame", payload);
        mqttEnqueueLastUs = micros() - started;
        if (mqttEnqueueLastUs > mqttEnqueueMaxUs) mqttEnqueueMaxUs = mqttEnqueueLastUs;
        if (id >= 0)
        {
            ++liveFramesEnqueued;
            return true;
        }
    }
    ++frameFallbacks;
    if (!fsReady || !outboxIndexReady || outboxCount >= SMO_OUTBOX_LIMIT)
    {
        droppedFrames++;
        return false;
    }
    char path[64];
    snprintf(path, sizeof(path), "/smo_outbox/%020llu.json", (unsigned long long)++fileSequence);
    const uint32_t writeStarted = micros();
    const bool stored = writeAtomic(path, payload);
    frameWriteLastUs = micros() - writeStarted;
    if (frameWriteLastUs > frameWriteMaxUs) frameWriteMaxUs = frameWriteLastUs;
    if (stored)
    {
        addIndexedOutbox(fileSequence);
        return true;
    }
    ++droppedFrames;
    return false;
}
void saveFault(const String &payload)
{
    JsonDocument d, frame;
    deserializeJson(d, history);
    deserializeJson(frame, payload);
    JsonArray rows = d.as<JsonArray>();
    if (rows.size() >= 50)
        rows.remove(0);
    rows.add(frame.as<JsonObject>());
    history = json(d);
    if (!writeAtomic("/smo_history.json", history))
        droppedFrames++;
}
void sendInfo()
{
    JsonDocument d;
    gatewayInfo(d);
    base(d);
    publish("up/info", json(d));
}
void sendState()
{
    JsonDocument d;
    base(d);
    d["gateway_fw"] = SMO_FW;
    d["rssi_dbm"] = WiFi.RSSI();
    d["run_state"] = button.runState();
    if (button.sessionId().isEmpty())
        d["session_id"] = nullptr;
    else
        d["session_id"] = button.sessionId();
    d["button_count"] = button.buttonCount();
    d["config"]["cycle_sec"] = button.cycleSeconds();
    d["config"]["limit"] = button.limit();
    publish("up/state", json(d), 1, true);
}
void heartbeat()
{
    JsonDocument d;
    base(d);
    d["online"] = true;
    d["ts"] = getUtcIsoTimestamp();
    publish("up/availability", json(d), 1, true);
    lastHeartbeat = millis();
}
esp_err_t onMqtt(esp_mqtt_event_handle_t e)
{
    mqttLastEvent.store(e->event_id);
    if (e->event_id == MQTT_EVENT_ERROR && e->error_handle)
    {
        mqttErrorType.store(e->error_handle->error_type);
        mqttTlsEspError.store(e->error_handle->esp_tls_last_esp_err);
        mqttTlsStackError.store(e->error_handle->esp_tls_stack_err);
        mqttCertVerifyFlags.store(e->error_handle->esp_tls_cert_verify_flags);
        mqttSocketError.store(e->error_handle->esp_transport_sock_errno);
        mqttConnectReturnCode.store(e->error_handle->connect_return_code);
    }
    // Update UART sender state directly from MQTT events.
    if (e->event_id == MQTT_EVENT_CONNECTED) logLinkReady.store(true);
    if (e->event_id == MQTT_EVENT_DISCONNECTED) logLinkReady.store(false);
    Event out = {};
    out.kind = e->event_id;
    out.id = e->msg_id;
    if (e->event_id == MQTT_EVENT_DATA)
    {
        if (e->current_data_offset == 0)
        {
            incoming = {};
            incoming.kind = MQTT_EVENT_DATA;
            incoming.retained = e->retain;
            incomingLength = 0;
            incomingValid = e->total_data_len > 0 && e->total_data_len <= SMO_MAX_COMMAND &&
                            e->topic_len > 0 && e->topic_len < (int)sizeof(incoming.topic);
            if (incomingValid)
                memcpy(incoming.topic, e->topic, e->topic_len);
        }
        if (!incomingValid)
            return ESP_OK;
        if ((size_t)e->current_data_offset != incomingLength ||
            incomingLength + e->data_len > SMO_MAX_COMMAND)
        {
            incomingValid = false;
            return ESP_OK;
        }
        memcpy(incoming.data + incomingLength, e->data, e->data_len);
        incomingLength += e->data_len;
        if (incomingLength == (size_t)e->total_data_len)
        {
            if (xQueueSend(events, &incoming, 0) != pdTRUE)
                eventOverflow = true;
            incomingValid = false;
        }
        return ESP_OK;
    }
    if (e->event_id == MQTT_EVENT_CONNECTED || e->event_id == MQTT_EVENT_DISCONNECTED ||
        e->event_id == MQTT_EVENT_SUBSCRIBED || e->event_id == MQTT_EVENT_PUBLISHED ||
        e->event_id == MQTT_EVENT_ERROR)
    {
        if (xQueueSend(events, &out, 0) != pdTRUE)
            eventOverflow = true;
    }
    return ESP_OK;
}
bool parseUtc(const String &s, time_t &result)
{
    // Accept UTC timestamps with zero or three fractional digits.
    if (s.length() != 20 && s.length() != 24)
        return false;
    if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' ||
        s[s.length() - 1] != 'Z')
        return false;
    if (s.length() == 24 && (s[19] != '.' || !isDigit(s[20]) || !isDigit(s[21]) || !isDigit(s[22])))
        return false;
    struct tm t = {};
    String seconds = s.substring(0, 19);
    char *end = strptime(seconds.c_str(), "%Y-%m-%dT%H:%M:%S", &t);
    if (!end || *end)
        return false;
    result = mktime(&t);
    char check[24];
    strftime(check, sizeof(check), "%Y-%m-%dT%H:%M:%S", &t);
    return seconds == check && result > 0;
}
// Cache command ACKs to prevent replay after reboot.
String cachedAck(const String &id)
{
    for (unsigned i = 0; i < 16; i++)
    {
        String key = "ack" + String(i);
        if (!prefs.isKey(key.c_str()))
            continue;
        String s = prefs.getString(key.c_str(), "");
        JsonDocument d;
        if (!deserializeJson(d, s) && d["cmd_id"].as<String>() == id)
            return s;
    }
    return "";
}
bool cacheAck(const String &payload, const String &id)
{
    unsigned slot = prefs.getUInt("ack_next", 0) % 16;
    for (unsigned i = 0; i < 16; i++)
    {
        String key = "ack" + String(i);
        if (!prefs.isKey(key.c_str()))
            continue;
        JsonDocument d;
        if (!deserializeJson(d, prefs.getString(key.c_str(), "")) && d["cmd_id"].as<String>() == id)
        {
            slot = i;
            break;
        }
    }
    if (prefs.putString(("ack" + String(slot)).c_str(), payload) != payload.length())
        return false;
    prefs.putUInt("ack_next", (slot + 1) % 16);
    return true;
}
void handleCommand(const Event &e)
{
    String topic = e.topic;
    String kind = topic.substring(prefix.length() + 5);
    if (topic != prefix + "down/control" && topic != prefix + "down/config" &&
        topic != prefix + "down/smo_command")
        return;
    JsonDocument command, ack;
    auto error = deserializeJson(command, e.data);
    String id = command["cmd_id"] | "";
    String action = command["action"] | "";
    if (kind == "smo_command")
    {
        lastUartCommand = id;
        lastUartAction = action;
    }
    ack["cmd_id"] = id;
    auto emitAck = [&](const String &payload)
    {
        JsonDocument stamped;
        deserializeJson(stamped, payload);
        stamped["ts"] = getUtcIsoTimestamp();
        if (kind == "smo_command")
        {
            lastUartAck = stamped["status"] | "";
            lastUartError = stamped["error_code"] | "";
        }
        String output = json(stamped);
        Serial.printf("[MQTT ACK] %s\n", output.c_str());
        publish("up/ack", output);
    };
    auto reject = [&](const char *code)
    {
        ack["status"] = "rejected";
        ack["error_code"] = code;
        emitAck(json(ack));
    };
    if (error || id.isEmpty() || id.length() > 96 || action.length() > 64)
    {
        reject("INVALID_COMMAND");
        return;
    }
    if (e.retained)
    {
        reject("RETAINED_COMMAND");
        return;
    }
    if (!command["schema"].is<int>() || command["schema"].as<int>() != 1)
    {
        reject("INVALID_SCHEMA");
        return;
    }
    time_t issued, expires;
    if (!parseUtc(command["issued_at"] | "", issued) ||
        !parseUtc(command["expires_at"] | "", expires) || expires <= issued)
    {
        reject("INVALID_TIMESTAMP");
        return;
    }
    time_t now = getUnixTimestamp();
    if (!now)
    {
        reject("TIME_NOT_SYNCED");
        return;
    }
    if (now >= expires)
    {
        reject("COMMAND_EXPIRED");
        return;
    }
    if (issued > now + 5)
    {
        reject("COMMAND_NOT_YET_VALID");
        return;
    }
    String previous = cachedAck(id);
    if (!previous.isEmpty())
    {
        JsonDocument old;
        deserializeJson(old, previous);
        if (old["status"] == "received")
        {
            old["status"] = "failed";
            old["error_code"] = "EXECUTION_UNKNOWN_AFTER_RESTART";
            previous = json(old);
        }
        emitAck(previous);
        return;
    }
    if (kind == "control" || kind == "config")
    {
        if (kind == "control")
        {
            if (action != "START_TEST" && action != "PAUSE_TEST" && action != "RESUME_TEST" &&
                action != "STOP_TEST" && action != "TRIGGER_MANUAL" && action != "RESET_COUNT")
            {
                reject("INVALID_ACTION");
                return;
            }
            if (action == "START_TEST" && !command["session_id"].isNull() &&
                                          (!command["session_id"].is<const char *>() ||
                                           command["session_id"].as<JsonString>().size() == 0 ||
                                           command["session_id"].as<JsonString>().size() > 96))
            {
                reject("INVALID_SESSION_ID");
                return;
            }
            // Use cmd_id to correlate ACKs and prevent duplicate execution.
        }
        else
        {
            if (!command["cycle_sec"].is<uint32_t>() || !command["limit"].is<uint32_t>() ||
                command["cycle_sec"].as<uint32_t>() < 1 ||
                command["cycle_sec"].as<uint32_t>() > 86400 ||
                command["limit"].as<uint32_t>() > 1000000)
            {
                reject("INVALID_CONFIG");
                return;
            }
        }
        // Cache receipt before changing GPIO.
        ack["status"] = "received";
        ack["error_code"] = nullptr;
        if (!cacheAck(json(ack), id))
        {
            reject("COMMAND_STORAGE_FAILED");
            return;
        }
        emitAck(json(ack));
        const uint32_t at = millis();
        const bool wasRunning = button.running();
        const char *result = nullptr;
        if (kind == "config")
            result = button.configure(command["cycle_sec"].as<uint32_t>(),
                                      command["limit"].as<uint32_t>());
        else if (action == "START_TEST")
            result = button.start(command["session_id"] | id.c_str(), at);
        else if (action == "PAUSE_TEST")
            result = button.pause(at);
        else if (action == "RESUME_TEST")
            result = button.resume(at);
        else if (action == "STOP_TEST")
            result = button.stop(at);
        else if (action == "TRIGGER_MANUAL")
            result = button.manual(at);
        else
            result = button.resetCount();
        if (wasRunning != button.running()) resetStatusFrameWindow();
        ack["status"] = result ? "rejected" : "applied";
        if (result)
            ack["error_code"] = result;
        else
            ack["error_code"] = nullptr;
        String payload = json(ack);
        if (!cacheAck(payload, id))
        {
            reject("COMMAND_STORAGE_FAILED");
            return;
        }
        emitAck(payload);
        if (button.takeStateChanged())
            sendState();
        return;
    }
    if (kind != "smo_command")
    {
        reject("BUTTON_CONTROL_NOT_IMPLEMENTED");
        return;
    }
    if (action.isEmpty())
    {
        reject("INVALID_ACTION");
        return;
    }
    uint8_t bytes[SMO_MAX_UART_TX];
    size_t length = 0;
    const char *encoding = command["encoding"] | "";
    if (!command["data"].is<const char *>() || !command["encoding"].is<const char *>() ||
        command["data"].as<JsonString>().size() != strlen(command["data"].as<const char *>()) ||
        !smo::uartBytes(encoding, command["data"], bytes, sizeof(bytes), length))
    {
        reject("INVALID_UART_DATA");
        return;
    }
    if (smo::equal(encoding, "ascii"))
    {
        // Clear incomplete CLI input, then execute the command.
        size_t first = 0, last = length;
        while (first < last && (bytes[first] == '\r' || bytes[first] == '\n')) ++first;
        while (last > first && (bytes[last - 1] == '\r' || bytes[last - 1] == '\n')) --last;
        const size_t commandLength = last - first;
        if (!commandLength || commandLength + 2 > sizeof(bytes))
        {
            reject("INVALID_UART_DATA");
            return;
        }
        memmove(bytes + 1, bytes + first, commandLength);
        bytes[0] = '\r';
        bytes[commandLength + 1] = '\r';
        length = commandLength + 2;
    }
    ack["status"] = "received";
    ack["error_code"] = nullptr;
    if (!cacheAck(json(ack), id))
    {
        reject("COMMAND_STORAGE_FAILED");
        return;
    }
    emitAck(json(ack));
    // Flush the previous response before transmitting.
    flushLog();
    size_t sent = uart.write(bytes, length);
    uart.flush();
    if (sent == length)
    {
        if (smo::UartLogCapture::isStopRequest(bytes, length)) uartLogCapture.stop();
        else
            beginUartLogCapture(millis());
    }
    ack["status"] = sent == length ? "sent_to_smo" : "failed";
    if (sent != length)
        ack["error_code"] = "UART_WRITE_FAILED";
    String payload = json(ack);
    cacheAck(payload, id);
    emitAck(payload);
    String raw;
    for (size_t i = 0; i < sent; i++)
    {
        char b[4];
        snprintf(b, sizeof(b), "%02X", bytes[i]);
        if (i)
            raw += ' ';
        raw += b;
    }
    lastUartTx = raw;
    publishUartLog("TX", "hex", raw, id.c_str());
    // UART transport cannot confirm that SMO applied the command.
}
void processEvents()
{
    Event e;
    while (xQueueReceive(events, &e, 0) == pdTRUE)
    {
        switch (e.kind)
        {
        case MQTT_EVENT_CONNECTED:
            online = true;
            ready = false;
            subscriptions = 0;
            esp_mqtt_client_subscribe(mqtt, (prefix + "down/control").c_str(), 1);
            esp_mqtt_client_subscribe(mqtt, (prefix + "down/config").c_str(), 1);
            esp_mqtt_client_subscribe(mqtt, (prefix + "down/smo_command").c_str(), 1);
            break;
        case MQTT_EVENT_SUBSCRIBED:
            if (++subscriptions == 3)
            {
                ready = true;
                logLinkReady.store(true);
                heartbeat();
                sendInfo();
                sendState();
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            localConfigMessageId = -1;
            logLinkReady.store(false);
            ++mqttDisconnects;
            online = false;
            ready = false;
            pendingId = -1;
            pendingSince = 0;
            pendingPath = "";
            break;
        case MQTT_EVENT_PUBLISHED:
            if (localConfigMessageId >= 0 && e.id == localConfigMessageId)
            {
                if (prefs.putBool("cfg_pending", false)) localConfigPending = false;
                localConfigMessageId = -1;
            }
            if (e.id == pendingId)
            {
                if (LittleFS.remove(pendingPath))
                {
                    removeIndexedOutbox(pendingPath);
                    ++outboxFramesAcked;
                }
                pendingId = -1;
                pendingSince = 0;
                pendingPath = "";
            }
            break;
        case MQTT_EVENT_DATA:
            handleCommand(e);
            break;
        case MQTT_EVENT_ERROR:
            Serial.println("[MQTT] Connection/TLS error");
            break;
        }
    }
    if (eventOverflow)
    {
        eventOverflow = false;
        rxOverflow++;
        // Reconnect when a queued publish remains unacknowledged.
        pendingId = -1;
        pendingSince = 0;
        pendingPath = "";
        online = false;
        ready = false;
        esp_mqtt_client_reconnect(mqtt);
    }
}
String hex(const uint8_t *bytes, size_t n)
{
    String s;
    s.reserve(n * 3);
    for (size_t i = 0; i < n; i++)
    {
        char b[4];
        snprintf(b, sizeof(b), "%02X", bytes[i]);
        if (i)
            s += ' ';
        s += b;
    }
    return s;
}
void publishStatusFrame(const smo::StatusRecord &record)
{
    const uint8_t *bytes = record.bytes;
    const size_t n = sizeof(record.bytes);
    smo::Decoded decoded = smo::decode(bytes, n);
    if (decoded.error)
    {
        ++invalidStatusFrames;
        return;
    }
    JsonDocument d;
    base(d);
    const bool forward = button.running() && record.publishAllowed;
    d["frame_seq"] = forward ? ++frameSeq : frameSeq;
    if (button.sessionId().isEmpty())
        d["session_id"] = nullptr;
    else
        d["session_id"] = button.sessionId();
    if (record.unixTime == 0)
        d["device_time_utc"] = nullptr;
    else
    {
        time_t captured = record.unixTime;
        struct tm utc = {};
        gmtime_r(&captured, &utc);
        char timestamp[25];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S.000Z", &utc);
        d["device_time_utc"] = timestamp;
    }
    d["raw_hex"] = hex(bytes, n);
    d["decode_ok"] = decoded.error == nullptr;
    if (decoded.error)
    {
        d["decoded"] = nullptr;
        d["decode_error"] = decoded.error;
    }
    else
    {
        auto result = d["decoded"].to<JsonObject>();
        result["door_state"] = decoded.door;
        result["direction"] = decoded.direction;
        result["light_on"] = decoded.light;
        result["remote_enabled"] = !decoded.remoteDisabled;
        result["auto_closer_counting"] = decoded.autoCloserCounting;
        result["ir_blocked"] = decoded.ir;
        if (smo::reportableFault(bytes[7]))
            result["fault_code"] = decoded.fault;
        else
            result["fault_code"] = nullptr;
        lastFrame = json(d);
        lastFrameAt = record.receivedAt;
        hasValidFrame = true;
        lastRemoteDisabled = decoded.remoteDisabled;
        lastAutoCloserCounting = decoded.autoCloserCounting;
    }
    if (!forward) return;
    String payload = json(d);
    queueFrame(payload);
    if (!decoded.error && smo::reportableFault(bytes[7]))
        saveFault(payload);
}
void publishSbFrame(const uint8_t *bytes, size_t n, uint32_t capturedTime, bool allowed)
{
    if (!button.running() || !allowed) return;
    JsonDocument d;
    base(d);
    d["frame_seq"] = ++frameSeq;
    if (button.sessionId().isEmpty()) d["session_id"] = nullptr;
    else d["session_id"] = button.sessionId();
    if (!capturedTime) d["device_time_utc"] = nullptr;
    else {
        time_t captured = capturedTime;
        struct tm utc = {};
        gmtime_r(&captured, &utc);
        char timestamp[25];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S.000Z", &utc);
        d["device_time_utc"] = timestamp;
    }
    d["raw_hex"] = hex(bytes, n);
    d["decode_ok"] = true;
    auto decoded = d["decoded"].to<JsonObject>();
    for (const char *field : {"door_state", "direction", "light_on", "remote_enabled",
                             "auto_closer_counting", "ir_blocked", "fault_code"})
        decoded[field] = "unknown";
    // SB notifications do not update door state or fault totals.
    queueFrame(json(d));
}
// Route binary frames to up/frame.
uint8_t logBuffer[192], frameBuffer[264];
size_t logLength = 0, frameLength = 0;
bool logContinuation = false;
uint32_t filteredUartFragments = 0;
bool discardingBinaryTail = false;
uint8_t previousDiscardedByte = 0;
uint32_t filteredBinaryTailBytes = 0;
uint32_t lastByteAt = 0, logWindow = 0, frameStartedAt = 0, frameUnixTime = 0;
unsigned logsInWindow = 0;
bool frameStartedWhileRunning = false;
void resetStatusFrameWindow()
{
    // Reset pending frames when a test starts or resumes.
    frameFilter.resetWindow();
    otherFrameDeduplicator = smo::FrameDeduplicator<264>();
    frameLength = 0;
    frameStartedWhileRunning = false;
}
void beginUartLogCapture(uint32_t now)
{
    flushLog();
    logContinuation = false;
    logsInWindow = 0;
    logWindow = now;
    uartLogCapture.begin();
}
void flushLog()
{
    if (!logLength)
        return;
    if (!uartLogCaptureActive())
    {
        logLength = 0;
        logContinuation = false;
        return;
    }
    // Drop short text fragments left by incomplete binary frames.
    size_t first = 0, last = logLength;
    while (first < last && (logBuffer[first] == ' ' || logBuffer[first] == '\r' ||
                            logBuffer[first] == '\n' || logBuffer[first] == '\t')) ++first;
    while (last > first && (logBuffer[last - 1] == ' ' || logBuffer[last - 1] == '\r' ||
                           logBuffer[last - 1] == '\n' || logBuffer[last - 1] == '\t')) --last;
    const size_t textLength = last - first;
    const bool prompt = textLength == 2 &&
                        ((logBuffer[first] == '>' && logBuffer[first + 1] == '>') ||
                         (logBuffer[first] == 'O' && logBuffer[first + 1] == 'K'));
    const bool shortResponse = (textLength == 2 && logBuffer[first] == 'O' &&
                                logBuffer[first + 1] == 'N') ||
                               (textLength == 3 &&
                                ((memcmp(logBuffer + first, "ERR", 3) == 0) ||
                                 (memcmp(logBuffer + first, "OFF", 3) == 0)));
    const bool continues = logLength == sizeof(logBuffer) && logBuffer[logLength - 1] != '\n';
    if (textLength < 4 && !prompt && !shortResponse && !logContinuation)
    {
        if (textLength) ++filteredUartFragments;
        logLength = 0;
        logContinuation = continues;
        return;
    }
    if (millis() - logWindow >= 1000)
    {
        logWindow = millis();
        logsInWindow = 0;
    }
    previewUartLog("RX", "ascii", String((const char *)logBuffer, logLength));
    logsInWindow++;
    bool ascii = true;
    for (size_t i = 0; i < logLength; i++)
        if ((logBuffer[i] < 32 && logBuffer[i] != '\r' && logBuffer[i] != '\n' &&
             logBuffer[i] != '\t') ||
            logBuffer[i] > 126)
            ascii = false;
    publishUartLog("RX", ascii ? "ascii" : "hex",
                   ascii ? String((const char *)logBuffer, logLength) : hex(logBuffer, logLength));
    logLength = 0;
    logContinuation = continues;
}
void logByte(uint8_t b)
{
    if (!uartLogCaptureActive())
    {
        logLength = 0;
        logContinuation = false;
        return;
    }
    if (!smo::UartLogCapture::ascii(b))
    {
        // Discard the remaining bytes of a broken binary frame.
        flushLog();
        logContinuation = false;
        discardingBinaryTail = true;
        previousDiscardedByte = b;
        ++filteredBinaryTailBytes;
        return;
    }
    logBuffer[logLength++] = b;
    if (logLength == sizeof(logBuffer) || b == '\n')
        flushLog();
}
void readUart()
{
    // Drain buffered UART data before checking the frame timeout.
    if (frameLength && !uart.available() && millis() - lastByteAt > SMO_UART_FRAME_GAP_MS)
    {
        ++partialTimeouts;

        // Drop an incomplete binary frame after the inter-byte timeout.
        frameLength = 0;
    }
    unsigned budget = 1024;
    while (uart.available() && budget--)
    {
        uint8_t b = uart.read();
        ++uartBytes;
        const uint32_t now = millis();
        if (discardingBinaryTail && now - lastByteAt > SMO_UART_FRAME_GAP_MS)
            discardingBinaryTail = false;
        lastByteAt = now;
        if (discardingBinaryTail)
        {
            ++filteredBinaryTailBytes;
            if (previousDiscardedByte == 0x55 && b == 0x55)
                discardingBinaryTail = false;
            previousDiscardedByte = b;
            continue;
        }
        if (!frameLength)
        {
            if (b == 0xAA)
            {
                flushLog();
                logContinuation = false;
                frameBuffer[frameLength++] = b;
                frameStartedAt = lastByteAt;
                frameUnixTime = getUnixTimestamp();
                frameStartedWhileRunning = button.running();
            }
            else
                logByte(b);
        }
        else if (frameLength == 1 && b != 0xAA)
        {
            // Treat a lone AA as a broken binary header.
            frameLength = 0;
            discardingBinaryTail = true;
            previousDiscardedByte = b;
            ++filteredBinaryTailBytes;
        }
        else
        {
            frameBuffer[frameLength++] = b;
            // Resync overlapping AA headers.
            if (frameLength == 3 && frameBuffer[2] == 0xAA)
            {
                frameLength = 2;
                frameStartedAt = lastByteAt;
                frameUnixTime = getUnixTimestamp();
                continue;
            }
            // Parse buffered frames immediately after resynchronizing.
            while (frameLength >= 2)
            {
                if (frameBuffer[0] != 0xAA || frameBuffer[1] != 0xAA)
                {
                    // Scan for the next binary header.
                    memmove(frameBuffer, frameBuffer + 1, --frameLength);
                    continue;
                }
                if (frameLength < 6)
                    break;
                const size_t expected = static_cast<size_t>(frameBuffer[5]) + 9;
                if (frameLength < expected)
                    break;
                if (frameBuffer[expected - 2] == 0x55 && frameBuffer[expected - 1] == 0x55)
                {
                    if (frameBuffer[3] == 0x02 && frameBuffer[4] == 0x03 && frameBuffer[5] == 2)
                    {
                        frameFilter.push(frameBuffer, frameStartedAt, millis(), frameUnixTime,
                                         frameStartedWhileRunning);
                    }
                    else if (smo::isFilteredSbFrame(frameBuffer, expected))
                    {
                        ++filteredSbFrames;
                    }
                    else
                    {
                        const bool sb = smo::isSbFrame(frameBuffer, expected);
                        if (!sb) ++unsupportedUartFrames;
                        if (otherFrameDeduplicator.duplicate(frameBuffer, expected, frameStartedAt,
                                                             SMO_FRAME_DUPLICATE_MS))
                            ++otherDuplicateFrames;
                        else if (sb)
                            publishSbFrame(frameBuffer, expected, frameUnixTime, frameStartedWhileRunning);
                        // Keep binary traffic out of UART logs.
                    }
                    memmove(frameBuffer, frameBuffer + expected, frameLength - expected);
                    frameLength -= expected;
                }
                else
                {
                    // Search again after an invalid footer.
                    ++badFooters;

                    memmove(frameBuffer, frameBuffer + 1, --frameLength);
                }
                frameStartedAt = lastByteAt;
                frameUnixTime = getUnixTimestamp();
                frameStartedWhileRunning = button.running();
            }
            if (frameLength == 1 && frameBuffer[0] != 0xAA)
            {
                frameLength = 0;
            }
        }
    }
    if (logLength && millis() - lastByteAt >= SMO_LOG_FLUSH_IDLE_MS)
        flushLog();
}
void processStatusFrames()
{
    // Drain queued frames before confirming an error.
    if (!uart.available())
        frameFilter.confirm(millis());
    smo::StatusRecord record;
    unsigned budget = 8;
    while (budget-- && frameFilter.pop(record))
        publishStatusFrame(record);
}
void startAp()
{
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(false);
    WiFi.softAPConfig(IPAddress(4, 4, 4, 4), IPAddress(4, 4, 4, 4), IPAddress(255, 255, 255, 0));
    WiFi.softAP(("SMO-Monitor-" + gatewayId.substring(gatewayId.length() - 6)).c_str(),
                SMO_AP_PASSWORD);
}

void createMqtt()
{
    brokerUri = useLocalBroker ? String("mqtt://") + SMO_LOCAL_MQTT_HOST + ":" + SMO_LOCAL_MQTT_PORT :
        String(SMO_MQTT_TLS ? "mqtts://" : "mqtt://") + SMO_MQTT_HOST + ":" + SMO_MQTT_PORT;
    JsonDocument lastWill;
    base(lastWill);
    lastWill["online"] = false;
    lastWill["ts"] = getUtcIsoTimestamp();
    will = json(lastWill);
    static String willTopic;
    willTopic = prefix + "up/availability";
    esp_mqtt_client_config_t c = {};
    c.event_handle = onMqtt;
    c.uri = brokerUri.c_str();
    c.client_id = gatewayId.c_str();
    c.username = useLocalBroker ? SMO_LOCAL_MQTT_USERNAME : SMO_MQTT_USERNAME;
    c.password = useLocalBroker ? SMO_LOCAL_MQTT_PASSWORD : SMO_MQTT_PASSWORD;
    c.cert_pem = !useLocalBroker && SMO_MQTT_TLS ? SMO_CA_CERT : nullptr;
    c.keepalive = 30;
    c.network_timeout_ms = 10000;
    c.disable_clean_session = false;
    c.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    c.buffer_size = 4096;
    c.out_buffer_size = 4096;
    c.lwt_topic = willTopic.c_str();
    c.lwt_msg = will.c_str();
    c.lwt_qos = 1;
    c.lwt_retain = true;
    mqtt = esp_mqtt_client_init(&c);
    if (mqtt)
    {
        logBatches = xQueueCreate(3, sizeof(LogBatch));
        if (logBatches && xTaskCreate(uartLogSender, "uart_log_tx", 6144, nullptr, 1, nullptr) != pdPASS)
        {
            vQueueDelete(logBatches);
            logBatches = nullptr;
        }
        if (!logBatches) Serial.println("[UART LOG] Sender allocation failed");
    }
}

} // namespace

void gatewayBegin()
{
    prefs.begin("smo_monitor", false);
    String brokerSetting = prefs.getString("broker_select", "cloud");
    useLocalBroker = brokerSetting.startsWith("local|");
    if (brokerSetting == "local") useLocalBroker = true;
    localConfigPending = prefs.getBool("cfg_pending", false);
    // Optional: clear the saved test session and counter.
    // prefs.remove("button_state");
    // prefs.remove("button_count");

    button.begin(prefs);
    // Optional: clear saved user configuration.
    // prefs.remove("device_name");
    // prefs.remove("tester");
    // prefs.remove("location");
    // prefs.remove("door");
    // prefs.remove("motor");
    // prefs.remove("info_rev");
    // prefs.remove("ssid");
    // prefs.remove("wifi_pass");

    tester = prefs.getString("tester", "");
    location = prefs.getString("location", "");
    doorType = prefs.getString("door", "");
    motorType = prefs.getString("motor", "");
    tester.trim();
    location.trim();
    doorType.trim();
    motorType.trim();
    infoRev = prefs.getUInt("info_rev", 0);
    wifiSsid = prefs.getString("ssid", "");
    wifiPassword = prefs.getString("wifi_pass", "");
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char id[20];
    snprintf(id, sizeof(id), "GW-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
    // Build the MQTT identity from the hardware MAC.
    gatewayId = id;
    deviceName = prefs.getString("device_name", "");
    deviceName.trim();
    uint32_t random[4];
    esp_fill_random(random, sizeof(random));
    char boot[40];
    snprintf(boot, sizeof(boot), "%08lx%08lx%08lx%08lx", (unsigned long)random[0],
             (unsigned long)random[1], (unsigned long)random[2], (unsigned long)random[3]);
    bootId = boot;
    prefix = "smo_aging/v1/" + gatewayId + "/";
    // Mount LittleFS without formatting existing data.
    fsReady = LittleFS.begin(false);
    if (fsReady)
    {
        if (!LittleFS.exists("/smo_outbox"))
            LittleFS.mkdir("/smo_outbox");
        clearOutboxOnce();
        scanOutbox();
        String saved = readFile("/smo_history.json");
        JsonDocument d;
        if (!deserializeJson(d, saved) && d.is<JsonArray>())
            history = saved;
    }
    uart.setRxBufferSize(4096);
    uart.begin(SMO_UART_BAUD, SERIAL_8N1, SMO_UART_RX, SMO_UART_TX);
#if SMO_UART_TX_OPEN_DRAIN
    const gpio_num_t uartTxPin = static_cast<gpio_num_t>(SMO_UART_TX);
    const esp_err_t txModeResult = gpio_set_direction(uartTxPin, GPIO_MODE_OUTPUT_OD);
    const esp_err_t txPullResult = gpio_set_pull_mode(uartTxPin, GPIO_PULLUP_ONLY);
    if (txModeResult != ESP_OK || txPullResult != ESP_OK)
        Serial.printf("[UART] GPIO%d open-drain setup failed: mode=%d pull=%d\n",
                      SMO_UART_TX, txModeResult, txPullResult);
#endif
    WiFi.persistent(false);
    startAp();
    if (!wifiSsid.isEmpty())
        WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    setupTime();
    events = xQueueCreate(12, sizeof(Event));
    if (!events)
    {
        Serial.println("[MQTT] Cannot allocate event queue");
        return;
    }
    Serial.printf("[SMO] %s, AP http://4.4.4.4, FS=%s\n", gatewayId.c_str(),
                  fsReady ? "ready" : "unavailable");
}
void gatewayLoop()
{
    if (brokerRestartAt && int32_t(millis() - brokerRestartAt) >= 0) ESP.restart();
    const uint32_t loopStarted = micros();
    readUart();
    if (logBatch["entries"].size() && uint32_t(millis() - logBatchStarted) >= 50) flushLogBatch();
    processStatusFrames();
    if (events)
        processEvents();
    uint32_t now = millis();
    const bool wasRunning = button.running();
    button.loop(now);
    if (wasRunning != button.running()) resetStatusFrameWindow();
    if (button.takeStateChanged() && ready)
        sendState();
    if (now - lastApCheck >= 5000)
    {
        lastApCheck = now;
        if (WiFi.getMode() != WIFI_AP_STA || WiFi.softAPIP() != IPAddress(4, 4, 4, 4))
            startAp();
    }
    if (WiFi.status() != WL_CONNECTED && !wifiSsid.isEmpty() && now - lastWifiRetry >= 60000 &&
        WiFi.softAPgetStationNum() == 0)
    {
        lastWifiRetry = now;
        WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    }
    if (hasSavedTestInfo() && events && WiFi.status() == WL_CONNECTED && getUnixTimestamp() != 0)
    {
        if (!mqtt)
            createMqtt();
    }
    if (hasSavedTestInfo() && mqtt && !started && WiFi.status() == WL_CONNECTED &&
        getUnixTimestamp() != 0)
    {
        mqttStartAttempted = true;
        mqttStartResult = esp_mqtt_client_start(mqtt);
        started = mqttStartResult == ESP_OK;
    }
    if (ready && now - lastHeartbeat >= 30000)
        heartbeat();
    if (now - lastDiagnostic >= 10000)
    {
        lastDiagnostic = now;
        Serial.printf("[DIAG] outbox=%u scan_last=%luus scan_max=%luus loop_max=%luus "
                      "web_gap_max=%luus read_max=%luus write_max=%luus mqtt_max=%luus "
                      "heap=%u heap_min=%u fs=%u/%u mqtt_outbox=%d\n",
                      outboxCount, (unsigned long)outboxScanLastUs,
                      (unsigned long)outboxScanMaxUs, (unsigned long)maxLoopUs,
                      (unsigned long)webLoopGapMaxUs, (unsigned long)frameReadMaxUs,
                      (unsigned long)frameWriteMaxUs, (unsigned long)mqttEnqueueMaxUs,
                      ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                      fsReady ? LittleFS.usedBytes() : 0,
                      fsReady ? LittleFS.totalBytes() : 0,
                      mqtt ? esp_mqtt_client_get_outbox_size(mqtt) : 0);
    }
    sendLocalConfig();
    // Retry stored frames when PUBACK is missing.
    if (pendingId >= 0 && uint32_t(now - pendingSince) >= 10000 && mqtt &&
        esp_mqtt_client_get_outbox_size(mqtt) == 0)
    {
        pendingId = -1;
        pendingSince = 0;
        pendingPath = "";
        ++stalePendingRetries;
    }
    if (button.running() && ready && fsReady && pendingId < 0 && outboxCount && now - lastPublishTry >= 50)
    {
        lastPublishTry = now;
        pendingPath = oldestPath();
        if (!pendingPath.isEmpty())
        {
            const uint32_t readStarted = micros();
            String payload = readFile(pendingPath);
            frameReadLastUs = micros() - readStarted;
            if (frameReadLastUs > frameReadMaxUs) frameReadMaxUs = frameReadLastUs;
            JsonDocument stored;
            uint8_t statusBytes[11];
            size_t statusLength = 0;
            const bool parsed = deserializeJson(stored, payload) == DeserializationError::Ok;
            const char *raw = parsed ? (stored["raw_hex"] | "") : "";
            const bool filteredSb = strncmp(raw, "AA AA 01 01 00 ", 15) == 0;
            const bool invalidStatus =
                smo::uartBytes("hex", raw, statusBytes, sizeof(statusBytes), statusLength) &&
                statusLength == sizeof(statusBytes) && statusBytes[3] == 0x02 &&
                statusBytes[4] == 0x03 && statusBytes[5] == 2 &&
                smo::decode(statusBytes, statusLength).error != nullptr;
            if (filteredSb || invalidStatus)
            {
                if (LittleFS.remove(pendingPath))
                {
                    removeIndexedOutbox(pendingPath);
                    if (filteredSb) ++filteredStoredSbFrames;
                    else ++invalidStoredStatusFrames;
                }
                pendingPath = "";
            }
            else
            {
                const uint32_t enqueueStarted = micros();
                if (esp_mqtt_client_get_outbox_size(mqtt) + payload.length() + 256 <= FrameMqttBudget)
                    pendingId = publish("up/frame", payload);
                if (pendingId >= 0) pendingSince = now;
                mqttEnqueueLastUs = micros() - enqueueStarted;
                if (mqttEnqueueLastUs > mqttEnqueueMaxUs) mqttEnqueueMaxUs = mqttEnqueueLastUs;
            }
        }
    }
    const uint32_t elapsed = micros() - loopStarted;
    if (elapsed > maxLoopUs) maxLoopUs = elapsed;
}
void gatewayWebLoopTick()
{
    const uint32_t now = micros();
    if (webLoopLastUs)
    {
        const uint32_t gap = now - webLoopLastUs;
        if (gap > webLoopGapMaxUs) webLoopGapMaxUs = gap;
    }
    webLoopLastUs = now;
}
void gatewayInfo(JsonDocument &d)
{
    d["gateway_name"] = deviceName;
    d["info_rev"] = infoRev;
    d["tester_name"] = tester;
    d["test_location"] = location;
    d["door_type"] = doorType;
    d["motor_type"] = motorType;
}
const char *gatewayLocalConfig(uint32_t cycle, uint32_t limit)
{
    if (cycle < 1 || cycle > 86400 || limit > 1000000) return "INVALID_CONFIG";
    if (!prefs.putBool("cfg_pending", true)) return "COMMAND_STORAGE_FAILED";
    const bool wasRunning = button.running();
    const char *error = button.configure(cycle, limit);
    // Report the applied configuration after a save failure.
    localConfigPending = true;
    localConfigMessageId = -1;
    if (wasRunning != button.running()) resetStatusFrameWindow();
    sendLocalConfig();
    if (ready) sendState();
    return error;
}
const char *gatewayLocalControl(const String &action)
{
    const bool wasRunning = button.running();
    const uint32_t now = millis();
    const char *error;
    if (action == "START_TEST")
    {
        String session = "AP_" + bootId + "_" + String(esp_random(), HEX);
        error = button.start(session, now);
    }
    else if (action == "PAUSE_TEST") error = button.pause(now);
    else if (action == "RESUME_TEST") error = button.resume(now);
    else if (action == "STOP_TEST") error = button.stop(now);
    else if (action == "TRIGGER_MANUAL") error = button.manual(now);
    else return "INVALID_ACTION";
    if (wasRunning != button.running()) resetStatusFrameWindow();
    if (ready) sendState();
    return error;
}
bool gatewaySaveInfo(const String &t, const String &l, const String &door, const String &motor,
                     const String &name)
{
    String displayName = name;
    displayName.trim();
    if (t.isEmpty() || l.isEmpty() || door.isEmpty() || motor.isEmpty() ||
        t.length() > 120 || l.length() > 120 || door.length() > 120 || motor.length() > 120 ||
        displayName.length() > 120)
        return false;
    if (t == tester && l == location && door == doorType && motor == motorType &&
        displayName == deviceName)
        return true;
    if (prefs.putString("tester", t) != t.length() ||
        prefs.putString("location", l) != l.length() ||
        prefs.putString("door", door) != door.length() ||
        prefs.putString("motor", motor) != motor.length() ||
        prefs.putString("device_name", displayName) != displayName.length() ||
        prefs.putUInt("info_rev", infoRev + 1) != sizeof(uint32_t))
        return false;
    tester = t;
    location = l;
    doorType = door;
    motorType = motor;
    deviceName = displayName;
    infoRev++;
    if (ready)
        sendInfo();
    return true;
}
void gatewaySaveWifi(const String &ssid, const String &password)
{
    wifiSsid = ssid;
    wifiPassword = password;
    prefs.putString("ssid", ssid);
    prefs.putString("wifi_pass", password);
    WiFi.begin(ssid.c_str(), password.c_str());
    lastWifiRetry = millis();
}
void gatewayStatus(JsonDocument &d)
{
    d["uptime_ms"] = millis();
    d["reset_reason"] = static_cast<int>(esp_reset_reason());
    d["heap_free"] = ESP.getFreeHeap();
    d["heap_min"] = ESP.getMinFreeHeap();
    d["heap_largest_block"] = ESP.getMaxAllocHeap();
    d["gateway_loop_max_us"] = maxLoopUs;
    d["web_loop_gap_max_us"] = webLoopGapMaxUs;
    d["outbox_scan_last_us"] = outboxScanLastUs;
    d["outbox_scan_max_us"] = outboxScanMaxUs;
    d["frame_read_last_us"] = frameReadLastUs;
    d["frame_read_max_us"] = frameReadMaxUs;
    d["frame_write_last_us"] = frameWriteLastUs;
    d["frame_write_max_us"] = frameWriteMaxUs;
    d["mqtt_enqueue_last_us"] = mqttEnqueueLastUs;
    d["mqtt_enqueue_max_us"] = mqttEnqueueMaxUs;
    d["littlefs_used_bytes"] = fsReady ? LittleFS.usedBytes() : 0;
    d["littlefs_total_bytes"] = fsReady ? LittleFS.totalBytes() : 0;
    d["mqtt_outbox_bytes"] = mqtt ? esp_mqtt_client_get_outbox_size(mqtt) : 0;
    d["ap_clients"] = WiFi.softAPgetStationNum();
    d["ap_ip"] = WiFi.softAPIP().toString();
    d["gateway_id"] = gatewayId;
    d["gateway_name"] = deviceName;
    d["boot_id"] = bootId;
    d["firmware"] = SMO_FW;
    d["wifi_connected"] = WiFi.status() == WL_CONNECTED;
    d["ssid"] = WiFi.SSID();
    if (WiFi.status() == WL_CONNECTED)
        d["rssi_dbm"] = WiFi.RSSI();
    else
        d["rssi_dbm"] = nullptr;
    d["ip"] = WiFi.localIP().toString();
    d["gateway_ip"] = WiFi.gatewayIP().toString();
    d["dns_ip"] = WiFi.dnsIP().toString();
    d["mqtt_connected"] = ready;
    d["mqtt_server"] = useLocalBroker ? "local" : "cloud";
    d["mqtt_local_host"] = SMO_LOCAL_MQTT_HOST;
    d["mqtt_client_created"] = mqtt != nullptr;
    d["mqtt_start_attempted"] = mqttStartAttempted;
    d["mqtt_started"] = started;
    d["mqtt_transport_connected"] = online;
    d["mqtt_subscriptions"] = subscriptions;
    d["mqtt_start_result"] = static_cast<int>(mqttStartResult);
    d["mqtt_last_event"] = mqttLastEvent.load();
    d["mqtt_error_type"] = mqttErrorType.load();
    d["mqtt_tls_esp_error"] = mqttTlsEspError.load();
    d["mqtt_tls_stack_error"] = mqttTlsStackError.load();
    d["mqtt_cert_verify_flags"] = mqttCertVerifyFlags.load();
    d["mqtt_socket_errno"] = mqttSocketError.load();
    d["mqtt_connect_return_code"] = mqttConnectReturnCode.load();
    d["mqtt_disconnects"] = mqttDisconnects;
    d["local_config_pending"] = localConfigPending;
    d["mqtt_prefix"] = prefix;
    d["button_gpio"] = SMO_BUTTON_GPIO;
    d["button_gpio_2"] = SMO_BUTTON_GPIO_2;
    d["wb350_gpio"] = SMO_WB350_GPIO;
    d["wb350_active_low"] = false;
    d["button_active_low"] = SMO_BUTTON_ACTIVE_LOW != 0;
    d["button_pulse_active"] = button.pulseActive();
    if (button.hasPressed())
        d["last_press_age_sec"] = uint32_t(millis() - button.lastPressAt()) / 1000UL;
    else
        d["last_press_age_sec"] = nullptr;
    d["run_state"] = button.runState();
    if (button.sessionId().isEmpty())
        d["session_id"] = nullptr;
    else
        d["session_id"] = button.sessionId();
    d["button_count"] = button.buttonCount();
    d["cycle_sec"] = button.cycleSeconds();
    d["button_limit"] = button.limit();
    d["elapsed_sec"] = button.elapsedSeconds(millis());
    d["time_synced"] = getUnixTimestamp() != 0;
    d["storage_ready"] = fsReady;
    d["outbox_count"] = outboxCount;
    d["live_frames_enqueued"] = liveFramesEnqueued;
    d["frame_fallbacks"] = frameFallbacks;
    d["outbox_frames_acked"] = outboxFramesAcked;
    d["outbox_pending_id"] = pendingId;
    d["outbox_pending_age_ms"] = pendingId >= 0 ? uint32_t(millis() - pendingSince) : 0;
    d["outbox_stale_retries"] = stalePendingRetries;
    d["dropped_frames_or_storage_errors"] = droppedFrames;
    d["invalid_status_frames"] = invalidStatusFrames;
    d["invalid_stored_status_frames"] = invalidStoredStatusFrames;
    d["filtered_sb_frames"] = filteredSbFrames;
    d["filtered_stored_sb_frames"] = filteredStoredSbFrames;
    d["dropped_logs"] = droppedLogs;
    d["mqtt_queue_overflows"] = rxOverflow;
    d["uart_duplicate_errors"] = frameFilter.duplicates;
    d["uart_duplicate_frames"] = frameFilter.duplicates + otherDuplicateFrames;
    d["uart_log_capture_active"] = uartLogCaptureActive();
    d["filtered_uart_fragments"] = filteredUartFragments;
    d["filtered_binary_tail_bytes"] = filteredBinaryTailBytes;
    d["uart_canceled_errors"] = frameFilter.canceled;
    d["uart_replaced_pending_errors"] = frameFilter.replaced;
    d["uart_filter_overflows"] = frameFilter.overflows;
    d["uart_unsupported_frames"] = unsupportedUartFrames;
    if (hasValidFrame)
    {
        d["smo_remote_disabled"] = lastRemoteDisabled;
        d["smo_auto_closer_counting"] = lastAutoCloserCounting;
    }
    d["frame_seq"] = frameSeq;
    d["uart_rx_pin"] = SMO_UART_RX;
    d["uart_tx_pin"] = SMO_UART_TX;
    d["uart_tx_open_drain"] = SMO_UART_TX_OPEN_DRAIN != 0;
    d["uart_baud"] = SMO_UART_BAUD;
    if (hasValidFrame)
        d["last_frame_age_ms"] = millis() - lastFrameAt;
    else
        d["last_frame_age_ms"] = nullptr;
    JsonDocument frame;
    deserializeJson(frame, lastFrame);
    d["last_frame"] = frame.as<JsonObject>();
    d["eeprom_decode_supported"] = false;
    d["button_control_supported"] = false;
}
String gatewayHistory()
{
    return history;
}
const char *gatewaySelectBroker(const String &mode)
{
    if (button.running() || button.pulseActive()) return "Stop the test before changing server.";
    if (mode != "cloud" && mode != "local") return "Invalid server selection.";
    if (!prefs.putString("broker_select", mode)) return "Could not save server settings.";
    useLocalBroker = mode == "local";
    brokerRestartAt = millis() + 2000;
    return nullptr;
}
void gatewayUartStatus(JsonDocument &d)
{
    d["boot_id"] = bootId;
    d["uptime_ms"] = millis();
    d["capture_open"] = uartLogCapture.active();
    d["mqtt_connected"] = ready;
    d["rx_bytes"] = uartBytes;
    if (uartBytes) d["last_rx_age_ms"] = uint32_t(millis() - lastByteAt);
    else d["last_rx_age_ms"] = nullptr;
    d["chunks_created"] = previewSeq;
    d["mqtt_queued"] = logQueued;
    d["mqtt_enqueue_failed"] = logQueueFailed;
    d["dropped_logs"] = droppedLogs;
    d["uart_log_sent"] = logSent.load();
    d["uart_log_send_failed"] = logSendFailed.load();
    d["uart_log_pending_batches"] = logBatches ? uxQueueMessagesWaiting(logBatches) : 0;
    d["uart_log_sender_ready"] = logBatches != nullptr;
    d["mqtt_outbox_bytes"] = mqtt ? esp_mqtt_client_get_outbox_size(mqtt) : 0;
    d["mqtt_disconnects"] = mqttDisconnects;
    d["heap_free"] = ESP.getFreeHeap();
    d["heap_min"] = ESP.getMinFreeHeap();
    d["cmd_id"] = lastUartCommand;
    d["action"] = lastUartAction;
    d["ack"] = lastUartAck;
    d["error"] = lastUartError;
    d["last_tx_hex"] = lastUartTx;
    auto rows = d["rows"].to<JsonArray>();
    const uint32_t count = previewSeq < 40 ? previewSeq : 40;
    for (uint32_t i = previewSeq - count; i < previewSeq; ++i)
    {
        const auto &entry = uartPreview[i % 40];
        auto row = rows.add<JsonObject>();
        row["seq"] = entry.seq;
        row["at_ms"] = entry.at;
        row["direction"] = entry.direction;
        row["encoding"] = entry.encoding;
        row["data"] = entry.data;
    }
}
