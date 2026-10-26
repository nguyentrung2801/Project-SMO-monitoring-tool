#include "time_manager.h"

#include <time.h>
#include <sys/time.h>

namespace
{
constexpr long GMT_OFFSET_SECONDS = 0;
constexpr int DAYLIGHT_OFFSET_SECONDS = 0;
constexpr time_t MINIMUM_VALID_UNIX_TIME = 1700000000;
} // namespace

void setupTime()
{
    configTime(GMT_OFFSET_SECONDS, DAYLIGHT_OFFSET_SECONDS, "pool.ntp.org", "time.google.com");
}

String getTimestamp()
{
    struct tm timeInfo;
    if (!getLocalTime(&timeInfo, 10))
    {
        return "Chua dong bo";
    }

    char timestamp[24];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &timeInfo);
    return String(timestamp);
}

uint32_t getUnixTimestamp()
{
    const time_t currentTime = time(nullptr);
    if (currentTime < MINIMUM_VALID_UNIX_TIME)
    {
        return 0;
    }

    return static_cast<uint32_t>(currentTime);
}

String getUtcIsoTimestamp()
{
    struct timeval now = {};
    gettimeofday(&now, nullptr);
    const time_t currentTime = now.tv_sec;
    if (currentTime < MINIMUM_VALID_UNIX_TIME)
    {
        return "";
    }

    struct tm utcTime;
    gmtime_r(&currentTime, &utcTime);

    char seconds[20], timestamp[25];
    strftime(seconds, sizeof(seconds), "%Y-%m-%dT%H:%M:%S", &utcTime);
    snprintf(timestamp, sizeof(timestamp), "%s.%03uZ", seconds,
             static_cast<unsigned>(now.tv_usec / 1000));
    return String(timestamp);
}
