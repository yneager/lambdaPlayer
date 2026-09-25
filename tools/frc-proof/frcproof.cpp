// Standalone proof: OpenGL renders into AMF-allocated D3D11 textures
// (WGL_NV_DX_interop2) -> AMF FRC (DX11) -> output copied into a D3D11
// display texture read by GL. 3840x2160 BGRA by default.
// Submission/polling follows AMD's SimpleFRC sample.
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>
#include <d3d11.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <map>
#include <chrono>

#include "public/common/AMFFactory.h"
#include "public/include/components/FRC.h"

typedef void (APIENTRY *PFNGLGENFRAMEBUFFERS)(GLsizei, GLuint *);
typedef void (APIENTRY *PFNGLBINDFRAMEBUFFER)(GLenum, GLuint);
typedef void (APIENTRY *PFNGLFRAMEBUFFERTEXTURE2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *PFNGLCHECKFRAMEBUFFERSTATUS)(GLenum);
typedef void (APIENTRY *PFNGLDELETEFRAMEBUFFERS)(GLsizei, const GLuint *);
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_BGRA 0x80E1
typedef HANDLE (WINAPI *PFNWGLDXOPENDEVICENV)(void *);
typedef HANDLE (WINAPI *PFNWGLDXREGISTEROBJECTNV)(HANDLE, void *, GLuint, GLenum, GLenum);
typedef BOOL (WINAPI *PFNWGLDXLOCKOBJECTSNV)(HANDLE, GLint, HANDLE *);
typedef BOOL (WINAPI *PFNWGLDXUNLOCKOBJECTSNV)(HANDLE, GLint, HANDLE *);
typedef const char *(WINAPI *PFNWGLGETEXTENSIONSSTRINGARB)(HDC);
#define WGL_ACCESS_READ_WRITE_NV 0x0001

static PFNGLGENFRAMEBUFFERS glGenFramebuffers;
static PFNGLBINDFRAMEBUFFER glBindFramebuffer;
static PFNGLFRAMEBUFFERTEXTURE2D glFramebufferTexture2D;
static PFNGLCHECKFRAMEBUFFERSTATUS glCheckFramebufferStatus;
static PFNGLDELETEFRAMEBUFFERS glDeleteFramebuffers;
static PFNWGLDXOPENDEVICENV wglDXOpenDeviceNV;
static PFNWGLDXREGISTEROBJECTNV wglDXRegisterObjectNV;
static PFNWGLDXLOCKOBJECTSNV wglDXLockObjectsNV;
static PFNWGLDXUNLOCKOBJECTSNV wglDXUnlockObjectsNV;
static HANDLE gInterop;

static int W = 3840, H = 2160;
static double now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct GlTex { GLuint tex = 0; HANDLE obj = nullptr; };
static std::map<void *, GlTex> gRegistered;
static GlTex glFor(ID3D11Texture2D *t)
{
    auto it = gRegistered.find(t);
    if (it != gRegistered.end()) return it->second;
    GlTex g;
    glGenTextures(1, &g.tex);
    g.obj = wglDXRegisterObjectNV(gInterop, t, g.tex, GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
    if (!g.obj) printf("wglDXRegisterObjectNV failed %lu\n", GetLastError());
    gRegistered[t] = g;
    return g;
}

static int squareX(GLuint tex)
{
    static std::vector<unsigned char> row;
    GLuint fbo; glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    row.resize(size_t(W) * 4);
    glReadPixels(0, H / 2, W, 1, GL_BGRA, GL_UNSIGNED_BYTE, row.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0); glDeleteFramebuffers(1, &fbo);
    int first = -1, last = -1;
    for (int x = 0; x < W; ++x) if (row[x * 4 + 1] > 128) { if (first < 0) first = x; last = x; }
    return first < 0 ? -1 : (first + last) / 2;
}

int main(int argc, char **argv)
{
    int profile = FRC_PROFILE_SUPER, search = FRC_MV_SEARCH_NATIVE, future = 0, fallback = 0, frames = 96;
    if (argc > 1) profile = atoi(argv[1]);
    if (argc > 2) search = atoi(argv[2]);
    if (argc > 3) future = atoi(argv[3]);
    if (argc > 4) fallback = atoi(argv[4]);
    if (argc > 6) W = atoi(argv[5]), H = atoi(argv[6]);
    const bool measurePositions = !(argc > 7 && atoi(argv[7]) == 0);

    WNDCLASSA wc{}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandle(nullptr); wc.lpszClassName = "frcproof";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("frcproof", "frcproof", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    HDC hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    SetPixelFormat(hdc, ChoosePixelFormat(hdc, &pfd), &pfd);
    HGLRC glrc = wglCreateContext(hdc); wglMakeCurrent(hdc, glrc);
    printf("GL: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    glGenFramebuffers = (PFNGLGENFRAMEBUFFERS)wglGetProcAddress("glGenFramebuffers");
    glBindFramebuffer = (PFNGLBINDFRAMEBUFFER)wglGetProcAddress("glBindFramebuffer");
    glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2D)wglGetProcAddress("glFramebufferTexture2D");
    glCheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUS)wglGetProcAddress("glCheckFramebufferStatus");
    glDeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERS)wglGetProcAddress("glDeleteFramebuffers");
    auto getExt = (PFNWGLGETEXTENSIONSSTRINGARB)wglGetProcAddress("wglGetExtensionsStringARB");
    printf("WGL_NV_DX_interop2: %s\n", getExt && strstr(getExt(hdc), "WGL_NV_DX_interop2") ? "yes" : "no");
    wglDXOpenDeviceNV = (PFNWGLDXOPENDEVICENV)wglGetProcAddress("wglDXOpenDeviceNV");
    wglDXRegisterObjectNV = (PFNWGLDXREGISTEROBJECTNV)wglGetProcAddress("wglDXRegisterObjectNV");
    wglDXLockObjectsNV = (PFNWGLDXLOCKOBJECTSNV)wglGetProcAddress("wglDXLockObjectsNV");
    wglDXUnlockObjectsNV = (PFNWGLDXUNLOCKOBJECTSNV)wglGetProcAddress("wglDXUnlockObjectsNV");

    AMF_RESULT res = g_AMFFactory.Init();
    printf("AMF factory: %d, runtime version %llx\n", res, (unsigned long long)g_AMFFactory.AMFQueryVersion());
    if (res != AMF_OK) return 1;
    amf::AMFContextPtr ctx;
    g_AMFFactory.GetFactory()->CreateContext(&ctx);
    res = ctx->InitDX11(nullptr); printf("InitDX11: %d\n", res);
    auto *dev = static_cast<ID3D11Device *>(ctx->GetDX11Device());
    ID3D11DeviceContext *d3dctx = nullptr; dev->GetImmediateContext(&d3dctx);
    gInterop = wglDXOpenDeviceNV(dev);
    printf("wglDXOpenDeviceNV: %p\n", gInterop);
    if (!gInterop) return 1;

    amf::AMFComponentPtr frc;
    res = g_AMFFactory.GetFactory()->CreateComponent(ctx, AMFFRC, &frc); printf("CreateComponent FRC: %d\n", res);
    if (res != AMF_OK) return 2;
    frc->SetProperty(AMF_FRC_ENGINE_TYPE, amf_int64(FRC_ENGINE_DX11));
    frc->SetProperty(AMF_FRC_MODE, amf_int64(FRC_x2_PRESENT));
    frc->SetProperty(AMF_FRC_ENABLE_FALLBACK, bool(fallback));
    frc->SetProperty(AMF_FRC_INDICATOR, false);
    frc->SetProperty(AMF_FRC_PROFILE, amf_int64(profile));
    frc->SetProperty(AMF_FRC_MV_SEARCH_MODE, amf_int64(search));
    frc->SetProperty(AMF_FRC_USE_FUTURE_FRAME, bool(future));
    res = frc->Init(amf::AMF_SURFACE_BGRA, W, H);
    printf("FRC Init %dx%d profile=%d search=%d future=%d fallback=%d: %d\n", W, H, profile, search, future, fallback, res);
    if (res != AMF_OK) return 3;

    D3D11_TEXTURE2D_DESC dd{}; dd.Width = W; dd.Height = H; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; dd.SampleDesc.Count = 1; dd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *display = nullptr; dev->CreateTexture2D(&dd, nullptr, &display);

    const int step = 64; // the square moves 64 px per source frame
    int submitted = 0, outputs = 0, repeats = 0;
    double tRender = 0, tSubmit = 0, tOut = 0, tFirst = -1;
    const double t0 = now();
    amf::AMFSurfacePtr in;
    std::vector<int> positions;
    while (submitted < frames) {
        if (!in) {
            const double a = now();
            res = ctx->AllocSurface(amf::AMF_MEMORY_DX11, amf::AMF_SURFACE_BGRA, W, H, &in);
            if (res != AMF_OK) { printf("AllocSurface %d\n", res); return 4; }
            auto *t = static_cast<ID3D11Texture2D *>(in->GetPlaneAt(0)->GetNative());
            if (submitted == 0) { D3D11_TEXTURE2D_DESC td; t->GetDesc(&td); printf("AMF input texture: fmt %d bind 0x%x misc 0x%x\n", td.Format, td.BindFlags, td.MiscFlags); }
            GlTex g = glFor(t);
            if (!wglDXLockObjectsNV(gInterop, 1, &g.obj)) { printf("lock failed %lu\n", GetLastError()); return 5; }
            GLuint fbo; glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.tex, 0);
            if (submitted == 0) printf("input FBO status 0x%x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER));
            glViewport(0, 0, W, H);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0.1f, 0.1f, 0.1f, 1); glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_SCISSOR_TEST);
            glScissor(200 + (submitted % 48) * step, H / 2 - 100, 200, 200);
            glClearColor(1, 1, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0); glDeleteFramebuffers(1, &fbo);
            wglDXUnlockObjectsNV(gInterop, 1, &g.obj);
            tRender += now() - a;
        }
        const double b = now();
        in->SetPts(amf_pts(submitted + 1) * 1000);
        res = frc->SubmitInput(in);
        tSubmit += now() - b;
        if (submitted < 8) printf("S%d:%d ", submitted, res);
        if (res == AMF_NEED_MORE_INPUT || res == AMF_INPUT_FULL) {
            // keep the same surface and submit it again (SimpleFRC)
        } else if (res != AMF_OK) {
            printf("SubmitInput failed %d\n", res); return 7;
        } else {
            in = nullptr;
            ++submitted;
        }
        while (true) {
            amf::AMFSurfacePtr out;
            const double c = now();
            res = frc->QueryOutput(reinterpret_cast<amf::AMFData **>(&out));
            tOut += now() - c;
            if (res == AMF_EOF) break;
            if (res != AMF_OK && res != AMF_REPEAT) break;
            if (!out) break;
            if (res == AMF_REPEAT) ++repeats;
            if (outputs < 16) printf("Q%s(pts %lld) ", res == AMF_REPEAT ? "rep" : "ok", (long long)out->GetPts());
            if (tFirst < 0) tFirst = now() - t0;
            ++outputs;
            if (measurePositions && positions.size() < 24) {
                d3dctx->CopyResource(display, static_cast<ID3D11Texture2D *>(out->GetPlaneAt(0)->GetNative()));
                GlTex g = glFor(display);
                wglDXLockObjectsNV(gInterop, 1, &g.obj);
                positions.push_back(squareX(g.tex));
                wglDXUnlockObjectsNV(gInterop, 1, &g.obj);
            }
        }
    }
    // Wait for the GPU so the throughput number is real.
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0}; ID3D11Query *q = nullptr; dev->CreateQuery(&qd, &q);
    d3dctx->End(q); BOOL done = FALSE; while (d3dctx->GetData(q, &done, sizeof(done), 0) != S_OK) Sleep(0);
    const double total = now() - t0;
    printf("submitted %d, outputs %d (AMF_REPEAT %d) in %.1f ms: %.1f source/s, %.1f outputs/s; first output after %.1f ms\n",
           submitted, outputs, repeats, total, submitted * 1000 / total, outputs * 1000 / total, tFirst);
    printf("CPU time per source frame: GL render %.2f ms, SubmitInput %.2f ms, QueryOutput %.2f ms\n",
           tRender / submitted, tSubmit / submitted, tOut / submitted);
    if (measurePositions) {
        printf("square center x per output (source step %d px):", step);
        for (int p : positions) printf(" %d", p);
        printf("\n");
    }
    frc->Terminate(); frc = nullptr; ctx->Terminate(); ctx = nullptr; g_AMFFactory.Terminate();
    return 0;
}
