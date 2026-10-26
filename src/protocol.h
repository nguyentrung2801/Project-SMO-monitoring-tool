#pragma once
#include "frame_decoder.h"
namespace smo
{
SMO_CONSTEXPR int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
SMO_CONSTEXPR bool equal(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        ++a;
        ++b;
    }
    return *a == *b;
}
SMO_CONSTEXPR bool uartBytes(const char *encoding, const char *data, uint8_t *out, size_t capacity,
                             size_t &length)
{
    length = 0;
    if (!encoding || !data)
        return false;
    if (equal(encoding, "ascii"))
    {
        for (const char *p = data; *p; ++p)
        {
            if (static_cast<unsigned char>(*p) > 127 || length >= capacity)
                return false;
            out[length++] = *p;
        }
    }
    else if (equal(encoding, "hex"))
    {
        int high = -1;
        for (const char *p = data; *p; ++p)
        {
            if (*p == ' ')
            {
                if (high >= 0)
                    return false;
                continue;
            }
            int v = hexDigit(*p);
            if (v < 0)
                return false;
            if (high < 0)
                high = v;
            else
            {
                if (length >= capacity)
                    return false;
                out[length++] = (uint8_t)((high << 4) | v);
                high = -1;
            }
        }
        if (high >= 0)
            return false;
    }
    else
        return false;
    return length > 0;
}
} // namespace smo
