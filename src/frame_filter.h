#pragma once
#include "frame_decoder.h"
#include "frame_deduplicator.h"

namespace smo
{
struct StatusRecord
{
    uint8_t bytes[11] = {};
    uint32_t receivedAt = 0;
    uint32_t unixTime = 0;
    bool publishAllowed = true;
};

// Confirm distinct errors in receive order after the configured delay.
class FrameFilter
{
  public:
    static constexpr size_t Capacity = 32;
    SMO_CONSTEXPR FrameFilter(uint32_t cancelMs = 40, uint32_t confirmMs = 100,
                              uint32_t duplicateMs = 100)
        : cancelMs_(cancelMs), confirmMs_(confirmMs), duplicateMs_(duplicateMs)
    {
    }

    bool push(const uint8_t *bytes, uint32_t receivedAt, uint32_t processedAt,
                            uint32_t unixTime = 0, bool publishAllowed = true)
    {
        const Decoded d = decode(bytes, 11);
        const bool valid = d.error == nullptr;
        if (valid && bytes[7] == 0)
        {
            for (size_t i = 0; i < count_; ++i)
            {
                const size_t slot = (head_ + i) % Capacity;
                if (states_[slot] == Pending &&
                    uint32_t(receivedAt - records_[slot].receivedAt) < cancelMs_)
                {
                    states_[slot] = Dropped;
                    ++canceled;
                }
            }
        }
        // Let No Error cancel recent pending errors before deduplication.
        if (deduplicator_.duplicate(bytes, 11, receivedAt, duplicateMs_))
        {
            ++duplicates;
            return true;
        }
        discardHead();
        if (count_ == Capacity)
        {
            ++overflows;
            return false;
        }

        const size_t slot = (head_ + count_) % Capacity;
        records_[slot].receivedAt = receivedAt;
        records_[slot].unixTime = unixTime;
        records_[slot].publishAllowed = publishAllowed;
        for (size_t i = 0; i < 11; ++i)
            records_[slot].bytes[i] = bytes[i];
        states_[slot] = Ready;
        ++count_;
        if (valid && reportableFault(bytes[7]))
        {
            states_[slot] = Pending;
            pendingStarted_[slot] = processedAt;
        }
        return true;
    }

    SMO_CONSTEXPR void confirm(uint32_t now)
    {
        for (size_t i = 0; i < count_; ++i)
        {
            const size_t slot = (head_ + i) % Capacity;
            if (states_[slot] == Pending && uint32_t(now - pendingStarted_[slot]) >= confirmMs_)
                states_[slot] = Ready;
        }
    }

    SMO_CONSTEXPR bool pop(StatusRecord &out)
    {
        discardHead();
        if (!count_ || states_[head_] == Pending)
            return false;
        out = records_[head_];
        head_ = (head_ + 1) % Capacity;
        --count_;
        return true;
    }

    uint32_t duplicates = 0;
    uint32_t canceled = 0;
    uint32_t replaced = 0; // Legacy status field; distinct errors are no longer replaced.
    uint32_t overflows = 0;
    void resetWindow()
    {
        head_ = count_ = 0;
        deduplicator_ = FrameDeduplicator<11, Capacity>();
    }

  private:
    enum State : uint8_t
    {
        Ready,
        Pending,
        Dropped
    };
    StatusRecord records_[Capacity] = {};
    State states_[Capacity] = {};
    size_t head_ = 0, count_ = 0;
    uint32_t pendingStarted_[Capacity] = {};
    uint32_t cancelMs_, confirmMs_, duplicateMs_;
    FrameDeduplicator<11, Capacity> deduplicator_;
    SMO_CONSTEXPR void discardHead()
    {
        while (count_ && states_[head_] == Dropped)
        {
            head_ = (head_ + 1) % Capacity;
            --count_;
        }
    }
};
} // namespace smo
