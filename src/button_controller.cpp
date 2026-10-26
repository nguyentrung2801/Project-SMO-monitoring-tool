#include "button_controller.h"
#include "config.h"
#include <cstring>

namespace {
// Persist session and configuration in one versioned NVS record.
struct SavedButtonState {
    uint32_t version, state, cycle, limit, count, elapsed, reserved;
    char session[100];
};
static_assert(sizeof(SavedButtonState) == 128, "NVS button state layout changed");
constexpr const char *stateKey = "button_state";
}

void ButtonController::begin(Preferences &storage)
{
    storage_ = &storage;
    // Release all button outputs before restoring state.
    digitalWrite(SMO_BUTTON_GPIO, SMO_BUTTON_ACTIVE_LOW ? HIGH : LOW);
    digitalWrite(SMO_BUTTON_GPIO_2, SMO_BUTTON_ACTIVE_LOW ? HIGH : LOW);
    digitalWrite(SMO_WB350_GPIO, LOW);
    pinMode(SMO_WB350_GPIO, OUTPUT);
    pinMode(SMO_BUTTON_GPIO, OUTPUT);
    pinMode(SMO_BUTTON_GPIO_2, OUTPUT);
    if (storage.isKey(stateKey))
    {
        SavedButtonState saved = {};
        const bool readOk = storage.getBytesLength(stateKey) == sizeof(saved) &&
            storage.getBytes(stateKey, &saved, sizeof(saved)) == sizeof(saved);
        const bool valid = readOk && saved.version == 1 && saved.state <= uint32_t(State::Fault) &&
            saved.cycle <= 86400 && saved.limit <= 1000000 && saved.session[96] == '\0' &&
            (saved.state == uint32_t(State::Idle) || saved.state == uint32_t(State::Fault) ||
             (saved.cycle > 0 && saved.session[0] != '\0')) &&
            (saved.state != uint32_t(State::Running) || !saved.limit || saved.count < saved.limit);
        if (!valid)
        {
            setState(State::Fault);
            Serial.println("[BUTTON] Invalid saved state; relay remains released");
            return;
        }
        state_ = State(saved.state);
        cycleSeconds_ = saved.cycle;
        limit_ = saved.limit;
        buttonCount_ = saved.count;
        sessionId_ = saved.session;
        elapsedBaseSeconds_ = saved.elapsed;
        runStartedAt_ = elapsedStoppedAt_ = millis();
        // Do not replay interrupted or missed presses.
        nextPressAt_ = runStartedAt_ + cycleSeconds_ * 1000UL;
        stateChanged_ = true;
        Serial.printf("[BUTTON] Restored state=%s count=%lu\n", runState(),
                      static_cast<unsigned long>(buttonCount_));
        return;
    }
    // Import settings saved by older firmware.
    cycleSeconds_ = storage.getUInt("cycle_sec", 0);
    if (cycleSeconds_ < 1 || cycleSeconds_ > 86400)
        cycleSeconds_ = 0;
    limit_ = storage.getUInt("press_limit", 0);
    if (limit_ > 1000000)
        limit_ = 0;
    buttonCount_ = storage.getUInt("button_count", 0);
}

bool ButtonController::persist(uint32_t now) const
{
    SavedButtonState saved = {};
    saved.version = 1;
    saved.state = uint32_t(state_);
    saved.cycle = cycleSeconds_;
    saved.limit = limit_;
    saved.count = buttonCount_;
    saved.elapsed = elapsedSeconds(now);
    sessionId_.toCharArray(saved.session, sizeof(saved.session));
    return storage_ && storage_->putBytes(stateKey, &saved, sizeof(saved)) == sizeof(saved);
}

bool ButtonController::commit(ButtonController &next, uint32_t now)
{
    if (!next.persist(now))
        return false;
    *this = next;
    stateChanged_ = true;
    return true;
}

void ButtonController::setState(State state)
{
    if (state_ != state)
    {
        state_ = state;
        stateChanged_ = true;
    }
}
void ButtonController::release()
{
    if (!pulseActive_)
        return;
    digitalWrite(SMO_BUTTON_GPIO, SMO_BUTTON_ACTIVE_LOW ? HIGH : LOW);
    digitalWrite(SMO_BUTTON_GPIO_2, SMO_BUTTON_ACTIVE_LOW ? HIGH : LOW);
    digitalWrite(SMO_WB350_GPIO, LOW);
    pulseActive_ = false;
    stateChanged_ = true;
}
const char *ButtonController::press(uint32_t now, bool applyTestLimit)
{
    if (pulseActive_)
        return "BUTTON_BUSY";
    if (applyTestLimit && limit_ && buttonCount_ >= limit_)
    {
        setState(State::Completed);
        return "BUTTON_LIMIT_REACHED";
    }
    ButtonController next = *this;
    ++next.buttonCount_;
    if (applyTestLimit && next.limit_ && next.buttonCount_ >= next.limit_)
    {
        next.elapsedStoppedAt_ = now;
        next.setState(State::Completed);
    }
    // Save the press before asserting GPIO.
    if (!commit(next, now))
        return "COMMAND_STORAGE_FAILED";
    digitalWrite(SMO_BUTTON_GPIO, SMO_BUTTON_ACTIVE_LOW ? LOW : HIGH);
    digitalWrite(SMO_BUTTON_GPIO_2, SMO_BUTTON_ACTIVE_LOW ? LOW : HIGH);
    digitalWrite(SMO_WB350_GPIO, HIGH);
    pulseActive_ = true;
    hasPressed_ = true;
    pulseStartedAt_ = now;
    nextPressAt_ = now + cycleSeconds_ * 1000UL;
    stateChanged_ = true;
    return nullptr;
}
void ButtonController::loop(uint32_t now)
{
    if (pulseActive_ && uint32_t(now - pulseStartedAt_) >= SMO_BUTTON_PULSE_MS)
        release();
    if (state_ == State::Running && !pulseActive_ && int32_t(now - nextPressAt_) >= 0)
    {
        const char *error = press(now);
        if (error)
        {
            elapsedStoppedAt_ = now;
            setState(State::Fault);
            persist(now);
        }
    }
}
const char *ButtonController::configure(uint32_t intervalSeconds, uint32_t pressLimit)
{
    if (intervalSeconds < 1 || intervalSeconds > 86400 || pressLimit > 1000000)
        return "INVALID_CONFIG";
    const uint32_t now = millis();
    ButtonController next = *this;
    next.cycleSeconds_ = intervalSeconds;
    next.limit_ = pressLimit;
    if (next.state_ == State::Running)
        next.nextPressAt_ = now + intervalSeconds * 1000UL;
    if (pressLimit && next.buttonCount_ >= pressLimit && next.state_ == State::Running)
    {
        next.elapsedStoppedAt_ = now;
        next.setState(State::Completed);
    }
    if (!commit(next, now))
        return "COMMAND_STORAGE_FAILED";
    return nullptr;
}
const char *ButtonController::start(const String &session, uint32_t now)
{
    if (cycleSeconds_ == 0)
        return "CONFIG_REQUIRED";
    if (session.isEmpty() || session.length() > 96)
        return "INVALID_SESSION_ID";
    if (state_ == State::Running || state_ == State::Paused)
        return "TEST_ALREADY_ACTIVE";
    if (pulseActive_)
        return "BUTTON_BUSY";
    ButtonController next = *this;
    next.buttonCount_ = 0;
    next.sessionId_ = session;
    next.runStartedAt_ = now;
    next.elapsedStoppedAt_ = 0;
    next.elapsedBaseSeconds_ = 0;
    next.setState(State::Running);
    // Save the session and first press before changing GPIO.
    const char *error = next.press(now);
    if (error) return error;
    *this = next;
    return nullptr;
}
const char *ButtonController::pause(uint32_t now)
{
    if (state_ != State::Running)
        return "TEST_NOT_RUNNING";
    ButtonController next = *this;
    next.setState(State::Paused);
    if (!commit(next, now)) return "COMMAND_STORAGE_FAILED";
    release();
    return nullptr;
}
const char *ButtonController::resume(uint32_t now)
{
    if (state_ != State::Paused)
        return "TEST_NOT_PAUSED";
    ButtonController next = *this;
    next.nextPressAt_ = now + cycleSeconds_ * 1000UL;
    next.setState(State::Running);
    if (next.limit_ && next.buttonCount_ >= next.limit_)
    {
        next.elapsedStoppedAt_ = now;
        next.setState(State::Completed);
    }
    if (!commit(next, now)) return "COMMAND_STORAGE_FAILED";
    return nullptr;
}
const char *ButtonController::stop(uint32_t now)
{
    if (state_ != State::Running && state_ != State::Paused)
        return "TEST_NOT_ACTIVE";
    ButtonController next = *this;
    next.elapsedStoppedAt_ = now;
    next.setState(State::Stopped);
    if (!commit(next, now)) return "COMMAND_STORAGE_FAILED";
    release();
    return nullptr;
}
const char *ButtonController::manual(uint32_t now)
{
    // Apply the press limit only during a running test.
    return press(now, state_ == State::Running);
}
const char *ButtonController::resetCount()
{
    if (state_ == State::Running || state_ == State::Paused)
        return "TEST_ACTIVE";
    ButtonController next = *this;
    next.buttonCount_ = 0;
    if (!commit(next, millis())) return "COMMAND_STORAGE_FAILED";
    return nullptr;
}
const char *ButtonController::runState() const
{
    switch (state_)
    {
    case State::Running:
        return "running";
    case State::Paused:
        return "paused";
    case State::Stopped:
        return "idle";
    case State::Completed:
        return "completed";
    case State::Fault:
        return "error";
    default:
        return "idle";
    }
}
uint32_t ButtonController::elapsedSeconds(uint32_t now) const
{
    if (state_ == State::Idle)
        return 0;
    const uint32_t end =
        (state_ == State::Running || state_ == State::Paused) ? now : elapsedStoppedAt_;
    return elapsedBaseSeconds_ + uint32_t(end - runStartedAt_) / 1000UL;
}
