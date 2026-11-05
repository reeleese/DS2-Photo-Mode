// Native checks for src/uilayout.h (the overlay's fixed width). Run with tests/run.sh.
#include "uilayout.h"
#include <stdio.h>

using namespace pm;

int main() {
    int fail = 0;
    WidthFit f;
    const float tabs[] = {70, 90, 40, 45};
    f.need_tabs(tabs, 4, 20);
    if (f.w != 70 + 90 + 40 + 45 + 60) { printf("FAIL tabs %g\n", f.w); fail++; }
    f.need_row(100, 300, 24);
    if (f.w != 424) { printf("FAIL row %g\n", f.w); fail++; }
    f.need_row(10, 10, 24);  // a narrower row never shrinks it
    f.need(200);
    if (f.w != 424) { printf("FAIL max %g\n", f.w); fail++; }
    if (!fail) printf("uilayout ok\n");
    return fail;
}
