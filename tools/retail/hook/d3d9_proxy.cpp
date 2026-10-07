// A proxy d3d9.dll for Mirror's Edge.
//
// Mirror's Edge has no windowed mode: the Video options menu offers only
// resolution, and neither `[SystemSettings] Fullscreen=False`,
// `[WinDrv.WindowsClient] StartupFullscreen=False`, nor the `-windowed`
// command-line switch has any effect. The game always takes exclusive
// fullscreen, which changes the desktop resolution and makes the machine
// unusable for anything else while it runs.
//
// This intercepts device creation and forces windowed presentation.
//
// Why a proxy rather than injection: d3d9.dll is not in the KnownDLLs list, and
// the executable's own directory is searched before System32, so a DLL of that
// name dropped next to MirrorsEdge.exe is loaded in preference to the system
// one. That needs no injector and no third-party hooking library, and it is
// active before the first frame - which matters, because the device is created
// during startup.
//
// Rather than write a full IDirect3D9 wrapper (16+ forwarding methods), we
// patch a single vtable slot on the object the real d3d9 returns.
//
// The game is 32-bit, so this must be built for x86.

// It also loads RenderDoc, which cannot otherwise attach to this game at all.
// RenderDoc must be present before the graphics API initialises, and neither
// route works here:
//
//   * `renderdoccmd inject` into the running game attaches but never hooks,
//     because the D3D9 device already exists by then.
//   * `renderdoccmd capture` launches the exe, but the Steam DRM stub asks
//     *Steam* to start the real process - the running game's parent is
//     steam.exe, not renderdoccmd - so it is not a child and
//     --opt-hook-children cannot follow it.
//
// This DLL, however, is loaded by the real process before it creates its
// device, which is exactly the window RenderDoc needs. Loading renderdoc.dll
// from here and driving it through the in-application API sidesteps the whole
// problem, with no Steam launch options and nothing for the user to do.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <share.h>
#include "renderdoc_app.h"

static HMODULE g_realD3D9 = nullptr;
static FILE *g_log = nullptr;
static RENDERDOC_API_1_4_1 *g_rdoc = nullptr;
static char g_triggerPath[MAX_PATH] = {0};
static int g_frame = 0;

static char g_dllDir[MAX_PATH] = {0};
static void pathInDllDir(char *out, const char *leaf);

// Vertex-shader constant dump state; used by both the Present hook (which opens
// and closes the dump on frame boundaries) and the constant hook itself.
static FILE *g_constDump = nullptr;
static int g_dumpFramesLeft = 0;
static int g_dumpCalls = 0;

// Draw-log dump state: one frame of SetPixelShaderConstantF + SetTexture +
// DrawIndexedPrimitive, tagged for offline replay (tools/retail/drawlog.py).
// This is the colour oracle's capture side - UE3 uploads each material's
// RESOLVED vector parameters (the parameter NAMES do not survive shader
// compilation, MaterialTemplate.usf pastes generated uniform slots), so the
// constants the pixel shader receives ARE the tints, attributable to a
// material by the diffuse texture bound alongside them. Separate trigger from
// the vsconst/RenderDoc path so a colour capture never fires a frame capture.
static char g_drawTrigger[MAX_PATH] = {0};
static FILE *g_drawDump = nullptr;
static int g_drawFramesLeft = 0;
static int g_drawCount = 0;      // DrawIndexedPrimitive ordinal within the frame
static int g_psConstLines = 0;   // PS float4 lines written this dump
// Sampler-0 latch and the once-described texture set (declared here because
// MyPresent resets the count when it opens a dump, above where the hooks live).
struct IDirect3DBaseTexture9;
static IDirect3DBaseTexture9 *g_boundTex0 = nullptr;
static const void *g_describedTex[512] = {0};
static int g_describedCount = 0;

// Free-camera plumbing, driven from Present but implemented further down.
static int g_camPollFrame = 0;
static void pollCameraCommand();
static void tickNoclip();
static void tickCursorClip();
static void installWndProc(HWND h);
static void releaseHeldKeys();
static HWND g_gameWindow = nullptr;
static int g_blockedKeyCount = 0;
static void writeCameraState();
static void traceTick();
static void traceStart();
static void traceStop();
static void resolveCameraForFrame();
static bool cameraFromOffAspectCandidate();
static void tickPlayerScan();
static void holdPlayerPosition();
static void tickPawnFind();
static void drivePawnWithCamera();
static void ensureTelemetry();
static void telemetryTick();
static void telemetryKey(unsigned vk, bool down, LPARAM l, bool blocked);
static void pawnTick();
static void animTick();
static void audioTick();

static void logf(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// --- CreateDevice interception ----------------------------------------------

typedef HRESULT(WINAPI *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND,
                                        DWORD, D3DPRESENT_PARAMETERS *,
                                        IDirect3DDevice9 **);
static CreateDevice_t g_realCreateDevice = nullptr;

// Give the game window a normal frame and centre it, so it behaves like any
// other window once it is no longer taking the whole display.
// The client size the device actually renders at, remembered so the frame can
// be put back when something else moves it - see reassertWindowFrame.
static int g_frameW = 0, g_frameH = 0;

static void makeWindowFramed(HWND hwnd, int w, int h) {
    if (!hwnd) return;
    LONG style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);

    RECT r = {0, 0, w, h};
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_APPWINDOW);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd, HWND_NOTOPMOST, (sx - ww) / 2, (sy - wh) / 2, ww, wh,
                 SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    g_frameW = w;
    g_frameH = h;
}

// Hold the window at the size the device renders at.
//
// Framing it once at CreateDevice is not enough: the game re-applies its own
// window management afterwards, and Windows restores the pre-minimise size,
// so the window drifts to a small default (measured: 1286x782 becoming
// 514x313 after a minimise and restore) while the back buffer stays 1280x720.
// The result is a window that keeps changing size under the player, which is
// what the fullscreen-to-windowed flapping looks like from outside.
//
// Only the SIZE is re-asserted, never the position: the frame has no
// WS_THICKFRAME so the player cannot resize it by hand, but they can and do
// drag it, and stealing the position back every other frame would be worse
// than the bug. Skipped while minimised, which is a legitimate state.
static void reassertWindowFrame() {
    if (!g_gameWindow || g_frameW <= 0 || g_frameH <= 0) return;
    if (IsIconic(g_gameWindow)) return;
    RECT c;
    if (!GetClientRect(g_gameWindow, &c)) return;
    const int cw = c.right - c.left, ch = c.bottom - c.top;
    if (cw == g_frameW && ch == g_frameH) return;

    LONG style = (LONG)GetWindowLongPtr(g_gameWindow, GWL_STYLE);
    LONG ex = (LONG)GetWindowLongPtr(g_gameWindow, GWL_EXSTYLE);
    RECT r = {0, 0, g_frameW, g_frameH};
    AdjustWindowRectEx(&r, style, FALSE, ex);
    SetWindowPos(g_gameWindow, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    logf("window drifted to %dx%d client; restored to %dx%d",
         cw, ch, g_frameW, g_frameH);
}

// --- RenderDoc ----------------------------------------------------------------

static void initRenderDoc() {
    // Prefer an already-loaded copy (if someone did manage to inject), then the
    // standard install locations.
    // Mirror's Edge is a 32-bit process, so it needs RenderDoc's x86 build. The
    // DLL at the install root is x64 and will not load here; the 32-bit one
    // lives in the x86\ subdirectory.
    static const char *kCandidates[] = {
        "C:\\Program Files\\RenderDoc\\x86\\renderdoc.dll",
        "C:\\Program Files (x86)\\RenderDoc\\x86\\renderdoc.dll",
        "C:\\Program Files (x86)\\RenderDoc\\renderdoc.dll",
        "C:\\Program Files\\RenderDoc\\renderdoc.dll",
    };
    HMODULE rd = GetModuleHandleA("renderdoc.dll");
    for (int i = 0; !rd && i < (int)(sizeof(kCandidates) / sizeof(*kCandidates)); ++i) {
        rd = LoadLibraryA(kCandidates[i]);
        if (rd) logf("loaded %s", kCandidates[i]);
    }
    if (!rd) {
        logf("renderdoc.dll not found; capture disabled");
        return;
    }
    auto getApi = (pRENDERDOC_GetAPI)GetProcAddress(rd, "RENDERDOC_GetAPI");
    if (!getApi) {
        logf("RENDERDOC_GetAPI missing");
        return;
    }
    if (getApi(eRENDERDOC_API_Version_1_4_1, (void **)&g_rdoc) != 1 || !g_rdoc) {
        logf("RENDERDOC_GetAPI refused version 1.4.1");
        g_rdoc = nullptr;
        return;
    }

    char tmpl[MAX_PATH];
    if (GetEnvironmentVariableA("MEDGE_RDC_TEMPLATE", tmpl, MAX_PATH) > 0)
        g_rdoc->SetCaptureFilePathTemplate(tmpl);

    // The default capture key is F12, which the Steam overlay takes first, so
    // no key is bound. Capture is triggered by Python dropping a sentinel file
    // instead, which is more precise anyway.
    RENDERDOC_InputButton none[] = {(RENDERDOC_InputButton)0};
    g_rdoc->SetCaptureKeys(none, 0);
    g_rdoc->MaskOverlayBits(RENDERDOC_OverlayBits::eRENDERDOC_Overlay_All,
                            RENDERDOC_OverlayBits::eRENDERDOC_Overlay_All);

    int maj = 0, min = 0, patch = 0;
    g_rdoc->GetAPIVersion(&maj, &min, &patch);
    logf("RenderDoc API %d.%d.%d ready, template=%s", maj, min, patch,
         g_rdoc->GetCaptureFilePathTemplate());
}

// --- Present interception (frame boundary + capture trigger) ------------------

typedef HRESULT(WINAPI *Present_t)(IDirect3DDevice9 *, const RECT *, const RECT *,
                                   HWND, const RGNDATA *);
static Present_t g_realPresent = nullptr;

// Reset takes a fresh set of presentation parameters, so it can put the device
// straight back into fullscreen behind our backs - see MyReset.
typedef HRESULT(WINAPI *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static Reset_t g_realReset = nullptr;

static bool g_pendingCapture = false;
static bool g_capturing = false;

// --- back-buffer grab ---------------------------------------------------------
//
// Screen-region capture breaks as soon as another window covers the game, and
// PrintWindow with PW_RENDERFULLCONTENT only partially redirects a D3D9 scene:
// the sky comes through but world geometry lands as black silhouettes. Reading
// the back buffer from inside Present sidesteps both - it is the exact frame the
// game rendered, regardless of occlusion, focus, or whether the window is even
// visible.

static char g_shotTrigger[MAX_PATH] = {0};
static char g_shotOut[MAX_PATH] = {0};

static bool writeBMP(const char *path, const BYTE *bgra, int w, int h, int pitch) {
    FILE *f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;

    const int rowBytes = w * 3;
    const int pad = (4 - (rowBytes % 4)) % 4;
    const int imgSize = (rowBytes + pad) * h;

#pragma pack(push, 1)
    struct { WORD bfType; DWORD bfSize; WORD r1, r2; DWORD bfOffBits; } fh
        = {0x4D42, (DWORD)(54 + imgSize), 0, 0, 54};
    struct {
        DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount;
        DWORD biCompression, biSizeImage; LONG bix, biy; DWORD biClrUsed, biClrImp;
    } ih = {40, w, h, 1, 24, 0, (DWORD)imgSize, 2835, 2835, 0, 0};
#pragma pack(pop)

    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);

    static const BYTE zero[3] = {0, 0, 0};
    // BMP rows run bottom-up.
    for (int y = h - 1; y >= 0; --y) {
        const BYTE *row = bgra + (size_t)y * pitch;
        for (int x = 0; x < w; ++x) {
            fwrite(row + x * 4, 3, 1, f);   // BGRA -> BGR
        }
        if (pad) fwrite(zero, pad, 1, f);
    }
    fclose(f);
    return true;
}

static void grabBackBuffer(IDirect3DDevice9 *dev, const char *outPath) {
    IDirect3DSurface9 *back = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) {
        logf("GetBackBuffer failed");
        return;
    }
    D3DSURFACE_DESC desc;
    back->GetDesc(&desc);

    IDirect3DSurface9 *sys = nullptr;
    HRESULT hr = dev->CreateOffscreenPlainSurface(desc.Width, desc.Height,
                                                  desc.Format, D3DPOOL_SYSTEMMEM,
                                                  &sys, NULL);
    if (SUCCEEDED(hr) && sys) {
        hr = dev->GetRenderTargetData(back, sys);
        if (SUCCEEDED(hr)) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(sys->LockRect(&lr, NULL, D3DLOCK_READONLY))) {
                if (writeBMP(outPath, (const BYTE *)lr.pBits, desc.Width,
                             desc.Height, lr.Pitch))
                    logf("wrote frame %dx%d -> %s", desc.Width, desc.Height, outPath);
                else
                    logf("failed writing %s", outPath);
                sys->UnlockRect();
            }
        } else {
            logf("GetRenderTargetData failed 0x%08lX", (unsigned long)hr);
        }
        sys->Release();
    } else {
        logf("CreateOffscreenPlainSurface failed 0x%08lX", (unsigned long)hr);
    }
    back->Release();
}

// --- video recording ----------------------------------------------------------
//
// One BMP per sentinel trigger tops out near 1-3 fps end to end - fine for
// stills, useless for footage. Instead, while recording is on, every Nth
// Present shrinks the back buffer on the GPU (StretchRect to a small render
// target, so the expensive readback moves ~170 KB instead of ~3.7 MB) and
// appends the raw BGR frame to one file. Python turns the raw stream into an
// actual video afterwards; the hook stays codec-free.
//
// Driven through the same command file as the camera ("rec 1 W H N" / "rec 0");
// a .meta file written on stop carries the dimensions and frame count so the
// encoder does not have to trust the requester's memory.

static bool g_recActive = false;
static int g_recW = 320, g_recH = 180, g_recEvery = 4;
static int g_recSkip = 0, g_recCount = 0;
static int g_recFirstFrame = -1;    // g_frame of the first captured frame, so a
                                    // reader can align video to telemetry exactly
static IDirect3DSurface9 *g_recSys = nullptr;   // full back-buffer size
static FILE *g_recFile = nullptr;

static void stopRecording() {
    if (g_recFile) {
        fclose(g_recFile);
        g_recFile = nullptr;
    }
    if (g_recSys) { g_recSys->Release(); g_recSys = nullptr; }
    if (g_recActive || g_recCount) {
        char metaPath[MAX_PATH];
        pathInDllDir(metaPath, "medge_video.meta");
        FILE *m = nullptr;
        if (fopen_s(&m, metaPath, "w") == 0 && m) {
            // Fifth field: the Present frame of captured frame 0. Video frame
            // k is Present frame first + k*every, which is the same counter
            // the telemetry ring and the trace stamp - so an overlay can put
            // a key press on the exact frame the game received it.
            fprintf(m, "%d %d %d %d %d\n", g_recW, g_recH, g_recEvery,
                    g_recCount, g_recFirstFrame);
            fclose(m);
        }
        logf("recording stopped: %d frames of %dx%d", g_recCount, g_recW, g_recH);
    }
    g_recActive = false;
}

static void startRecording(int w, int h, int every) {
    stopRecording();
    g_recW = (w >= 16 && w <= 1920) ? w : 320;
    g_recH = (h >= 16 && h <= 1080) ? h : 180;
    g_recEvery = (every >= 1 && every <= 60) ? every : 4;
    char rawPath[MAX_PATH];
    pathInDllDir(rawPath, "medge_video.raw");
    if (fopen_s(&g_recFile, rawPath, "wb") != 0 || !g_recFile) {
        logf("recording: cannot open %s", rawPath);
        return;
    }
    g_recSkip = 0;
    g_recCount = 0;
    g_recFirstFrame = -1;
    g_recActive = true;
    logf("recording started: %dx%d every %d frames -> %s",
         g_recW, g_recH, g_recEvery, rawPath);
}

// Full-resolution readback + CPU box-downscale. This replaced a GPU-side
// StretchRect into a small render target during a frozen-frames scare that
// turned out to be a Python-side misdiagnosis (list(ImageSequence.Iterator())
// yields N references to ONE seeked image, so every frame LOOKED identical) -
// the StretchRect path may well have been fine. This path is kept because it
// is byte-identical to the still-shot path, which has been correct all along,
// and a 4x4 box average at 15 fps costs nothing worth optimising.
static void captureRecFrame(IDirect3DDevice9 *dev) {
    IDirect3DSurface9 *back = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back)
        return;
    D3DSURFACE_DESC desc;
    back->GetDesc(&desc);

    if (!g_recSys &&
        FAILED(dev->CreateOffscreenPlainSurface(desc.Width, desc.Height,
                                                desc.Format, D3DPOOL_SYSTEMMEM,
                                                &g_recSys, NULL))) {
        logf("recording: CreateOffscreenPlainSurface failed; stopping");
        back->Release();
        stopRecording();
        return;
    }

    if (SUCCEEDED(dev->GetRenderTargetData(back, g_recSys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(g_recSys->LockRect(&lr, NULL, D3DLOCK_READONLY))) {
            const int kx = desc.Width / g_recW > 0 ? desc.Width / g_recW : 1;
            const int ky = desc.Height / g_recH > 0 ? desc.Height / g_recH : 1;
            const int norm = kx * ky;
            static BYTE packed[1920 * 3];
            for (int y = 0; y < g_recH; ++y) {
                int sy = y * desc.Height / g_recH;
                if (sy > (int)desc.Height - ky) sy = desc.Height - ky;
                for (int x = 0; x < g_recW; ++x) {
                    int sx = x * desc.Width / g_recW;
                    if (sx > (int)desc.Width - kx) sx = desc.Width - kx;
                    unsigned b = 0, g = 0, r = 0;
                    for (int j = 0; j < ky; ++j) {
                        const BYTE *row = (const BYTE *)lr.pBits
                                          + (size_t)(sy + j) * lr.Pitch
                                          + (size_t)sx * 4;
                        for (int i = 0; i < kx; ++i) {
                            b += row[i * 4 + 0];
                            g += row[i * 4 + 1];
                            r += row[i * 4 + 2];
                        }
                    }
                    packed[x * 3 + 0] = (BYTE)(b / norm);
                    packed[x * 3 + 1] = (BYTE)(g / norm);
                    packed[x * 3 + 2] = (BYTE)(r / norm);
                }
                fwrite(packed, 3, g_recW, g_recFile);
            }
            g_recSys->UnlockRect();
            if (g_recFirstFrame < 0) g_recFirstFrame = g_frame;
            ++g_recCount;
        }
    }
    back->Release();
}

static HRESULT WINAPI MyPresent(IDirect3DDevice9 *self, const RECT *src,
                                const RECT *dst, HWND wnd, const RGNDATA *dirty) {
    ++g_frame;

    if (g_recActive && ++g_recSkip >= g_recEvery) {
        g_recSkip = 0;
        captureRecFrame(self);
    }

    // Close a constant dump that was opened on the previous frame.
    if (g_constDump && g_dumpFramesLeft > 0) {
        if (--g_dumpFramesLeft == 0) {
            logf("constant dump complete: %d calls at frame %d", g_dumpCalls, g_frame);
            fclose(g_constDump);
            g_constDump = nullptr;
        }
    }

    // Close a draw-log dump opened on the previous frame, then poll its own
    // trigger. The whole frame between two Presents is captured, exactly like
    // the vsconst dump; the counters reset when it opens (below).
    if (g_drawDump && g_drawFramesLeft > 0) {
        if (--g_drawFramesLeft == 0) {
            logf("draw log complete: %d draws, %d PS-const lines at frame %d",
                 g_drawCount, g_psConstLines, g_frame);
            fclose(g_drawDump);
            g_drawDump = nullptr;
        }
    }
    if (g_drawTrigger[0] && !g_drawDump && (g_frame % 10) == 0 &&
        GetFileAttributesA(g_drawTrigger) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(g_drawTrigger);
        char drawPath[MAX_PATH];
        pathInDllDir(drawPath, "medge_drawlog.txt");
        g_drawDump = _fsopen(drawPath, "w", _SH_DENYNO);
        g_drawFramesLeft = 1;
        g_drawCount = 0;
        g_psConstLines = 0;
        g_describedCount = 0;
        logf("draw-log requested at frame %d -> %s", g_frame, drawPath);
    }

    // Poll cheaply: a capture is a rare, externally driven event.
    if (g_triggerPath[0] && !g_pendingCapture && !g_capturing && !g_constDump &&
        (g_frame % 10) == 0) {
        if (GetFileAttributesA(g_triggerPath) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA(g_triggerPath);
            g_pendingCapture = true;

            char dumpPath[MAX_PATH];
            pathInDllDir(dumpPath, "medge_vsconst.txt");
            g_constDump = _fsopen(dumpPath, "w", _SH_DENYNO);
            g_dumpFramesLeft = 1;
            g_dumpCalls = 0;
            logf("capture requested at frame %d; dumping VS constants to %s",
                 g_frame, dumpPath);
        }
    }

    // Close a capture that was opened on the previous frame. Explicit
    // Start/EndFrameCapture is used rather than TriggerCapture because the
    // latter depends on RenderDoc spotting the frame boundary itself, and this
    // Present is already wrapped by our own hook.
    if (g_rdoc && g_capturing) {
        uint32_t ok = g_rdoc->EndFrameCapture(NULL, NULL);
        g_capturing = false;
        uint32_t n = g_rdoc->GetNumCaptures();
        logf("EndFrameCapture -> %u, total captures=%u", ok, n);
        if (n > 0) {
            char path[MAX_PATH] = {0};
            uint32_t len = MAX_PATH;
            uint64_t ts = 0;
            if (g_rdoc->GetCapture(n - 1, path, &len, &ts))
                logf("capture file: %s", path);
        }
    }

    // Frame grab: cheap poll, then read the back buffer before it is presented.
    if (g_shotTrigger[0] && (g_frame % 4) == 0) {
        if (GetFileAttributesA(g_shotTrigger) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA(g_shotTrigger);
            grabBackBuffer(self, g_shotOut);
        }
    }

    // Decide which of this frame's view-projection candidates was the camera.
    resolveCameraForFrame();

    // EVERY frame, unlike the state file below, which polls at 2. Movement is
    // measured by differentiating this, so halving the rate halves the
    // resolution of every velocity and doubles the aliasing on an 0.8 s jump.
    // Two consumers, one cadence: traceTick feeds the A/B recorder's file
    // channel, the telemetry ring feeds live agents over shared memory.
    // Before the ring, so the pawn snapshot and the camera in the same record
    // describe the same frame.
    pawnTick();
    traceTick();
    ensureTelemetry();
    animTick();
    audioTick();
    telemetryTick();

    // Re-assert a held position every frame, before the game gets to integrate.
    holdPlayerPosition();
    tickPawnFind();
    tickNoclip();
    drivePawnWithCamera();

    // Camera control: poll for a command a few times a second and publish the
    // position read out of the view-projection so Python can see where it is.
    // 6 frames is fine for one-off placement but visibly steppy when a flight
    // path is streamed in; at 2 the poll keeps up with a ~25 Hz update.
    if (++g_camPollFrame >= 2) {
        g_camPollFrame = 0;
        tickCursorClip();
        pollCameraCommand();
        tickPlayerScan();
        writeCameraState();
        reassertWindowFrame();
    }

    HRESULT hr = g_realPresent(self, src, dst, wnd, dirty);

    // Open the capture just after a frame boundary so it spans a whole frame.
    if (g_rdoc && g_pendingCapture) {
        g_rdoc->StartFrameCapture(NULL, NULL);
        g_capturing = true;
        g_pendingCapture = false;
        logf("StartFrameCapture at frame %d (capturing=%u)",
             g_frame, g_rdoc->IsFrameCapturing());
    }
    return hr;
}

// --- vertex shader constant capture ------------------------------------------
//
// This is the part that actually answers the question RenderDoc was wanted for.
// UE3 uploads its view-projection matrix as vertex shader constants, so dumping
// one frame's worth of SetVertexShaderConstantF calls gives the camera's
// projection - and therefore the true field of view - plus the view transform,
// without depending on RenderDoc hooking anything.

typedef HRESULT(WINAPI *SetVSConstF_t)(IDirect3DDevice9 *, UINT, const float *, UINT);
static SetVSConstF_t g_realSetVSConstF = nullptr;

// --- pixel constant / texture / draw capture (the colour oracle) -------------
//
// Same mechanism as the VS-constant dump, on three more slots. Indices are the
// canonical IDirect3DDevice9 vtable, cross-checked by the two this file already
// proves in patchDeviceVTable (Present 17, SetVertexShaderConstantF 94 - both
// canonical): SetTexture 65, DrawIndexedPrimitive 82, SetPixelShaderConstantF
// 109 (NOT 95/105/107 - those are getters or the vertex-side setter).
typedef HRESULT(WINAPI *SetTexture_t)(IDirect3DDevice9 *, DWORD,
                                      IDirect3DBaseTexture9 *);
typedef HRESULT(WINAPI *SetPSConstF_t)(IDirect3DDevice9 *, UINT, const float *,
                                       UINT);
typedef HRESULT(WINAPI *DrawIndexed_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE,
                                       INT, UINT, UINT, UINT, UINT);
static SetTexture_t g_realSetTexture = nullptr;
static SetPSConstF_t g_realSetPSConstF = nullptr;
static DrawIndexed_t g_realDrawIndexed = nullptr;

// g_boundTex0 / g_describedTex / g_describedCount are declared up with the
// draw-log state (MyPresent resets the count). D3D9 state persists across
// draws, so the sampler-0 latch is the whole story - no per-draw snapshot.

static void describeTexOnce(IDirect3DBaseTexture9 *tex) {
    if (!tex || !g_drawDump) return;
    for (int i = 0; i < g_describedCount; ++i)
        if (g_describedTex[i] == tex) return;
    if (g_describedCount < 512) g_describedTex[g_describedCount++] = tex;
    // Only 2D textures carry a diffuse sheet. GetType() avoids QueryInterface
    // (which would pull in dxguid.lib for IID_IDirect3DTexture9); these COM
    // interfaces are single-inheritance, so the derived pointer is the same
    // address and needs no AddRef/Release.
    if (tex->GetType() == D3DRTYPE_TEXTURE) {
        IDirect3DTexture9 *t2 = static_cast<IDirect3DTexture9 *>(tex);
        D3DSURFACE_DESC d;
        if (SUCCEEDED(t2->GetLevelDesc(0, &d)))
            fprintf(g_drawDump, "X %p %u %u %d\n", (void *)tex,
                    d.Width, d.Height, (int)d.Format);
    }
}

static HRESULT WINAPI MySetTexture(IDirect3DDevice9 *self, DWORD stage,
                                   IDirect3DBaseTexture9 *tex) {
    // Stage 0 is the diffuse sampler in every UE3 base-pass shader here; that is
    // the pointer the offline join attributes a constant block to.
    if (stage == 0) {
        g_boundTex0 = tex;
        if (g_drawDump && g_drawFramesLeft > 0) describeTexOnce(tex);
    }
    return g_realSetTexture(self, stage, tex);
}

static HRESULT WINAPI MySetPixelShaderConstantF(IDirect3DDevice9 *self,
                                                UINT startRegister,
                                                const float *data, UINT count) {
    if (g_drawDump && g_drawFramesLeft > 0 && data && count > 0 && count <= 256) {
        // P before the draw that consumes it: tag by the NEXT draw ordinal so
        // the replay associates a constant block with the draw it precedes.
        for (UINT i = 0; i < count; ++i) {
            const float *v = data + i * 4;
            fprintf(g_drawDump, "P %d %u %.6f %.6f %.6f %.6f\n",
                    g_drawCount, startRegister + i, v[0], v[1], v[2], v[3]);
        }
        ++g_psConstLines;
    }
    return g_realSetPSConstF(self, startRegister, data, count);
}

static HRESULT WINAPI MyDrawIndexedPrimitive(IDirect3DDevice9 *self,
                                             D3DPRIMITIVETYPE prim,
                                             INT baseVertex, UINT minIndex,
                                             UINT numVertices, UINT startIndex,
                                             UINT primCount) {
    if (g_drawDump && g_drawFramesLeft > 0) {
        // D closes the draw: the sampler-0 pointer active for it, and its
        // triangle count (a size hint for matching to a mesh section).
        fprintf(g_drawDump, "D %d %p %u\n", g_drawCount, (void *)g_boundTex0,
                primCount);
        ++g_drawCount;
    }
    return g_realDrawIndexed(self, prim, baseVertex, minIndex, numVertices,
                             startIndex, primCount);
}

// --- free camera --------------------------------------------------------------
//
// Mirror's Edge uploads a single shared view-projection for world geometry: the
// same matrix appears in ~50 draws per frame, identical bar float jitter, and
// it is a true VP rather than a per-object world-view-projection (verified by
// comparing every aspect-matching quad in a frame dump). That makes the camera
// movable by rewriting one matrix.
//
// Layout, from the game's shaders: the four registers are the ROWS of M and the
// shader computes o = mul(v, M). So each *column* is a row of the effective
// transform:
//
//     col_j   = (M[0][j], M[1][j], M[2][j])      j = 0,1,2 -> scaled axes
//     col_3   = camera forward, unit length
//     row 3   = translation, row3[j] = -dot(camPos, col_j)
//
// Inverting that last relation recovers the current camera position, and
// rewriting row 3 places it anywhere - exactly, with no effect on orientation
// or projection.

static bool g_freeCamEnabled = false;
static float g_camPos[3] = {0, 0, 0};       // desired world position
static bool g_haveCamPos = false;
static float g_lastCamPos[3] = {0, 0, 0};   // last position read out of the VP
static bool g_lastCamValid = false;

// Picking the camera matrix by shape alone is not enough. The game also uploads
// a screen-space transform whose scales are exactly 1.0 and 1.7778, with a unit
// forward of (0,0,1) - so it satisfies every structural test, matches 16:9, and
// solves to a "camera position" of (640, 360, ...): half the render target.
//
// What separates them is how often each appears. The world view-projection is
// used by ~50 draws per frame; the screen-space one shows up once or twice. So
// candidates are tallied per frame and the most frequent wins.
#define MAX_CAM_CANDIDATES 8
struct CamCandidate { float pos[3]; int count; };
static CamCandidate g_camCand[MAX_CAM_CANDIDATES];
static int g_camCandCount = 0;

static void noteCameraCandidate(const float *pos) {
    for (int i = 0; i < g_camCandCount; ++i) {
        float dx = g_camCand[i].pos[0] - pos[0];
        float dy = g_camCand[i].pos[1] - pos[1];
        float dz = g_camCand[i].pos[2] - pos[2];
        if (dx * dx + dy * dy + dz * dz < 1.0f) {   // same spot, within 1 uu
            g_camCand[i].count++;
            return;
        }
    }
    if (g_camCandCount < MAX_CAM_CANDIDATES) {
        CamCandidate &c = g_camCand[g_camCandCount++];
        c.pos[0] = pos[0]; c.pos[1] = pos[1]; c.pos[2] = pos[2];
        c.count = 1;
    }
}

// The aspect gate can go quiet. On 2026-09-07 a human recording on escape_p
// lost the camera for good at the device Reset the game requested 14 s before
// the first key (fullscreen requested, forced windowed): from that frame on no
// matrix passed the gate, so position and yaw stayed parked at the spawn while
// the pawn ran 10 km, and the trace came back with no mouse in it. Window,
// back buffer and desktop were all 16:9, so which stage went dark is not
// known; hence two things. Per-frame stage counters (structural / solved /
// aspect-ok), logged for a few frames after every Reset and whenever the
// camera has not moved for a while, so the next recording's log says. And a
// fallback: when the gate finds nothing, the unit-forward matrix with the most
// votes in the frame is taken instead, provided it has plenty (the world
// view-projection is used by ~50 draws, the screen-space transform by one or
// two), with the aspect it carried logged once.
#define MAX_OFF_CANDIDATES 4
struct OffCandidate { float pos[3]; int count; float aspect; float fwd[3], right[3], up[3]; };
static OffCandidate g_offCand[MAX_OFF_CANDIDATES];
static int g_offCandCount = 0;
static int g_stageStructural = 0, g_stageSolved = 0, g_stageAspect = 0;
static int g_camDiagFrames = 0;             // frames left to log after a Reset
static int g_camStaleFrames = 0;            // frames since the camera last moved
static bool g_camFallbackOn = false;

static void noteOffAspectCandidate(const float *pos, float aspect, const float *fwd,
                                   const float *right, const float *up) {
    OffCandidate *c = nullptr;
    for (int i = 0; i < g_offCandCount; ++i) {
        float dx = g_offCand[i].pos[0] - pos[0];
        float dy = g_offCand[i].pos[1] - pos[1];
        float dz = g_offCand[i].pos[2] - pos[2];
        if (dx * dx + dy * dy + dz * dz < 1.0f) { c = &g_offCand[i]; c->count++; break; }
    }
    if (!c) {
        if (g_offCandCount >= MAX_OFF_CANDIDATES) return;
        c = &g_offCand[g_offCandCount++];
        c->pos[0] = pos[0]; c->pos[1] = pos[1]; c->pos[2] = pos[2];
        c->count = 1;
    }
    c->aspect = aspect;
    for (int k = 0; k < 3; ++k) { c->fwd[k] = fwd[k]; c->right[k] = right[k]; c->up[k] = up[k]; }
}

static void resolveCameraForFrame() {
    int best = -1;
    for (int i = 0; i < g_camCandCount; ++i)
        if (best < 0 || g_camCand[i].count > g_camCand[best].count) best = i;
    float before[3] = {g_lastCamPos[0], g_lastCamPos[1], g_lastCamPos[2]};
    // Require corroboration: a one-off match is more likely the screen-space
    // transform than the camera.
    if (best >= 0 && g_camCand[best].count >= 3) {
        g_lastCamPos[0] = g_camCand[best].pos[0];
        g_lastCamPos[1] = g_camCand[best].pos[1];
        g_lastCamPos[2] = g_camCand[best].pos[2];
        g_lastCamValid = true;
        if (g_camFallbackOn) { logf("camera: aspect gate is back"); g_camFallbackOn = false; }
    } else if (cameraFromOffAspectCandidate()) {
        // taken from the most voted off-aspect matrix; logged inside
    }
    bool moved = g_lastCamPos[0] != before[0] || g_lastCamPos[1] != before[1]
              || g_lastCamPos[2] != before[2];
    g_camStaleFrames = moved ? 0 : g_camStaleFrames + 1;
    int ob = -1;
    for (int i = 0; i < g_offCandCount; ++i)
        if (ob < 0 || g_offCand[i].count > g_offCand[ob].count) ob = i;
    if (g_camDiagFrames > 0 || (g_camStaleFrames > 600 && (g_camStaleFrames % 3600) == 0)) {
        if (g_camDiagFrames > 0) --g_camDiagFrames;
        logf("camera diag: frame %d structural %d solved %d aspect-ok %d; best votes %d;"
             " best off-aspect votes %d aspect %.3f; stale %d frames",
             g_frame, g_stageStructural, g_stageSolved, g_stageAspect,
             best >= 0 ? g_camCand[best].count : 0,
             ob >= 0 ? g_offCand[ob].count : 0, ob >= 0 ? g_offCand[ob].aspect : 0.0f,
             g_camStaleFrames);
    }
    g_stageStructural = g_stageSolved = g_stageAspect = 0;
    g_offCandCount = 0;
    g_camCandCount = 0;
}
static char g_camCmdPath[MAX_PATH] = {0};
static char g_camStatePath[MAX_PATH] = {0};

// Defined with the noclip state further down; used by the command handlers.
static void syncNoclipToCamera();


static bool looksLikeViewProj(const float *m, float aspect) {
    // col_3 must be a unit vector (the camera forward).
    float fx = m[0 * 4 + 3], fy = m[1 * 4 + 3], fz = m[2 * 4 + 3];
    float n = sqrtf(fx * fx + fy * fy + fz * fz);
    if (fabsf(n - 1.0f) > 5e-3f) return false;

    float sx = sqrtf(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
    float sy = sqrtf(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);

    // A perspective projection's scales are cot(fov/2), so for any sane field
    // of view they sit near 1. Screen-space quads use half the render target as
    // their scale (640/360 at 1280x720) and would otherwise pass every other
    // test here - including the aspect check, since 640.5/640.9 is close to
    // 16:9 by coincidence. Reading one of those as the camera yields a nonsense
    // "position" of (640, 360, ...).
    if (sx < 0.2f || sx > 20.0f || sy < 0.2f || sy > 20.0f) return false;

    // The camera's own projection matches the render target's aspect; shadow
    // cascades and cubemap faces come out square.
    return fabsf((sy / sx) - aspect) < 0.02f;
}

// Solve row3 = -A^T * camPos for camPos, where A's columns are col_0..col_2.
static bool cameraPositionFrom(const float *m, float *out) {
    float a[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            a[r][c] = m[r * 4 + c];       // a[r][c] = col_c component r

    // We need to solve A^T * p = -row3, i.e. dot(col_j, p) = -row3[j].
    float b[3] = {-m[3 * 4 + 0], -m[3 * 4 + 1], -m[3 * 4 + 2]};
    float mtx[3][3];
    for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k)
            mtx[j][k] = a[k][j];          // row j = col_j

    // Gaussian elimination with partial pivoting.
    for (int i = 0; i < 3; ++i) {
        int piv = i;
        for (int r = i + 1; r < 3; ++r)
            if (fabsf(mtx[r][i]) > fabsf(mtx[piv][i])) piv = r;
        if (fabsf(mtx[piv][i]) < 1e-8f) return false;
        if (piv != i) {
            for (int k = 0; k < 3; ++k) { float t = mtx[i][k]; mtx[i][k] = mtx[piv][k]; mtx[piv][k] = t; }
            float t = b[i]; b[i] = b[piv]; b[piv] = t;
        }
        for (int r = i + 1; r < 3; ++r) {
            float f = mtx[r][i] / mtx[i][i];
            for (int k = i; k < 3; ++k) mtx[r][k] -= f * mtx[i][k];
            b[r] -= f * b[i];
        }
    }
    for (int i = 2; i >= 0; --i) {
        float s = b[i];
        for (int k = i + 1; k < 3; ++k) s -= mtx[i][k] * out[k];
        out[i] = s / mtx[i][i];
    }
    return true;
}

// Move the eye by TRANSLATING row 3 rather than rebuilding it.
//
// Rebuilding it as row3[j] = -dot(pos, col_j) looks equivalent and is not. It
// holds for the x and y columns, but the depth column of a D3D projection is
// clip.z = z_view*A + B, so row3[2] = -A*dot(pos, fwd) + B. Recomputing from
// position alone silently drops B, which wrecks the depth of every draw in the
// frame: near geometry vanishes and the backdrop paints a band across the
// middle distance. It is not a displacement artefact - it corrupts the image
// just as badly when the "new" position equals the current one.
//
// Translating leaves every other term, B included, untouched: shifting the eye
// by d changes row3[j] by exactly -dot(d, col_j), for all four columns.
static void translateCamera(float *m, const float *delta) {
    for (int j = 0; j < 4; ++j) {
        float cx = m[0 * 4 + j], cy = m[1 * 4 + j], cz = m[2 * 4 + j];
        m[3 * 4 + j] -= delta[0] * cx + delta[1] * cy + delta[2] * cz;
    }
}

static bool g_orientEnabled = false;
static float g_camYaw = 0.0f, g_camPitch = 0.0f;    // radians
static float g_lastYaw = 0.0f, g_lastPitch = 0.0f;  // as read out of the VP
// The camera's full orientation basis, published alongside yaw/pitch because
// those two cannot express roll and go singular when the view passes vertical.
static float g_lastFwd[3] = {1.0f, 0.0f, 0.0f};
static float g_lastRight[3] = {0.0f, 1.0f, 0.0f};
static float g_lastUp[3] = {0.0f, 0.0f, 1.0f};

// The fallback resolveCameraForFrame reaches for when the aspect gate found
// nothing this frame: the most voted unit-forward matrix, if it has the kind
// of vote count only the world view-projection gets.
static bool cameraFromOffAspectCandidate() {
    int ob = -1;
    for (int i = 0; i < g_offCandCount; ++i)
        if (ob < 0 || g_offCand[i].count > g_offCand[ob].count) ob = i;
    if (ob < 0 || g_offCand[ob].count < 10) return false;
    const OffCandidate &c = g_offCand[ob];
    g_lastCamPos[0] = c.pos[0]; g_lastCamPos[1] = c.pos[1]; g_lastCamPos[2] = c.pos[2];
    g_lastCamValid = true;
    for (int k = 0; k < 3; ++k) { g_lastFwd[k] = c.fwd[k]; g_lastRight[k] = c.right[k]; g_lastUp[k] = c.up[k]; }
    g_lastYaw = atan2f(c.fwd[1], c.fwd[0]);
    float fz = c.fwd[2];
    g_lastPitch = asinf(fz < -1.0f ? -1.0f : (fz > 1.0f ? 1.0f : fz));
    if (!g_camFallbackOn) {
        logf("camera: aspect gate (16:9) found nothing; taking the most voted view"
             " matrix instead, aspect %.3f with %d votes", c.aspect, c.count);
        g_camFallbackOn = true;
    }
    return true;
}

// Whether cross(worldUp, forward) points to SCREEN right or screen left, read
// off the game's own matrix by reorientCamera. Noclip strafes along it, so
// assuming a handedness here would silently invert A and D.
static float g_rightSign = 1.0f;

static float dot3(const float *a, const float *b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static void cross3(const float *a, const float *b, float *o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
static bool norm3(float *v) {
    float n = sqrtf(dot3(v, v));
    if (n < 1e-6f) return false;
    v[0] /= n; v[1] /= n; v[2] /= n;
    return true;
}

// Aim the camera at an absolute yaw/pitch, keeping the eye where it is.
//
// The matrix is decomposed rather than replaced, because each pass carries its
// own projection and those terms must survive untouched:
//
//     col_0 = right * sx      col_2 = fwd * A     row3[2] = -A*dot(p,fwd) + B
//     col_1 = up    * sy      col_3 = fwd
//
// so sx, sy, A and the near-plane constant B are recovered from the matrix
// itself and written back around the new basis. Handedness is likewise read
// off the existing basis instead of assumed, so this needs no knowledge of the
// engine's axis conventions - it reproduces whatever the frame already uses.
static bool reorientCamera(float *m, const float *pos, float yaw, float pitch) {
    float fwd[3] = {m[0 * 4 + 3], m[1 * 4 + 3], m[2 * 4 + 3]};
    float col0[3] = {m[0 * 4 + 0], m[1 * 4 + 0], m[2 * 4 + 0]};
    float col1[3] = {m[0 * 4 + 1], m[1 * 4 + 1], m[2 * 4 + 1]};
    float col2[3] = {m[0 * 4 + 2], m[1 * 4 + 2], m[2 * 4 + 2]};

    float sx = sqrtf(dot3(col0, col0)), sy = sqrtf(dot3(col1, col1));
    if (sx < 1e-6f || sy < 1e-6f) return false;
    float A = dot3(col2, fwd);
    // Only a standard perspective pass, whose depth column is parallel to the
    // forward axis, can be rebuilt this way. Anything else is left alone.
    if (sqrtf(dot3(col2, col2)) > 1e-6f &&
        fabsf(A) < 0.99f * sqrtf(dot3(col2, col2))) return false;

    // Recover the constant term of EVERY column, not just depth. row3[j] is
    // -dot(pos, col_j) plus a constant, and assuming that constant is zero is
    // the same mistake that rebuilding the eye made: it holds for x and y in a
    // centred projection but not for depth, and not for x/y under any
    // off-centre or jittered projection. Reading all four back from the matrix
    // and re-adding them keeps whatever the pass actually uses.
    float ofs[4];
    for (int j = 0; j < 4; ++j) {
        float cx = m[0 * 4 + j], cy = m[1 * 4 + j], cz = m[2 * 4 + j];
        ofs[j] = m[3 * 4 + j] + (pos[0] * cx + pos[1] * cy + pos[2] * cz);
    }

    float right[3] = {col0[0] / sx, col0[1] / sx, col0[2] / sx};
    float up[3] = {col1[0] / sy, col1[1] / sy, col1[2] / sy};

    // Build the horizontal reference from the YAW, not from the forward axis.
    // cross(worldUp, fwd) collapses when fwd is vertical, which is exactly the
    // straight-down view worth having and the one the game's own look cannot
    // reach. The heading stays well defined at every pitch, including +-90.
    const float wUp[3] = {0.0f, 0.0f, 1.0f};
    float hCur[3] = {fwd[0], fwd[1], 0.0f};
    float r0[3], u0[3];
    float sRight = 1.0f, sUp = 1.0f;
    if (norm3(hCur)) {
        cross3(wUp, hCur, r0);
        if (!norm3(r0)) return false;
        cross3(fwd, r0, u0);
        if (!norm3(u0)) return false;
        sRight = dot3(right, r0) >= 0.0f ? 1.0f : -1.0f;
        sUp = dot3(up, u0) >= 0.0f ? 1.0f : -1.0f;
        g_rightSign = sRight;
    }

    float cp = cosf(pitch), sp = sinf(pitch);
    float nf[3] = {cp * cosf(yaw), cp * sinf(yaw), sp};
    float nh[3] = {cosf(yaw), sinf(yaw), 0.0f};
    float nr[3], nu[3];
    cross3(wUp, nh, nr);
    if (!norm3(nr)) return false;
    cross3(nf, nr, nu);
    if (!norm3(nu)) return false;
    for (int i = 0; i < 3; ++i) { nr[i] *= sRight; nu[i] *= sUp; }

    for (int r = 0; r < 3; ++r) {
        m[r * 4 + 0] = nr[r] * sx;
        m[r * 4 + 1] = nu[r] * sy;
        m[r * 4 + 2] = nf[r] * A;
        m[r * 4 + 3] = nf[r];
    }
    for (int j = 0; j < 4; ++j) {
        float cx = m[0 * 4 + j], cy = m[1 * 4 + j], cz = m[2 * 4 + j];
        m[3 * 4 + j] = ofs[j] - (pos[0] * cx + pos[1] * cy + pos[2] * cz);
    }
    return true;
}

// Structural tests decide which matrices are the camera; the aspect ratio must
// not be one of them when deciding what to MOVE.
//
// A frame dump settles what is actually uploaded. Of the 4-register blocks that
// match the render target's aspect, 51 of 59 are the same bare view-projection
// at register 0 - UE3 uploads it once and thousands of draws reuse it, so
// patching it moves nearly all world geometry. There are no per-object
// world-view-projections to chase.
//
// What the aspect test *did* exclude were 28 draws at register 234 that solve
// to the camera but carry a square, ~9.6-degree frustum of their own. Those are
// a separate camera-anchored pass, and leaving them behind is what put a band
// across the frame. Position is the honest test of "is this the eye"; aspect
// only ever served to tell the true camera from screen-space quads, and the
// distance gate already does that job better.

// --- interactive noclip -------------------------------------------------------
//
// V toggles a fly camera driven from the keyboard, so the level can be explored
// by hand instead of through scripted paths.
//
//   V              toggle on/off          W/S/A/D   fly (relative to the view)
//   mouse          look (game's own)      Space/C   up / down
//   arrows         look (unclamped)       Shift/Alt faster / slower
//
// Mouse look is free: the orientation read out of the view-projection is the
// GAME's, taken before the patch is applied, so it still tracks the mouse while
// the override is active. Feeding that frame-to-frame delta into our own
// heading gives the game's exact sensitivity and feel for nothing. The arrow
// keys then add what the mouse cannot reach, since the game clamps pitch well
// short of vertical.
//
// V is safe to claim: TdInput.ini binds it only as Ctrl+V ("EDIT PASTE").
//
// WASD still reaches the game, so Faith walks around underneath. That is
// harmless to the camera - position is ours outright, and orientation moves on
// deltas, which walking does not produce.

static bool g_noclip = false;
static bool g_ncPrevToggle = false;
static float g_ncPos[3] = {0, 0, 0};
static float g_ncYaw = 0.0f, g_ncPitch = 0.0f;      // radians
static float g_ncGameYaw = 0.0f, g_ncGamePitch = 0.0f;
static bool g_ncHaveGame = false;
static LARGE_INTEGER g_ncLast = {0};

static bool keyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Adopt whatever the camera was just commanded to, so a scripted pose does not
// get overwritten by the flying state on the very next frame.
static void syncNoclipToCamera() {
    if (!g_noclip) return;
    g_ncPos[0] = g_camPos[0];
    g_ncPos[1] = g_camPos[1];
    g_ncPos[2] = g_camPos[2];
    if (g_orientEnabled) {
        g_ncYaw = g_camYaw;
        g_ncPitch = g_camPitch;
    }
}

static float wrapPi(float a) {
    const float kPi = 3.14159265358979f;
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

static void tickNoclip() {
    if (g_gameWindow && GetForegroundWindow() != g_gameWindow) {
        // Losing focus stops the camera being driven, but must NOT silently
        // look like noclip is off - the state file still says it is on, so say
        // so in the log rather than leaving a confusing gap.
        static int quiet = 0;
        if (g_noclip && ++quiet % 120 == 0)
            logf("noclip active but window is not foreground; input ignored");
        g_ncPrevToggle = false;
        return;
    }
    if (g_noclip) {
        static int beat = 0;
        if (++beat % 120 == 0)
            logf("noclip active: blocked %d key presses so far", g_blockedKeyCount);
    }

    bool toggle = keyDown('V');
    if (toggle && !g_ncPrevToggle) {
        if (!g_noclip && g_lastCamValid) {
            // Start exactly where the player is looking, so it never jumps.
            g_ncPos[0] = g_lastCamPos[0];
            g_ncPos[1] = g_lastCamPos[1];
            g_ncPos[2] = g_lastCamPos[2];
            g_ncYaw = g_lastYaw;
            g_ncPitch = g_lastPitch;
            g_ncHaveGame = false;
            g_noclip = true;
            // Anything held at this moment has already reached the game, and
            // its release would now be blocked - so hand the game a release for
            // every blocked key or Faith walks on forever.
            releaseHeldKeys();
            logf("noclip ON at %.1f %.1f %.1f", g_ncPos[0], g_ncPos[1], g_ncPos[2]);
        } else if (g_noclip) {
            g_noclip = false;
            g_freeCamEnabled = false;
            g_orientEnabled = false;
            logf("noclip OFF");
        }
    }
    g_ncPrevToggle = toggle;
    if (!g_noclip) return;

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = 0.0f;
    if (g_ncLast.QuadPart)
        dt = (float)(now.QuadPart - g_ncLast.QuadPart) / (float)freq.QuadPart;
    g_ncLast = now;
    if (dt <= 0.0f) return;
    if (dt > 0.1f) dt = 0.1f;          // a hitch must not fling the camera

    // Mouse look, as the game's own change in heading since last frame.
    if (g_lastCamValid) {
        if (g_ncHaveGame) {
            float dy = wrapPi(g_lastYaw - g_ncGameYaw);
            float dp = g_lastPitch - g_ncGamePitch;
            // A respawn or cutscene cut snaps the camera; that is not a mouse
            // movement, so reject anything larger than a plausible flick.
            if (fabsf(dy) < 0.5f && fabsf(dp) < 0.5f) {
                g_ncYaw += dy;
                g_ncPitch += dp;
            }
        }
        g_ncGameYaw = g_lastYaw;
        g_ncGamePitch = g_lastPitch;
        g_ncHaveGame = true;
    }

    const float kDeg = 3.14159265358979f / 180.0f;
    float look = 100.0f * kDeg * dt;
    if (keyDown(VK_LEFT)) g_ncYaw -= look;
    if (keyDown(VK_RIGHT)) g_ncYaw += look;
    if (keyDown(VK_UP)) g_ncPitch += look;
    if (keyDown(VK_DOWN)) g_ncPitch -= look;

    const float kMaxPitch = 89.5f * kDeg;
    if (g_ncPitch > kMaxPitch) g_ncPitch = kMaxPitch;
    if (g_ncPitch < -kMaxPitch) g_ncPitch = -kMaxPitch;
    g_ncYaw = wrapPi(g_ncYaw);

    float speed = 700.0f;                        // uu/s, i.e. 7 m/s
    if (keyDown(VK_SHIFT)) speed *= 4.0f;
    if (keyDown(VK_CONTROL)) speed *= 0.25f;     // not Alt - see kBlockedKeys
    speed *= dt;

    float cp = cosf(g_ncPitch), sp = sinf(g_ncPitch);
    float cy = cosf(g_ncYaw), sy = sinf(g_ncYaw);
    float fwd[3] = {cp * cy, cp * sy, sp};
    // cross(worldUp, fwd), flipped to whichever way the game actually renders
    // as screen-right, so D always strafes right.
    float right[3] = {-sy * g_rightSign, cy * g_rightSign, 0.0f};

    if (keyDown('W')) for (int i = 0; i < 3; ++i) g_ncPos[i] += fwd[i] * speed;
    if (keyDown('S')) for (int i = 0; i < 3; ++i) g_ncPos[i] -= fwd[i] * speed;
    if (keyDown('D')) for (int i = 0; i < 3; ++i) g_ncPos[i] += right[i] * speed;
    if (keyDown('A')) for (int i = 0; i < 3; ++i) g_ncPos[i] -= right[i] * speed;
    if (keyDown(VK_SPACE)) g_ncPos[2] += speed;
    if (keyDown('C')) g_ncPos[2] -= speed;

    g_camPos[0] = g_ncPos[0];
    g_camPos[1] = g_ncPos[1];
    g_camPos[2] = g_ncPos[2];
    g_camYaw = g_ncYaw;
    g_camPitch = g_ncPitch;
    g_haveCamPos = true;
    g_orientEnabled = true;
    g_freeCamEnabled = true;
}

// --- the pawn, through UE3's own object table ---------------------------------
//
// Everything above this point knows only where the CAMERA is, solved out of a
// view-projection matrix on its way to the GPU. That is an honest measurement
// and it is also all we had: the harness has been reconstructing "which move is
// Faith in" from position and height curves, at considerable cost (see
// doc/medge_autoplay.md sections 6 and 7a), because the authoritative answer
// lives in the game's heap and we could not reach it.
//
// We can now. The retail game is UE3, so every UObject is registered in a
// global table, and the layouts are known (doc/medge_sdk.md). What follows
// finds that table, walks it to the player's pawn, and publishes the move
// state, the pawn's own velocity and the world's gravity into the ring.
//
// Two things make this safe to do by structure rather than by copying someone
// else's byte signatures:
//
//   * GObjects is a TArray<UObject*> - {Data, Count, Max} - and it IDENTIFIES
//     ITSELF, because every UObject stores its own index in that table at
//     offset 0x04. Checking 64 consecutive entries against their slot numbers
//     is a 64-way self-consistency test; nothing else in the address space
//     passes it by accident. Same discipline as the BSP decode, where the
//     TTransArray owner reference resolving to the Model is what made the
//     layout self-checking.
//   * The pawn is then accepted only if its Location agrees with the camera we
//     solved from the GPU pipeline. That is a measurement against something
//     INDEPENDENT, not a tolerance - which is what CLAUDE.md says to reach for,
//     and it is why the earlier value-scan for the pawn (see the dead-ends
//     table) is not what this is.
//
// The scan is confined to the exe's own .data section. Note that most of .data
// exists only in memory - virtual size 0x141C44 against 0x45000 on disk - so
// GObjects, being zero-initialised, is not in the file at all. This cannot be
// done offline, DRM or no DRM.

// UObject
#define UO_INDEX          0x004    // ObjectInternalInteger: its own GObjects slot
// AActor
#define AA_PHYSICS        0x068
#define AA_LOCATION       0x0E8
// Rotation is not independently calibrated: it is the 12 bytes BETWEEN the two
// fields that are - Location 0xE8 + 12 = 0xF4, + 12 = 0x100 = Velocity, which
// is UE3's declared order (Location, Rotation, Velocity, all contiguous). A
// rotator is 3 ints, 65536 per turn, Pitch then Yaw then Roll. pawnset
// verifies its write by reading the yaw back.
#define AA_ROTATION       0x0F4
#define AA_VELOCITY       0x100
// AController / APlayerController
#define AC_ACKPAWN        0x2F8    // AcknowledgedPawn
// AWorldInfo
#define AW_WORLDGRAVITY   0xCCC
#define AW_DEFAULTGRAVITY 0xCD0
#define AW_CONTROLLERLIST 0xCE0
// ATdPawn
#define TD_VELMAG2D       0x4B8
#define TD_OLDMOVE        0x4FC
#define TD_PENDINGMOVE    0x4FD
#define TD_MOVESTATE      0x4FE
#define TD_WALKINGSTATE   0x503
#define TD_MOVES          0x5FC    // TArray<UTdMove*>, indexed by EMovement

#define EMOVEMENT_COUNT   95       // decoded from TdGame.u, tools/retail/movenames.py
#define MEDGE_INI_GRAVITY (-800.0f)  // DefaultGravityZ, the one value we know

// The ADDRESS of the GObjects TArray in .data - not its Data pointer.
//
// GObjects grows as a level streams in, and a TArray reallocates when it does,
// so the Data pointer is only valid until the next spawn. Caching it looked
// like it worked for as long as the scan was only ever run by hand, after
// loading had finished; running it from startup instead latched the table as
// it was during the load and then swept freed memory forever. Re-read Data and
// Count from the array every time.
static unsigned char *g_gobjectsArray = nullptr;
static void **g_gobjects = nullptr;
static int    g_gobjectCount = 0;

static bool gobjectsRefresh();      // defined below the SEH read helpers
static void  *g_worldInfo = nullptr;
static void  *g_playerController = nullptr;
static int    g_pawnScanFrame = -100000;

// Snapshot published each frame.
static bool  g_pawnValid = false;
static void *g_pawnObj = nullptr;   // the validated UObject, for Location writes
// The scan's own pawn, kept for maps where no controller's AckPawn ever points
// at her. Authored showroom maps are like this: acceptPawn finds the pawn but
// the controller sweep comes up empty, and the controller-only path in
// pawnTick then yields null forever - so tp and pawn-carry silently no-op and
// per-prop distance culling eats every pad past ~4500uu (the all-black
// catwalk sweep). Guarded by pawnStillLooksRight on every use.
static void *g_pawnFallback = nullptr;
static bool  g_worldValid = false;
static unsigned char g_moveState = 0xFF, g_oldMoveState = 0xFF;
static unsigned char g_pendingMoveState = 0xFF, g_walkingState = 0xFF;
static unsigned char g_pawnPhysics = 0xFF;
static float g_pawnPos[3] = {0, 0, 0};
static float g_pawnVel[3] = {0, 0, 0};
static float g_ctrlYawDeg = 0.0f, g_ctrlPitchDeg = 0.0f, g_pawnYawDeg = 0.0f;
static bool  g_rotValid = false;
static float g_worldGravityZ = 0.0f;

static bool sehRead(const void *src, void *dst, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The one write primitive, and it is deliberately the only one: everything
// else this hook does is read-only, and the A/B harness that needs this
// (doc/medge_movement_ab.md) needs exactly one kind of write - placing the
// pawn at a known pose so an open-loop input script starts from the same
// state in both engines. btbd/mmultiplayer teleports retail by writing the
// same Location field, so the write itself is known-safe.
static bool sehWrite(void *dst, const void *src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool readPtr(const void *p, void **out) {
    return sehRead(p, out, sizeof(void *));
}
static bool readI32(const void *p, int *out) {
    return sehRead(p, out, 4);
}
static bool readF32(const void *p, float *out) {
    return sehRead(p, out, 4);
}
static bool readU8(const void *p, unsigned char *out) {
    return sehRead(p, out, 1);
}

static bool finiteF(float v) { return v == v && v > -3.0e38f && v < 3.0e38f; }

// Re-read Data and Count from the GObjects TArray. Must happen before every
// sweep: the array reallocates as a level streams in, so a Data pointer cached
// at startup is freed memory by the time there is a pawn to find.
static bool gobjectsRefresh() {
    if (!g_gobjectsArray) return false;
    void *data = nullptr;
    int count = 0;
    if (!readPtr(g_gobjectsArray, &data) ||
        !readI32(g_gobjectsArray + 4, &count))
        return false;
    if (!data || count < 1 || count > 4000000) return false;
    g_gobjects = (void **)data;
    g_gobjectCount = count;
    return true;
}

// The exe's .data section, where a zero-initialised global lives.
static bool gameDataRange(unsigned char **lo, unsigned char **hi) {
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    if (!base) return false;
    IMAGE_DOS_HEADER dos;
    if (!sehRead(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    IMAGE_NT_HEADERS32 nt;
    if (!sehRead(base + dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE)
        return false;
    IMAGE_SECTION_HEADER *sec = (IMAGE_SECTION_HEADER *)
        (base + dos.e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
         nt.FileHeader.SizeOfOptionalHeader);
    for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; i++) {
        IMAGE_SECTION_HEADER s;
        if (!sehRead(sec + i, &s, sizeof(s))) return false;
        if (memcmp(s.Name, ".data", 5) == 0) {
            *lo = base + s.VirtualAddress;
            *hi = *lo + s.Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// Is this 12-byte window a TArray whose entries know their own slot numbers?
static bool looksLikeGObjects(unsigned char *p, void ***dataOut, int *countOut) {
    void *data = nullptr;
    int count = 0, maxn = 0;
    if (!readPtr(p, &data) || !readI32(p + 4, &count) || !readI32(p + 8, &maxn))
        return false;
    if (!data || count < 1000 || count > 2000000 || maxn < count || maxn > 4000000)
        return false;
    int checked = 0;
    for (int i = 0; i < 4096 && checked < 64; i++) {
        void *obj = nullptr;
        if (!readPtr((unsigned char *)data + i * sizeof(void *), &obj))
            return false;
        if (!obj) continue;                       // free slots are normal
        int self = 0;
        if (!readI32((unsigned char *)obj + UO_INDEX, &self)) return false;
        if (self != i) return false;              // the whole test
        checked++;
    }
    if (checked < 16) return false;
    *dataOut = (void **)data;
    *countOut = count;
    return true;
}

static void findGObjects() {
    unsigned char *lo = nullptr, *hi = nullptr;
    if (!gameDataRange(&lo, &hi)) {
        logf("pawn: no .data section - not the game process?");
        return;
    }
    for (unsigned char *p = lo; p + 12 <= hi; p += 4) {
        void **data = nullptr;
        int count = 0;
        if (looksLikeGObjects(p, &data, &count)) {
            g_gobjectsArray = p;
            g_gobjects = data;
            g_gobjectCount = count;
            logf("pawn: GObjects array at %p (data %p, %d objects), .data %p..%p",
                 p, data, count, lo, hi);
            return;
        }
    }
    logf("pawn: GObjects not found in .data (%p..%p)", lo, hi);
}

// How much of an object we are willing to read in one go when calibrating.
static size_t snapshotObject(void *obj, unsigned char *buf, size_t maxn) {
    static const size_t sizes[] = {0x1000, 0x800, 0x400, 0x200, 0x100, 0x80};
    for (int i = 0; i < 6; i++) {
        if (sizes[i] > maxn) continue;
        if (sehRead(obj, buf, sizes[i])) return sizes[i];
    }
    return 0;
}

// CALIBRATION. The offsets above come from a third-party dump of a build that
// may not be ours, and UE3 does not serialise UProperty::Offset into the
// package, so they cannot be checked offline - they have to be found in the
// process. Location can be, because we already know where the player is by a
// completely independent route: the camera solved from the GPU pipeline. So
// sweep each object for a float triple sitting where Faith's feet must be, and
// let the answer say what the real offset is. Everything else is then checked
// against it rather than assumed.
static int g_locOffset = 0;
static int g_calibLogged = 0;

static float g_calibXY = 150.0f;      // widened by the `pawn` command's args
static float g_calibZLo = -60.0f, g_calibZHi = 260.0f;

static bool matchesCamera(const float *p) {
    if (!finiteF(p[0]) || !finiteF(p[1]) || !finiteF(p[2])) return false;
    if (fabsf(p[0] - g_lastCamPos[0]) > g_calibXY) return false;
    if (fabsf(p[1] - g_lastCamPos[1]) > g_calibXY) return false;
    const float dz = g_lastCamPos[2] - p[2];
    return dz >= g_calibZLo && dz <= g_calibZHi;
}

// Find the offset of a camera-agreeing position triple, if the object has one.
static bool findLocationOffset(void *obj, int *offOut) {
    unsigned char buf[0x1000];
    const size_t n = snapshotObject(obj, buf, sizeof(buf));
    if (n < 0x40) return false;
    for (size_t off = 0x20; off + 12 <= n; off += 4) {
        if (matchesCamera((const float *)(buf + off))) {
            *offOut = (int)off;
            return true;
        }
    }
    return false;
}

// Does this object look like the player's pawn, AND stand where the camera says?
//
// Two independent signatures, and it needs both. The move fields alone match
// hundreds of class-default templates, which sit at Location (0,0,0). The
// position alone matches every mesh component hanging off her. Together,
// nothing else in 79465 objects qualifies.
//
// NOT gated on Moves. Both the third-party dump and an independent derivation
// of UE3's Link rules from our own packages put that TArray at 0x5FC, and yet
// requiring a sane one there rejects the pawn - twice, measured. Since the
// three fields this DOES check are confirmed by the same two derivations and
// by a behavioural test (MovementState went MOVE_Walking -> MOVE_Jump on a
// jump, with OldMovementState inheriting MOVE_Walking, at exactly 0x4FE and
// 0x4FC), the sensible reading is that our TdPlayerPawn's array lives
// somewhere the model does not predict - a question for later, not a reason
// to refuse the pawn we can otherwise identify three independent ways.
static bool acceptPawn(void *obj) {
    unsigned char *o = (unsigned char *)obj;
    unsigned char mv = 0, old = 0, pend = 0, phys = 0;
    if (!readU8(o + TD_MOVESTATE, &mv) || mv >= EMOVEMENT_COUNT) return false;
    if (!readU8(o + TD_OLDMOVE, &old) || old >= EMOVEMENT_COUNT) return false;
    if (!readU8(o + TD_PENDINGMOVE, &pend) || pend >= EMOVEMENT_COUNT) return false;
    if (!readU8(o + AA_PHYSICS, &phys) || phys >= 32) return false;
    if (mv == 0) return false;              // a live pawn is always in a move

    float pos[3], vel[3];
    if (!sehRead(o + AA_LOCATION, pos, 12) || !sehRead(o + AA_VELOCITY, vel, 12))
        return false;
    for (int i = 0; i < 3; i++)
        if (!finiteF(pos[i]) || !finiteF(vel[i])) return false;
    if (fabsf(vel[0]) > 5000.0f || fabsf(vel[1]) > 5000.0f ||
        fabsf(vel[2]) > 8000.0f) return false;

    // THE ORACLE. The camera comes from the GPU pipeline; this comes from the
    // heap. Agreeing in x/y to a body width, with the eye somewhere sane
    // relative to the pawn origin, is not something a random object does.
    //
    // The lower bound is NEGATIVE on purpose. It was +10, on the reasoning
    // that the eye is always above the origin - and a slide puts it BELOW.
    // The first human recording lost both slides to that: the eye drops ~96 uu
    // as she goes down, dz fell under 10, acceptPawn refused, and because a
    // failed pawn also triggers the re-acquisition throttle it then blackholed
    // 120 further frames. Five gaps of exactly 121 frames, 4.3% of the
    // session, each starting 0.34 s into a slide or a vault - and the gap hid
    // the very move the recording was made to capture.
    //
    // A tolerance that quietly deletes the interesting data is worse than no
    // tolerance. The x/y agreement plus the three coherent move-enum bytes are
    // what actually identify the pawn; this axis only needs to exclude objects
    // in a different part of the level.
    if (!g_lastCamValid) return false;
    if (fabsf(pos[0] - g_lastCamPos[0]) > 60.0f) return false;
    if (fabsf(pos[1] - g_lastCamPos[1]) > 60.0f) return false;
    const float dz = g_lastCamPos[2] - pos[2];
    if (dz < -120.0f || dz > 260.0f) return false;
    return true;
}

// A weaker test for a pawn we already hold: is this still a pawn at all?
// No camera agreement - see the note in pawnTick about why re-litigating the
// oracle every frame threw away the slides.
static bool pawnStillLooksRight(void *obj) {
    unsigned char *o = (unsigned char *)obj;
    unsigned char mv = 0, old = 0, pend = 0;
    if (!readU8(o + TD_MOVESTATE, &mv) || mv >= EMOVEMENT_COUNT) return false;
    if (!readU8(o + TD_OLDMOVE, &old) || old >= EMOVEMENT_COUNT) return false;
    if (!readU8(o + TD_PENDINGMOVE, &pend) || pend >= EMOVEMENT_COUNT) return false;
    if (mv == 0) return false;
    float pos[3];
    if (!sehRead(o + AA_LOCATION, pos, 12)) return false;
    return finiteF(pos[0]) && finiteF(pos[1]) && finiteF(pos[2]);
}

// WorldInfo identifies itself by a value we already know from the ini.
static bool acceptWorldInfo(void *obj) {
    unsigned char *o = (unsigned char *)obj;
    float def = 0.0f, world = 0.0f;
    if (!readF32(o + AW_DEFAULTGRAVITY, &def)) return false;
    if (fabsf(def - MEDGE_INI_GRAVITY) > 0.5f) return false;
    if (!readF32(o + AW_WORLDGRAVITY, &world)) return false;
    if (!finiteF(world) || world > -50.0f || world < -20000.0f) return false;
    void *list = nullptr;
    if (!readPtr(o + AW_CONTROLLERLIST, &list)) return false;
    return list != nullptr;
}

// One-shot: report every object standing where the camera says the player is,
// with the offset the position was found at and what the assumed move bytes
// read there. Drives the offsets from evidence instead of from a stranger's
// dump. Triggered by the `pawn` camera command; logs and returns.
static void pawnCalibrate() {
    if (!g_gobjectsArray) findGObjects();
    gobjectsRefresh();
    if (!g_gobjects || !g_lastCamValid) {
        logf("pawncal: no table (%p) or no camera yet", g_gobjects);
        return;
    }
    logf("pawncal: camera at %.1f %.1f %.1f, sweeping %d objects",
         g_lastCamPos[0], g_lastCamPos[1], g_lastCamPos[2], g_gobjectCount);
    int hits = 0;
    for (int i = 0; i < g_gobjectCount && hits < 24; i++) {
        void *obj = nullptr;
        if (!readPtr((unsigned char *)g_gobjects + i * sizeof(void *), &obj))
            break;
        if (!obj) continue;
        int off = 0;
        if (!findLocationOffset(obj, &off)) continue;
        hits++;
        float pos[3] = {0, 0, 0};
        sehRead((unsigned char *)obj + off, pos, 12);
        unsigned char mv = 0xFF, old = 0xFF, pend = 0xFF, phys = 0xFF;
        readU8((unsigned char *)obj + off + (TD_MOVESTATE - AA_LOCATION), &mv);
        readU8((unsigned char *)obj + off + (TD_OLDMOVE - AA_LOCATION), &old);
        readU8((unsigned char *)obj + off + (TD_PENDINGMOVE - AA_LOCATION), &pend);
        readU8((unsigned char *)obj + off + (AA_PHYSICS - AA_LOCATION), &phys);
        float vel[3] = {0, 0, 0};
        sehRead((unsigned char *)obj + off + (AA_VELOCITY - AA_LOCATION), vel, 12);
        logf("pawncal: obj[%d] %p loc+0x%X (%.1f %.1f %.1f) "
             "vel(%.1f %.1f %.1f) move %u/%u/%u phys %u",
             i, obj, off, pos[0], pos[1], pos[2], vel[0], vel[1], vel[2],
             mv, old, pend, phys);
    }
    logf("pawncal: %d objects stand where the camera does", hits);

    // And what carries the ini's own gravity value?
    int gh = 0;
    for (int i = 0; i < g_gobjectCount && gh < 6; i++) {
        void *obj = nullptr;
        if (!readPtr((unsigned char *)g_gobjects + i * sizeof(void *), &obj))
            break;
        if (!obj) continue;
        unsigned char buf[0x1000];
        const size_t n = sehRead(obj, buf, 0x1000) ? 0x1000 : 0;
        if (!n) continue;
        for (size_t off = 0x40; off + 4 <= n; off += 4) {
            float v = 0.0f;
            memcpy(&v, buf + off, 4);
            if (fabsf(v - MEDGE_INI_GRAVITY) < 0.01f) {
                float wg = 0.0f;
                memcpy(&wg, buf + off - 4, 4);
                logf("pawncal: obj[%d] %p has %.1f at +0x%X (prev float %.1f)",
                     i, obj, v, (unsigned)off, wg);
                gh++;
                break;
            }
        }
    }
    logf("pawncal: %d objects carry DefaultGravityZ", gh);
}

// BEHAVIOURAL CALIBRATION. Position alone cannot say which of the objects
// standing where the player stands IS the pawn, and the offsets from the
// third-party dump demonstrably do not fit this build (its AActor::Location is
// 0xE8; here nothing matches there). So identify the field by what it DOES:
// snapshot the candidates while she stands, jump, snapshot again, and report
// every byte that went from one plausible move enum to another. MOVE_Walking
// is 1 and MOVE_Falling is 2, so the pawn's MovementState announces itself.
#define CALIB_MAX 64
#define CALIB_SNAP 0x1000
static void *g_calibObjs[CALIB_MAX];
static size_t g_calibLen[CALIB_MAX];      // how much of each we actually read
static unsigned char *g_calibSnap = nullptr;
static int g_calibCount = 0;

static void pawnSnapshot() {
    if (!g_gobjectsArray) findGObjects();
    gobjectsRefresh();
    if (!g_gobjects || !g_lastCamValid) { logf("pawnsnap: not ready"); return; }
    if (!g_calibSnap)
        g_calibSnap = (unsigned char *)malloc(CALIB_MAX * CALIB_SNAP);
    if (!g_calibSnap) return;
    g_calibCount = 0;
    int found = 0;
    bool capped = false;
    for (int i = 0; i < g_gobjectCount; i++) {
        if (g_calibCount >= CALIB_MAX) { capped = true; break; }
        void *obj = nullptr;
        if (!readPtr((unsigned char *)g_gobjects + i * sizeof(void *), &obj))
            break;
        if (!obj) continue;
        int off = 0;
        if (!findLocationOffset(obj, &off)) continue;
        found++;
        // Use the SAME falling-back read the detector used. A fixed 0x1000
        // here is what lost the first calibration: 22 objects passed detection
        // (which falls back to 0x80) and only 4 survived capture, because an
        // object shorter than 4 KB, or one whose next page is unmapped, faults
        // the whole read. The pawn was not among the four.
        const size_t n = snapshotObject(obj, g_calibSnap + g_calibCount * CALIB_SNAP,
                                        CALIB_SNAP);
        if (!n) continue;
        g_calibLen[g_calibCount] = n;
        g_calibObjs[g_calibCount++] = obj;
    }
    // Say so when the cap bit. A capped sweep did not find everything, it
    // STOPPED - reading "64 matched" as "64 exist" is the trap the dead-ends
    // table already records once.
    logf("pawnsnap: %d matched (xy<%.0f, dz %.0f..%.0f), captured %d%s, "
         "cam %.1f %.1f %.1f",
         found, g_calibXY, g_calibZLo, g_calibZHi, g_calibCount,
         capped ? " [CAPPED - widen CALIB_MAX or tighten the bounds]" : "",
         g_lastCamPos[0], g_lastCamPos[1], g_lastCamPos[2]);
}

// `want` is the transition to hunt for: pass the two EMovement values and only
// that exact change is reported. 0 for either means "any plausible move byte",
// which is the noisy mode - it produced 26 hits and no answer the first time.
static void pawnDiff(int wantFrom, int wantTo) {
    if (!g_calibCount || !g_calibSnap) { logf("pawndiff: snapshot first"); return; }
    logf("pawndiff: comparing %d candidates for %d -> %d",
         g_calibCount, wantFrom, wantTo);
    int reported = 0, exact = 0;
    for (int c = 0; c < g_calibCount; c++) {
        unsigned char now[CALIB_SNAP];
        const size_t n = g_calibLen[c];
        if (!sehRead(g_calibObjs[c], now, n)) continue;
        const unsigned char *was = g_calibSnap + c * CALIB_SNAP;
        for (size_t off = 0; off < n && reported < 80; off++) {
            const unsigned char a = was[off], b = now[off];
            if (a == b) continue;
            if (wantFrom > 0 && wantTo > 0) {
                if (a != wantFrom || b != wantTo) continue;
                exact++;
            } else {
                if (a >= EMOVEMENT_COUNT || b >= EMOVEMENT_COUNT) continue;
                if (a == 0 && b == 0) continue;
            }
            logf("pawndiff: obj[%d] %p +0x%X : %u -> %u",
                 c, g_calibObjs[c], (unsigned)off, a, b);
            reported++;
        }
    }
    logf("pawndiff: %d reported (%d exact %d->%d)",
         reported, exact, wantFrom, wantTo);
}

// The decisive filter.
//
// A bare byte diff drowns in float noise: any byte of a moving float that
// happens to land in 0..94 looks like a move enum, and the first run reported
// 80 such hits with nothing to choose between them. But the move fields are
// not one byte, they are a GROUP - MovementState with OldMovementState and
// PendingMovementState beside it - and they are coherent: when the move
// changes, the OLD field takes the value the current field just had.
//
// So look for a pair of nearby bytes (K, J) where K changed and J now holds
// exactly what K held before. A float's bytes do not do that. This is the same
// self-validating-structure trick that found GObjects, applied to a state
// machine instead of a table.
static void pawnStateHunt() {
    if (!g_calibCount || !g_calibSnap) { logf("pawnstate: snapshot first"); return; }
    logf("pawnstate: %d candidates", g_calibCount);
    int hits = 0;
    for (int c = 0; c < g_calibCount; c++) {
        unsigned char now[CALIB_SNAP];
        const size_t n = g_calibLen[c];
        if (!sehRead(g_calibObjs[c], now, n)) continue;
        const unsigned char *was = g_calibSnap + c * CALIB_SNAP;
        for (size_t k = 0; k < n && hits < 40; k++) {
            const unsigned char a = was[k], b = now[k];
            if (a == b || a >= EMOVEMENT_COUNT || b >= EMOVEMENT_COUNT) continue;
            // Did a neighbour just inherit K's previous value?
            for (int d = -4; d <= 4; d++) {
                if (!d) continue;
                const size_t j = k + d;
                if (j >= n) continue;
                if (now[j] != a || was[j] == a) continue;
                if (now[j] >= EMOVEMENT_COUNT) continue;
                logf("pawnstate: obj[%d] %p move+0x%X %u->%u, old+0x%X %u->%u",
                     c, g_calibObjs[c], (unsigned)k, a, b,
                     (unsigned)j, was[j], now[j]);
                hits++;
                break;
            }
        }
    }
    logf("pawnstate: %d coherent move-field groups", hits);
}

// Why does acceptPawn refuse? Sweep the whole table for anything whose MOVE
// FIELDS look right - three plausible enum bytes at the documented offsets
// with a sane Moves array beside them - and print every other field acceptPawn
// tests, so the failing condition names itself instead of being guessed at.
static void pawnWhy() {
    if (!g_gobjectsArray) findGObjects();
    gobjectsRefresh();
    if (!g_gobjects) { logf("pawnwhy: no table"); return; }
    int hits = 0;
    for (int i = 0; i < g_gobjectCount && hits < 12; i++) {
        void *obj = nullptr;
        if (!readPtr((unsigned char *)g_gobjects + i * sizeof(void *), &obj))
            break;
        if (!obj) continue;
        unsigned char *o = (unsigned char *)obj;
        unsigned char mv = 0, old = 0, pend = 0, phys = 0;
        if (!readU8(o + TD_MOVESTATE, &mv) || mv >= EMOVEMENT_COUNT) continue;
        if (!readU8(o + TD_OLDMOVE, &old) || old >= EMOVEMENT_COUNT) continue;
        if (!readU8(o + TD_PENDINGMOVE, &pend) || pend >= EMOVEMENT_COUNT) continue;
        // A live pawn is always IN some move; MOVE_None is the tell of a
        // coincidental match.
        if (mv == 0) continue;
        // AND it must stand where the camera says. Either test alone drowns:
        // the state bytes alone match hundreds of class-default templates
        // (Location 0,0,0), and the position alone matches every mesh
        // component hanging off her. Together they are decisive. The offset
        // the position was found at is the answer we are here for - it says
        // whether AActor::Location really is at the dump's 0xE8.
        int locOff = -1;
        if (!findLocationOffset(obj, &locOff)) continue;
        // Moves is REPORTED, not required. Requiring a sane TArray at the
        // dump's 0x5FC rejected the object the behavioural calibration had
        // already proved was the pawn - so the dump is right about the state
        // bytes and wrong about this, and the two must not be coupled.
        void *movesData = nullptr;
        int movesCount = 0, movesMax = 0;
        readPtr(o + TD_MOVES, &movesData);
        readI32(o + TD_MOVES + 4, &movesCount);
        readI32(o + TD_MOVES + 8, &movesMax);

        readU8(o + AA_PHYSICS, &phys);
        float pos[3] = {0, 0, 0}, vel[3] = {0, 0, 0};
        const bool gotPos = sehRead(o + AA_LOCATION, pos, 12);
        const bool gotVel = sehRead(o + AA_VELOCITY, vel, 12);
        hits++;
        float lpos[3] = {0, 0, 0};
        sehRead(o + locOff, lpos, 12);
        logf("pawnwhy: obj[%d] %p move %u old %u pend %u phys %u moves %d/%d",
             i, obj, mv, old, pend, phys, movesCount, movesMax);
        logf("pawnwhy:   CAMERA-AGREEING POSITION AT +0x%X (%.1f %.1f %.1f)%s",
             locOff, lpos[0], lpos[1], lpos[2],
             locOff == AA_LOCATION ? "  == the dump's AActor::Location" : "");
        logf("pawnwhy:   loc+0x%X (%.1f %.1f %.1f)%s  vel+0x%X (%.1f %.1f %.1f)%s",
             AA_LOCATION, pos[0], pos[1], pos[2], gotPos ? "" : " UNREADABLE",
             AA_VELOCITY, vel[0], vel[1], vel[2], gotVel ? "" : " UNREADABLE");
        logf("pawnwhy:   camera (%.1f %.1f %.1f) -> dx %.1f dy %.1f dz %.1f",
             g_lastCamPos[0], g_lastCamPos[1], g_lastCamPos[2],
             pos[0] - g_lastCamPos[0], pos[1] - g_lastCamPos[1],
             g_lastCamPos[2] - pos[2]);
    }
    logf("pawnwhy: %d objects have plausible move fields", hits);
}

static void pawnAcquire() {
    if (!g_gobjectsArray) findGObjects();
    gobjectsRefresh();
    if (!g_gobjects) return;

    void *pawn = nullptr;
    for (int i = 0; i < g_gobjectCount; i++) {
        void *obj = nullptr;
        if (!readPtr((unsigned char *)g_gobjects + i * sizeof(void *), &obj))
            break;
        if (!obj) continue;
        if (acceptPawn(obj)) { pawn = obj; break; }
    }
    if (pawn) {
        // Cache the CONTROLLER, not the pawn: it outlives her, so a death and
        // respawn costs one dereference instead of another scan.
        for (int i = 0; i < g_gobjectCount; i++) {
            void *obj = nullptr;
            if (!readPtr((unsigned char *)g_gobjects + i * sizeof(void *), &obj))
                break;
            if (!obj) continue;
            void *ack = nullptr;
            if (readPtr((unsigned char *)obj + AC_ACKPAWN, &ack) && ack == pawn) {
                g_playerController = obj;
                break;
            }
        }
        g_pawnFallback = pawn;
        logf("pawn: acquired %p, controller %p", pawn, g_playerController);
    }
    if (!g_worldInfo) {
        for (int i = 0; i < g_gobjectCount; i++) {
            void *obj = nullptr;
            if (!readPtr((unsigned char *)g_gobjects + i * sizeof(void *), &obj))
                break;
            if (!obj) continue;
            if (acceptWorldInfo(obj)) {
                g_worldInfo = obj;
                float wg = 0.0f;
                readF32((unsigned char *)obj + AW_WORLDGRAVITY, &wg);
                logf("pawn: WorldInfo at %p, WorldGravityZ %.2f (ini says %.0f)",
                     obj, wg, MEDGE_INI_GRAVITY);
                break;
            }
        }
    }
}

// ON: acceptPawn's offsets are pinned against this build (doc/medge_sdk.md).
// The cost is bounded - once the controller is cached, a frame is a couple of
// dereferences, and the ~80k-object re-acquisition sweep is throttled to once
// per 120 frames and only runs while there is no valid pawn (menus, loads).
// `pawn` forces a re-acquire and dumps diagnostics.
static bool g_pawnEnabled = true;

static void pawnTick() {
    g_pawnValid = false;
    if (!g_pawnEnabled) return;
    void *pawn = nullptr;
    if (g_playerController)
        readPtr((unsigned char *)g_playerController + AC_ACKPAWN, &pawn);

    // Keep a pawn we already hold even if the oracle blinks.
    //
    // Once the controller is cached, the pawn it points at is the pawn; the
    // camera check exists to FIND it, not to re-litigate it every frame. Using
    // acceptPawn as a per-frame gate meant one bad frame - an unusual eye
    // height, a camera solve that missed - dropped the cached pointer AND
    // started the re-acquisition throttle, costing 2 s of data for a 1-frame
    // blink. Here the held pawn only has to still look like a pawn: three
    // coherent move bytes and a finite position. The full oracle applies to
    // candidates during a scan.
    if (pawn && !pawnStillLooksRight(pawn)) pawn = nullptr;

    // No controller acked her (authored maps) - take the scan's own pawn
    // directly. Placed BEFORE the rescan so a permanently-null controller
    // does not trigger a full GObjects sweep every 20 frames forever.
    if (!pawn && g_pawnFallback && pawnStillLooksRight(g_pawnFallback))
        pawn = g_pawnFallback;

    // Re-acquire on loss, but not every frame: a menu or a load screen has no
    // pawn at all, and a full table scan per frame would hitch visibly. The
    // throttle is short enough now that a genuine loss costs a third of a
    // second, not two seconds.
    if (!pawn && g_frame - g_pawnScanFrame > 20) {
        g_pawnScanFrame = (int)g_frame;
        g_playerController = nullptr;
        pawnAcquire();
        if (g_playerController)
            readPtr((unsigned char *)g_playerController + AC_ACKPAWN, &pawn);
    }

    if (pawn) {
        unsigned char *o = (unsigned char *)pawn;
        float pos[3], vel[3];
        // The look: the controller's Rotation (what the mouse writes) and the
        // pawn's own yaw. FRotator is three int32 in 65536ths of a turn.
        g_rotValid = false;
        {
            int32_t prot[3], crot[3];
            if (sehRead(o + AA_ROTATION, prot, 12)) {
                g_pawnYawDeg = (float)prot[1] * (360.0f / 65536.0f);
                if (g_playerController &&
                    sehRead((unsigned char *)g_playerController + AA_ROTATION, crot, 12)) {
                    g_ctrlYawDeg = (float)crot[1] * (360.0f / 65536.0f);
                    g_ctrlPitchDeg = (float)crot[0] * (360.0f / 65536.0f);
                    if (g_ctrlPitchDeg > 180.0f) g_ctrlPitchDeg -= 360.0f;
                    g_rotValid = true;
                }
            }
        }
        if (sehRead(o + AA_LOCATION, pos, 12) && sehRead(o + AA_VELOCITY, vel, 12) &&
            readU8(o + TD_MOVESTATE, &g_moveState) &&
            readU8(o + TD_OLDMOVE, &g_oldMoveState) &&
            readU8(o + TD_PENDINGMOVE, &g_pendingMoveState) &&
            readU8(o + TD_WALKINGSTATE, &g_walkingState) &&
            readU8(o + AA_PHYSICS, &g_pawnPhysics)) {
            memcpy(g_pawnPos, pos, 12);
            memcpy(g_pawnVel, vel, 12);
            g_pawnValid = true;
        }
    }
    g_pawnObj = g_pawnValid ? pawn : nullptr;
    if (!g_pawnValid) {
        g_moveState = g_oldMoveState = g_pendingMoveState = 0xFF;
        g_walkingState = g_pawnPhysics = 0xFF;
    }

    g_worldValid = false;
    if (g_worldInfo) {
        float wg = 0.0f;
        if (readF32((unsigned char *)g_worldInfo + AW_WORLDGRAVITY, &wg) &&
            finiteF(wg)) {
            g_worldGravityZ = wg;
            g_worldValid = true;
        }
    }
}

// Place the pawn: Location, zero Velocity, optionally yaw. The A/B anchor -
// an open-loop input script only measures the engine if it starts from the
// same pose every run, and walking there accumulates drift the comparison
// then misreads as physics. This is the only game-state write in the hook.
//
// The pawn pointer comes from the same controller->AcknowledgedPawn path
// pawnTick trusts, gated by the same plausibility check, so "no pawn" (menu,
// load screen, cutscene) refuses rather than scribbling. Velocity is zeroed
// so the pose is a STANDING start; the physics mode is left alone - one tick
// of PHYS_Walking snaps her to the floor under the new position, which is
// the settle the harness wants anyway.
static void pawnSet(float x, float y, float z, bool haveYaw, float yawDeg) {
    void *pawn = nullptr;
    if (g_playerController)
        readPtr((unsigned char *)g_playerController + AC_ACKPAWN, &pawn);
    if (pawn && !pawnStillLooksRight(pawn)) pawn = nullptr;
    // Same fallback pawnTick uses: authored maps have a pawn no controller
    // ever acks.
    if (!pawn && g_pawnFallback && pawnStillLooksRight(g_pawnFallback))
        pawn = g_pawnFallback;
    if (!pawn) {
        logf("pawnset: no valid pawn (menu or load screen?)");
        return;
    }
    unsigned char *o = (unsigned char *)pawn;
    float pos[3] = {x, y, z};
    float vel[3] = {0, 0, 0};
    if (!sehWrite(o + AA_LOCATION, pos, 12) ||
        !sehWrite(o + AA_VELOCITY, vel, 12)) {
        logf("pawnset: write failed");
        return;
    }
    if (haveYaw) {
        // Only yaw. Writing pitch would fight the controller, and roll is
        // never the pawn's to carry.
        int yawUnits = (int)(yawDeg * (65536.0f / 360.0f));
        if (!sehWrite(o + AA_ROTATION + 4, &yawUnits, 4))
            logf("pawnset: yaw write failed");
        else {
            int back = 0;
            readI32(o + AA_ROTATION + 4, &back);
            if (back != yawUnits)
                logf("pawnset: yaw readback %d != %d (controller overrode?)",
                     back, yawUnits);
        }
    }
    logf("pawnset: pawn %p -> (%.1f, %.1f, %.1f)%s", pawn, x, y, z,
         haveYaw ? " +yaw" : "");
}

// --- names, meshes, animations and sounds -------------------------------------
//
// Two more things the pawn's own state can say, and the recording could not:
// WHICH ANIMATION the first-person body is playing, and WHICH SOUND the game
// just started. Both are read the same way the move state is - off UE3's own
// objects - and both need one thing the pawn readers never did: names. A move
// is a byte from an enum; an animation is an FName on an AnimNodeSequence and a
// sound is a SoundCue object, and an FName is an index into FName::Names.
//
// So the name table is found the way GObjects was: a TArray<FNameEntry*> in
// .data whose entries know their own slot numbers (FNameEntry::Index at +8,
// after the QWORD Flags). Then every pointer this code follows is checked by
// structure before it is trusted - the target's ObjectInternalInteger must
// index GObjects back to the target itself - and by name, its UClass's Name
// against the class families the build's script packages declare
// (tools/retail/native.py --layout, Engine.u + TdGame.u: 74 AnimNodeBlendBase
// subclasses, 4 AnimNodeSequence ones, and their offsets below).
//
// The animation walk: TdPawn.Mesh1p (0x5D8) and Mesh3p are the first- and
// third-person SkeletalMeshComponents; each has an AnimTree at Animations,
// and the tree is AnimNodeBlendBase nodes with Children arrays of
// FAnimBlendChild {Name, Anim, Weight, TotalWeight ...} down to
// AnimNodeSequence leaves carrying AnimSeqName / CurrentTime / Rate. A leaf's
// NodeTotalWeight is how much of the final pose it is; the three heaviest
// per mesh go into the ring by NAME, so a reader needs no name table.
//
// The sounds: the AudioDevice (the one live object of a class named
// *AudioDevice, found once) keeps every playing AudioComponent in its
// AudioComponents array. Diffing that array frame to frame gives start and
// stop events; each carries the SoundCue's name, whether the component's
// Owner is the pawn, and its Location.
//
// Offsets: UObject::Name 0x2C and Class 0x34 are the UE3 UObject of this era
// (sizeof 0x3C, which tools/retail/native.py already assumes). The rest come
// from the script layouts, anchored on the pawn's verified MovementState
// (0x4FE); where a chain crosses a package (Core's Component under Engine's
// ActorComponent) the base is checked at runtime by scanning a few words
// either side for the pointer whose target has the expected class, and the
// offset found is logged.

// UObject
#define UO_NAME           0x02C    // FName {Index, Number}
#define UO_CLASS          0x034    // UClass*
// FNameEntry: QWORD Flags, INT Index, FNameEntry *HashNext, TCHAR Name[]
#define NE_INDEX          0x008
#define NE_NAME           0x010
// ATdPawn (anchored layout, MovementState 0x4FE)
#define TD_MESH1P         0x5D8    // USkeletalMeshComponent*
#define TD_MESH3P         0x5DC
// USkeletalMeshComponent (Component 0x3C+0xC, ActorComponent, Primitive, Mesh)
#define SMC_ANIMATIONS    0x218    // UAnimNode*, the tree INSTANCE (the layout said
                                   // 0x200; measured 0x218, with AnimTreeTemplate - the
                                   // never-ticked asset - at 0x214 just before it)
// UAnimNode / UAnimNodeBlendBase / UAnimNodeSequence (AnimNode block at 0x3C)
#define AN_TOTALWEIGHT    0x098
#define ANB_CHILDREN      0x0BC    // TArray<FAnimBlendChild>
#define ABC_SIZE          0x040
#define ABC_ANIM          0x008
#define ANS_SEQNAME       0x0BC    // FName
#define ANS_RATE          0x0C4
#define ANS_FLAGS         0x0C8    // bit0 bPlaying, bit1 bLooping
#define ANS_CURTIME       0x110    // the layout said 0x10C; measured 0x110, PreviousTime
                                   // one frame behind it at 0x114 and AnimSeq at 0x118
// UActorComponent / UAudioComponent / UAudioDevice
#define ACOMP_OWNER       0x04C
#define AUC_SOUNDCUE      0x058
#define AUC_LOCATION      0x118    // the layout said 0xAC; measured 0x118 (level coordinates)
#define AUD_COMPONENTS    0x048    // TArray<UAudioComponent*>; the layout said 0x44, the scan 0x48

#define ANIM_NAME_LEN     28
#define ANIM_TOP          3
#define SOUND_NAME_LEN    40

static unsigned char *g_gnamesArray = nullptr;
static bool g_namesWide = true;
static bool g_namesTried = false;
static int  g_neIndex = NE_INDEX;      // where an entry keeps its slot number
static int  g_neName = NE_NAME;        // and where its characters start

static bool gnamesRefresh(void ***data, int *count) {
    if (!g_gnamesArray) return false;
    void *d = nullptr;
    int n = 0;
    if (!readPtr(g_gnamesArray, &d) || !readI32(g_gnamesArray + 4, &n)) return false;
    if (!d || n < 1 || n > 4000000) return false;
    *data = (void **)d;
    *count = n;
    return true;
}

// The slot number may sit at +8 (QWORD Flags first, the UT3-era layout), at
// +4 or at +0; whichever 64 consecutive entries agree on is the layout, and
// the characters follow the header's last pointer.
static bool looksLikeGNames(unsigned char *p, int *indexOff) {
    void *data = nullptr;
    int count = 0, maxn = 0;
    if (!readPtr(p, &data) || !readI32(p + 4, &count) || !readI32(p + 8, &maxn))
        return false;
    if (!data || count < 1000 || count > 2000000 || maxn < count || maxn > 4000000)
        return false;
    static const int tryOff[] = {8, 4, 0, 12};
    for (int k = 0; k < 4; k++) {
        int checked = 0;
        bool ok = true;
        for (int i = 0; i < 4096 && checked < 64; i++) {
            void *e = nullptr;
            if (!readPtr((unsigned char *)data + i * sizeof(void *), &e)) { ok = false; break; }
            if (!e) continue;
            int self = 0;
            if (!readI32((unsigned char *)e + tryOff[k], &self) || self != i) { ok = false; break; }
            checked++;
        }
        if (ok && checked >= 16) { *indexOff = tryOff[k]; return true; }
    }
    return false;
}

// The entry's characters, ANSI or wide, read in small pieces so a name that
// ends a page cannot fault the whole read.
static bool nameEntryStr(void *entry, char *out, size_t cap) {
    size_t n = 0;
    for (int chunk = 0; chunk < 8 && n + 1 < cap; chunk++) {
        unsigned char raw[16];
        if (!sehRead((unsigned char *)entry + g_neName + chunk * 16, raw, 16)) break;
        if (g_namesWide) {
            for (int i = 0; i + 1 < 16; i += 2) {
                if (!raw[i] && !raw[i + 1]) { out[n] = 0; return n > 0; }
                if (n + 1 < cap) out[n++] = raw[i + 1] ? '?' : (char)raw[i];
            }
        } else {
            for (int i = 0; i < 16; i++) {
                if (!raw[i]) { out[n] = 0; return n > 0; }
                if (n + 1 < cap) out[n++] = (char)raw[i];
            }
        }
    }
    out[n] = 0;
    return n > 0;
}

static void findGNames() {
    if (g_namesTried) return;
    g_namesTried = true;
    unsigned char *lo = nullptr, *hi = nullptr;
    if (!gameDataRange(&lo, &hi)) return;
    for (unsigned char *p = lo; p + 12 <= hi; p += 4) {
        if (p == g_gobjectsArray) continue;
        int indexOff = 0;
        if (!looksLikeGNames(p, &indexOff)) continue;
        g_gnamesArray = p;
        g_neIndex = indexOff;
        // Entry 0 is "None" in every UE3 build: find where its characters
        // start (the first "N" after the index, ANSI or wide) and how wide.
        void **data = nullptr;
        int count = 0;
        char probe[16] = {0};
        if (gnamesRefresh(&data, &count)) {
            void *e0 = nullptr;
            unsigned char raw[32] = {0};
            if (readPtr(data, &e0) && e0 && sehRead(e0, raw, 32)) {
                g_neName = -1;
                for (int off = indexOff + 4; off + 8 <= 32; off += 4) {
                    if (raw[off] == 'N' && raw[off + 1] == 0 && raw[off + 2] == 'o' && raw[off + 3] == 0) {
                        g_neName = off; g_namesWide = true; break;
                    }
                    if (raw[off] == 'N' && raw[off + 1] == 'o' && raw[off + 2] == 'n' && raw[off + 3] == 'e') {
                        g_neName = off; g_namesWide = false; break;
                    }
                }
                if (g_neName < 0) {
                    logf("names: candidate at %p has no \"None\" at entry 0 (bytes %02X %02X %02X %02X ...)",
                         p, raw[indexOff + 4], raw[indexOff + 5], raw[indexOff + 6], raw[indexOff + 7]);
                    g_gnamesArray = nullptr;
                    g_neName = NE_NAME;
                    continue;
                }
                nameEntryStr(e0, probe, sizeof(probe));
            }
        }
        logf("names: GNames array at %p (%d names, index at +%d, chars at +%d, %s), entry 0 \"%s\"",
             p, count, g_neIndex, g_neName, g_namesWide ? "wide" : "ansi", probe);
        return;
    }
    logf("names: GNames not found in .data (%p..%p)", lo, hi);
}

static bool nameStr(int index, char *out, size_t cap) {
    void **data = nullptr;
    int count = 0;
    if (!gnamesRefresh(&data, &count) || index < 0 || index >= count) return false;
    void *e = nullptr;
    if (!readPtr(data + index, &e) || !e) return false;
    return nameEntryStr(e, out, cap);
}

// A pointer is a live UObject when the table slot it names points back at it.
static bool objIsObject(void *obj) {
    if (!obj || !g_gobjects) return false;
    int idx = 0;
    if (!readI32((unsigned char *)obj + UO_INDEX, &idx)) return false;
    if (idx < 0 || idx >= g_gobjectCount) return false;
    void *back = nullptr;
    if (!readPtr((unsigned char *)g_gobjects + idx * sizeof(void *), &back)) return false;
    return back == obj;
}

static bool objName(void *obj, char *out, size_t cap) {
    int idx = 0;
    if (!readI32((unsigned char *)obj + UO_NAME, &idx)) return false;
    return nameStr(idx, out, cap);
}

static bool objClassName(void *obj, char *out, size_t cap) {
    void *cls = nullptr;
    if (!readPtr((unsigned char *)obj + UO_CLASS, &cls) || !cls) return false;
    return objName(cls, out, cap);
}

// The build's AnimNode families, from Engine.u and TdGame.u (see the header
// comment). Kind 1 has Children to descend, kind 2 is a playing sequence,
// kind 3 is anything else (a leaf with no animation of its own, or a class
// this table does not know - logged once so the table can grow).
static const char *const kBlendNodeClasses[] = {
    "AnimNodeAimOffset", "AnimNodeBlend", "AnimNodeBlendBase", "AnimNodeBlendByBase",
    "AnimNodeBlendByPhysics", "AnimNodeBlendByPosture", "AnimNodeBlendBySpeed",
    "AnimNodeBlendDirectional", "AnimNodeBlendList", "AnimNodeBlendMultiBone",
    "AnimNodeBlendPerBone", "AnimNodeCrossfader", "AnimNodeMirror", "AnimNodePlayCustomAnim",
    "AnimNodeRandom", "AnimNodeScalePlayRate", "AnimNodeScaleRateBySpeed", "AnimNodeSlot",
    "AnimNodeSynch", "AnimTree", "TdAnimNodeAgainstWallState", "TdAnimNodeAiAnimationState",
    "TdAnimNodeAimNodeState", "TdAnimNodeAimOffset", "TdAnimNodeAimState",
    "TdAnimNodeAllowAimOffsetSwitch", "TdAnimNodeAnimationPoseOffset", "TdAnimNodeBalanceBlend",
    "TdAnimNodeBalanceWalk", "TdAnimNodeBlendBoneArmed", "TdAnimNodeBlendByGender",
    "TdAnimNodeBlendBySpeed", "TdAnimNodeBlendDirectional", "TdAnimNodeBlendList",
    "TdAnimNodeCinematicSwitch", "TdAnimNodeClimb", "TdAnimNodeCoverDirection",
    "TdAnimNodeCoverState", "TdAnimNodeCoverType", "TdAnimNodeCustomBlend", "TdAnimNodeDirBone",
    "TdAnimNodeDirBoneAI", "TdAnimNodeDirSwitch", "TdAnimNodeGrabAimOffset", "TdAnimNodeGrabSlope",
    "TdAnimNodeGrabTransfer", "TdAnimNodeGrabbing", "TdAnimNodeIKEffectorController",
    "TdAnimNodeIdleAnimationSwitch", "TdAnimNodeIgnoreTransforms", "TdAnimNodeInAir",
    "TdAnimNodeIsCrouchSittingSwitch", "TdAnimNodeIsInCoverSwitch", "TdAnimNodeIsReloadingSwitch",
    "TdAnimNodeIsWalkingSwitch", "TdAnimNodeLandOffset", "TdAnimNodeLean", "TdAnimNodeLedgeWalk",
    "TdAnimNodeMeleeIdleAnimation", "TdAnimNodeMovementState", "TdAnimNodePawnRotation",
    "TdAnimNodePoseOffset", "TdAnimNodeRandom", "TdAnimNodeSequencer", "TdAnimNodeSlot",
    "TdAnimNodeState", "TdAnimNodeSwing", "TdAnimNodeSwitch", "TdAnimNodeSynch", "TdAnimNodeTurn",
    "TdAnimNodeWalkingState", "TdAnimNodeWeaponPoseOffset", "TdAnimNodeWeaponState",
    "TdAnimNodeWeaponTypeState",
};
static const char *const kSequenceNodeClasses[] = {
    "AnimNodeSequence", "AnimNodeSequenceBlendBase", "AnimNodeSequenceBlendByAim",
    "TdAnimNodeSequence",
};

struct ClassKind { void *cls; unsigned char kind; };
static ClassKind g_classKinds[512];
static int g_classKindCount = 0;

static unsigned char classKind(void *cls) {
    for (int i = 0; i < g_classKindCount; i++)
        if (g_classKinds[i].cls == cls) return g_classKinds[i].kind;
    char name[64] = {0};
    unsigned char kind = 3;
    if (objName(cls, name, sizeof(name))) {
        for (size_t i = 0; i < sizeof(kBlendNodeClasses) / sizeof(kBlendNodeClasses[0]); i++)
            if (strcmp(name, kBlendNodeClasses[i]) == 0) { kind = 1; break; }
        if (kind == 3)
            for (size_t i = 0; i < sizeof(kSequenceNodeClasses) / sizeof(kSequenceNodeClasses[0]); i++)
                if (strcmp(name, kSequenceNodeClasses[i]) == 0) { kind = 2; break; }
        if (kind == 3 && strstr(name, "AnimNode"))
            logf("anim: node class \"%s\" is not in the tables; treated as a leaf", name);
    }
    if (g_classKindCount < 512) {
        g_classKinds[g_classKindCount].cls = cls;
        g_classKinds[g_classKindCount].kind = kind;
        g_classKindCount++;
    }
    return kind;
}

struct AnimHit { void *node; int nameIdx; float time; float weight; float rate; unsigned char flags; };

// The tree is a DAG - a slot node hangs under several parents - so a leaf
// can be reached more than once; it keeps one entry, at its heaviest edge.
static void animInsert(AnimHit *top, const AnimHit &h) {
    for (int i = 0; i < ANIM_TOP; i++) {
        if (top[i].node == h.node) {
            if (h.weight > top[i].weight) top[i] = h;
            return;
        }
    }
    for (int i = 0; i < ANIM_TOP; i++) {
        if (h.weight > top[i].weight) {
            for (int j = ANIM_TOP - 1; j > i; j--) top[j] = top[j - 1];
            top[i] = h;
            return;
        }
    }
}

// Ranked by the leaf's own NodeTotalWeight (0x98: 0.99998 on `runfwd` while
// she runs, 0 on the rest - the frozen TEMPLATE tree reads 1.0 on every
// node, which is what made this look wrong before the instance was found).
// The edge's FAnimBlendChild::TotalWeight is NOT used, even to prune: under
// a BlendPerBone it is a bone mask (both children 1.0), and on the third-
// person tree the locomotion branch hangs under an edge that reads 0 while
// its leaves carry full node weight - pruning on it lost `runfwd` there.
// Two leaves are left out by name: the `notifierdummy*` clips, which play
// at full weight on their own branch and draw nothing (DICE's carriers for
// the footstep and breathing notifies - the `Run`/`Walk`/`Crouch` cues the
// sound ring sees the pawn own), and `TestAnim`, a placeholder parked at
// 0.98 under a bone mask on the third-person tree for the whole session.
static bool animLeafHidden(int nameIdx) {
    char nm[24] = {0};
    if (!nameStr(nameIdx, nm, sizeof(nm))) return false;
    return strncmp(nm, "notifierdummy", 13) == 0 || strcmp(nm, "TestAnim") == 0;
}

static void animWalk(void *node, int depth, float edgeTotal, AnimHit *top, int &budget) {
    if (!node || depth > 24 || --budget < 0) return;
    if (!objIsObject(node)) return;
    void *cls = nullptr;
    if (!readPtr((unsigned char *)node + UO_CLASS, &cls) || !cls) return;
    const unsigned char kind = classKind(cls);
    unsigned char *o = (unsigned char *)node;
    if (kind == 2) {
        AnimHit h;
        h.node = node;
        float w = 0.0f;
        if (!readF32(o + AN_TOTALWEIGHT, &w) || !finiteF(w) || w <= 0.005f || w > 1.01f) return;
        if (!readI32(o + ANS_SEQNAME, &h.nameIdx)) return;
        if (animLeafHidden(h.nameIdx)) return;
        if (!readF32(o + ANS_CURTIME, &h.time) || !finiteF(h.time)) h.time = -1.0f;
        if (!readF32(o + ANS_RATE, &h.rate) || !finiteF(h.rate)) h.rate = 0.0f;
        if (!readU8(o + ANS_FLAGS, &h.flags)) h.flags = 0;
        h.weight = w;
        animInsert(top, h);
    } else if (kind == 1) {
        void *data = nullptr;
        int count = 0;
        if (!readPtr(o + ANB_CHILDREN, &data) || !readI32(o + ANB_CHILDREN + 4, &count)) return;
        if (!data || count <= 0 || count > 256) return;
        for (int i = 0; i < count; i++) {
            void *child = nullptr;
            float tot = 0.0f;
            if (!readPtr((unsigned char *)data + i * ABC_SIZE + ABC_ANIM, &child)) return;
            if (!readF32((unsigned char *)data + i * ABC_SIZE + ABC_ANIM + 8, &tot)) tot = 0.0f;
            if (child) animWalk(child, depth + 1, tot, top, budget);
        }
    }
}

// The mesh's tree root. The offset comes from a layout whose base crosses
// packages, so it is confirmed once by finding the pointer whose target is
// a blend node (the AnimTree), scanning a few words either side if need be.
static int g_smcAnimOff = 0;
static void *animRoot(void *mesh) {
    if (!objIsObject(mesh)) return nullptr;
    if (g_smcAnimOff) {
        void *root = nullptr;
        if (readPtr((unsigned char *)mesh + g_smcAnimOff, &root) && root && objIsObject(root)) {
            void *cls = nullptr;
            if (readPtr((unsigned char *)root + UO_CLASS, &cls) && cls && classKind(cls) == 1)
                return root;
        }
        return nullptr;
    }
    // The template and the instance are adjacent AnimTrees, template first,
    // and the template never ticks: a scan that took the first AnimTree it
    // met read a tree frozen at every child weight 1.0 - so a candidate only
    // counts when the word BEFORE it is an AnimTree too.
    for (int d = 0; d <= 0x20; d += 4) {
        for (int sgn = 0; sgn < 2; sgn++) {
            const int off = SMC_ANIMATIONS + (sgn ? -d : d);
            if (sgn && !d) continue;
            void *root = nullptr, *tmpl = nullptr;
            if (!readPtr((unsigned char *)mesh + off, &root) || !root || !objIsObject(root)) continue;
            void *cls = nullptr;
            if (!readPtr((unsigned char *)root + UO_CLASS, &cls) || !cls) continue;
            if (classKind(cls) != 1) continue;
            if (!readPtr((unsigned char *)mesh + off - 4, &tmpl) || !tmpl || !objIsObject(tmpl)) continue;
            void *tcls = nullptr;
            if (!readPtr((unsigned char *)tmpl + UO_CLASS, &tcls) || !tcls || classKind(tcls) != 1) continue;
            g_smcAnimOff = off;
            char cn[64] = {0};
            objClassName(root, cn, sizeof(cn));
            logf("anim: SkeletalMeshComponent.Animations at +0x%X (root %p, a %s)%s",
                 off, root, cn, off == SMC_ANIMATIONS ? "" : " - NOT the layout's offset");
            return root;
        }
    }
    return nullptr;
}

// The pawn's two meshes, checked by class name the first time and whenever
// the pawn changes.
static void *g_pawnMeshFor = nullptr;
static void *g_mesh1p = nullptr, *g_mesh3p = nullptr;
static int g_meshLogged = 0;

static bool isSkelMesh(void *p) {
    if (!objIsObject(p)) return false;
    char cn[64] = {0};
    if (!objClassName(p, cn, sizeof(cn))) return false;
    return strcmp(cn, "SkeletalMeshComponent") == 0 || strcmp(cn, "TdSkeletalMeshComponent") == 0;
}

static void pawnMeshes(void *pawn) {
    if (pawn == g_pawnMeshFor) return;
    g_pawnMeshFor = pawn;
    g_mesh1p = g_mesh3p = nullptr;
    void *m1 = nullptr, *m3 = nullptr;
    unsigned char *o = (unsigned char *)pawn;
    if (readPtr(o + TD_MESH1P, &m1) && isSkelMesh(m1)) g_mesh1p = m1;
    if (readPtr(o + TD_MESH3P, &m3) && isSkelMesh(m3)) g_mesh3p = m3;
    if (!g_mesh1p || !g_mesh3p) {
        // Two consecutive SkeletalMeshComponent pointers near the layout's
        // offset: Mesh1p then Mesh3p.
        for (int off = TD_MESH1P - 0x40; off <= TD_MESH1P + 0x40; off += 4) {
            void *a = nullptr, *b = nullptr;
            if (!readPtr(o + off, &a) || !readPtr(o + off + 4, &b)) continue;
            if (a && b && a != b && isSkelMesh(a) && isSkelMesh(b)) {
                g_mesh1p = a; g_mesh3p = b;
                if (off != TD_MESH1P) logf("anim: Mesh1p/Mesh3p found at +0x%X, not the layout's 0x%X", off, TD_MESH1P);
                break;
            }
        }
    }
    if (g_meshLogged < 4) {
        g_meshLogged++;
        logf("anim: pawn %p Mesh1p %p Mesh3p %p", pawn, g_mesh1p, g_mesh3p);
    }
}

static AnimHit g_anim1p[ANIM_TOP], g_anim3p[ANIM_TOP];
static bool g_animValid = false;
static void *g_audioDevice = nullptr;    // the sounds' device, found by audioAcquire

// The heaviest leaf of a tree by the EDGE weight its parent gives it
// (FAnimBlendChild::TotalWeight, the leaf's share of the final pose), with
// the leaf's name and clip time. `animscan` uses it over every skeletal mesh
// component in the object table, to say which trees are alive.
struct LeafPick { void *node; int nameIdx; float total; float time; };
static void animHeaviest(void *node, int depth, float edgeTotal, LeafPick *best, int &budget) {
    if (!node || depth > 24 || --budget < 0 || !objIsObject(node)) return;
    void *cls = nullptr;
    if (!readPtr((unsigned char *)node + UO_CLASS, &cls) || !cls) return;
    const unsigned char kind = classKind(cls);
    unsigned char *o = (unsigned char *)node;
    if (kind == 2) {
        if (edgeTotal > best->total) {
            best->node = node; best->total = edgeTotal;
            readI32(o + ANS_SEQNAME, &best->nameIdx);
            readF32(o + ANS_CURTIME, &best->time);
        }
        return;
    }
    if (kind != 1) return;
    void *data = nullptr; int count = 0;
    if (!readPtr(o + ANB_CHILDREN, &data) || !readI32(o + ANB_CHILDREN + 4, &count)) return;
    if (!data || count <= 0 || count > 256) return;
    for (int i = 0; i < count; i++) {
        void *child = nullptr; float tot = 0.0f;
        if (!readPtr((unsigned char *)data + i * ABC_SIZE + ABC_ANIM, &child)) return;
        readF32((unsigned char *)data + i * ABC_SIZE + ABC_ANIM + 8, &tot);
        if (child && finiteF(tot) && tot > 0.001f) animHeaviest(child, depth + 1, tot, best, budget);
    }
}

static int g_animScanLeft = 0;
static void animScan() {
    if (!g_gobjects || !g_gnamesArray) return;
    // The pawn's own meshes first: every object pointer in the window around
    // SMC_ANIMATIONS, with its class and name - the template and the
    // instance sit next to each other, and only the instance ticks.
    for (int m = 0; m < 2; m++) {
        void *mesh = m ? g_mesh3p : g_mesh1p;
        if (!mesh || !objIsObject(mesh)) continue;
        for (int off = SMC_ANIMATIONS - 0x30; off <= SMC_ANIMATIONS + 0x40; off += 4) {
            void *q = nullptr;
            if (!readPtr((unsigned char *)mesh + off, &q) || !q || !objIsObject(q)) continue;
            char cn[64] = {0}, on[64] = {0};
            objClassName(q, cn, sizeof(cn)); objName(q, on, sizeof(on));
            logf("animscan: mesh%dp +0x%X -> %p %s \"%s\"", m ? 3 : 1, off, q, cn, on);
        }
    }
    int found = 0;
    for (int i = 0; i < g_gobjectCount && found < 200; i++) {
        void *obj = nullptr;
        if (!readPtr((unsigned char *)g_gobjects + i * sizeof(void *), &obj)) break;
        if (!obj || !isSkelMesh(obj)) continue;
        void *root = animRoot(obj);
        if (!root) continue;
        void *owner = nullptr, *skel = nullptr;
        readPtr((unsigned char *)obj + ACOMP_OWNER, &owner);
        char on[64] = {0}, oc[64] = {0}, sn[64] = {0}, rn[64] = {0}, ln[64] = {0};
        objName(obj, on, sizeof(on));
        if (owner && objIsObject(owner)) objClassName(owner, oc, sizeof(oc));
        if (strcmp(oc, "CrowdAgent") == 0) continue;     // the pedestrians, dozens of them
        objName(root, rn, sizeof(rn));
        // the SkeletalMesh sits 0x50 under Animations in the layout
        if (readPtr((unsigned char *)obj + g_smcAnimOff - 0x50, &skel) && skel && objIsObject(skel)) objName(skel, sn, sizeof(sn));
        LeafPick best = {nullptr, -1, 0.0f, 0.0f};
        int budget = 4096;
        animHeaviest(root, 0, 1.0f, &best, budget);
        if (best.nameIdx >= 0) nameStr(best.nameIdx, ln, sizeof(ln));
        logf("animscan: %p \"%s\" owner %p %s%s mesh \"%s\" tree \"%s\" -> \"%s\" total %.3f t %.3f",
             obj, on, owner, oc, owner == g_pawnObj ? " (THE PAWN)" : "", sn, rn, ln, best.total, best.time);
        if (best.node && owner == g_pawnObj) {
            // the leaf's words, so CurrentTime can name itself between two scans
            for (int base = 0xC0; base < 0x180; base += 0x20) {
                float fw[8];
                if (!sehRead((unsigned char *)best.node + base, fw, 32)) break;
                logf("animscan:   leaf +%03X  %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f",
                     base, fw[0], fw[1], fw[2], fw[3], fw[4], fw[5], fw[6], fw[7]);
            }
        }
        found++;
    }
    logf("animscan: %d skeletal mesh components with a tree", found);
}

// Where does the AudioDevice keep its active components? Scan its words for
// a TArray of AudioComponent objects and log every offset that qualifies.
static void audioScan() {
    if (!g_audioDevice || !g_gnamesArray) { logf("audioscan: no device"); return; }
    for (int off = 0x3C; off < 0x300; off += 4) {
        void *data = nullptr; int count = 0, maxn = 0;
        if (!readPtr((unsigned char *)g_audioDevice + off, &data) ||
            !readI32((unsigned char *)g_audioDevice + off + 4, &count) ||
            !readI32((unsigned char *)g_audioDevice + off + 8, &maxn)) continue;
        if (!data || count < 1 || count > 4096 || maxn < count || maxn > 65536) continue;
        int comps = 0, objs = 0;
        for (int i = 0; i < count && i < 16; i++) {
            void *e = nullptr;
            if (!readPtr((unsigned char *)data + i * sizeof(void *), &e) || !e) break;
            if (!objIsObject(e)) break;
            objs++;
            char cn[64] = {0};
            if (objClassName(e, cn, sizeof(cn)) && strcmp(cn, "AudioComponent") == 0) comps++;
        }
        if (objs) {
            char first[64] = {0};
            void *e = nullptr;
            readPtr(data, &e);
            if (e) objName(e, first, sizeof(first));
            logf("audioscan: +0x%X TArray count %d max %d: %d objects, %d AudioComponents, first \"%s\"",
                 off, count, maxn, objs, comps, first);
            if (comps < 4) continue;
            // A few components' words: every pointer-looking word resolved to
            // an object's class and name, floats as floats - so SoundCue,
            // Owner, Location and PlaybackTime name themselves.
            for (int k = 0; k < count && k < 6; k++) {
                void *c = nullptr;
                if (!readPtr((unsigned char *)data + k * sizeof(void *), &c) || !c) continue;
                char cn[64] = {0};
                objName(c, cn, sizeof(cn));
                logf("audioscan:  component %d %p \"%s\"", k, c, cn);
                for (int w = 0x3C; w < 0x120; w += 4) {
                    unsigned v = 0; float fv = 0.0f;
                    if (!sehRead((unsigned char *)c + w, &v, 4)) break;
                    memcpy(&fv, &v, 4);
                    void *q = (void *)(uintptr_t)v;
                    if (v > 0x10000 && objIsObject(q)) {
                        char qc[64] = {0}, qn[64] = {0};
                        objClassName(q, qc, sizeof(qc)); objName(q, qn, sizeof(qn));
                        logf("audioscan:    +%03X %08X -> %s \"%s\"", w, v, qc, qn);
                    } else if (v && (fabsf(fv) > 1e-6f && fabsf(fv) < 1e7f)) {
                        logf("audioscan:    +%03X %08X  %.4f", w, v, fv);
                    } else if (v) {
                        logf("audioscan:    +%03X %08X", w, v);
                    }
                }
            }
        }
    }
}

// `animdump` in the command file: log the first-person tree once, every
// node with its class, its NodeTotalWeight and, for a sequence, its name,
// time and rate, plus the raw words around the offsets this code reads -
// so a wrong offset shows up as a column of nonsense rather than as a
// plausible name in every slot.
static int g_animDumpLeft = 0;
static bool g_animDumpWords = false;   // animdump N 1: the raw words of each leaf too
static void animDump(void *node, int depth, int childIdx) {
    if (!node || depth > 24 || g_animDumpLeft <= 0) return;
    g_animDumpLeft--;
    if (!objIsObject(node)) { logf("animdump: %*s[%d] %p not an object", depth * 2, "", childIdx, node); return; }
    char cn[64] = {0}, on[64] = {0};
    objClassName(node, cn, sizeof(cn));
    objName(node, on, sizeof(on));
    void *cls = nullptr;
    readPtr((unsigned char *)node + UO_CLASS, &cls);
    const unsigned char kind = classKind(cls);
    unsigned char *o = (unsigned char *)node;
    float w = -1.0f;
    readF32(o + AN_TOTALWEIGHT, &w);
    if (kind == 2) {
        int nm = -1; float t = -1, rate = -1; unsigned char fl = 0;
        readI32(o + ANS_SEQNAME, &nm); readF32(o + ANS_CURTIME, &t); readF32(o + ANS_RATE, &rate); readU8(o + ANS_FLAGS, &fl);
        char sn[64] = {0};
        nameStr(nm, sn, sizeof(sn));
        logf("animdump: %*s[%d] %s \"%s\" seq w %.3f name %d \"%s\" t %.3f rate %.2f fl %02X",
             depth * 2, "", childIdx, cn, on, w, nm, sn, t, rate, fl);
        if (g_animDumpWords) {
            // Every word of the node past AnimSeqName, as float and hex, so
            // the field that advances between two dumps names itself.
            for (int base = 0x80; base < 0x140; base += 0x20) {
                float fw[8]; unsigned hw[8];
                if (!sehRead(o + base, fw, 32) || !sehRead(o + base, hw, 32)) break;
                logf("animdump:   +%03X  %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f | %08X %08X %08X %08X %08X %08X %08X %08X",
                     base, fw[0], fw[1], fw[2], fw[3], fw[4], fw[5], fw[6], fw[7],
                     hw[0], hw[1], hw[2], hw[3], hw[4], hw[5], hw[6], hw[7]);
            }
        }
        return;
    }
    void *data = nullptr; int count = 0, maxn = 0;
    readPtr(o + ANB_CHILDREN, &data); readI32(o + ANB_CHILDREN + 4, &count); readI32(o + ANB_CHILDREN + 8, &maxn);
    logf("animdump: %*s[%d] %s \"%s\" kind %d w %.3f children %d/%d at %p", depth * 2, "", childIdx, cn, on, kind, w, count, maxn, data);
    if (kind != 1 || !data || count <= 0 || count > 256) return;
    for (int i = 0; i < count && i < 32; i++) {
        void *child = nullptr;
        float cw[4] = {0, 0, 0, 0};
        readPtr((unsigned char *)data + i * ABC_SIZE + ABC_ANIM, &child);
        sehRead((unsigned char *)data + i * ABC_SIZE + ABC_ANIM + 4, cw, 16);
        if (child && (!g_animDumpWords || cw[1] > 0.01f)) { logf("animdump: %*s  child %d weight %.3f total %.3f", depth * 2, "", i, cw[0], cw[1]); animDump(child, depth + 1, i); }
    }
}

static void animTick() {
    g_animValid = false;
    for (int i = 0; i < ANIM_TOP; i++) {
        g_anim1p[i].node = nullptr; g_anim1p[i].nameIdx = -1; g_anim1p[i].weight = 0.0f;
        g_anim1p[i].time = 0.0f; g_anim1p[i].rate = 0.0f; g_anim1p[i].flags = 0;
        g_anim3p[i] = g_anim1p[i];
    }
    if (!g_pawnValid || !g_pawnObj) { g_pawnMeshFor = nullptr; return; }
    if (!g_gnamesArray) findGNames();
    if (!g_gnamesArray || !gobjectsRefresh()) return;
    pawnMeshes(g_pawnObj);
    int budget = 4096;
    if (g_animScanLeft > 0) { g_animScanLeft = 0; animScan(); audioScan(); }
    if (g_animDumpLeft > 0) {
        void *root = animRoot(g_mesh1p);
        logf("animdump: mesh1p %p root %p", g_mesh1p, root);
        animDump(root, 0, 0);
        g_animDumpLeft = 0;
    }
    if (void *root = animRoot(g_mesh1p)) animWalk(root, 0, 1.0f, g_anim1p, budget);
    budget = 4096;
    if (void *root = animRoot(g_mesh3p)) animWalk(root, 0, 1.0f, g_anim3p, budget);
    g_animValid = g_anim1p[0].nameIdx >= 0 || g_anim3p[0].nameIdx >= 0;
}

// --- sounds: the AudioDevice's active components, diffed per frame ------------

static int g_audioScanFrame = -100000;
static bool g_audioOffOk = false;

struct ActiveSound {
    void *comp;
    uint32_t startFrame;
    uint64_t startQpc;
    char cue[SOUND_NAME_LEN];
    float x, y, z;
    unsigned char pawn;
    unsigned char seen;          // touched this frame
};
// The device lists EVERY playing component - 455 ambient loops on the edge_p
// rooftop before a single footstep - so the held set is sized for a level,
// the per-frame match is a hash set rather than a nested loop, and the
// frame that first sees the device takes the list as a baseline instead of
// publishing 455 starts.
#define MAX_ACTIVE_SOUNDS 1024
#define SOUND_HASH_SLOTS 4096
static ActiveSound g_active[MAX_ACTIVE_SOUNDS];
static int g_activeCount = 0;
static bool g_audioBaselined = false;

static void soundPublish(const ActiveSound &s, bool start, float dur);

static bool readComponents(void **out, int cap, int *countOut) {
    void *data = nullptr;
    int count = 0;
    if (!readPtr((unsigned char *)g_audioDevice + AUD_COMPONENTS, &data) ||
        !readI32((unsigned char *)g_audioDevice + AUD_COMPONENTS + 4, &count))
        return false;
    if (count < 0 || count > 4096) return false;
    if (count > cap) count = cap;
    if (count && !data) return false;
    for (int i = 0; i < count; i++)
        if (!readPtr((unsigned char *)data + i * sizeof(void *), &out[i])) return false;
    *countOut = count;
    return true;
}

static void audioAcquire() {
    if (!g_gobjects || !g_gnamesArray) return;
    for (int i = 0; i < g_gobjectCount; i++) {
        void *obj = nullptr;
        if (!readPtr((unsigned char *)g_gobjects + i * sizeof(void *), &obj)) break;
        if (!obj) continue;
        char cn[64] = {0};
        if (!objClassName(obj, cn, sizeof(cn))) continue;
        const size_t n = strlen(cn);
        if (n < 11 || strcmp(cn + n - 11, "AudioDevice") != 0) continue;
        char on[64] = {0};
        objName(obj, on, sizeof(on));
        if (strncmp(on, "Default__", 9) == 0) continue;     // the class default object
        g_audioDevice = obj;
        void *comps[8];
        int count = 0;
        g_audioOffOk = readComponents(comps, 8, &count);
        char c0[64] = {0};
        if (g_audioOffOk && count > 0 && comps[0] && objIsObject(comps[0]))
            objClassName(comps[0], c0, sizeof(c0));
        logf("sound: AudioDevice %p (%s \"%s\"), AudioComponents at +0x%X: %d active%s%s",
             obj, cn, on, AUD_COMPONENTS, count, c0[0] ? ", first a " : "", c0);
        return;
    }
    logf("sound: no *AudioDevice object among %d", g_gobjectCount);
}

static void audioTick() {
    if (!g_gnamesArray) findGNames();
    if (!g_audioDevice) {
        if (g_frame - g_audioScanFrame < 120) return;
        g_audioScanFrame = (int)g_frame;
        gobjectsRefresh();
        audioAcquire();
        if (!g_audioDevice) return;
    }
    if (!objIsObject(g_audioDevice)) {          // a level change freed it
        logf("sound: AudioDevice %p gone", g_audioDevice);
        g_audioDevice = nullptr;
        g_activeCount = 0;
        g_audioBaselined = false;
        return;
    }
    static void *cur[MAX_ACTIVE_SOUNDS];
    int n = 0;
    if (!readComponents(cur, MAX_ACTIVE_SOUNDS, &n)) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    static uint64_t freq = 0;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = (uint64_t)f.QuadPart; }

    // The held set, hashed by pointer, so a frame is O(listed + held).
    static int slots[SOUND_HASH_SLOTS];
    for (int i = 0; i < SOUND_HASH_SLOTS; i++) slots[i] = -1;
    for (int i = 0; i < g_activeCount; i++) {
        g_active[i].seen = 0;
        unsigned h = ((unsigned)(uintptr_t)g_active[i].comp >> 5) & (SOUND_HASH_SLOTS - 1);
        while (slots[h] >= 0) h = (h + 1) & (SOUND_HASH_SLOTS - 1);
        slots[h] = i;
    }
    const bool quiet = !g_audioBaselined;
    // Starts: anything listed that is not held.
    for (int j = 0; j < n; j++) {
        void *c = cur[j];
        if (!c) continue;
        unsigned h = ((unsigned)(uintptr_t)c >> 5) & (SOUND_HASH_SLOTS - 1);
        int idx = -1;
        while (slots[h] >= 0) {
            if (g_active[slots[h]].comp == c) { idx = slots[h]; break; }
            h = (h + 1) & (SOUND_HASH_SLOTS - 1);
        }
        if (idx >= 0) { g_active[idx].seen = 1; continue; }
        if (g_activeCount >= MAX_ACTIVE_SOUNDS || !objIsObject(c)) continue;
        ActiveSound s;
        memset(&s, 0, sizeof(s));
        s.comp = c;
        s.seen = 1;
        s.startFrame = (uint32_t)g_frame;
        s.startQpc = (uint64_t)now.QuadPart;
        void *cue = nullptr;
        if (readPtr((unsigned char *)c + AUC_SOUNDCUE, &cue) && cue && objIsObject(cue))
            objName(cue, s.cue, sizeof(s.cue));
        if (!s.cue[0]) strcpy(s.cue, "?");
        void *owner = nullptr;
        if (readPtr((unsigned char *)c + ACOMP_OWNER, &owner) && owner && owner == g_pawnObj)
            s.pawn = 1;
        float loc[3];
        if (sehRead((unsigned char *)c + AUC_LOCATION, loc, 12) &&
            finiteF(loc[0]) && finiteF(loc[1]) && finiteF(loc[2])) {
            s.x = loc[0]; s.y = loc[1]; s.z = loc[2];
        }
        g_active[g_activeCount++] = s;
        if (!quiet) soundPublish(s, true, 0.0f);
    }
    // Stops: anything held that the device no longer lists.
    for (int i = 0; i < g_activeCount;) {
        if (g_active[i].seen) { i++; continue; }
        const float dur = (float)((double)((uint64_t)now.QuadPart - g_active[i].startQpc) / (double)freq);
        if (!quiet) soundPublish(g_active[i], false, dur);
        g_active[i] = g_active[g_activeCount - 1];
        g_activeCount--;
    }
    if (quiet) {
        g_audioBaselined = true;
        logf("sound: baseline %d playing components", g_activeCount);
    }
}

// --- telemetry ring buffers ---------------------------------------------------
//
// The state file (medge_camera.state) is rewritten every 2nd frame with no
// timestamp and no history: adequate for "where is the camera", useless for
// kinematics. Every measurement that needs a time series - gravity, jump arcs,
// wallrun entry speeds - previously depended on ad-hoc polling that oversampled
// the same write and could not tell a missed frame from a duplicate.
//
// So publish two ring buffers in a named shared-memory section instead, one
// record per PRESENTED frame:
//
//   telem: frame #, QPC timestamp, camera pos (uu), yaw/pitch/roll (deg), flags
//   input: frame #, QPC timestamp, virtual key, down/up, repeat/blocked flags
//
// The input ring is fed from the window subclass, so it records every key the
// GAME received - injected and human alike - stamped with the frame it landed
// on. That is what makes input->effect latency measurable, and it doubles as a
// free "record the human playing" tap: telemetry plus input events is a full
// trace of a play session.
//
// Single writer (the render thread; the window is pumped on the same thread),
// lock-free readers: each record is fully written before the count is
// published with an interlocked increment, and Python reads [last, count).
// A reader that falls behind by more than the capacity DETECTS the gap from
// the count rather than silently losing samples. The section is page-file
// backed and session-local; whichever side maps it first creates it, and the
// hook re-initialises the header on attach (magic written last), so a reader
// spanning a game restart sees sessionQpc change and resyncs.

#pragma pack(push, 4)
struct TelemHeader {
    uint32_t magic;              // TELEM_MAGIC once the header is valid
    uint32_t version;
    uint32_t telemCapacity;
    uint32_t inputCapacity;
    uint64_t qpcFreq;
    uint64_t sessionQpc;         // QPC at init; readers detect restarts
    volatile LONG telemCount;    // records ever published; slot = n % capacity
    volatile LONG inputCount;
    uint32_t soundCapacity;      // v6: the third ring, sound start/stop events
    volatile LONG soundCount;
    uint32_t reserved[4];
};                               // 64 bytes
// v6: which animation. One of the three heaviest AnimNodeSequence leaves of
// a mesh's tree - the clip's name as a string (so no reader needs the name
// table), its CurrentTime and its NodeTotalWeight, the share of the final
// pose it is. Empty name = no such leaf.
struct AnimSlot {
    char name[ANIM_NAME_LEN];
    float time;
    float weight;
};                               // 36 bytes
struct TelemRecord {
    uint32_t frame;
    uint32_t flags;              // 1 camValid, 2 freecam, 4 noclip,
                                 // 8 pawnValid, 16 worldValid
    uint64_t qpc;
    float x, y, z;               // game camera, world uu (pre-patch, like .state)
    float yawDeg, pitchDeg;
    // Roll, which yaw/pitch cannot carry - see rollFromBasis. The wall-run
    // lean lives here and nowhere else, so v2 traces cannot test it at all.
    float rollDeg;
    // From the game's own object table, not from the GPU pipeline. moveState
    // is EMovement (tools/retail/movenames.py); 0xFF whenever !pawnValid, so a
    // gap reads as a gap rather than as MOVE_None.
    uint8_t moveState, oldMoveState, pendingMoveState, walkingState;
    float vx, vy, vz;            // pawn Velocity, uu/s - no camera spring in it
    float px, py, pz;            // pawn Location, uu - the feet, not the eye
    float gravityZ;              // WorldInfo::WorldGravityZ, live
    // v5: THE LOOK, from the object table. The camera yaw/pitch above come
    // off the view-projection matrix and froze for a whole human recording
    // after a device Reset (doc/medge_port_parity.md, "Replaying a human
    // run"), which left the replay guessing the look from the velocity. The
    // controller's Rotation is what the mouse writes; the pawn's yaw is the
    // body's, which the wall moves turn independently. Degrees; valid when
    // flag 32 is set.
    float ctrlYawDeg, ctrlPitchDeg, pawnYawDeg;
    uint8_t physics;             // EPhysics; 12/13 are the custom wall modes
    uint8_t pad0, pad1, pad2;
    // v6: the first-person body's and the third-person body's animations,
    // heaviest first (see animTick). Valid when flag 64 is set.
    AnimSlot anim1p[ANIM_TOP];
    AnimSlot anim3p[ANIM_TOP];
};                               // 304 bytes
struct InputRecord {
    uint32_t frame;
    uint32_t vk;                 // virtual key; mouse buttons arrive as VK_LBUTTON etc.
    uint32_t down;               // 1 press, 0 release
    uint32_t flags;              // 1 auto-repeat, 2 blocked by noclip, 4 extended
    uint64_t qpc;
    uint32_t scan;               // scan code from lParam bits 16-23
    uint32_t pad;
};                               // 32 bytes
// v6: a sound starting or stopping - an AudioComponent appearing in or
// leaving the AudioDevice's active list (see audioTick). The cue's name, the
// component's Location (uu) and, on a stop, how long it played.
struct SoundRecord {
    uint32_t frame;
    uint32_t flags;              // 1 start, 2 stop, 4 the pawn owns it
    uint64_t qpc;
    char cue[SOUND_NAME_LEN];
    float x, y, z;
    float dur;
};                               // 72 bytes
#pragma pack(pop)

#define TELEM_MAGIC 0x4D45544Cu  // "LTEM" little-endian; bumped with VERSION
#define TELEM_VERSION 6
// 18 minutes at 60 fps, 9 at 120. It was 8192 - two minutes - which is fine
// when record_session.py is draining incrementally and useless when it is not:
// a session recorded by leaving the game running and draining at the end lost
// 15783 of its 24000 frames, silently, because the ring had lapped. The ring is
// the backstop for forgetting the recorder, so size it for a whole session.
// 5 MB of page-file-backed shared memory is not worth economising on.
#define TELEM_CAPACITY 65536
#define INPUT_CAPACITY 2048
#define SOUND_CAPACITY 4096
// The NAME changes with the LAYOUT - record size or capacity - not just the
// version. telemetry.py CREATES the section if it does not exist, and
// `trials.py --status` is routinely run before the game is up, so a stale
// reader can leave a differently-sized section behind. CreateFileMapping then
// returns that existing object and ignores the size asked for, and the
// oversized MapViewOfFile fails - leaving telemetry silently off. Renaming
// makes that impossible rather than unlikely.
#define TELEM_MAP_NAME "medge_telemetry_v6"

static TelemHeader *g_telem = nullptr;
static TelemRecord *g_telemRecs = nullptr;
static InputRecord *g_inputRecs = nullptr;
static SoundRecord *g_soundRecs = nullptr;

static void ensureTelemetry() {
    static bool tried = false;
    if (g_telem || tried) return;
    tried = true;

    const DWORD size = sizeof(TelemHeader) + TELEM_CAPACITY * sizeof(TelemRecord)
                       + INPUT_CAPACITY * sizeof(InputRecord)
                       + SOUND_CAPACITY * sizeof(SoundRecord);
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                        PAGE_READWRITE, 0, size, TELEM_MAP_NAME);
    if (!mapping) {
        logf("telemetry: CreateFileMapping failed (%lu)", GetLastError());
        return;
    }
    void *base = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!base) {
        logf("telemetry: MapViewOfFile failed (%lu)", GetLastError());
        CloseHandle(mapping);
        return;
    }

    TelemHeader *h = (TelemHeader *)base;
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    // Invalidate first: a Python reader may have created (or still hold) the
    // section, and it must not read a half-updated header as valid.
    h->magic = 0;
    MemoryBarrier();
    h->version = TELEM_VERSION;
    h->telemCapacity = TELEM_CAPACITY;
    h->inputCapacity = INPUT_CAPACITY;
    h->qpcFreq = (uint64_t)freq.QuadPart;
    h->sessionQpc = (uint64_t)now.QuadPart;
    h->telemCount = 0;
    h->inputCount = 0;
    h->soundCapacity = SOUND_CAPACITY;
    h->soundCount = 0;
    memset((void *)h->reserved, 0, sizeof(h->reserved));
    MemoryBarrier();
    h->magic = TELEM_MAGIC;

    g_telemRecs = (TelemRecord *)(h + 1);
    g_inputRecs = (InputRecord *)(g_telemRecs + TELEM_CAPACITY);
    g_soundRecs = (SoundRecord *)(g_inputRecs + INPUT_CAPACITY);
    g_telem = h;
    logf("telemetry ring ready: %d telem + %d input + %d sound records (%lu bytes, \"%s\")",
         TELEM_CAPACITY, INPUT_CAPACITY, SOUND_CAPACITY, (unsigned long)size, TELEM_MAP_NAME);
}

static void soundPublish(const ActiveSound &s, bool start, float dur) {
    if (!g_telem || !g_soundRecs) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    SoundRecord &r = g_soundRecs[(uint32_t)g_telem->soundCount % SOUND_CAPACITY];
    r.frame = (uint32_t)g_frame;
    r.flags = (start ? 1u : 2u) | (s.pawn ? 4u : 0u);
    r.qpc = (uint64_t)now.QuadPart;
    memcpy(r.cue, s.cue, SOUND_NAME_LEN);
    r.cue[SOUND_NAME_LEN - 1] = 0;
    r.x = s.x; r.y = s.y; r.z = s.z;
    r.dur = dur;
    InterlockedIncrement(&g_telem->soundCount);
}

static void animSlotFill(AnimSlot *out, const AnimHit *in) {
    for (int i = 0; i < ANIM_TOP; i++) {
        memset(out[i].name, 0, ANIM_NAME_LEN);
        out[i].time = 0.0f;
        out[i].weight = 0.0f;
        if (in[i].nameIdx < 0) continue;
        if (!nameStr(in[i].nameIdx, out[i].name, ANIM_NAME_LEN)) { out[i].name[0] = 0; continue; }
        out[i].time = in[i].time;
        out[i].weight = in[i].weight;
    }
}

// Roll, recovered from the basis rather than from the matrix directly.
//
// Where world-up lands in the camera's own (right, up) plane IS the roll: with
// no roll it lies along up, and rolling by theta swings it to
// (-sin theta, cos theta). So the angle back is atan2(-x, y) - hence the
// negation, without which the sign is inverted.
//
// Positive means the camera's UP axis tilts toward its own RIGHT (tipping your
// head to the right). Confirmed against the game: a wall on the LEFT reads
// positive here (five runs, +11.9 to +23.8) and a wall on the right negative
// (-24.2), with the side established from the exit push, which is the 520 uu/s
// PushAwaySpeedNoob+ProAdd already pinned as pointing away from the wall.
//
// The port uses the same handedness - it leans away from the wall on both
// sides. Note that physics.cpp's own `wallright` was MISNAMED and true for a
// wall on the LEFT, because medge_dirs' second output points left; an earlier
// version of this comment repeated that error in the opposite direction.
// Neither the formula here nor the port's behaviour was ever affected.
//
// Handedness needs no correction here, unlike the freecam's strafe axis:
// column 0 of a projection maps a world direction to clip x by construction, so
// the axis recovered from it is screen right whatever convention built it. The
// derivation is also symmetric in the (right, up) plane - it depends on the
// rotation between the two axes, not on where either one points in the world.
//
// Undefined when looking straight up or down, where world-up has no component
// in the plane at all; that reads as 0 from atan2(0, 0) rather than as noise.
static float rollFromBasis() {
    const float wx = g_lastRight[2];   // dot(worldUp, right), worldUp = +Z
    const float wy = g_lastUp[2];      // dot(worldUp, up)
    if (fabsf(wx) < 1e-6f && fabsf(wy) < 1e-6f) return 0.0f;
    return -atan2f(wx, wy) * (180.0f / 3.14159265358979f);
}

static void telemetryTick() {
    if (!g_telem) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    TelemRecord &r = g_telemRecs[(uint32_t)g_telem->telemCount % TELEM_CAPACITY];
    r.frame = (uint32_t)g_frame;
    r.flags = (g_lastCamValid ? 1u : 0u) | (g_freeCamEnabled ? 2u : 0u)
              | (g_noclip ? 4u : 0u);
    r.qpc = (uint64_t)now.QuadPart;
    r.x = g_lastCamPos[0];
    r.y = g_lastCamPos[1];
    r.z = g_lastCamPos[2];
    const float kRad = 180.0f / 3.14159265358979f;
    r.yawDeg = g_lastYaw * kRad;
    r.pitchDeg = g_lastPitch * kRad;
    r.rollDeg = rollFromBasis();
    r.flags |= (g_pawnValid ? 8u : 0u) | (g_worldValid ? 16u : 0u)
             | (g_rotValid ? 32u : 0u);
    r.ctrlYawDeg = g_ctrlYawDeg; r.ctrlPitchDeg = g_ctrlPitchDeg; r.pawnYawDeg = g_pawnYawDeg;
    r.moveState = g_moveState;
    r.oldMoveState = g_oldMoveState;
    r.pendingMoveState = g_pendingMoveState;
    r.walkingState = g_walkingState;
    r.vx = g_pawnVel[0]; r.vy = g_pawnVel[1]; r.vz = g_pawnVel[2];
    r.px = g_pawnPos[0]; r.py = g_pawnPos[1]; r.pz = g_pawnPos[2];
    r.gravityZ = g_worldGravityZ;
    r.physics = g_pawnPhysics;
    r.pad0 = r.pad1 = r.pad2 = 0;
    r.flags |= (g_animValid ? 64u : 0u);
    animSlotFill(r.anim1p, g_anim1p);
    animSlotFill(r.anim3p, g_anim3p);
    // Publish only after the record is complete.
    InterlockedIncrement(&g_telem->telemCount);
}

static void telemetryKey(unsigned vk, bool down, LPARAM l, bool blocked) {
    if (!g_telem) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    InputRecord &r = g_inputRecs[(uint32_t)g_telem->inputCount % INPUT_CAPACITY];
    r.frame = (uint32_t)g_frame;
    r.vk = vk;
    r.down = down ? 1u : 0u;
    r.flags = ((l & (1 << 30)) && down ? 1u : 0u)   // bit 30: was already down
              | (blocked ? 2u : 0u)
              | ((l & (1 << 24)) ? 4u : 0u);        // bit 24: extended key
    r.qpc = (uint64_t)now.QuadPart;
    r.scan = (uint32_t)((l >> 16) & 0xFF);
    r.pad = 0;
    InterlockedIncrement(&g_telem->inputCount);
}

// --- keeping the player still while noclip flies ------------------------------
//
// Without this, WASD reaches the game as well as the camera: Faith walks around
// underneath, and on a rooftop she eventually walks off the edge and dies.
//
// Mirror's Edge takes keyboard input from the window message queue - it imports
// PeekMessageW/DispatchMessageW and neither dinput8 nor RegisterRawInputDevices
// - so subclassing the window and dropping the presses is enough. There is no
// need to hook DirectInput, and no need for the inert CheatManager.
//
// Only the DOWN edges are blocked. Passing every UP through is what makes the
// transition safe: a key already held when noclip is switched on has already
// been delivered, so the game would walk forever if its release were swallowed
// too. Entering noclip also posts a release for each blocked key, which reaches
// the game precisely because UP is never blocked.
//
// Mouse MOTION is deliberately left alone - the free camera's mouse look is the
// game's own view rotation, read back out of the view-projection.

static WNDPROC g_origWndProc = nullptr;
static bool g_wndUnicode = true;

// Alt is deliberately absent. Blocking it is not worth the hazard: a lone Alt
// press or release reaches DefWindowProc as SC_KEYMENU, which puts the window
// into the modal menu loop - that stalls the message pump and drops mouse
// capture. releaseHeldKeys() posting a bare Alt-up was doing exactly that. The
// noclip slow modifier is Ctrl for the same reason.
static const WPARAM kBlockedKeys[] = {
    'W', 'A', 'S', 'D', 'Q', 'E', 'R', 'F', 'C', VK_SPACE,
    VK_SHIFT, VK_LSHIFT, VK_RSHIFT,
    VK_CONTROL, VK_LCONTROL, VK_RCONTROL,
    VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN,
};

static bool isBlockedKey(WPARAM vk) {
    for (size_t i = 0; i < sizeof(kBlockedKeys) / sizeof(kBlockedKeys[0]); ++i)
        if (kBlockedKeys[i] == vk) return true;
    return false;
}

static LRESULT CALLBACK MyWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    // Record before blocking, so a trace shows what ARRIVED - the blocked flag
    // says whether the game was allowed to see it.
    bool blocked = false;
    if (g_noclip) {
        switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            // Escape and the function keys stay live, so the pause menu and the
            // screenshot bindings still work while flying.
            if (isBlockedKey(w)) { ++g_blockedKeyCount; blocked = true; }
            break;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDBLCLK:
            blocked = true;           // a stray click would jump or punch
            break;
        default:
            break;
        }
    }

    switch (msg) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        telemetryKey((unsigned)w, true, l, blocked);
        break;
    case WM_KEYUP: case WM_SYSKEYUP:
        telemetryKey((unsigned)w, false, l, blocked);
        break;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        telemetryKey(VK_LBUTTON, true, 0, blocked);
        break;
    case WM_LBUTTONUP:
        telemetryKey(VK_LBUTTON, false, 0, blocked);
        break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
        telemetryKey(VK_RBUTTON, true, 0, blocked);
        break;
    case WM_RBUTTONUP:
        telemetryKey(VK_RBUTTON, false, 0, blocked);
        break;
    case WM_MBUTTONDOWN:
        telemetryKey(VK_MBUTTON, true, 0, blocked);
        break;
    case WM_MBUTTONUP:
        telemetryKey(VK_MBUTTON, false, 0, blocked);
        break;
    case WM_XBUTTONDOWN:
        telemetryKey(VK_XBUTTON1, true, 0, blocked);
        break;
    default:
        break;
    }

    if (blocked) return 0;
    return g_wndUnicode ? CallWindowProcW(g_origWndProc, h, msg, w, l)
                        : CallWindowProcA(g_origWndProc, h, msg, w, l);
}

static void installWndProc(HWND h) {
    if (!h || g_origWndProc) return;
    // The subclass feeds the input ring, and keys can arrive before the first
    // Present - so the section must exist as soon as the window is ours.
    ensureTelemetry();
    // The A/W variants must match the window, or Windows inserts a translation
    // layer and messages arrive mangled.
    g_wndUnicode = IsWindowUnicode(h) != FALSE;
    g_origWndProc = g_wndUnicode
        ? (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)MyWndProc)
        : (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)MyWndProc);
    logf("subclassed window %p (unicode=%d, orig=%p)", (void *)h,
         (int)g_wndUnicode, (void *)g_origWndProc);
}

// --- keeping the cursor inside the window -------------------------------------
//
// Mirror's Edge never calls ClipCursor during gameplay - fullscreen confines the
// pointer for it, and it reads mouse look from relative motion rather than from
// the cursor position. Forcing windowed mode (see MyCreateDevice) removes that
// confinement, so the OS cursor wanders off the window and onto other monitors,
// where clicks land in other applications.
//
// Measured: from a clean boot, straight into gameplay and BEFORE noclip is ever
// enabled, GetClipCursor returns the whole virtual screen and the pointer walks
// to the far screen edge. This is a consequence of the windowed-mode patch, not
// of the free camera - the free camera only makes it obvious, because flying
// moves the mouse far more than normal play does.
//
// Reasserting the clip while the game is focused restores what fullscreen used
// to provide. Windows drops cursor clipping by itself when another window is
// activated, so alt-tab still works and needs no special handling.

static bool g_confineCursor = true;

static void tickCursorClip() {
    if (!g_confineCursor || !g_gameWindow) return;
    if (GetForegroundWindow() != g_gameWindow) return;
    RECT rc;
    if (!GetClientRect(g_gameWindow, &rc)) return;
    POINT tl = {rc.left, rc.top};
    POINT br = {rc.right, rc.bottom};
    if (!ClientToScreen(g_gameWindow, &tl)) return;
    if (!ClientToScreen(g_gameWindow, &br)) return;
    RECT screenRect = {tl.x, tl.y, br.x, br.y};
    ClipCursor(&screenRect);
}

// Tell the game every blocked key is up, for anything held across the toggle.
static void releaseHeldKeys() {
    if (!g_gameWindow) return;
    for (size_t i = 0; i < sizeof(kBlockedKeys) / sizeof(kBlockedKeys[0]); ++i) {
        UINT sc = MapVirtualKeyW((UINT)kBlockedKeys[i], MAPVK_VK_TO_VSC);
        LPARAM l = (LPARAM)(0xC0000001u | (sc << 16));
        PostMessageW(g_gameWindow, WM_KEYUP, kBlockedKeys[i], l);
    }
}

// --- locating the player position in memory -----------------------------------
//
// Every CheatManager command is inert in this retail build - God, Ghost, Fly,
// DropMe and SpawnAt all do nothing (verified: walking off a roof with God
// pressed still kills you and respawns at the checkpoint). So there is no
// console route to a flycam, and rewriting the view-projection alone leaves
// culling, streaming and the sky pass following the player, which shreds the
// image past a few hundred units.
//
// Moving the *player* fixes all of that at once. Finding where the engine keeps
// its position needs no UE3 reflection: the view-projection already tells us
// exactly where the camera is, so we can scan the heap for three consecutive
// floats matching it, then narrow the candidate list over successive frames as
// the value moves. Anything that keeps tracking the camera is the real thing.

// Narrowing works on *deltas*, not absolute values. The pawn's Location differs
// from the eye position by a view offset that is not a fixed 76 uu - lean, head
// bob and the camera system all contribute - so matching absolute coordinates
// misses it. But whatever the offset, the pawn moves by the same amount the
// camera does, so tracking the change identifies it regardless.
//
// This also rejects the decoys that defeated the first attempt: the addresses
// found there (0x73096DE8/0x73096DF8, 16 bytes apart, in the system DLL range)
// are the D3D9 runtime's shader-constant cache. They mirror the camera exactly
// because they *are* the uploaded matrix, and writes to them are overwritten
// before the next frame.
// Sized so a scan never hits the cap. An earlier run filled 65536 slots and
// stopped mid-sweep, leaving most of the heap unexamined - which is why the
// pawn was never among the candidates no matter how the narrowing was tuned.
#define MAX_ADDR_CANDIDATES 400000
static float **g_addrCand = nullptr;
static float *g_addrPrev = nullptr;       // 3 floats per candidate
static int g_addrCandCount = 0;
static double g_skippedBigBytes = 0.0;    // memory the region-size cap hid
static bool g_havePrevCam = false;
static float g_prevCam[3] = {0, 0, 0};
static bool g_addrLocked = false;
static float *g_playerPos = nullptr;      // confirmed address, once narrowed
static int g_scanState = 0;               // 0 idle, 1 scanning, 2 narrowing
static float g_scanRef[3] = {0, 0, 0};
static float g_scanZOffset = 0.0f;        // camera-to-target Z offset
static int g_candIndex = 0;

// Continuous position hold. Writing every frame beats the engine's own
// integration, which reverts a one-shot write before anything is drawn.
static bool g_tpActive = false;
static float g_tpPos[3] = {0, 0, 0};
static int g_tpFrames = 0;

// When holding, write through *all* surviving candidates rather than one. Only
// one is authoritative, but writing the others is harmless - they are copies
// the engine rewrites anyway - and it removes the need to guess which is which.
// --- finding the pawn by EXPERIMENT, not by inference -------------------------
//
// Earlier attempts scanned for addresses tracking the camera, narrowed them to
// ~90, wrote all of them at once, saw nothing move, and concluded the pawn was
// not reachable. That conclusion was never actually tested: writing ninety
// addresses simultaneously cannot say which one mattered, and a wrong one can
// disturb the engine enough to mask a right one.
//
// So test them one at a time, and use the only unambiguous oracle available -
// does the CAMERA move? The camera position is recovered from the
// view-projection every frame and is downstream of the pawn, so if writing an
// address lifts the view, that address is the pawn's Location.
//
// Each candidate is offered +400 uu of Z for a few frames, then restored. The
// value is written every frame because the engine integrates the pawn each
// tick and would otherwise undo a single write before anything is drawn.

static bool g_pawnFound = false;
static float *g_pawnAddr = nullptr;
static float g_pawnOffset[3] = {0, 0, 0};   // camera minus pawn, measured once
static int g_findState = 0;                 // 0 idle, 1 sweeping
static int g_findIndex = 0;
static int g_findFrames = 0;
static float g_findSaved[3] = {0, 0, 0};
static float g_findBaseCam[3] = {0, 0, 0};
static int g_findTested = 0;

#define FIND_FRAMES_PER_CANDIDATE 10
#define FIND_LIFT 400.0f
#define FIND_ACCEPT 150.0f

static bool readTriple(float *p, float *out) {
    __try {
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool writeTriple(float *p, const float *v) {
    __try {
        p[0] = v[0]; p[1] = v[1]; p[2] = v[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Keep only candidates that sit where a pawn must sit relative to the eye:
// directly below it, by less than a capsule. This turns a few hundred
// camera-tracking addresses into a handful worth writing to.
static void filterCandidatesToPawnShape() {
    int kept = 0;
    for (int i = 0; i < g_addrCandCount; ++i) {
        float v[3];
        if (!readTriple(g_addrCand[i], v)) continue;
        float dx = v[0] - g_lastCamPos[0];
        float dy = v[1] - g_lastCamPos[1];
        float dz = g_lastCamPos[2] - v[2];        // eye is ABOVE the pawn
        if (fabsf(dx) > 12.0f || fabsf(dy) > 12.0f) continue;
        if (dz < 20.0f || dz > 140.0f) continue;
        g_addrCand[kept++] = g_addrCand[i];
    }
    g_addrCandCount = kept;
    logf("pawn-shape filter kept %d candidates", kept);
}

static void tickPawnFind() {
    if (g_findState != 1 || !g_lastCamValid) return;

    if (g_findIndex >= g_addrCandCount) {
        logf("pawn sweep finished: tested %d, none moved the camera", g_findTested);
        g_findState = 0;
        return;
    }

    float *p = g_addrCand[g_findIndex];

    if (g_findFrames == 0) {
        if (!readTriple(p, g_findSaved)) {      // stale address, skip
            ++g_findIndex;
            return;
        }
        g_findBaseCam[0] = g_lastCamPos[0];
        g_findBaseCam[1] = g_lastCamPos[1];
        g_findBaseCam[2] = g_lastCamPos[2];
    }

    float target[3] = {g_findSaved[0], g_findSaved[1], g_findSaved[2] + FIND_LIFT};
    if (!writeTriple(p, target)) {
        ++g_findIndex;
        g_findFrames = 0;
        return;
    }

    if (++g_findFrames < FIND_FRAMES_PER_CANDIDATE) return;

    if ((g_findIndex % 25) == 0)
        logf("pawn sweep progress: %d/%d", g_findIndex, g_addrCandCount);

    float rise = g_lastCamPos[2] - g_findBaseCam[2];
    ++g_findTested;
    if (rise > FIND_ACCEPT) {
        g_pawnAddr = p;
        g_pawnFound = true;
        // The eye sits a little above the pawn; measure it rather than assume,
        // so driving the pawn from the camera puts the view back where it was.
        g_pawnOffset[0] = g_findBaseCam[0] - g_findSaved[0];
        g_pawnOffset[1] = g_findBaseCam[1] - g_findSaved[1];
        g_pawnOffset[2] = g_findBaseCam[2] - g_findSaved[2];
        writeTriple(p, g_findSaved);            // put the player back
        logf("PAWN FOUND at %p after %d candidates: camera rose %.1f uu; "
             "eye offset %.1f %.1f %.1f", (void *)p, g_findTested, rise,
             g_pawnOffset[0], g_pawnOffset[1], g_pawnOffset[2]);
        g_findState = 0;
        return;
    }

    writeTriple(p, g_findSaved);                // restore, next candidate
    ++g_findIndex;
    g_findFrames = 0;
}

// The pawn Location the GObjects walk validated (pawnTick) is authoritative -
// no scan or sweep needed. Zero Velocity in the same frame: the engine
// integrates the pawn every tick, so a held Location with a live velocity
// accumulates fall speed all the while and releases it as a lethal launch,
// and the fall state's screen effects would smear the frame being captured.
#define PAWN_EYE_Z 76.0f            // BaseEyeHeight; exactness does not matter
                                    // for carrying, only staying out of frame

static bool writePawnObj(const float *pos) {
    if (!g_pawnValid || !g_pawnObj) return false;
    unsigned char *o = (unsigned char *)g_pawnObj;
    const float zero[3] = {0, 0, 0};
    return writeTriple((float *)(o + AA_LOCATION), pos) &&
           writeTriple((float *)(o + AA_VELOCITY), zero);
}

// With the pawn located, noclip can take it along: streaming, culling and
// anything else keyed to the player then follows the camera instead of being
// left behind at the spawn point.
static void drivePawnWithCamera() {
    if (!g_noclip) return;
    if (g_pawnValid && g_pawnObj) {
        static void *logged = nullptr;
        float v[3] = {g_ncPos[0], g_ncPos[1], g_ncPos[2] - PAWN_EYE_Z};
        if (writePawnObj(v)) {
            if (logged != g_pawnObj) {
                logged = g_pawnObj;
                logf("pawn carry: via GObjects pawn %p", g_pawnObj);
            }
            return;
        }
    }
    // Legacy fallback: the address the one-at-a-time sweep confirmed.
    if (!g_pawnFound || !g_pawnAddr) return;
    float v[3] = {g_ncPos[0] - g_pawnOffset[0],
                  g_ncPos[1] - g_pawnOffset[1],
                  g_ncPos[2] - g_pawnOffset[2]};
    if (!writeTriple(g_pawnAddr, v)) {
        logf("pawn address went stale; dropping it");
        g_pawnFound = false;
        g_pawnAddr = nullptr;
    }
}

static void holdPlayerPosition() {
    if (!g_tpActive) return;
    ++g_tpFrames;
    if (writePawnObj(g_tpPos)) {
        if (g_tpFrames == 1)
            logf("position hold: via GObjects pawn %p", g_pawnObj);
        return;                     // authoritative; skip the shotgun writes
    }
    if (g_addrCandCount <= 0) return;
    for (int i = 0; i < g_addrCandCount; ++i) {
        float *p = g_addrCand[i];
        __try {
            p[0] = g_tpPos[0];
            p[1] = g_tpPos[1];
            p[2] = g_tpPos[2];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Candidate went stale; leave it, the next scan will drop it.
        }
    }
}

// Match on X and Y only. The pawn's Location and the camera's eye position
// share their horizontal coordinates and differ only in Z (by BaseEyeHeight,
// plus whatever the camera system adds for lean, bob and view offsets - which
// is why scanning for a fixed Z offset finds nothing). Ignoring Z finds both,
// and the authoritative one is whichever still moves the world when written.
static bool closeTo(const float *a, const float *b, float eps) {
    return fabsf(a[0] - b[0]) < eps && fabsf(a[1] - b[1]) < eps;
}

static bool isInsideModule(const void *p) {
    // Anything inside a loaded image is engine code or static data, not the
    // live actor heap. The D3D runtime's own allocations are excluded later by
    // the delta test.
    HMODULE mod = NULL;
    return GetModuleHandleExA(
               GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
               (LPCSTR)p, &mod) &&
           mod != NULL;
}

static void scanForPosition(const float *ref) {
    if (!g_addrCand)
        g_addrCand = (float **)VirtualAlloc(NULL, MAX_ADDR_CANDIDATES * sizeof(float *),
                                            MEM_COMMIT, PAGE_READWRITE);
    if (!g_addrPrev)
        g_addrPrev = (float *)VirtualAlloc(NULL, MAX_ADDR_CANDIDATES * 3 * sizeof(float),
                                           MEM_COMMIT, PAGE_READWRITE);
    if (!g_addrCand || !g_addrPrev) return;
    g_addrCandCount = 0;
    g_havePrevCam = false;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    BYTE *addr = (BYTE *)si.lpMinimumApplicationAddress;
    BYTE *maxAddr = (BYTE *)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;

    int regions = 0, scannedRegions = 0;
    unsigned __int64 scannedBytes = 0;
    int looseHits = 0;      // matches on x alone, to tell "wrong value" from
                            // "wrong memory"

    while (addr < maxAddr && g_addrCandCount < MAX_ADDR_CANDIDATES) {
        if (!VirtualQuery(addr, &mbi, sizeof(mbi))) break;
        ++regions;
        bool usable = mbi.State == MEM_COMMIT &&
                      !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                      (mbi.Protect == PAGE_READWRITE ||
                       mbi.Protect == PAGE_EXECUTE_READWRITE ||
                       mbi.Protect == PAGE_WRITECOPY ||
                       mbi.Protect == PAGE_EXECUTE_WRITECOPY);
        // Never scan our own bookkeeping. g_addrPrev is filled with copies of
        // the very position being searched for, so once the sweep reaches it
        // every entry matches, the candidate list explodes to the cap, and the
        // sweep aborts before covering the rest of the heap.
        bool isOurs = (g_addrCand &&
                       (BYTE *)mbi.BaseAddress <= (BYTE *)g_addrCand &&
                       (BYTE *)g_addrCand < (BYTE *)mbi.BaseAddress + mbi.RegionSize) ||
                      (g_addrPrev &&
                       (BYTE *)mbi.BaseAddress <= (BYTE *)g_addrPrev &&
                       (BYTE *)g_addrPrev < (BYTE *)mbi.BaseAddress + mbi.RegionSize);

        if (usable && !isOurs && mbi.RegionSize > (SIZE_T)256 * 1024 * 1024)
            g_skippedBigBytes += (double)mbi.RegionSize;

        if (usable && !isOurs && mbi.RegionSize < (SIZE_T)2048 * 1024 * 1024) {
            ++scannedRegions;
            scannedBytes += mbi.RegionSize;
            BYTE *p = (BYTE *)mbi.BaseAddress;
            BYTE *end = p + mbi.RegionSize - 12;
            __try {
                for (; p < end; p += 4) {
                    const float *f = (const float *)p;
                    // Wide tolerance on purpose: the pawn sits some unknown
                    // offset from the eye, so anything in the neighbourhood is
                    // a candidate. The delta test does the real filtering.
                    // Tight horizontally, loose vertically. The camera sits at
                    // the pawn's eye - directly above it bar a small forward
                    // offset - so X and Y are near-identical while Z differs by
                    // most of a capsule. Loose horizontal bounds match the
                    // level's own vertex data, which surrounds the player by
                    // definition and floods the buffer.
                    if (fabsf(f[0] - ref[0]) < 40.0f) {
                        ++looseHits;
                        if (fabsf(f[1] - ref[1]) < 40.0f &&
                            fabsf(f[2] - ref[2]) < 250.0f &&
                            !isInsideModule(f)) {
                            g_addrCand[g_addrCandCount] = (float *)p;
                            g_addrPrev[g_addrCandCount * 3 + 0] = f[0];
                            g_addrPrev[g_addrCandCount * 3 + 1] = f[1];
                            g_addrPrev[g_addrCandCount * 3 + 2] = f[2];
                            ++g_addrCandCount;
                            if (g_addrCandCount >= MAX_ADDR_CANDIDATES) break;
                        }
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // Region vanished or turned unreadable mid-scan; skip it.
            }
        }
        addr = (BYTE *)mbi.BaseAddress + mbi.RegionSize;
    }
    logf("scan (%.1f %.1f %.1f): regions=%d scanned=%d bytes=%llu "
         "x-only-hits=%d xyz-hits=%d over-256MB-skipped=%.0fMB",
         ref[0], ref[1], ref[2], regions, scannedRegions, scannedBytes,
         looseHits, g_addrCandCount, g_skippedBigBytes / (1024.0 * 1024.0));
}

// Keep only candidates that moved by the same vector the camera did.
static void narrowByDelta(const float *camNow) {
    if (!g_havePrevCam) return;
    float cdx = camNow[0] - g_prevCam[0];
    float cdy = camNow[1] - g_prevCam[1];
    float cdz = camNow[2] - g_prevCam[2];

    int kept = 0;
    for (int i = 0; i < g_addrCandCount; ++i) {
        float *p = g_addrCand[i];
        float *prev = &g_addrPrev[i * 3];
        bool ok = false;
        float cur[3] = {0, 0, 0};
        __try {
            cur[0] = p[0]; cur[1] = p[1]; cur[2] = p[2];
            float dx = cur[0] - prev[0];
            float dy = cur[1] - prev[1];
            float dz = cur[2] - prev[2];
            // Proportional tolerance, not a fixed one. Mirror's Edge smooths
            // its camera, so the eye lags the pawn and their per-interval
            // displacements differ by a few percent - a hard +/-3 uu test
            // rejects the very value we are looking for.
            float mag = fabsf(cdx) + fabsf(cdy) + fabsf(cdz);
            float tol = 6.0f + 0.40f * mag;
            ok = fabsf(dx - cdx) < tol && fabsf(dy - cdy) < tol &&
                 fabsf(dz - cdz) < tol &&
                 // Must actually be moving with us, not merely static.
                 (fabsf(dx) + fabsf(dy) + fabsf(dz)) > 0.2f * mag;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        if (ok) {
            g_addrCand[kept] = p;
            g_addrPrev[kept * 3 + 0] = cur[0];
            g_addrPrev[kept * 3 + 1] = cur[1];
            g_addrPrev[kept * 3 + 2] = cur[2];
            ++kept;
        }
    }
    g_addrCandCount = kept;
    logf("delta-narrowed to %d candidates (cam moved %.1f %.1f %.1f)",
         kept, cdx, cdy, cdz);
    if (kept > 0 && kept <= 32) {
        g_playerPos = g_addrCand[0];
        g_addrLocked = true;
        logf("locked candidate 1/%d at %p", kept, (void *)g_playerPos);
    }
}

// Command file: "pos X Y Z" to place the camera, "off" to hand control back,
// "rel DX DY DZ" to nudge from wherever it currently is,
// "scan" to begin locating the player in memory, "tp X Y Z" to teleport it.
static void pollCameraCommand() {
    if (!g_camCmdPath[0]) return;
    if (GetFileAttributesA(g_camCmdPath) == INVALID_FILE_ATTRIBUTES) return;

    FILE *f = nullptr;
    if (fopen_s(&f, g_camCmdPath, "r") != 0 || !f) return;
    char verb[32] = {0};
    float x = 0, y = 0, z = 0, a = 0, b = 0;
    int n = fscanf_s(f, "%31s %f %f %f %f %f", verb, (unsigned)sizeof(verb),
                     &x, &y, &z, &a, &b);
    fclose(f);
    DeleteFileA(g_camCmdPath);
    if (n < 1) return;

    if (_stricmp(verb, "pawn") == 0) {
        // Calibrate the UE3 object offsets against the camera; log only.
        g_pawnEnabled = true;
        pawnCalibrate();
    } else if (_stricmp(verb, "pawnsnap") == 0) {
        // pawnsnap [xyTolerance [dzLo [dzHi]]] - the acceptance box around the
        // camera. Tunable from Python so narrowing it does not cost a rebuild.
        g_pawnEnabled = true;
        if (n >= 2) g_calibXY = x;
        if (n >= 3) g_calibZLo = y;
        if (n >= 4) g_calibZHi = z;
        pawnSnapshot();
    } else if (_stricmp(verb, "pawndiff") == 0) {
        // `pawndiff <from> <to>` hunts one exact transition; bare is the
        // noisy any-move-byte mode.
        pawnDiff(n >= 2 ? (int)x : 0, n >= 3 ? (int)y : 0);
    } else if (_stricmp(verb, "pawnset") == 0) {
        // pawnset X Y Z [yawDeg] - place the pawn, zero its velocity. The
        // anchor for A/B input replay; see pawnSet.
        if (n >= 4) {
            g_pawnEnabled = true;
            pawnSet(x, y, z, n >= 5, a);
        } else logf("pawnset: need X Y Z [yawDeg]");
    } else if (_stricmp(verb, "animscan") == 0) {
        // Every skeletal mesh component's tree and the audio device's arrays.
        g_animScanLeft = 1;
    } else if (_stricmp(verb, "animdump") == 0) {
        // animdump [N] - log the first-person AnimTree, up to N nodes.
        g_animDumpLeft = n >= 2 ? (int)x : 400;
        g_animDumpWords = n >= 3 && y > 0.0f;
    } else if (_stricmp(verb, "pawnstate") == 0) {
        pawnStateHunt();
    } else if (_stricmp(verb, "pawnwhy") == 0) {
        g_pawnEnabled = true;
        if (n >= 2) g_calibXY = x;
        if (n >= 3) g_calibZLo = y;
        if (n >= 4) g_calibZHi = z;
        pawnWhy();
    } else if (_stricmp(verb, "scan") == 0) {
        if (g_lastCamValid) {
            // An optional Z offset lets us hunt the pawn's Location rather than
            // the camera's. Faith's eye sits BaseEyeHeight = 76 uu above the
            // capsule centre, and the authoritative Location is the one the
            // engine integrates - the camera position is a derived copy that is
            // rewritten every frame, so writing to it has no effect.
            float dz = (n >= 2) ? x : 0.0f;
            g_scanRef[0] = g_lastCamPos[0];
            g_scanRef[1] = g_lastCamPos[1];
            g_scanRef[2] = g_lastCamPos[2] + dz;
            g_scanZOffset = dz;
            scanForPosition(g_scanRef);
            g_scanState = 2;              // narrow on subsequent frames
            g_addrLocked = false;
            g_playerPos = nullptr;
            g_candIndex = 0;
        } else {
            logf("scan requested but no camera position known yet");
        }
    } else if (_stricmp(verb, "find") == 0) {
        if (g_addrCandCount > 0 && g_lastCamValid) {
            // "find 0" sweeps everything the scan produced. The shape filter
            // encodes an ASSUMPTION about where the eye sits relative to the
            // pawn; if that assumption is wrong it silently discards the very
            // address being looked for, so it must be possible to skip.
            if ((n < 2) || (x != 0.0f)) filterCandidatesToPawnShape();
            else logf("pawn sweep: shape filter skipped, testing all %d",
                      g_addrCandCount);
            g_findState = 1;
            g_findIndex = 0;
            g_findFrames = 0;
            g_findTested = 0;
            g_pawnFound = false;
            g_pawnAddr = nullptr;
            logf("pawn sweep started over %d candidates", g_addrCandCount);
        } else {
            logf("pawn sweep needs a scan first");
        }
    } else if (_stricmp(verb, "next") == 0) {
        // Several addresses can track the position; only one is authoritative.
        if (g_addrCandCount > 0) {
            g_candIndex = (g_candIndex + 1) % g_addrCandCount;
            g_playerPos = g_addrCand[g_candIndex];
            g_addrLocked = true;
            logf("candidate %d/%d -> %p", g_candIndex + 1, g_addrCandCount,
                 (void *)g_playerPos);
        }
    } else if (_stricmp(verb, "tp") == 0 && n >= 4) {
        // Hold the value rather than writing once. The engine integrates the
        // pawn every tick, so a single write is overwritten before the next
        // frame is drawn - which is exactly why testing candidates with one-shot
        // writes made every one of them look inert.
        g_tpPos[0] = x; g_tpPos[1] = y; g_tpPos[2] = z;
        g_tpActive = true;
        g_tpFrames = 0;
        logf("hold position -> %.1f %.1f %.1f", x, y, z);
    } else if (_stricmp(verb, "release") == 0) {
        g_tpActive = false;
        logf("position hold released");
    } else if (_stricmp(verb, "rec") == 0) {
        if (n >= 2 && x != 0.0f)
            startRecording((int)y, (int)z, (int)a);
        else
            stopRecording();
    } else if (_stricmp(verb, "off") == 0) {
        // Also drop noclip, or it would re-arm the override on the next frame
        // and camera_release() would appear to do nothing.
        g_noclip = false;
        g_freeCamEnabled = false;
        g_orientEnabled = false;
        logf("freecam off");
    } else if (_stricmp(verb, "cursor") == 0) {
        g_confineCursor = (n < 2) || (x != 0.0f);
        if (!g_confineCursor) ClipCursor(NULL);
        logf("cursor confinement %s", g_confineCursor ? "ON" : "OFF");
    } else if (_stricmp(verb, "noclip") == 0) {
        bool want = (n < 2) || (x != 0.0f);
        if (want && !g_noclip && g_lastCamValid) {
            g_ncPos[0] = g_lastCamPos[0];
            g_ncPos[1] = g_lastCamPos[1];
            g_ncPos[2] = g_lastCamPos[2];
            g_ncYaw = g_lastYaw;
            g_ncPitch = g_lastPitch;
            g_ncHaveGame = false;
            g_noclip = true;
            releaseHeldKeys();
        } else if (!want && g_noclip) {
            g_noclip = false;
            g_freeCamEnabled = false;
            g_orientEnabled = false;
        }
        logf("noclip %s (via command)", g_noclip ? "ON" : "OFF");
    } else if (_stricmp(verb, "pose") == 0 && n >= 6) {
        // Place and aim in ONE command. Streaming a flight path as separate
        // "pos" and "look" writes lets the poll land between them, so a frame
        // renders at the new position with the old heading and the motion
        // judders.
        const float kDeg = 3.14159265358979f / 180.0f;
        g_camPos[0] = x; g_camPos[1] = y; g_camPos[2] = z;
        g_camYaw = a * kDeg;
        g_camPitch = b * kDeg;
        g_haveCamPos = true;
        g_orientEnabled = true;
        g_freeCamEnabled = true;
        // Carry noclip with us. tickNoclip rewrites g_camPos every frame from
        // its own state, so without this a scripted pose is overwritten before
        // it is ever drawn - the shot comes back identical every time and looks
        // like the command was ignored. Syncing instead of fighting lets a
        // script place the camera and the keys carry on from there.
        syncNoclipToCamera();
    } else if (_stricmp(verb, "look") == 0 && n >= 3) {
        // Absolute aim, degrees, in the same convention the state file reports.
        const float kDeg = 3.14159265358979f / 180.0f;
        g_camYaw = x * kDeg;
        g_camPitch = y * kDeg;
        g_orientEnabled = true;
        if (!g_haveCamPos && g_lastCamValid) {
            g_camPos[0] = g_lastCamPos[0];
            g_camPos[1] = g_lastCamPos[1];
            g_camPos[2] = g_lastCamPos[2];
            g_haveCamPos = true;
        }
        g_freeCamEnabled = true;
        syncNoclipToCamera();
        logf("freecam look yaw=%.2f pitch=%.2f", x, y);
    } else if (_stricmp(verb, "lookoff") == 0) {
        g_orientEnabled = false;
        logf("freecam orientation released");
    } else if (_stricmp(verb, "trace") == 0) {
        // `trace` / `trace 1` starts a fresh log, `trace 0` closes it. Starting
        // truncates: each trial is its own file, so a stale tail from the
        // previous trial can never be read as part of this one.
        if (n >= 2 && x == 0.0f) traceStop();
        else traceStart();
    } else if (_stricmp(verb, "pos") == 0 && n >= 4) {
        g_camPos[0] = x; g_camPos[1] = y; g_camPos[2] = z;
        g_haveCamPos = true; g_freeCamEnabled = true;
        syncNoclipToCamera();
        logf("freecam pos %.2f %.2f %.2f", x, y, z);
    } else if (_stricmp(verb, "rel") == 0 && n >= 4 && g_lastCamValid) {
        g_camPos[0] = g_lastCamPos[0] + x;
        g_camPos[1] = g_lastCamPos[1] + y;
        g_camPos[2] = g_lastCamPos[2] + z;
        g_haveCamPos = true; g_freeCamEnabled = true;
        syncNoclipToCamera();
        logf("freecam rel -> %.2f %.2f %.2f", g_camPos[0], g_camPos[1], g_camPos[2]);
    }
}

// ---------------------------------------------------------------- trace log
//
// medge_camera.state cannot be used to measure movement, and it is worth being
// explicit about why rather than discovering it from bad numbers. It is
// rewritten every 2 frames with the file truncated each time, it carries no
// frame number and no timestamp, and fopen_s opens it non-shareable - so a
// reader cannot tell a fresh sample from a repeat, cannot know when the sample
// was taken, and intermittently reads a torn or empty file. Velocity is a
// derivative; all three of those destroy it.
//
// So this is a separate, append-only channel: one line per frame, with the
// frame number and a QueryPerformanceCounter timestamp, opened once with
// _SH_DENYNO so Python can tail it while the game writes. It exists only
// between `trace on` and `trace off`, costs one fprintf per frame, and is what
// the movement A/B harness measures from.
// Left armed for a whole play session, so it cannot be allowed to grow without
// bound - at 60 Hz this is about 4 KB/s, 15 MB an hour, written into the game's
// own directory. Two files used as a ring: when the live one passes the cap it
// is rotated onto the spare and a fresh one started. That keeps AT LEAST
// TRACE_MAX_BYTES of history and at most twice it, for one rename per rotation
// rather than any per-line shuffling. Readers concatenate .1 then the live one.
#define TRACE_MAX_BYTES (8 * 1024 * 1024)

static FILE *g_trace = nullptr;
static char g_tracePath[MAX_PATH] = {0};
static char g_tracePathOld[MAX_PATH] = {0};
static long g_traceBytes = 0;
static LARGE_INTEGER g_traceFreq = {0};
static LARGE_INTEGER g_traceStart = {0};

static void traceStop() {
    if (!g_trace) return;
    fclose(g_trace);
    g_trace = nullptr;
    logf("trace stopped at frame %d", g_frame);
}

static void traceOpen(bool fresh) {
    g_trace = _fsopen(g_tracePath, "w", _SH_DENYNO);
    if (!g_trace) { logf("trace: could not open %s", g_tracePath); return; }
    g_traceBytes = 0;
    if (fresh) {
        QueryPerformanceFrequency(&g_traceFreq);
        QueryPerformanceCounter(&g_traceStart);
    }
    // A header, so a reader never has to guess the column order or the units.
    // Written to each rotated part too, so a part is self-describing on its own.
    g_traceBytes += fprintf(g_trace,
        "# medge trace: frame t_s cam_x cam_y cam_z yaw_deg pitch_deg valid "
        "fwd_x fwd_y fwd_z up_x up_y up_z\n");
    g_traceBytes += fprintf(g_trace,
        "# positions are UNREAL UNITS (1 uu = 1 cm), camera (eye), game frame rate\n");
    fflush(g_trace);
}

// Rotate rather than truncate. Truncating in place would throw away the most
// recent history at exactly the moment it filled up - which is the history
// somebody is about to ask about.
static void traceRotate() {
    if (!g_trace) return;
    fclose(g_trace);
    g_trace = nullptr;
    if (g_tracePathOld[0])
        MoveFileExA(g_tracePath, g_tracePathOld, MOVEFILE_REPLACE_EXISTING);
    traceOpen(false);          // same clock: timestamps stay continuous
    logf("trace rotated at frame %d", g_frame);
}

static void traceStart() {
    traceStop();
    if (!g_tracePath[0]) return;
    if (g_tracePathOld[0]) DeleteFileA(g_tracePathOld);   // a new take, not a resume
    traceOpen(true);
    if (g_trace) logf("trace started -> %s (rotating at %d MB)",
                      g_tracePath, TRACE_MAX_BYTES / (1024 * 1024));
}

static void traceTick() {
    if (!g_trace) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = g_traceFreq.QuadPart
             ? double(now.QuadPart - g_traceStart.QuadPart) / double(g_traceFreq.QuadPart)
             : 0.0;
    const float kRad = 180.0f / 3.14159265358979f;
    // `valid` is published rather than the sample being dropped: a gap in the
    // series is information (a loading screen, a cutscene), and silently
    // omitting it would let the reader interpolate straight across it.
    // Columns 8..13 are the forward and up axes. APPENDED, so a reader that
    // only knows the first eight fields keeps working unchanged.
    int n = fprintf(g_trace,
                    "%d %.6f %.4f %.4f %.4f %.4f %.4f %d "
                    "%.5f %.5f %.5f %.5f %.5f %.5f\n",
                    g_frame, t, g_lastCamPos[0], g_lastCamPos[1], g_lastCamPos[2],
                    g_lastYaw * kRad, g_lastPitch * kRad, g_lastCamValid ? 1 : 0,
                    g_lastFwd[0], g_lastFwd[1], g_lastFwd[2],
                    g_lastUp[0], g_lastUp[1], g_lastUp[2]);
    if (n > 0) g_traceBytes += n;
    // Flushed every frame on purpose. The harness reads this file while the
    // game is still running, and a buffered last second is exactly the second
    // that matters when a trial ends on a landing.
    fflush(g_trace);
    if (g_traceBytes >= TRACE_MAX_BYTES) traceRotate();
}

static void writeCameraState() {
    if (!g_camStatePath[0] || !g_lastCamValid) return;
    FILE *f = nullptr;
    if (fopen_s(&f, g_camStatePath, "w") != 0 || !f) return;
    // The first triple is always the GAME's camera - where the player is
    // looking from - because it is read before any patch is applied. When
    // noclip is flying, that is no longer where the picture is taken from, so
    // the free camera's own pose is published alongside it rather than instead.
    const float kRad = 180.0f / 3.14159265358979f;
    fprintf(f, "%.4f %.4f %.4f %d %d %d %.4f %.4f %d %.4f %.4f %.4f %.4f %.4f "
               "%d %d %d %d\n",
            g_lastCamPos[0], g_lastCamPos[1], g_lastCamPos[2],
            g_freeCamEnabled ? 1 : 0, g_addrCandCount, g_addrLocked ? 1 : 0,
            g_lastYaw * kRad, g_lastPitch * kRad,
            g_noclip ? 1 : 0, g_ncPos[0], g_ncPos[1], g_ncPos[2],
            g_ncYaw * kRad, g_ncPitch * kRad,
            g_pawnFound ? 1 : 0, g_findState, g_findIndex, g_addrCandCount);
    fclose(f);
}

// Narrowing runs a frame after the scan, once the camera has moved, so that
// unrelated memory that merely happened to hold the same value drops out.
static void tickPlayerScan() {
    if (g_scanState != 2 || !g_lastCamValid) return;

    if (!g_havePrevCam) {
        g_prevCam[0] = g_lastCamPos[0];
        g_prevCam[1] = g_lastCamPos[1];
        g_prevCam[2] = g_lastCamPos[2];
        g_havePrevCam = true;
        return;
    }
    // Wait for real movement: with the camera still, every candidate has a zero
    // delta and the test teaches nothing.
    float d = fabsf(g_lastCamPos[0] - g_prevCam[0]) +
              fabsf(g_lastCamPos[1] - g_prevCam[1]) +
              fabsf(g_lastCamPos[2] - g_prevCam[2]);
    if (d < 20.0f) return;

    narrowByDelta(g_lastCamPos);
    g_prevCam[0] = g_lastCamPos[0];
    g_prevCam[1] = g_lastCamPos[1];
    g_prevCam[2] = g_lastCamPos[2];
    if (g_addrCandCount == 0) g_scanState = 0;
}

static float g_aspect = 16.0f / 9.0f;

static HRESULT WINAPI MySetVertexShaderConstantF(IDirect3DDevice9 *self,
                                                 UINT startRegister,
                                                 const float *data, UINT count) {
    if (g_constDump && g_dumpFramesLeft > 0 && data && count > 0 && count <= 256) {
        // One line per register: frame, call ordinal, register index, 4 floats.
        for (UINT i = 0; i < count; ++i) {
            const float *v = data + i * 4;
            fprintf(g_constDump, "%d %d %u %.6f %.6f %.6f %.6f\n",
                    g_frame, g_dumpCalls, startRegister + i, v[0], v[1], v[2], v[3]);
        }
        ++g_dumpCalls;
    }

    // Scan for the shared view-projection and, if the free camera is armed,
    // substitute a copy with the translation rewritten. The game's own buffer
    // is never modified - we hand the real call a patched duplicate.
    if (data && count >= 4 && count <= 64) {
        static float patched[64 * 4];
        bool copied = false;

        for (UINT i = 0; i + 4 <= count; ++i) {
            const float *m = data + i * 4;

            // Structural test only: unit-length forward column and sane scales.
            // Deliberately NOT the aspect ratio - that drops the camera-
            // anchored passes that carry a frustum of their own.
            float fx = m[0 * 4 + 3], fy = m[1 * 4 + 3], fz = m[2 * 4 + 3];
            float n = sqrtf(fx * fx + fy * fy + fz * fz);
            if (fabsf(n - 1.0f) > 5e-3f) continue;
            float sx = sqrtf(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
            float sy = sqrtf(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
            if (sx < 0.2f || sx > 20.0f || sy < 0.2f || sy > 20.0f) continue;
            ++g_stageStructural;

            float pos[3];
            if (!cameraPositionFrom(m, pos)) continue;
            ++g_stageSolved;

            // Only the aspect-matching matrix votes on where the camera IS:
            // screen-space quads are near 1:1 and would otherwise win the poll.
            if (fabsf((sy / sx) - g_aspect) >= 0.02f) {
                float fwd[3] = {fx, fy, fz};
                float right[3] = {m[0] / sx, m[4] / sx, m[8] / sx};
                float up[3] = {m[1] / sy, m[5] / sy, m[9] / sy};
                noteOffAspectCandidate(pos, sy / sx, fwd, right, up);
            } else {
                ++g_stageAspect;
                noteCameraCandidate(pos);
                g_lastYaw = atan2f(fy, fx);
                g_lastPitch = asinf(fz < -1.0f ? -1.0f : (fz > 1.0f ? 1.0f : fz));
                // The full basis, not just the forward axis. Yaw and pitch
                // alone cannot express ROLL - rotation about the view axis -
                // and asin(fz) tumbles when the camera passes vertical, which
                // a somersault does: a recorded roll came back as a +129 deg
                // "pitch dip" that was the decomposition falling over, not the
                // camera moving. Columns 0 and 1 are the right and up axes
                // scaled by the projection terms sx and sy, so normalising
                // recovers them. With forward, right and up there is no
                // singularity and roll is just a rotation of the basis.
                g_lastFwd[0] = fx; g_lastFwd[1] = fy; g_lastFwd[2] = fz;
                g_lastRight[0] = m[0] / sx; g_lastRight[1] = m[4] / sx;
                g_lastRight[2] = m[8] / sx;
                g_lastUp[0] = m[1] / sy; g_lastUp[1] = m[5] / sy;
                g_lastUp[2] = m[9] / sy;
            }

            if (!g_freeCamEnabled || !g_haveCamPos || !g_lastCamValid) continue;

            // Move anything already sitting at the eye. That takes in the bare
            // view-projection, the camera-anchored passes at r234, and the
            // previous-frame matrix motion blur reprojects through - a frame's
            // motion away, so it moves with us and the smearing stops. The sky
            // solves to the origin and screen-space quads to half the render
            // target, so both stay outside the gate, untouched.
            float dx = pos[0] - g_lastCamPos[0];
            float dy = pos[1] - g_lastCamPos[1];
            float dz = pos[2] - g_lastCamPos[2];
            if (dx * dx + dy * dy + dz * dz > 200.0f * 200.0f) continue;

            if (!copied) {
                memcpy(patched, data, count * 4 * sizeof(float));
                copied = true;
            }
            // Aim first (about this matrix's own eye), then move the eye to
            // the requested spot. Each matrix keeps its own projection.
            if (g_orientEnabled)
                reorientCamera(patched + i * 4, pos, g_camYaw, g_camPitch);
            float delta[3] = {g_camPos[0] - pos[0],
                              g_camPos[1] - pos[1],
                              g_camPos[2] - pos[2]};
            translateCamera(patched + i * 4, delta);
        }

        if (copied) return g_realSetVSConstF(self, startRegister, patched, count);
    }

    return g_realSetVSConstF(self, startRegister, data, count);
}

// Force windowed presentation. Shared by CreateDevice and Reset, because doing
// it in only one of them is the same bug twice: the game asks for fullscreen
// every time it hands over presentation parameters.
static bool forceWindowed(D3DPRESENT_PARAMETERS *pp) {
    if (!pp || pp->Windowed) return false;
    pp->Windowed = TRUE;
    // Both are required to be zero/unknown for windowed presentation; leaving
    // a fullscreen refresh rate set makes the call fail with D3DERR_INVALIDCALL.
    pp->FullScreen_RefreshRateInHz = 0;
    pp->BackBufferFormat = D3DFMT_UNKNOWN;
    return true;
}

// Every device Reset re-applies the game's own presentation parameters, and
// Mirror's Edge has no windowed mode - so an unpatched Reset puts the device
// back into fullscreen and undoes MyCreateDevice's work. The game resets
// whenever it applies video settings or recovers a lost device, which is why
// leaving this alone showed up as the window flapping between fullscreen and
// windowed about once a second: one path forcing windowed, another restoring
// fullscreen, neither winning.
static HRESULT WINAPI MyReset(IDirect3DDevice9 *self, D3DPRESENT_PARAMETERS *pp) {
    int w = 0, h = 0;
    if (pp) {
        w = (int)pp->BackBufferWidth;
        h = (int)pp->BackBufferHeight;
        if (forceWindowed(pp))
            logf("Reset %dx%d requested fullscreen -> forced windowed", w, h);
    }
    HRESULT hr = g_realReset(self, pp);
    // The reset rebuilds the swap chain and the game re-applies its own window
    // style with it, so put the frame back the way CreateDevice does.
    if (SUCCEEDED(hr) && g_gameWindow && w > 0 && h > 0)
        makeWindowFramed(g_gameWindow, w, h);
    logf("Reset -> 0x%08lx; camera diag follows for 5 frames", (unsigned long)hr);
    g_camDiagFrames = 5;
    return hr;
}

static void patchDeviceVTable(IDirect3DDevice9 *dev) {
    if (!dev || g_realPresent) return;
    void **vtbl = *(void ***)dev;
    // IDirect3DDevice9: ... 16 Reset, 17 Present, ... 94 SetVertexShaderConstantF
    const int kReset = 16;
    const int kPresent = 17;
    const int kSetVSConstF = 94;
    DWORD old = 0;

    if (VirtualProtect(&vtbl[kReset], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) {
        g_realReset = (Reset_t)vtbl[kReset];
        vtbl[kReset] = (void *)&MyReset;
        VirtualProtect(&vtbl[kReset], sizeof(void *), old, &old);
        logf("patched IDirect3DDevice9::Reset (real=%p)", (void *)g_realReset);
    }

    if (VirtualProtect(&vtbl[kPresent], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) {
        g_realPresent = (Present_t)vtbl[kPresent];
        vtbl[kPresent] = (void *)&MyPresent;
        VirtualProtect(&vtbl[kPresent], sizeof(void *), old, &old);
        logf("patched IDirect3DDevice9::Present (real=%p)", (void *)g_realPresent);
    }

    if (VirtualProtect(&vtbl[kSetVSConstF], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) {
        g_realSetVSConstF = (SetVSConstF_t)vtbl[kSetVSConstF];
        vtbl[kSetVSConstF] = (void *)&MySetVertexShaderConstantF;
        VirtualProtect(&vtbl[kSetVSConstF], sizeof(void *), old, &old);
        logf("patched IDirect3DDevice9::SetVertexShaderConstantF (real=%p)",
             (void *)g_realSetVSConstF);
    }

    // The colour-oracle trio. Canonical vtable indices, cross-checked by the
    // two above (Present 17, SetVSConstF 94 both canonical for this device).
    const int kSetTexture = 65;
    const int kDrawIndexed = 82;
    const int kSetPSConstF = 109;

    if (VirtualProtect(&vtbl[kSetTexture], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) {
        g_realSetTexture = (SetTexture_t)vtbl[kSetTexture];
        vtbl[kSetTexture] = (void *)&MySetTexture;
        VirtualProtect(&vtbl[kSetTexture], sizeof(void *), old, &old);
        logf("patched IDirect3DDevice9::SetTexture (real=%p)", (void *)g_realSetTexture);
    }

    if (VirtualProtect(&vtbl[kDrawIndexed], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) {
        g_realDrawIndexed = (DrawIndexed_t)vtbl[kDrawIndexed];
        vtbl[kDrawIndexed] = (void *)&MyDrawIndexedPrimitive;
        VirtualProtect(&vtbl[kDrawIndexed], sizeof(void *), old, &old);
        logf("patched IDirect3DDevice9::DrawIndexedPrimitive (real=%p)",
             (void *)g_realDrawIndexed);
    }

    if (VirtualProtect(&vtbl[kSetPSConstF], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) {
        g_realSetPSConstF = (SetPSConstF_t)vtbl[kSetPSConstF];
        vtbl[kSetPSConstF] = (void *)&MySetPixelShaderConstantF;
        VirtualProtect(&vtbl[kSetPSConstF], sizeof(void *), old, &old);
        logf("patched IDirect3DDevice9::SetPixelShaderConstantF (real=%p)",
             (void *)g_realSetPSConstF);
    }
}

static HRESULT WINAPI MyCreateDevice(IDirect3D9 *self, UINT adapter,
                                     D3DDEVTYPE type, HWND focusWindow,
                                     DWORD behaviorFlags,
                                     D3DPRESENT_PARAMETERS *pp,
                                     IDirect3DDevice9 **ppDevice) {
    HWND target = focusWindow;
    int w = 0, h = 0;

    if (pp) {
        w = (int)pp->BackBufferWidth;
        h = (int)pp->BackBufferHeight;
        logf("CreateDevice requested %dx%d windowed=%d fmt=%d refresh=%u",
             w, h, (int)pp->Windowed, (int)pp->BackBufferFormat,
             pp->FullScreen_RefreshRateInHz);

        if (forceWindowed(pp)) logf("  -> forced windowed");
        if (pp->hDeviceWindow) target = pp->hDeviceWindow;
    }

    if (target && w > 0 && h > 0) makeWindowFramed(target, w, h);
    // Remembered so the noclip keys only fire while the game has focus -
    // GetAsyncKeyState is global and would otherwise react to typing elsewhere.
    if (target) {
        g_gameWindow = target;
        installWndProc(target);
    }

    HRESULT hr = g_realCreateDevice(self, adapter, type, focusWindow,
                                    behaviorFlags, pp, ppDevice);
    logf("  real CreateDevice -> 0x%08lX", (unsigned long)hr);

    // Reassert the frame: the game repositions its window during startup.
    if (SUCCEEDED(hr) && target && w > 0 && h > 0) makeWindowFramed(target, w, h);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) patchDeviceVTable(*ppDevice);
    return hr;
}

static void patchVTable(IDirect3D9 *d3d) {
    if (!d3d || g_realCreateDevice) return;
    void **vtbl = *(void ***)d3d;
    // IDirect3D9: QueryInterface, AddRef, Release, RegisterSoftwareDevice,
    // GetAdapterCount, GetAdapterIdentifier, GetAdapterModeCount,
    // EnumAdapterModes, GetAdapterDisplayMode, CheckDeviceType,
    // CheckDeviceFormat, CheckDeviceMultiSampleType, CheckDepthStencilMatch,
    // CheckDeviceFormatConversion, GetDeviceCaps, GetAdapterMonitor,
    // CreateDevice  <- index 16
    const int kCreateDevice = 16;
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[kCreateDevice], sizeof(void *),
                        PAGE_EXECUTE_READWRITE, &old)) {
        logf("VirtualProtect failed: %lu", GetLastError());
        return;
    }
    g_realCreateDevice = (CreateDevice_t)vtbl[kCreateDevice];
    vtbl[kCreateDevice] = (void *)&MyCreateDevice;
    VirtualProtect(&vtbl[kCreateDevice], sizeof(void *), old, &old);
    logf("patched IDirect3D9::CreateDevice (real=%p)", (void *)g_realCreateDevice);
}

// --- exports ------------------------------------------------------------------

// d3d9.h already declares these as dllimport, so the proxy implementations get
// distinct names and are exported under the real ones via linker aliases. The
// @N suffixes are __stdcall decoration for x86: the byte count of the argument
// list. Getting one wrong produces an unresolved-alias link error rather than
// anything subtle.
#pragma comment(linker, "/EXPORT:Direct3DCreate9=_Proxy_Direct3DCreate9@4")
#pragma comment(linker, "/EXPORT:Direct3DCreate9Ex=_Proxy_Direct3DCreate9Ex@8")
#pragma comment(linker, "/EXPORT:D3DPERF_BeginEvent=_Proxy_D3DPERF_BeginEvent@8")
#pragma comment(linker, "/EXPORT:D3DPERF_EndEvent=_Proxy_D3DPERF_EndEvent@0")
#pragma comment(linker, "/EXPORT:D3DPERF_SetMarker=_Proxy_D3DPERF_SetMarker@8")
#pragma comment(linker, "/EXPORT:D3DPERF_GetStatus=_Proxy_D3DPERF_GetStatus@0")
#pragma comment(linker, "/EXPORT:D3DPERF_SetOptions=_Proxy_D3DPERF_SetOptions@4")

typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);

extern "C" IDirect3D9 *WINAPI Proxy_Direct3DCreate9(UINT sdk) {
    auto real = (Direct3DCreate9_t)GetProcAddress(g_realD3D9, "Direct3DCreate9");
    if (!real) {
        logf("real Direct3DCreate9 missing");
        return nullptr;
    }
    IDirect3D9 *d3d = real(sdk);
    logf("Direct3DCreate9(%u) -> %p", sdk, (void *)d3d);
    patchVTable(d3d);
    return d3d;
}

// Forwarded verbatim; the game may call these, and the loader will not fall back
// to System32 for a name this DLL exports.
extern "C" HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex **out) {
    typedef HRESULT(WINAPI * fn_t)(UINT, IDirect3D9Ex **);
    auto real = (fn_t)GetProcAddress(g_realD3D9, "Direct3DCreate9Ex");
    return real ? real(sdk, out) : E_NOTIMPL;
}

extern "C" int WINAPI Proxy_D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n) {
    typedef int(WINAPI * fn_t)(D3DCOLOR, LPCWSTR);
    auto real = (fn_t)GetProcAddress(g_realD3D9, "D3DPERF_BeginEvent");
    return real ? real(c, n) : 0;
}

extern "C" int WINAPI Proxy_D3DPERF_EndEvent(void) {
    typedef int(WINAPI * fn_t)(void);
    auto real = (fn_t)GetProcAddress(g_realD3D9, "D3DPERF_EndEvent");
    return real ? real() : 0;
}

extern "C" void WINAPI Proxy_D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n) {
    typedef void(WINAPI * fn_t)(D3DCOLOR, LPCWSTR);
    auto real = (fn_t)GetProcAddress(g_realD3D9, "D3DPERF_SetMarker");
    if (real) real(c, n);
}

extern "C" DWORD WINAPI Proxy_D3DPERF_GetStatus(void) {
    typedef DWORD(WINAPI * fn_t)(void);
    auto real = (fn_t)GetProcAddress(g_realD3D9, "D3DPERF_GetStatus");
    return real ? real() : 0;
}

extern "C" void WINAPI Proxy_D3DPERF_SetOptions(DWORD o) {
    typedef void(WINAPI * fn_t)(DWORD);
    auto real = (fn_t)GetProcAddress(g_realD3D9, "D3DPERF_SetOptions");
    if (real) real(o);
}

// All paths are derived from this DLL's own directory rather than the
// environment. The game is not started by us: the Steam DRM stub hands off to
// steam.exe, which spawns the real process with Steam's environment, so any
// variables we set are lost. The DLL's location is the one thing we can rely on.
static void pathInDllDir(char *out, const char *leaf) {
    strcpy_s(out, MAX_PATH, g_dllDir);
    strcat_s(out, MAX_PATH, leaf);
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        GetModuleFileNameA(self, g_dllDir, MAX_PATH);
        char *slash = strrchr(g_dllDir, '\\');
        if (slash) slash[1] = '\0';

        char logPath[MAX_PATH];
        pathInDllDir(logPath, "medge_hook.log");
        // _fsopen with _SH_DENYNO so the log stays readable while the game runs;
        // a plain fopen holds an exclusive lock and the log is only useful live.
        g_log = _fsopen(logPath, "w", _SH_DENYNO);
        logf("dll dir: %s", g_dllDir);

        pathInDllDir(g_triggerPath, "medge_capture.trigger");
        logf("capture trigger file: %s", g_triggerPath);

        pathInDllDir(g_drawTrigger, "medge_drawlog.trigger");
        logf("draw-log trigger file: %s", g_drawTrigger);

        pathInDllDir(g_shotTrigger, "medge_shot.trigger");
        pathInDllDir(g_shotOut, "medge_frame.bmp");
        logf("frame-grab trigger: %s -> %s", g_shotTrigger, g_shotOut);

        pathInDllDir(g_camCmdPath, "medge_camera.cmd");
        pathInDllDir(g_camStatePath, "medge_camera.state");
        logf("camera command: %s", g_camCmdPath);

        pathInDllDir(g_tracePath, "medge_trace.txt");
        pathInDllDir(g_tracePathOld, "medge_trace.1.txt");
        logf("movement trace: %s (idle until `trace` is sent)", g_tracePath);

        char tmpl[MAX_PATH];
        pathInDllDir(tmpl, "medge_captures\\medge");
        SetEnvironmentVariableA("MEDGE_RDC_TEMPLATE", tmpl);

        // RenderDoc first, before the real d3d9 is loaded. It installs its
        // hooks when it initialises and watches subsequent module loads; if
        // d3d9.dll is already resident by then it does not intercept it, and
        // StartFrameCapture silently does nothing (IsFrameCapturing stays 0).
        initRenderDoc();

        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat_s(path, "\\d3d9.dll");
        g_realD3D9 = LoadLibraryA(path);
        logf("proxy attached, real d3d9=%p (%s)", (void *)g_realD3D9, path);
    }
    return TRUE;
}
