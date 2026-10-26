#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace smo
{
class UartLogCapture
{
  public:
    static bool isRequest(const uint8_t *bytes, size_t length)
    {
        static const uint8_t request[] = {0xAA, 0xAA, 1, 3, 0, 1, 1, 2, 0x55, 0x55};
        return length == sizeof(request) && memcmp(bytes, request, sizeof(request)) == 0;
    }
    static bool isStopRequest(const uint8_t *bytes, size_t length)
    {
        static const uint8_t request[] = {'D', 'I', 'S', 'A', 'B', 'L', 'E'};
        size_t first = 0, last = length;
        while (first < last && (bytes[first] == '\r' || bytes[first] == '\n')) ++first;
        while (last > first && (bytes[last - 1] == '\r' || bytes[last - 1] == '\n')) --last;
        return last - first == sizeof(request) && memcmp(bytes + first, request, sizeof(request)) == 0;
    }
    void stop()
    {
        open_ = false;
    }
    void begin() { open_ = true; }
    bool active() const { return open_; }
    static bool ascii(uint8_t b)
    {
        return (b >= 32 && b <= 126) || b == '\r' || b == '\n' || b == '\t';
    }
  private:
    bool open_ = false;
};
} // namespace smo
