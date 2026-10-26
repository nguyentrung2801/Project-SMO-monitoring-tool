#pragma once
#include <Arduino.h>
#include <Preferences.h>

// Drive both relay outputs for each RF remote press.
class ButtonController
{
  public:
    void begin(Preferences &storage);
    void loop(uint32_t now);
    const char *configure(uint32_t intervalSeconds, uint32_t pressLimit);
    const char *start(const String &session, uint32_t now);
    const char *pause(uint32_t now);
    const char *resume(uint32_t now);
    const char *stop(uint32_t now);
    const char *manual(uint32_t now);
    const char *resetCount();
    const char *runState() const;
    bool running() const { return state_ == State::Running; }
    const String &sessionId() const
    {
        return sessionId_;
    }
    uint32_t buttonCount() const
    {
        return buttonCount_;
    }
    uint32_t cycleSeconds() const
    {
        return cycleSeconds_;
    }
    uint32_t limit() const
    {
        return limit_;
    }
    bool pulseActive() const
    {
        return pulseActive_;
    }
    bool hasPressed() const
    {
        return hasPressed_;
    }
    uint32_t lastPressAt() const
    {
        return pulseStartedAt_;
    }
    uint32_t elapsedSeconds(uint32_t now) const;
    bool takeStateChanged()
    {
        bool changed = stateChanged_;
        stateChanged_ = false;
        return changed;
    }

  private:
    enum class State : uint8_t
    {
        Idle,
        Running,
        Paused,
        Stopped,
        Completed,
        Fault
    };
    const char *press(uint32_t now, bool applyTestLimit = true);
    void release();
    void setState(State state);
    bool persist(uint32_t now) const;
    bool commit(ButtonController &next, uint32_t now);
    Preferences *storage_ = nullptr;
    State state_ = State::Idle;
    String sessionId_;
    // Zero means no cycle configuration is saved.
    uint32_t cycleSeconds_ = 0, limit_ = 0, buttonCount_ = 0;
    uint32_t nextPressAt_ = 0, pulseStartedAt_ = 0, runStartedAt_ = 0, elapsedStoppedAt_ = 0;
    uint32_t elapsedBaseSeconds_ = 0;
    bool pulseActive_ = false, stateChanged_ = false, hasPressed_ = false;
};
