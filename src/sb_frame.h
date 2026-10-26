#pragma once
#include <stddef.h>
#include <stdint.h>
namespace smo {
// Accept complete SB envelopes and forward their payload unchanged.
inline bool isSbFrame(const uint8_t *b, size_t n) {
    if (n < 9 || n != size_t(b[5]) + 9 || b[0] != 0xAA || b[1] != 0xAA ||
        b[2] != 1 || b[3] != 1 || b[n-2] != 0x55 || b[n-1] != 0x55) return false;
    return true;
}
inline bool isFilteredSbFrame(const uint8_t *b, size_t n) {
    return isSbFrame(b, n) && b[4] == 0x00;
}
}
