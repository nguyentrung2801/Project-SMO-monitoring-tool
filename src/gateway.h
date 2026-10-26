#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
void gatewayBegin();
const char *gatewaySelectBroker(const String &mode);
void gatewayLoop();
void gatewayWebLoopTick();
void gatewayStatus(JsonDocument &doc);
void gatewayUartStatus(JsonDocument &doc);
const char *gatewayLocalConfig(uint32_t cycle, uint32_t limit);
const char *gatewayLocalControl(const String &action);
void gatewayInfo(JsonDocument &doc);
bool gatewaySaveInfo(const String &tester, const String &location,
                     const String &door, const String &motor, const String &deviceName);
void gatewaySaveWifi(const String &ssid, const String &password);
String gatewayHistory();
void webBegin();
void webLoop();
