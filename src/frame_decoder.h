#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#if __cplusplus >= 201402L
#define SMO_CONSTEXPR constexpr
#else
#define SMO_CONSTEXPR inline
#endif
namespace smo
{
// Fault codes reported to the server.
SMO_CONSTEXPR bool reportableFault(uint8_t code)
{
    return code >= 0x01 && code <= 0x07 && code != 0x04;
}
struct Decoded
{
    const char *door = "unknown";
    const char *direction = "unknown";
    const char *fault = nullptr;
    const char *error = nullptr;
    bool light = false;
    bool ir = false;
    bool remoteDisabled = false;
    bool autoCloserCounting = false;
};
SMO_CONSTEXPR Decoded decode(const uint8_t *b, size_t n)
{
    Decoded d;
    if (n != 11)
    {
        d.error = "INVALID_LENGTH";
        return d;
    }
    if (b[0] != 0xAA || b[1] != 0xAA || b[9] != 0x55 || b[10] != 0x55)
    {
        d.error = "INVALID_FRAME";
        return d;
    }
    uint8_t cs = 0;
    for (int i = 2; i <= 7; i++)
        cs ^= b[i];
    if (cs != b[8])
    {
        d.error = "CHECKSUM_FAILED";
        return d;
    }
    if (b[2] != 1 || b[3] != 2 || b[4] != 3 || b[5] != 2)
    {
        d.error = "UNSUPPORTED_FRAME";
        return d;
    }
    const char *doors[] = {"opening", "open", "partial", "closing", "closed"};
    unsigned state = b[6] >> 4;
    if (state >= 5 || b[7] > 7)
    {
        d.error = "UNKNOWN_STATUS_OR_FAULT";
        return d;
    }
    d.door = doors[state];
    // Open and closed states are idle; partial has no direction.
    d.direction = state == 0 ? "up" : state == 3 ? "down" :
                  (state == 1 || state == 4) ? "idle" : "unknown";
    d.light = (b[6] & 1) != 0;
    d.remoteDisabled = (b[6] & 2) != 0;
    d.autoCloserCounting = (b[6] & 4) != 0;
    const char *faults[] = {nullptr,
                            "MOTOR_TIMEOUT",
                            "OVER_FORCE",
                            "IR_BLOCKED",
                            "REMOTE_DISABLED",
                            "WALL_CONSOLE_SHORT",
                            "MOTOR_SENSOR_NO_SIGNAL",
                            "IR_FAULT"};
    d.fault = faults[b[7]];
    d.ir = b[7] == 3;
    return d;
}
} // namespace smo
