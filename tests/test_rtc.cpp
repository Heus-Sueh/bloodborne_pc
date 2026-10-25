#include "runtime_rtc.hpp"
#include "runtime.h"

#include <cassert>
#include <cstring>
#include <iostream>

/* Mock runtime_registry_resolve if needed */
extern "C" {
uintptr_t runtime_registry_resolve(const char*, int) { return 0; }
void runtime_register_exports(const RuntimeExport*, size_t) {}
}

int main() {
    std::cout << "[RTC C++23 TEST] Running unit tests...\n";

    // 1. Leap year calculations
    assert(bb::rtc::IsLeap(2000) == true);
    assert(bb::rtc::IsLeap(1900) == false);
    assert(bb::rtc::IsLeap(2024) == true);
    assert(bb::rtc::IsLeap(2025) == false);

    // 2. Days in month
    assert(bb::rtc::DaysInMonth(2024, 2) == 29);
    assert(bb::rtc::DaysInMonth(2025, 2) == 28);
    assert(bb::rtc::DaysInMonth(2026, 1) == 31);
    assert(bb::rtc::DaysInMonth(2026, 4) == 30);

    // 3. Civil / Days roundtrip
    int y = 0, m = 0, d = 0;
    int64_t days_epoch = bb::rtc::DaysFromCivil(1, 1, 1);
    assert(days_epoch == 0);
    bb::rtc::CivilFromDays(0, y, m, d);
    assert(y == 1 && m == 1 && d == 1);

    int64_t days_2026 = bb::rtc::DaysFromCivil(2026, 10, 25);
    bb::rtc::CivilFromDays(days_2026, y, m, d);
    assert(y == 2026 && m == 10 && d == 25);

    // 4. Validation
    bb::rtc::DateTime dt{};
    dt.year = 2026;
    dt.month = 10;
    dt.day = 25;
    dt.hour = 12;
    dt.minute = 30;
    dt.second = 45;
    dt.microsecond = 500000;
    assert(bb::rtc::Validate(&dt) == 0);

    dt.year = 0;
    assert(bb::rtc::Validate(&dt) == bb::rtc::ERR_INVALID_YEAR);
    dt.year = 2026;

    dt.month = 13;
    assert(bb::rtc::Validate(&dt) == bb::rtc::ERR_INVALID_MONTH);
    dt.month = 2;
    dt.day = 29; // 2026 is not leap
    assert(bb::rtc::Validate(&dt) == bb::rtc::ERR_INVALID_DAY);
    dt.month = 10;
    dt.day = 25;

    dt.hour = 24;
    assert(bb::rtc::Validate(&dt) == bb::rtc::ERR_INVALID_HOUR);
    dt.hour = 12;

    dt.minute = 60;
    assert(bb::rtc::Validate(&dt) == bb::rtc::ERR_INVALID_MINUTE);
    dt.minute = 30;

    dt.second = 60;
    assert(bb::rtc::Validate(&dt) == bb::rtc::ERR_INVALID_SECOND);
    dt.second = 45;

    dt.microsecond = 1000000;
    assert(bb::rtc::Validate(&dt) == bb::rtc::ERR_INVALID_MICROSECOND);
    dt.microsecond = 500000;

    // 5. ToTick / FromTick roundtrip
    uint64_t tick = bb::rtc::ToTick(&dt);
    bb::rtc::DateTime roundtrip{};
    bb::rtc::FromTick(tick, &roundtrip);
    assert(roundtrip.year == dt.year);
    assert(roundtrip.month == dt.month);
    assert(roundtrip.day == dt.day);
    assert(roundtrip.hour == dt.hour);
    assert(roundtrip.minute == dt.minute);
    assert(roundtrip.second == dt.second);
    assert(roundtrip.microsecond == dt.microsecond);

    // 6. Day of week
    // 0001-01-01 was Monday (1)
    int dow_0001 = static_cast<int>((bb::rtc::DaysFromCivil(1, 1, 1) + 1) % 7);
    assert(dow_0001 == 1);

    // 1970-01-01 was Thursday (4)
    int dow_1970 = static_cast<int>((bb::rtc::DaysFromCivil(1970, 1, 1) + 1) % 7);
    assert(dow_1970 == 4);

    std::cout << "PASS: RTC C++23 unit tests succeeded!\n";
    return 0;
}
