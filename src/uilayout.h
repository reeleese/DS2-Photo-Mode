// The overlay's fixed width: the widest thing any panel or value can need (measured by the
// caller with the real font), so the window's right edge never moves. Pure, so tests/ can check it.
#pragma once

namespace pm {

struct WidthFit {
    float w = 0;
    void need(float x) { if (x > w) w = x; }
    // A row: label left, value right-aligned, at least `gap` between them.
    void need_row(float label_w, float value_w, float gap) { need(label_w + gap + value_w); }
    // The tab names in a line, `gap` apart.
    void need_tabs(const float* name_w, int n, float gap) {
        float t = 0;
        for (int i = 0; i < n; i++) t += name_w[i] + (i ? gap : 0);
        need(t);
    }
};

}  // namespace pm
