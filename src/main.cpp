// RTSP 멀티채널 뷰어
// 카메라 N대의 RTSP 영상을 한 창에 바둑판으로 띄우고, 끊긴 채널은 스스로 다시 연결한다.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "log.h"
#include "stream_worker.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

namespace {
std::vector<std::unique_ptr<StreamWorker>> g_workers;
int g_focus = -1;  // -1: 바둑판, 0~: 해당 채널 크게 보기

// 매 프레임 다시 만들지 않도록 글꼴과 뒷버퍼는 한 번 만들어 재사용한다.
HFONT g_small = nullptr;
HFONT g_big = nullptr;
HDC g_backDc = nullptr;
HBITMAP g_backBmp = nullptr;
HGDIOBJ g_backOldBmp = nullptr;
int g_backW = 0;
int g_backH = 0;

// 실제 화면 갱신 횟수 (디코딩 FPS와 별개)
int g_paintCount = 0;
int64_t g_paintWindowStart = 0;
double g_renderFps = 0.0;

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

COLORREF StateColor(ChannelState s) {
    switch (s) {
        case ChannelState::Live: return RGB(46, 204, 113);
        case ChannelState::Connecting: return RGB(149, 165, 166);
        case ChannelState::Reconnecting: return RGB(241, 196, 15);
        case ChannelState::Stopped: return RGB(231, 76, 60);
    }
    return RGB(255, 255, 255);
}

void FillRectColor(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void DrawFrameBorder(HDC dc, RECT r, COLORREF c, int thickness) {
    for (int i = 0; i < thickness; ++i) {
        HBRUSH b = CreateSolidBrush(c);
        FrameRect(dc, &r, b);
        DeleteObject(b);
        InflateRect(&r, -1, -1);
    }
}

// 비율을 유지한 채 타일 안에 영상을 그린다.
void DrawVideo(HDC dc, const RECT& tile, const Frame* f) {
    if (!f || f->bgra.empty() || f->width <= 0 || f->height <= 0) return;
    const int tw = tile.right - tile.left;
    const int th = tile.bottom - tile.top;
    const double scale = std::min(static_cast<double>(tw) / f->width, static_cast<double>(th) / f->height);
    const int dw = static_cast<int>(f->width * scale);
    const int dh = static_cast<int>(f->height * scale);
    const int dx = tile.left + (tw - dw) / 2;
    const int dy = tile.top + (th - dh) / 2;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = f->width;
    bi.bmiHeader.biHeight = -f->height;  // 위에서 아래로 저장된 픽셀
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, f->width, f->height, f->bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

constexpr int64_t kStallWarnMs = 1000;  // 연결은 살아 있어도 이 시간 동안 새 프레임이 없으면 화면에 경고
const COLORREF kStallColor = RGB(230, 126, 34);

void DrawTile(HDC dc, const RECT& tile, StreamWorker& w, HFONT small, HFONT big) {
    const ChannelSnapshot s = w.Snapshot();
    const int64_t now = NowMs();
    const int64_t age = s.lastFrameMs ? now - s.lastFrameMs : 0;
    const bool stalled = s.state == ChannelState::Live && age > kStallWarnMs;
    const COLORREF accent = stalled ? kStallColor : StateColor(s.state);

    FillRectColor(dc, tile, RGB(18, 20, 24));
    DrawVideo(dc, tile, s.frame.get());

    SetBkMode(dc, TRANSPARENT);

    // 영상이 멈춘 채널: 마지막 화면 위에 상태를 크게 표시
    if (s.state != ChannelState::Live || stalled) {
        std::wstring msg;
        if (stalled) {
            wchar_t buf[128];
            swprintf_s(buf, L"STALLED  no frame for %.1fs", age / 1000.0);
            msg = buf;
        } else if (s.state == ChannelState::Reconnecting) {
            const double sec = std::max<int64_t>(0, s.retryAtMs - now) / 1000.0;
            wchar_t buf[128];
            swprintf_s(buf, L"RECONNECTING  in %.1fs  (attempt %d)", sec, s.attempt);
            msg = buf;
        } else if (s.state == ChannelState::Connecting) {
            msg = s.lastFrameMs ? L"RECONNECTING  ..." : L"CONNECTING ...";
        } else {
            msg = L"STOPPED";
        }
        RECT band = tile;
        band.top = tile.top + (tile.bottom - tile.top) / 2 - 34;
        band.bottom = band.top + 68;
        FillRectColor(dc, band, RGB(0, 0, 0));
        SelectObject(dc, big);
        SetTextColor(dc, accent);
        RECT t1 = band;
        t1.bottom = band.top + 40;
        DrawTextW(dc, msg.c_str(), -1, &t1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        if (stalled) {
            SelectObject(dc, small);
            SetTextColor(dc, RGB(200, 200, 200));
            RECT t2 = band;
            t2.top = band.top + 38;
            DrawTextW(dc, L"connection open, waiting for video (reconnect after 3.0s)", -1, &t2,
                      DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        } else if (!s.lastError.empty()) {
            SelectObject(dc, small);
            SetTextColor(dc, RGB(200, 200, 200));
            RECT t2 = band;
            t2.top = band.top + 38;
            DrawTextW(dc, Widen(s.lastError).c_str(), -1, &t2, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }

    // 하단 정보 바: 채널명 · 상태 · 디코딩 fps · 해상도 · 코덱 · 재연결 횟수 · 마지막 프레임 이후 경과
    RECT bar = tile;
    bar.top = tile.bottom - 26;
    FillRectColor(dc, bar, RGB(0, 0, 0));
    SelectObject(dc, small);
    wchar_t info[256];
    const long long ageMs = s.lastFrameMs ? age : -1;
    const int fw = s.frame ? s.frame->width : 0;
    const int fh = s.frame ? s.frame->height : 0;
    swprintf_s(info, L"  %s   %S   decode %.1f fps   %dx%d   %S   reconnects %d   last frame %lld ms",
               Widen(w.Name()).c_str(), stalled ? "STALLED" : ToString(s.state), s.decodeFps, fw, fh,
               s.codec.empty() ? "-" : s.codec.c_str(), s.reconnects, ageMs);
    SetTextColor(dc, RGB(235, 235, 235));
    DrawTextW(dc, info, -1, &bar, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    // 상태 점
    RECT dot = {bar.left + 6, bar.top + 9, bar.left + 14, bar.top + 17};
    FillRectColor(dc, dot, accent);

    DrawFrameBorder(dc, tile, accent, 3);
}

void ReleaseBackBuffer() {
    if (!g_backDc) return;
    SelectObject(g_backDc, g_backOldBmp);
    DeleteObject(g_backBmp);
    DeleteDC(g_backDc);
    g_backDc = nullptr;
    g_backBmp = nullptr;
}

void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    const int cw = client.right;
    const int ch = client.bottom;

    // 깜빡임 없이 그리기 위해 메모리 DC에 먼저 그린다. 창 크기가 바뀔 때만 뒷버퍼를 다시 만든다.
    if (!g_backDc || cw != g_backW || ch != g_backH) {
        ReleaseBackBuffer();
        g_backDc = CreateCompatibleDC(wdc);
        g_backBmp = CreateCompatibleBitmap(wdc, std::max(cw, 1), std::max(ch, 1));
        g_backOldBmp = SelectObject(g_backDc, g_backBmp);
        g_backW = cw;
        g_backH = ch;
    }
    HDC dc = g_backDc;
    HFONT small = g_small;
    HFONT big = g_big;
    FillRectColor(dc, client, RGB(8, 9, 11));
    SelectObject(dc, small);

    const int n = static_cast<int>(g_workers.size());
    if (g_focus >= 0 && g_focus < n) {
        RECT tile = {4, 4, cw - 4, ch - 4};
        DrawTile(dc, tile, *g_workers[g_focus], small, big);
    } else if (n > 0) {
        const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
        const int rows = (n + cols - 1) / cols;
        const int gap = 4;
        const int tw = (cw - gap * (cols + 1)) / cols;
        const int th = (ch - gap * (rows + 1)) / rows;
        for (int i = 0; i < n; ++i) {
            const int c = i % cols;
            const int r = i / cols;
            RECT tile = {gap + c * (tw + gap), gap + r * (th + gap), 0, 0};
            tile.right = tile.left + tw;
            tile.bottom = tile.top + th;
            DrawTile(dc, tile, *g_workers[i], small, big);
        }
    }

    // 화면 갱신 FPS: 실제로 그린 횟수를 1초 단위로 센다.
    ++g_paintCount;
    const int64_t now = NowMs();
    if (now - g_paintWindowStart >= 1000) {
        g_renderFps = g_paintCount * 1000.0 / (now - g_paintWindowStart);
        g_paintCount = 0;
        g_paintWindowStart = now;
        // 영상을 가리지 않도록 창 제목에 표시한다.
        wchar_t title[128];
        swprintf_s(title, L"RTSP Multiview  -  render %.1f fps  -  keys: 1~9 focus, 0/Esc grid", g_renderFps);
        SetWindowTextW(hwnd, title);
    }

    BitBlt(wdc, 0, 0, cw, ch, dc, 0, 0, SRCCOPY);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            Paint(hwnd);
            return 0;
        case WM_KEYDOWN:
            if (wp >= '1' && wp <= '9') g_focus = static_cast<int>(wp - '1');
            if (wp == '0' || wp == VK_ESCAPE) g_focus = -1;
            return 0;
        case WM_LBUTTONDBLCLK:
            g_focus = -1;
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// channels.txt: 한 줄에 "채널이름 RTSP주소". #으로 시작하면 주석.
std::vector<std::pair<std::string, std::string>> LoadChannels(const std::string& path) {
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string name, url;
        if (ss >> name >> url) out.emplace_back(name, url);
    }
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    const std::string config = argc > 1 ? argv[1] : "channels.txt";
    const auto channels = LoadChannels(config);
    if (channels.empty()) {
        std::fprintf(stderr, "no channels in %s\n", config.c_str());
        return 1;
    }

    CreateDirectoryW(L"logs", nullptr);
    LogOpen("logs/multiview.log");
    av_log_set_level(AV_LOG_QUIET);
    avformat_network_init();

    for (const auto& [name, url] : channels) g_workers.push_back(std::make_unique<StreamWorker>(name, url));
    for (auto& w : g_workers) w->Start();
    Log("main", "started " + std::to_string(g_workers.size()) + " channels");

    g_small = CreateFontW(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    g_big = CreateFontW(26, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    g_paintWindowStart = NowMs();

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RtspMultiview";
    RegisterClassW(&wc);

    RECT r = {0, 0, 1280, 760};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"RTSP Multiview  -  keys: 1~9 focus, 0/Esc grid",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    SetTimer(hwnd, 1, 33, nullptr);  // 약 30fps로 화면 갱신

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    for (auto& w : g_workers) w->Stop();
    ReleaseBackBuffer();
    DeleteObject(g_small);
    DeleteObject(g_big);
    avformat_network_deinit();
    return 0;
}
