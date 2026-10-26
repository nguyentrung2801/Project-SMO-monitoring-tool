#pragma once

#include <Arduino.h>

void setupTime();
String getTimestamp();
uint32_t getUnixTimestamp();
String getUtcIsoTimestamp();
