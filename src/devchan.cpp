// Dev builds only (PM_DEV): a command channel through files in data_dir(), used to
// find the camera in a running game. Write commands to photo-mode-cmd.txt (one per line); the
// DLL picks the file up, deletes it, and appends the results to photo-mode-out.txt.
//
// Addresses are expressions: hex numbers, `base` (DarkSoulsII.exe), + and -, and [x]
// to read the 8-byte pointer at x. Example: [[base+16148f0]+20]+48
//
//   rd EXPR LEN            hex dump with floats
//   rf EXPR N              N floats
//   wf EXPR F...           write floats
//   wb EXPR HEX            write bytes (code too: the page is made writable first)
//   hold EXPR F...         rewrite these floats every frame (in Present); `unhold`
//   scanf TOL F...         find the float sequence in writable memory (max 64 hits)
//   scanp VALUE            find 8-byte values equal to VALUE in writable memory
//   bp w|rw EXPR LEN SECS  hardware watchpoint on every thread for SECS seconds; reports
//                          the code addresses that hit it, with return addresses
//   pgw EXPR LEN MS        page watch: who writes [EXPR, +LEN) in MS ms (read-only page + trap flag)
//   objs EXPR LEN          for each 8-byte pointer in the range: the RTTI class of the
//                          object it points to (if it has one)
//   sleep MS
#include "common.h"
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <map>

namespace pm {

static FILE* g_out;

static void outf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_out, fmt, ap);
    va_end(ap);
}

static bool readable(uintptr_t a, size_t n) {
    while (n) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void*)a, &mi, sizeof mi)) return false;
        if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        uintptr_t end = (uintptr_t)mi.BaseAddress + mi.RegionSize;
        size_t chunk = end - a < n ? end - a : n;
        a += chunk;
        n -= chunk;
    }
    return true;
}

// ---- expressions
struct Parser {
    const char* p;
    bool ok = true;
    void ws() { while (*p == ' ' || *p == '\t') p++; }
    uintptr_t atom() {
        ws();
        if (*p == '[') {
            p++;
            uintptr_t a = expr();
            ws();
            if (*p == ']') p++; else ok = false;
            if (!readable(a, 8)) { ok = false; return 0; }
            return *(uintptr_t*)a;
        }
        if (!strncmp(p, "base", 4)) { p += 4; return game_base(); }
        if (isxdigit((unsigned char)*p)) {
            char* e;
            uintptr_t v = strtoull(p, &e, 16);
            p = e;
            return v;
        }
        ok = false;
        return 0;
    }
    uintptr_t expr() {
        uintptr_t v = atom();
        for (;;) {
            ws();
            if (*p == '+') { p++; v += atom(); }
            else if (*p == '-') { p++; v -= atom(); }
            else return v;
        }
    }
};

static bool parse_addr(const char*& s, uintptr_t& out) {
    // An expression runs to the next space outside brackets.
    while (*s == ' ') s++;
    std::string tok;
    int depth = 0;
    while (*s && (depth || *s != ' ')) {
        if (*s == '[') depth++;
        if (*s == ']') depth--;
        tok += *s++;
    }
    Parser ps{tok.c_str()};
    out = ps.expr();
    return ps.ok;
}

static void dump(uintptr_t a, size_t n) {
    if (!readable(a, n)) { outf("unreadable %p\n", (void*)a); return; }
    for (size_t i = 0; i < n; i += 16) {
        outf("%012llx +%03zx:", (unsigned long long)(a + i), i);
        for (size_t j = 0; j < 16 && i + j < n; j++) outf(" %02x", ((uint8_t*)a)[i + j]);
        outf("  |");
        for (size_t j = 0; j + 4 <= 16 && i + j < n; j += 4) outf(" %12.5g", *(float*)(a + i + j));
        outf("\n");
    }
}

// ---- hold
static CRITICAL_SECTION g_hold_lock;
struct Hold { uintptr_t a; std::vector<float> v; };
static std::vector<Hold> g_holds;
static volatile bool g_hold_ready;

void devchan_frame() {
    if (!g_hold_ready) return;
    EnterCriticalSection(&g_hold_lock);
    for (auto& h : g_holds)
        if (readable(h.a, h.v.size() * 4)) memcpy((void*)h.a, h.v.data(), h.v.size() * 4);
    LeaveCriticalSection(&g_hold_lock);
}

// ---- scans
template <class F>
static void each_writable(F f) {
    uintptr_t a = 0x10000;
    MEMORY_BASIC_INFORMATION mi;
    while (VirtualQuery((void*)a, &mi, sizeof mi)) {
        uintptr_t b = (uintptr_t)mi.BaseAddress, e = b + mi.RegionSize;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)))
            if (!f(b, e)) return;
        if (e <= a) break;
        a = e;
    }
}

// ---- hardware watchpoints
static volatile LONG g_bp_on;
static uintptr_t g_bp_addr;
struct Hit { uintptr_t rip; uintptr_t ret[4]; };
static Hit g_hits[4096];
static volatile LONG g_nhits;

static LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!(c->Dr6 & 0xf)) return EXCEPTION_CONTINUE_SEARCH;
    if (g_bp_on) {
        LONG i = InterlockedIncrement(&g_nhits) - 1;
        if (i < (LONG)(sizeof g_hits / sizeof g_hits[0])) {
            Hit& h = g_hits[i];
            h.rip = c->Rip;
            uintptr_t base = game_base();
            int k = 0;
            uintptr_t* sp = (uintptr_t*)c->Rsp;
            for (int j = 0; j < 256 && k < 4; j++) {
                uintptr_t v = sp[j];
                if (v > base + 0x1000 && v < base + 0x10a6000) h.ret[k++] = v;
            }
            for (; k < 4; k++) h.ret[k] = 0;
        }
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void set_debug_regs(uintptr_t addr, DWORD64 dr7) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {sizeof te};
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    int n = 0, fail = 0;
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) { fail++; continue; }
        SuspendThread(t);
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) {
            c.Dr0 = addr;
            c.Dr7 = dr7;
            c.Dr6 = 0;
            if (SetThreadContext(t, &c)) n++; else fail++;
        } else fail++;
        ResumeThread(t);
        CloseHandle(t);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    outf("debug regs set on %d threads (%d failed)\n", n, fail);
}

static void cmd_bp(const char* s) {
    char mode[4] = {};
    while (*s == ' ') s++;
    int m = 0;
    while (*s && *s != ' ' && m < 3) mode[m++] = *s++;
    uintptr_t a;
    if (!parse_addr(s, a)) { outf("bad address\n"); return; }
    int len = strtol(s, (char**)&s, 10);
    int secs = strtol(s, (char**)&s, 10);
    if (secs <= 0) secs = 2;
    DWORD64 rw = !strcmp(mode, "rw") ? 3 : 1;
    DWORD64 ln = len >= 8 ? 2 : len >= 4 ? 3 : len >= 2 ? 1 : 0;
    a &= ~(uintptr_t)(len >= 8 ? 7 : len >= 4 ? 3 : len >= 2 ? 1 : 0);
    DWORD64 dr7 = 1 | (rw << 16) | (ln << 18);
    static PVOID h;
    if (!h) h = AddVectoredExceptionHandler(1, veh);
    g_nhits = 0;
    g_bp_addr = a;
    g_bp_on = 1;
    outf("bp %s %p len %d for %ds\n", mode, (void*)a, len, secs);
    set_debug_regs(a, dr7);
    Sleep(secs * 1000);
    set_debug_regs(0, 0);
    g_bp_on = 0;
    LONG n = g_nhits;
    if (n > (LONG)(sizeof g_hits / sizeof g_hits[0])) n = sizeof g_hits / sizeof g_hits[0];
    std::map<std::string, int> uniq;
    uintptr_t base = game_base();
    for (LONG i = 0; i < n; i++) {
        char buf[200];
        Hit& h = g_hits[i];
        snprintf(buf, sizeof buf, "rip %llx  ret %llx %llx %llx %llx", (unsigned long long)(h.rip - base),
                 (unsigned long long)(h.ret[0] ? h.ret[0] - base : 0), (unsigned long long)(h.ret[1] ? h.ret[1] - base : 0),
                 (unsigned long long)(h.ret[2] ? h.ret[2] - base : 0), (unsigned long long)(h.ret[3] ? h.ret[3] - base : 0));
        uniq[buf]++;
    }
    outf("%ld hits (rva; rip is after the access)\n", (long)g_nhits);
    for (auto& kv : uniq) outf("  %5d  %s\n", kv.second, kv.first.c_str());
}

// ---- page watch: the page made read-only; a write to it faults, the writer is noted if it
// hit [lo, hi), the page is opened for one instruction (trap flag), then shut again. No
// debug registers (they crash the game).
static volatile LONG g_pw_on;
static uintptr_t g_pw_page, g_pw_lo, g_pw_hi;
static thread_local bool g_pw_step;
static LONG CALLBACK pw_veh(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    CONTEXT* c = ep->ContextRecord;
    DWORD old;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 && r->ExceptionInformation[0] == 1 &&
        (r->ExceptionInformation[1] & ~(uintptr_t)0xfff) == g_pw_page && g_pw_page) {
        uintptr_t a = r->ExceptionInformation[1];
        if (g_pw_on && a >= g_pw_lo && a < g_pw_hi) {
            LONG i = InterlockedIncrement(&g_nhits) - 1;
            if (i < (LONG)(sizeof g_hits / sizeof g_hits[0])) {
                Hit& h = g_hits[i];
                h.rip = c->Rip;
                uintptr_t base = game_base();
                int k = 0;
                uintptr_t* sp = (uintptr_t*)c->Rsp;
                for (int j = 0; j < 256 && k < 4; j++) {
                    uintptr_t v = sp[j];
                    if (v > base + 0x1000 && v < base + 0x10a6000) h.ret[k++] = v;
                }
                for (; k < 4; k++) h.ret[k] = 0;
            }
        }
        VirtualProtect((void*)g_pw_page, 0x1000, PAGE_READWRITE, &old);
        c->EFlags |= 0x100;
        g_pw_step = true;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && g_pw_step) {
        g_pw_step = false;
        c->EFlags &= ~0x100u;
        if (g_pw_on) VirtualProtect((void*)g_pw_page, 0x1000, PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void cmd_pgw(const char* s) {
    uintptr_t a;
    if (!parse_addr(s, a)) { outf("bad address\n"); return; }
    int len = strtol(s, (char**)&s, 10);
    int ms = strtol(s, (char**)&s, 10);
    if (ms <= 0) ms = 200;
    static PVOID h;
    if (!h) h = AddVectoredExceptionHandler(1, pw_veh);
    g_nhits = 0;
    g_pw_lo = a;
    g_pw_hi = a + (len > 0 ? len : 4);
    g_pw_page = a & ~(uintptr_t)0xfff;
    DWORD old;
    g_pw_on = 1;
    VirtualProtect((void*)g_pw_page, 0x1000, PAGE_READONLY, &old);
    Sleep(ms);
    g_pw_on = 0;
    VirtualProtect((void*)g_pw_page, 0x1000, PAGE_READWRITE, &old);
    Sleep(50);
    LONG n = g_nhits;
    if (n > (LONG)(sizeof g_hits / sizeof g_hits[0])) n = sizeof g_hits / sizeof g_hits[0];
    std::map<std::string, int> uniq;
    uintptr_t base = game_base();
    for (LONG i = 0; i < n; i++) {
        char buf[200];
        Hit& hh = g_hits[i];
        snprintf(buf, sizeof buf, "rip %llx  ret %llx %llx %llx %llx", (unsigned long long)(hh.rip - base),
                 (unsigned long long)(hh.ret[0] ? hh.ret[0] - base : 0), (unsigned long long)(hh.ret[1] ? hh.ret[1] - base : 0),
                 (unsigned long long)(hh.ret[2] ? hh.ret[2] - base : 0), (unsigned long long)(hh.ret[3] ? hh.ret[3] - base : 0));
        uniq[buf]++;
    }
    outf("%ld writes (rva; rip is at the writing instruction)\n", (long)g_nhits);
    for (auto& kv : uniq) outf("  %5d  %s\n", kv.second, kv.first.c_str());
}

// RTTI class name of the object at p (MSVC x64: vtable[-1] -> complete object locator).
static const char* rtti_name(uintptr_t p) {
    uintptr_t base = game_base();
    if (!readable(p, 8)) return nullptr;
    uintptr_t vt = *(uintptr_t*)p;
    if (vt < base + 0x1000 || vt > base + 0x1d76000 || !readable(vt - 8, 8)) return nullptr;
    uintptr_t col = *(uintptr_t*)(vt - 8);
    if (col < base || col > base + 0x1d76000 || !readable(col, 24)) return nullptr;
    if (*(uint32_t*)col != 1) return nullptr;
    uintptr_t td = base + *(uint32_t*)(col + 12);
    if (!readable(td + 16, 4)) return nullptr;
    return (const char*)(td + 16);
}

static void run(char* line) {
    char* s = line;
    while (*s == ' ') s++;
    if (!*s || *s == '#') return;
    outf("> %s\n", s);
    char cmd[16] = {};
    int i = 0;
    while (*s && *s != ' ' && i < 15) cmd[i++] = *s++;
    const char* r = s;
    uintptr_t a;
    if (!strcmp(cmd, "rd") || !strcmp(cmd, "rf")) {
        if (!parse_addr(r, a)) { outf("bad address\n"); return; }
        long n = strtol(r, nullptr, 0);
        if (!strcmp(cmd, "rd")) dump(a, n > 0 && n <= 0x4000 ? n : 0x100);
        else {
            if (!readable(a, n * 4)) { outf("unreadable\n"); return; }
            outf("%p:", (void*)a);
            for (long k = 0; k < n; k++) outf(" %.6g", ((float*)a)[k]);
            outf("\n");
        }
    } else if (!strcmp(cmd, "wf") || !strcmp(cmd, "hold")) {
        if (!parse_addr(r, a)) { outf("bad address\n"); return; }
        std::vector<float> v;
        char* e;
        for (;;) {
            float f = strtof(r, &e);
            if (e == r) break;
            v.push_back(f);
            r = e;
        }
        if (!readable(a, v.size() * 4)) { outf("unreadable\n"); return; }
        if (cmd[0] == 'w') memcpy((void*)a, v.data(), v.size() * 4);
        else {
            EnterCriticalSection(&g_hold_lock);
            g_holds.push_back({a, v});
            LeaveCriticalSection(&g_hold_lock);
        }
        outf("ok %zu floats at %p\n", v.size(), (void*)a);
    } else if (!strcmp(cmd, "unhold")) {
        EnterCriticalSection(&g_hold_lock);
        g_holds.clear();
        LeaveCriticalSection(&g_hold_lock);
        outf("ok\n");
    } else if (!strcmp(cmd, "wb")) {
        if (!parse_addr(r, a)) { outf("bad address\n"); return; }
        std::vector<uint8_t> b;
        while (*r) {
            while (*r == ' ') r++;
            if (!isxdigit((unsigned char)r[0]) || !isxdigit((unsigned char)r[1])) break;
            char h[3] = {r[0], r[1], 0};
            b.push_back((uint8_t)strtoul(h, nullptr, 16));
            r += 2;
        }
        DWORD old;
        if (!VirtualProtect((void*)a, b.size(), PAGE_EXECUTE_READWRITE, &old)) { outf("VirtualProtect failed\n"); return; }
        memcpy((void*)a, b.data(), b.size());
        VirtualProtect((void*)a, b.size(), old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)a, b.size());
        outf("ok %zu bytes at %p\n", b.size(), (void*)a);
    } else if (!strcmp(cmd, "scanf")) {
        char* e;
        float tol = strtof(r, &e);
        r = e;
        std::vector<float> v;
        for (;;) {
            float f = strtof(r, &e);
            if (e == r) break;
            v.push_back(f);
            r = e;
        }
        if (v.empty()) { outf("no values\n"); return; }
        int hits = 0;
        size_t need = v.size() * 4;
        each_writable([&](uintptr_t b, uintptr_t en) {
            for (uintptr_t p = b; p + need <= en; p += 4) {
                const float* f = (const float*)p;
                bool ok = true;
                for (size_t k = 0; k < v.size() && ok; k++) ok = f[k] >= v[k] - tol && f[k] <= v[k] + tol;
                if (ok) {
                    outf("  %p\n", (void*)p);
                    if (++hits >= 64) return false;
                }
            }
            return true;
        });
        outf("%d hits\n", hits);
    } else if (!strcmp(cmd, "scanp")) {
        uintptr_t v;
        if (!parse_addr(r, v)) { outf("bad value\n"); return; }
        int hits = 0;
        each_writable([&](uintptr_t b, uintptr_t en) {
            for (uintptr_t p = b; p + 8 <= en; p += 8)
                if (*(uintptr_t*)p == v) {
                    outf("  %p\n", (void*)p);
                    if (++hits >= 64) return false;
                }
            return true;
        });
        outf("%d hits\n", hits);
    } else if (!strcmp(cmd, "pgw")) {
        cmd_pgw(r);
    } else if (!strcmp(cmd, "bp")) {
        cmd_bp(r);
    } else if (!strcmp(cmd, "objs")) {
        if (!parse_addr(r, a)) { outf("bad address\n"); return; }
        long n = strtol(r, nullptr, 0);
        if (n <= 0 || n > 0x10000) n = 0x200;
        if (!readable(a, n)) { outf("unreadable\n"); return; }
        for (long k = 0; k + 8 <= n; k += 8) {
            uintptr_t p = *(uintptr_t*)(a + k);
            const char* nm = rtti_name(p);
            if (nm) outf("  +%04lx %012llx %s\n", k, (unsigned long long)p, nm);
        }
    } else if (!strcmp(cmd, "save") || !strcmp(cmd, "diff")) {
        // save EXPR LEN: remember the bytes; diff: show what changed since (offset old new)
        static std::vector<uint8_t> snap;
        static uintptr_t snap_at;
        if (cmd[0] == 's') {
            if (!parse_addr(r, a)) { outf("bad address\n"); return; }
            long n = strtol(r, nullptr, 0);
            if (n <= 0 || n > 0x100000 || !readable(a, n)) { outf("unreadable\n"); return; }
            snap.assign((uint8_t*)a, (uint8_t*)a + n);
            snap_at = a;
            outf("saved %ld bytes at %p\n", n, (void*)a);
        } else {
            if (snap.empty() || !readable(snap_at, snap.size())) { outf("nothing saved\n"); return; }
            int shown = 0;
            for (size_t k = 0; k < snap.size() && shown < 200; k++) {
                uint8_t now = ((uint8_t*)snap_at)[k];
                if (now != snap[k]) { outf("  +%05zx %02x -> %02x\n", k, snap[k], now); shown++; }
            }
            outf("%d changed bytes shown\n", shown);
        }
    } else if (!strcmp(cmd, "scanb") || !strcmp(cmd, "filterb")) {
        // scanb V: every writable byte equal to V; filterb V: keep those that now equal V
        static std::vector<uintptr_t> cand;
        int v = (int)strtol(r, nullptr, 0);
        if (cmd[0] == 's') {
            cand.clear();
            cand.reserve(1 << 24);
            each_writable([&](uintptr_t b, uintptr_t en) {
                if (en - b > (256u << 20)) return true;  // skip giant buffers
                for (uintptr_t p = b; p < en; p++)
                    if (*(uint8_t*)p == v) {
                        if (cand.size() >= 60000000) return false;
                        cand.push_back(p);
                    }
                return true;
            });
        } else {
            std::vector<uintptr_t> keep;
            for (uintptr_t p : cand)
                if (readable(p, 1) && *(uint8_t*)p == v) keep.push_back(p);
            cand.swap(keep);
        }
        outf("%zu candidates\n", cand.size());
        uintptr_t base = game_base();
        for (size_t k = 0; k < cand.size() && k < 40; k++) {
            uintptr_t p = cand[k];
            if (p >= base && p < base + 0x1d76000) outf("  %p (base+%llx)\n", (void*)p, (unsigned long long)(p - base));
            else outf("  %p\n", (void*)p);
        }
    } else if (!strcmp(cmd, "usnap") || !strcmp(cmd, "uchg") || !strcmp(cmd, "usame")) {
        // Unknown-value scan over writable memory below 8 GB (the game's own heaps and
        // .data): usnap remembers it all; uchg keeps the bytes that differ from the
        // snapshot now; usame keeps those equal to it (the first uchg/usame picks from all).
        struct Reg { uintptr_t a; std::vector<uint8_t> d; };
        static std::vector<Reg> regs;
        static std::vector<uintptr_t> cand;
        static bool started;
        auto orig = [&](uintptr_t p, uint8_t* out) {  // regions are in address order
            size_t lo = 0, hi = regs.size();
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (regs[mid].a <= p) lo = mid + 1; else hi = mid;
            }
            if (!lo) return false;
            auto& rg = regs[lo - 1];
            if (p >= rg.a + rg.d.size()) return false;
            *out = rg.d[p - rg.a];
            return true;
        };
        if (!strcmp(cmd, "usnap")) {
            regs.clear();
            cand.clear();
            started = false;
            size_t total = 0;
            each_writable([&](uintptr_t b, uintptr_t en) {
                // usnap low: the game's own heaps below 8 GB; usnap (default): the big
                // heaps up at 0x7fxx... where GameManagerImp's managers live, and .data
                bool low = strstr(r, "low") != nullptr;
                bool in_module = b >= game_base() && b < game_base() + 0x1d76000;
                if (low ? (b >= 0x200000000ull && !in_module) : (b < 0x7f0000000000ull && !in_module)) return true;
                if (en - b > (512u << 20)) return true;  // giant buffers
                if (total + (en - b) > (768u << 20)) return true;
                regs.push_back({b, std::vector<uint8_t>((uint8_t*)b, (uint8_t*)en)});
                total += en - b;
                return true;
            });
            outf("snapshot: %zu regions, %zu MB\n", regs.size(), total >> 20);
        } else {
            bool want_diff = cmd[1] == 'c';
            std::vector<uintptr_t> keep;
            if (!started) {
                for (auto& rg : regs) {
                    if (!readable(rg.a, rg.d.size())) continue;
                    const uint8_t* now = (const uint8_t*)rg.a;
                    for (size_t k = 0; k < rg.d.size(); k++)
                        if ((now[k] != rg.d[k]) == want_diff) {
                            if (keep.size() >= 40000000) break;
                            keep.push_back(rg.a + k);
                        }
                }
                started = true;
            } else {
                for (uintptr_t p : cand) {
                    uint8_t o;
                    if (!orig(p, &o) || !readable(p, 1)) continue;
                    if ((*(uint8_t*)p != o) == want_diff) keep.push_back(p);
                }
            }
            cand.swap(keep);
            outf("%zu candidates\n", cand.size());
            for (size_t k = 0; k < cand.size() && k < 60; k++) {
                uint8_t o = 0;
                orig(cand[k], &o);
                outf("  %p  %02x -> %02x\n", (void*)cand[k], o, *(uint8_t*)cand[k]);
            }
        }
    } else if (!strcmp(cmd, "ptrsto")) {
        // ptrsto EXPR RANGE: 8-byte values in [EXPR-RANGE, EXPR] anywhere writable (who points
        // at the struct holding EXPR), with the RTTI class of the holder when it has one
        if (!parse_addr(r, a)) { outf("bad address\n"); return; }
        uintptr_t range = strtoull(r, nullptr, 0);
        int hits = 0;
        uintptr_t base = game_base();
        each_writable([&](uintptr_t b, uintptr_t en) {
            for (uintptr_t p = b; p + 8 <= en; p += 8) {
                uintptr_t v = *(uintptr_t*)p;
                if (v <= a && v + range >= a) {
                    outf("  %p -> %p (+%llx)%s\n", (void*)p, (void*)v, (unsigned long long)(a - v),
                         p >= base && p < base + 0x1d76000 ? " [module]" : "");
                    if (++hits >= 100) return false;
                }
            }
            return true;
        });
        outf("%d hits\n", hits);
    } else if (!strcmp(cmd, "pose")) {
        char buf[1024];
        pose_dev(r, buf, sizeof buf);
        outf("%s\n", buf);
    } else if (!strcmp(cmd, "posn")) {
        char buf[2048];
        position_dev(r, buf, sizeof buf);
        outf("%s\n", buf);
    } else if (!strcmp(cmd, "sleep")) {
        Sleep(strtol(r, nullptr, 10));
    } else {
        outf("unknown command\n");
    }
}

static DWORD WINAPI chan_thread(void*) {
    wchar_t cmdp[MAX_PATH], outp[MAX_PATH], tmpp[MAX_PATH];
    _snwprintf(cmdp, MAX_PATH, L"%lsphoto-mode-cmd.txt", data_dir());
    _snwprintf(tmpp, MAX_PATH, L"%lsphoto-mode-cmd.run", data_dir());
    _snwprintf(outp, MAX_PATH, L"%lsphoto-mode-out.txt", data_dir());
    for (;;) {
        Sleep(200);
        if (GetFileAttributesW(cmdp) == INVALID_FILE_ATTRIBUTES) continue;
        DeleteFileW(tmpp);
        if (!MoveFileW(cmdp, tmpp)) continue;
        FILE* in = _wfopen(tmpp, L"r");
        if (!in) continue;
        g_out = _wfopen(outp, L"a");
        if (!g_out) { fclose(in); continue; }
        char line[1024];
        while (fgets(line, sizeof line, in)) {
            line[strcspn(line, "\r\n")] = 0;
            run(line);
            fflush(g_out);
        }
        outf("done\n");
        fclose(in);
        fclose(g_out);
        DeleteFileW(tmpp);
    }
    return 0;
}

// A crash's place, logged before the game dies: the faulting rva, the address it touched,
// and the game-module addresses on the stack (return addresses, mostly).
static LONG CALLBACK on_crash(EXCEPTION_POINTERS* e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t base = game_base(), at = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
    CONTEXT* c = e->ContextRecord;
    logf("crash: code %lx at %p (rva %llx) touching %llx; rcx %llx rdx %llx rax %llx rbx %llx", code, (void*)at,
         (unsigned long long)(at - base), (unsigned long long)e->ExceptionRecord->ExceptionInformation[1],
         (unsigned long long)c->Rcx, (unsigned long long)c->Rdx, (unsigned long long)c->Rax, (unsigned long long)c->Rbx);
    uintptr_t* sp = (uintptr_t*)c->Rsp;
    char buf[1024];
    int n = 0;
    for (int i = 0; i < 512 && n < (int)sizeof buf - 24; i++) {
        if (!readable((uintptr_t)(sp + i), 8)) break;
        uintptr_t v = sp[i];
        if (v > base + 0x1000 && v < base + 0x1d76000) n += snprintf(buf + n, sizeof buf - n, " %llx", (unsigned long long)(v - base));
    }
    logf("crash: stack rvas%s", buf);
    return EXCEPTION_CONTINUE_SEARCH;
}

void devchan_start() {
    AddVectoredExceptionHandler(1, on_crash);
    InitializeCriticalSection(&g_hold_lock);
    g_hold_ready = true;
    HANDLE t = CreateThread(nullptr, 0, chan_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    logf("devchan: listening for photo-mode-cmd.txt");
}

}  // namespace pm
