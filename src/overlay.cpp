// Draws the panels: hooks IDXGISwapChain::Present (and ResizeBuffers) through the
// vtable of a throwaway swap chain, and renders Dear ImGui into the back buffer.
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
static bool g_ready;
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
                                                                 : td.Format;
        rd.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g_dev->CreateRenderTargetView(bb, &rd, &g_rtv))) logf("overlay: CreateRenderTargetView failed");
        logf("overlay: back buffer %ux%u format %d samples %u", td.Width, td.Height, (int)td.Format, td.SampleDesc.Count);
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
    logf("overlay: ImGui ready");
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
    if ((!bgr && !rgba) || td.SampleDesc.Count != 1) {
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

    DXGI_SWAP_CHAIN_DESC d;
    sc->GetDesc(&d);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)d.BufferDesc.Width, (float)d.BufferDesc.Height);
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

static LRESULT CALLBACK dummy_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

bool overlay_init() {
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
    void** vt = *(void***)sc;
    void* present = vt[8];
    void* resize = vt[13];
    sc->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    if (MH_CreateHook(present, (void*)hk_present, (void**)&o_present) != MH_OK ||
        MH_CreateHook(resize, (void*)hk_resize, (void**)&o_resize) != MH_OK) {
        logf("overlay: MH_CreateHook failed");
        return false;
    }
    MH_EnableHook(present);
    MH_EnableHook(resize);
    logf("overlay: Present hooked at %p", present);
    return true;
}

}  // namespace pm
