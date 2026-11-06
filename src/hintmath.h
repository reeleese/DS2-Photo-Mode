// The start hint's date logic (no Windows here, so tests/hintmath_test.cpp can run it).
// photo-mode.ini beside the DLL: "[hint]" then "last_opened=YYYY-MM-DD".
#pragma once
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace pm {

enum { HINT_DAYS = 30 };

// Days since 1970-01-01 of a civil date (proleptic Gregorian).
inline long hint_days(int y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// The last_opened date in the ini's text, as days; false if absent or malformed.
inline bool hint_parse(const char* text, size_t n, long* days) {
    const char key[] = "last_opened=";
    for (size_t i = 0; i + sizeof key <= n; i++) {
        if (i && text[i - 1] != '\n') continue;  // start of a line only
        if (memcmp(text + i, key, sizeof key - 1)) continue;
        const char* p = text + i + sizeof key - 1;
        size_t left = n - (i + sizeof key - 1);
        if (left < 10) return false;
        for (int k = 0; k < 10; k++) {
            bool dash = k == 4 || k == 7;
            if (dash ? p[k] != '-' : (p[k] < '0' || p[k] > '9')) return false;
        }
        int y = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
        int m = (p[5] - '0') * 10 + (p[6] - '0'), d = (p[8] - '0') * 10 + (p[9] - '0');
        if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return false;
        *days = hint_days(y, m, d);
        return true;
    }
    return false;
}

// Show the hint? No readable date (first run, missing or damaged file, a date in the future)
// or more than HINT_DAYS since photo mode was last opened.
inline bool hint_due(bool have_date, long last, long today) {
    if (!have_date || last > today) return true;
    return today - last > HINT_DAYS;
}

inline void hint_format(char out[32], int y, int m, int d) {
    snprintf(out, 32, "[hint]\nlast_opened=%04d-%02d-%02d\n", y, m, d);
}

}  // namespace pm
