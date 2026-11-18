// Draws the panels: hooks IDXGISwapChain::Present (and ResizeBuffers) of the game's own
// swap chain and renders Dear ImGui into its back buffer.
//
// The swap chain is caught as the game makes it: CreateDXGIFactory is hooked while the DLL
// loads (before the game's code runs), and CreateSwapChain on the factories the game's own code
// makes, so the swap chain the game gets back is the one hooked.
// No device or swap chain of our own: renderer mods that take the first swap chain made as
// theirs (DS2LE PathTracing, a ReShade fork) stopped the game with an assertion
// when photo mode made a throwaway one to find Present. Only when the game made no factory
// through the hook in 30 s (photo mode loaded late) does it fall back to that throwaway one.
#include "common.h"
#include <d3d11.h>
#include <dxgi.h>
#include "MinHook.h"
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include <vector>

namespace pm {

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* ResizeFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static PresentFn o_present;
static ResizeFn o_resize;
static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static ID3D11RenderTargetView* g_rtv;
static IDXGISwapChain* g_chain;
static IDXGISwapChain* volatile g_game_chain;  // the swap chain the game made through its factory
static bool g_ready;
static UINT g_bb_w, g_bb_h;  // the back buffer texture's real size (make_rtv)
static LARGE_INTEGER g_freq, g_last;

static void make_rtv(IDXGISwapChain* sc) {
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
        D3D11_TEXTURE2D_DESC td;
        bb->GetDesc(&td);
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        // Typeless or sRGB back buffers need an explicit view format.
        rd.Format = td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM
                    : td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM
                    : td.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS ? DXGI_FORMAT_R10G10B10A2_UNORM
                                                                 : td.Format;
        rd.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g_dev->CreateRenderTargetView(bb, &rd, &g_rtv))) logf("overlay: CreateRenderTargetView failed");
        g_bb_w = td.Width;
        g_bb_h = td.Height;
        logf("overlay: back buffer %ux%u format %d samples %u", td.Width, td.Height, (int)td.Format, td.SampleDesc.Count);
        // Upscalers (DS2LE's DLSS) can make GetDesc report the render size, not the buffer's.
        DXGI_SWAP_CHAIN_DESC d;
        if (SUCCEEDED(sc->GetDesc(&d)) && (d.BufferDesc.Width != td.Width || d.BufferDesc.Height != td.Height))
            logf("overlay: GetDesc says %ux%u, back buffer is %ux%u; using the back buffer", d.BufferDesc.Width,
                 d.BufferDesc.Height, td.Width, td.Height);
        bb->Release();
    }
}

static bool init_imgui(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_dev)) || !g_dev) {
        logf("overlay: swap chain has no D3D11 device");
        return false;
    }
    g_dev->GetImmediateContext(&g_ctx);
    g_chain = sc;
    make_rtv(sc);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGuiStyle& st = ImGui::GetStyle();
    st.FontSizeBase = 22.0f;  // legible on the Deck's 1280x800 at arm's length
    io.Fonts->AddFontDefaultVector();  // the scalable default font: crisp at this size
    st.WindowRounding = 6.0f;
    st.WindowBorderSize = 0.0f;
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.05f, 0.07f, 0.55f);
    if (!ImGui_ImplDX11_Init(g_dev, g_ctx)) {
        logf("overlay: ImGui_ImplDX11_Init failed");
        return false;
    }
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_last);
    logf("overlay: ImGui ready, swap chain %p", (void*)sc);
    return true;
}

// Copies a rectangle of the back buffer into 24-bit RGB rows (for a snap).
static bool capture(IDXGISwapChain* sc, int x, int y, int w, int h, std::vector<uint8_t>& rgb) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return false;
    D3D11_TEXTURE2D_DESC td;
    bb->GetDesc(&td);
    bool bgr = td.Format == DXGI_FORMAT_B8G8R8A8_UNORM || td.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
               td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || td.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
    bool rgba = td.Format == DXGI_FORMAT_R8G8B8A8_UNORM || td.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    bool rgb10 = td.Format == DXGI_FORMAT_R10G10B10A2_UNORM || td.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS;
    if ((!bgr && !rgba && !rgb10) || td.SampleDesc.Count != 1) {
        logf("snap: back buffer format %d samples %u not supported", (int)td.Format, td.SampleDesc.Count);
        bb->Release();
        return false;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > (int)td.Width) w = td.Width - x;
    if (y + h > (int)td.Height) h = td.Height - y;
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_dev->CreateTexture2D(&sd, nullptr, &st)) && st) {
        g_ctx->CopyResource(st, bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            rgb.resize((size_t)w * h * 3);
            for (int r = 0; r < h; r++) {
                const uint8_t* s = (const uint8_t*)m.pData + (size_t)(y + r) * m.RowPitch + (size_t)x * 4;
                uint8_t* d = rgb.data() + (size_t)r * w * 3;
                for (int c = 0; c < w; c++, s += 4, d += 3) {
                    if (rgb10) {  // DS2LE: R bits 0-9, G 10-19, B 20-29
                        uint32_t v = *(const uint32_t*)s;
                        d[0] = (uint8_t)(((v & 1023) * 255 + 511) / 1023);
                        d[1] = (uint8_t)((((v >> 10) & 1023) * 255 + 511) / 1023);
                        d[2] = (uint8_t)((((v >> 20) & 1023) * 255 + 511) / 1023);
                        continue;
                    }
                    d[0] = bgr ? s[2] : s[0];
                    d[1] = s[1];
                    d[2] = bgr ? s[0] : s[2];
                }
            }
            g_ctx->Unmap(st, 0);
            ok = true;
        }
        st->Release();
    }
    bb->Release();
    return ok;
}

static void frame(IDXGISwapChain* sc) {
    if (g_game_chain && sc != g_game_chain) return;  // not the game's own (a renderer mod's, a dummy)
    if (!g_ready) {
        static bool tried;
        if (tried) return;
        tried = true;
        g_ready = init_imgui(sc);
        if (!g_ready) return;
    }
    if (sc != g_chain) return;  // only the game's main swap chain
    if (!g_rtv) make_rtv(sc);
    if (!g_rtv) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = float(now.QuadPart - g_last.QuadPart) / float(g_freq.QuadPart);
    g_last = now;
    if (dt <= 0 || dt > 0.25f) dt = 1 / 60.0f;

    const Pad& pad = pad_poll();
    ui_input(pad, dt);
    camera_frame(pad, dt);
    pose_frame(dt);
    position_frame(dt);
#ifdef PM_DEV
    devchan_frame();
#endif

    // The size of the texture we draw into, not GetDesc's (see make_rtv).
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_bb_w, (float)g_bb_h);
    io.DeltaTime = dt;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();

    // The picture is the whole back buffer, except on screens wider than 16:9, where the game
    // pillarboxes it. A 16:10 buffer (the Deck, 1280x800) is filled edge to edge by the game
    // (seen in a Deck screenshot), so it is not letterboxed here: the panels, the grid and a snap
    // all use the whole buffer, like Steam's own screenshot does.
    float W = io.DisplaySize.x, H = io.DisplaySize.y;
    float pw = W, ph = H;
    if (W * 9.0f > H * 16.0f) pw = H * 16.0f / 9.0f;
    float px = (W - pw) / 2, py = (H - ph) / 2;
    hud_frame();
    int snap = snap_frame(dt);
    if (snap & SNAP_CAPTURE) {  // the game's frame, before anything of ours is drawn on it
        std::vector<uint8_t> rgb;
        int cw = (int)pw, ch = (int)ph;
        bool ok = capture(sc, (int)px, (int)py, cw, ch, rgb);
        snap_captured(ok ? rgb.data() : nullptr, cw, ch);
    }
    if (!(snap & SNAP_HIDE)) {  // a snap in progress: nothing of ours in the picture
        ui_draw(px, py, pw, ph);
        hint_draw(px, py, ph, dt);
    }

    ImGui::Render();
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

static HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) frame(sc);
    return o_present(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE hk_resize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
    HRESULT r = o_resize(sc, n, w, h, f, fl);
    logf("overlay: ResizeBuffers %ux%u -> 0x%08lx", w, h, (unsigned long)r);
    return r;
}

static void* g_present_fn;  // the Present function hooked (one per swap chain class)

// Hooks Present and ResizeBuffers in `sc`'s vtable (MinHook on the functions, so every caller
// is seen; frame() keeps to the game's swap chain).
static SRWLOCK g_hook_lock = SRWLOCK_INIT;

static bool hook_chain_class_locked(IDXGISwapChain* sc, const char* how);
static bool hook_chain_class(IDXGISwapChain* sc, const char* how) {
    AcquireSRWLockExclusive(&g_hook_lock);
    bool ok = hook_chain_class_locked(sc, how);
    ReleaseSRWLockExclusive(&g_hook_lock);
    return ok;
}

static bool hook_chain_class_locked(IDXGISwapChain* sc, const char* how) {
    void** vt = *(void***)sc;
    if (g_present_fn == vt[8]) return true;
    if (g_present_fn) {
        logf("overlay: swap chain %p has another Present (%p, hooked %p); not hooked", (void*)sc, vt[8], g_present_fn);
        return false;
    }
    if (MH_CreateHook(vt[8], (void*)hk_present, (void**)&o_present) != MH_OK ||
        MH_CreateHook(vt[13], (void*)hk_resize, (void**)&o_resize) != MH_OK) {
        logf("overlay: MH_CreateHook failed");
        return false;
    }
    MH_EnableHook(vt[8]);
    MH_EnableHook(vt[13]);
    g_present_fn = vt[8];
    logf("overlay: Present hooked at %p (%s)", vt[8], how);
    return true;
}

// ---- the game's own swap chain, from its DXGI factory
typedef HRESULT(STDMETHODCALLTYPE* CreateSCFn)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(WINAPI* CreateFactoryFn)(REFIID, void**);
static CreateSCFn o_create_sc;
static CreateFactoryFn o_create_factory, o_create_factory1;
static void* g_create_sc_fn;                   // the CreateSwapChain function hooked
static void* volatile g_game_factories[16];   // factories the game's own code made
static volatile LONG g_n_factories;

static bool from_game(void* ret) {
    uintptr_t b = game_base(), r = (uintptr_t)ret;
    auto* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    return r >= b && r < b + nt->OptionalHeader.SizeOfImage;
}

static bool game_factory(void* f) {
    LONG n = g_n_factories < 16 ? g_n_factories : 16;
    for (LONG i = 0; i < n; i++)
        if (g_game_factories[i] == f) return true;
    return false;
}

static HRESULT STDMETHODCALLTYPE hk_create_sc(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) {
    HRESULT r = o_create_sc(f, dev, d, out);
    // Only on a factory the game made: a renderer mod hooked in front of this calls on with
    // the same factory, so it's the swap chain the game itself holds and presents.
    if (SUCCEEDED(r) && out && *out && game_factory(f)) {
        logf("overlay: the game made swap chain %p (%ux%u)", (void*)*out, d ? d->BufferDesc.Width : 0, d ? d->BufferDesc.Height : 0);
        g_game_chain = *out;
        hook_chain_class(*out, "the game's swap chain");
    } else if (!g_game_chain) {
        logf("overlay: CreateSwapChain %p on factory %p not the game's (hr %08lx)", out ? (void*)*out : nullptr, (void*)f, (unsigned long)r);
    }
    return r;
}

static void game_made_factory(HRESULT r, void** out, const char* fn) {
    if (FAILED(r) || !out || !*out) return;
    LONG i = InterlockedIncrement(&g_n_factories) - 1;
    if (i < 16) g_game_factories[i] = *out;
    void* create = (*(void***)*out)[10];  // IDXGIFactory::CreateSwapChain
    if (!g_create_sc_fn) {
        if (MH_CreateHook(create, (void*)hk_create_sc, (void**)&o_create_sc) == MH_OK && MH_EnableHook(create) == MH_OK) {
            g_create_sc_fn = create;
            logf("overlay: the game's DXGI factory %p (%s), CreateSwapChain hooked at %p", *out, fn, create);
        } else {
            logf("overlay: CreateSwapChain hook failed");
        }
    } else if (create != g_create_sc_fn) {
        logf("overlay: the game's DXGI factory %p (%s) has another CreateSwapChain %p", *out, fn, create);
    }
}

static HRESULT WINAPI hk_create_factory(REFIID riid, void** out) {
    HRESULT r = o_create_factory(riid, out);
    if (from_game(__builtin_return_address(0))) game_made_factory(r, out, "CreateDXGIFactory");
    return r;
}

static HRESULT WINAPI hk_create_factory1(REFIID riid, void** out) {
    HRESULT r = o_create_factory1(riid, out);
    if (from_game(__builtin_return_address(0))) game_made_factory(r, out, "CreateDXGIFactory1");
    return r;
}

// ---- fallback: Present from the vtable of a throwaway swap chain
static LRESULT CALLBACK dummy_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static bool hook_from_dummy() {
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = dummy_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"pm_dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc,
                                               &dev, &fl, &ctx);
    if (FAILED(hr)) {
        logf("overlay: dummy D3D11 device failed 0x%08lx", (unsigned long)hr);
        DestroyWindow(hwnd);
        return false;
    }
    bool ok = hook_chain_class(sc, "a throwaway swap chain");
    sc->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

// Falls back to the throwaway swap chain when the game's wasn't caught (photo mode loaded
// after the game made it, say).
// Once the game's factory was seen the hook is working, and it waits for the swap chain as
// long as it takes: the game makes it late under Seamless Co-op with DS2LE (34 s after the
// factory), and a throwaway one made meanwhile is what crashes DS2LE.
static DWORD WINAPI overlay_thread(void*) {
    for (int s = 0; s < 30 && !g_game_chain && !g_n_factories; s++) Sleep(1000);
    if (!g_game_chain && !g_n_factories) {
        logf("overlay: the game made no DXGI factory through the hook in 30 s; using a throwaway swap chain");
        hook_from_dummy();
    }
    return 0;
}

// Called from DllMain, before the game's code runs: hooks the CreateDXGIFactory exports the
// game imports (from whichever dxgi.dll it loaded: the system's, or a renderer mod's proxy).
bool overlay_init() {
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    void* f0 = dxgi ? (void*)GetProcAddress(dxgi, "CreateDXGIFactory") : nullptr;
    void* f1 = dxgi ? (void*)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
    bool ok = f0 && MH_CreateHook(f0, (void*)hk_create_factory, (void**)&o_create_factory) == MH_OK;
    if (f1 && MH_CreateHook(f1, (void*)hk_create_factory1, (void**)&o_create_factory1) != MH_OK) f1 = nullptr;
    if (ok) MH_EnableHook(f0);
    if (f1) MH_EnableHook(f1);
    logf("overlay: CreateDXGIFactory %s, CreateDXGIFactory1 %s", ok ? "hooked" : "not hooked", f1 ? "hooked" : "not hooked");
    HANDLE t = CreateThread(nullptr, 0, overlay_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    return ok;
}

}  // namespace pm
