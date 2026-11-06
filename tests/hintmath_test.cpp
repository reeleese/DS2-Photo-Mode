// Native checks for src/hintmath.h (when the start hint shows). Run with tests/run.sh.
#include "hintmath.h"
#include <stdio.h>

using namespace pm;

static int fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fail++; } } while (0)

static bool due(const char* ini, int y, int m, int d) {
    long last = 0;
    bool have = ini && hint_parse(ini, strlen(ini), &last);
    return hint_due(have, last, hint_days(y, m, d));
}

int main() {
    CHECK(hint_days(1970, 1, 1) == 0);
    CHECK(hint_days(2026, 3, 1) - hint_days(2026, 2, 1) == 28);
    CHECK(hint_days(2024, 3, 1) - hint_days(2024, 2, 1) == 29);
    CHECK(hint_days(2027, 1, 1) - hint_days(2026, 1, 1) == 365);

    const char* ini = "[hint]\nlast_opened=2026-10-06\n";
    CHECK(!due(ini, 2026, 10, 6));    // same day
    CHECK(!due(ini, 2026, 11, 5));    // exactly 30 days
    CHECK(due(ini, 2026, 11, 6));     // 31 days
    CHECK(due(ini, 2027, 6, 1));
    CHECK(!due("[hint]\r\nlast_opened=2026-10-06\r\n", 2026, 10, 20));  // CRLF
    CHECK(!due("last_opened=2026-10-06", 2026, 10, 20));                // no newline, no section

    CHECK(due(nullptr, 2026, 11, 5));                  // no file: first run
    CHECK(due("", 2026, 11, 5));                       // empty
    CHECK(due("[hint]\nlast_opened=garbage\n", 2026, 11, 5));
    CHECK(due("[hint]\nlast_opened=2026-13-01\n", 2026, 11, 5));
    CHECK(due("[hint]\nlast_opened=2026-1-1\n", 2026, 11, 5));
    CHECK(due("[hint]\nxlast_opened=2026-11-04\n", 2026, 11, 5));       // key must start a line
    CHECK(due("[hint]\nlast_opened=2030-01-01\n", 2026, 11, 5));        // future: clock moved, ask again
    CHECK(due("[hint]\nlast_opened=2026-11-0", 2026, 11, 5));           // truncated

    char out[32];
    hint_format(out, 2026, 3, 7);
    CHECK(!strcmp(out, "[hint]\nlast_opened=2026-03-07\n"));
    long d;
    CHECK(hint_parse(out, strlen(out), &d) && d == hint_days(2026, 3, 7));

    if (!fail) printf("hintmath ok\n");
    return fail;
}
