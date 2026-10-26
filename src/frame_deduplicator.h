#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace smo
{
// Suppress identical raw frames within the configured interval.
template <size_t MaxLength, size_t Capacity = 32> class FrameDeduplicator
{
  public:
    bool duplicate(const uint8_t *bytes, size_t length, uint32_t at, uint32_t window)
    {
        if (!length || length > MaxLength)
            return false;
        for (size_t i = 0; i < Capacity; ++i)
        {
            if (entries_[i].length == length && uint32_t(at - entries_[i].at) <= window &&
                memcmp(entries_[i].bytes, bytes, length) == 0)
                return true;
        }
        Entry &entry = entries_[next_];
        memcpy(entry.bytes, bytes, length);
        entry.length = length;
        entry.at = at;
        next_ = (next_ + 1) % Capacity;
        return false;
    }

  private:
    struct Entry
    {
        uint8_t bytes[MaxLength] = {};
        size_t length = 0;
        uint32_t at = 0;
    };
    Entry entries_[Capacity] = {};
    size_t next_ = 0;
};
} // namespace smo
