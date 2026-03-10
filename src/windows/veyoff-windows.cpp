// veyoff-windows.cpp — Veyon screen-surveillance MITM proxy
//
// Architecture:
//   Veyon VncProxyServer (port 11100+session)
//       connects to what it thinks is the internal VNC server at port 11200+session
//       ↓
//   VEYOFF RFB PROXY (binds port 11200+session, the original VNC port)
//       forwards RFB traffic transparently, except:
//       - freeze mode: serves a cached framebuffer instead of real updates
//       - blacklist mode: captures screen while keeping blacklisted windows
//         hidden from the master's view
//       connects upstream to:
//       ↓
//   Real UltraVNC (binds port 11200+50+session, redirected via registry)
//
// Setup (automated on launch):
//   1. Read VncServerPort from HKLM\SOFTWARE\Veyon Solutions\Veyon\Network
//   2. Change it to originalPort + 50
//   3. Restart VeyonService
//   4. Bind our proxy on originalPort + session
//   5. On exit, restore and restart

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <dwmapi.h>
#include <iphlpapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

// ─── Constants ────────────────────────────────────────────────────────────────

constexpr wchar_t kWindowClassName[] = L"veyoff-overlay-window";
constexpr wchar_t kRegistryPath[] = L"SOFTWARE\\Veyon Solutions\\Veyon\\Network";
constexpr wchar_t kVncPortValueName[] = L"VncServerPort";
constexpr wchar_t kServiceName[] = L"VeyonService";
constexpr int kDefaultVncBasePort = 11200;
constexpr int kPortOffset = 50;
constexpr int kBannerHeight = 72;
constexpr int kOverlayAlpha = 220;
constexpr int kHotkeyFreezeId = 1;
constexpr int kHotkeyQuitId = 2;
constexpr UINT_PTR kPollTimerId = 1;
constexpr int kPollTimerIntervalMs = 500;

// Overlay presence levels (higher = more urgent)
enum class PresenceLevel { kNone, kAppOpen, kViewing };

// RFB protocol constants
constexpr int kRfbVersionLength = 12;
constexpr int kRfbChallengeSize = 16;
constexpr uint8_t kRfbSecTypeVncAuth = 2;
constexpr uint32_t kRfbAuthOk = 0;
constexpr uint8_t kRfbFramebufferUpdate = 0;
constexpr uint8_t kRfbSetColourMapEntries = 1;
constexpr uint8_t kRfbBell = 2;
constexpr uint8_t kRfbServerCutText = 3;
constexpr uint8_t kRfbSetPixelFormat = 0;
constexpr uint8_t kRfbSetEncodings = 2;
constexpr uint8_t kRfbFramebufferUpdateRequest = 3;
constexpr uint8_t kRfbKeyEvent = 4;
constexpr uint8_t kRfbPointerEvent = 5;
constexpr uint8_t kRfbClientCutText = 6;
constexpr int32_t kRfbEncodingRaw = 0;
constexpr int32_t kRfbEncodingCopyRect = 1;
constexpr int32_t kRfbEncodingLastRect = static_cast<int32_t>(0xFFFFFF20);
constexpr int32_t kRfbEncodingNewFBSize = static_cast<int32_t>(0xFFFFFF21);

// ─── Utility ──────────────────────────────────────────────────────────────────

[[nodiscard]] std::wstring trim(std::wstring value) {
    const auto isSpace = [](wchar_t ch) { return std::iswspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                [&](wchar_t ch) { return !isSpace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                [&](wchar_t ch) { return !isSpace(ch); }).base(), value.end());
    return value;
}

[[nodiscard]] std::wstring toLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

[[nodiscard]] std::wstring expandEnv(const std::wstring& raw) {
    std::array<wchar_t, 32768> buf{};
    auto n = ExpandEnvironmentStringsW(raw.c_str(), buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n > buf.size()) return raw;
    return {buf.data(), n - 1};
}

// Big-endian read/write helpers
[[nodiscard]] uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

[[nodiscard]] uint32_t readU32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
            static_cast<uint32_t>(p[3]);
}

void writeU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

void writeU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

void writeI32(uint8_t* p, int32_t v) { writeU32(p, static_cast<uint32_t>(v)); }

[[nodiscard]] std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   value.data(), static_cast<int>(value.size()),
                                   nullptr, 0);
    if (size <= 0) {
        return std::wstring(value.begin(), value.end());
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}

void closeSocket(SOCKET& sock) {
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
}

void shutdownSocket(SOCKET sock) {
    if (sock != INVALID_SOCKET) {
        shutdown(sock, SD_BOTH);
    }
}

void trySetWindowDisplayAffinity(HWND hwnd);

// ─── Reliable socket I/O ─────────────────────────────────────────────────────

// Returns true if exactly `len` bytes were sent/received.
bool sendAll(SOCKET sock, const void* data, int len) {
    const auto* ptr = static_cast<const char*>(data);
    int remaining = len;
    while (remaining > 0) {
        int sent = send(sock, ptr, remaining, 0);
        if (sent <= 0) return false;
        ptr += sent;
        remaining -= sent;
    }
    return true;
}

bool recvAll(SOCKET sock, void* data, int len) {
    auto* ptr = static_cast<char*>(data);
    int remaining = len;
    while (remaining > 0) {
        int got = recv(sock, ptr, remaining, 0);
        if (got <= 0) return false;
        ptr += got;
        remaining -= got;
    }
    return true;
}

// ─── Blacklist ────────────────────────────────────────────────────────────────

struct WindowMask {
    std::wstring title;
    RECT rect{};
};

[[nodiscard]] std::vector<std::wstring> loadBlacklist(const std::wstring& path) {
    std::ifstream input{std::filesystem::path(path)};
    if (!input.is_open()) return {};
    std::vector<std::wstring> entries;
    std::string line;
    while (std::getline(input, line)) {
        std::wstring wide = utf8ToWide(line);
        auto cleaned = trim(wide);
        if (cleaned.empty() || cleaned.starts_with(L"#")) continue;
        entries.push_back(toLower(cleaned));
    }
    return entries;
}

[[nodiscard]] bool titleMatchesBlacklist(const std::wstring& title,
                                         const std::vector<std::wstring>& blacklist) {
    if (title.empty() || blacklist.empty()) return false;
    const auto low = toLower(title);
    return std::any_of(blacklist.begin(), blacklist.end(),
                       [&](const auto& kw) { return low.find(kw) != std::wstring::npos; });
}

struct WindowEnumContext {
    HWND overlayWindow = nullptr;
    const std::vector<std::wstring>* blacklist = nullptr;
    std::vector<WindowMask>* results = nullptr;
};

BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<WindowEnumContext*>(lParam);
    if (hwnd == ctx->overlayWindow || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    auto titleLen = GetWindowTextLengthW(hwnd);
    if (titleLen <= 0) return TRUE;
    std::wstring title(static_cast<size_t>(titleLen) + 1, L'\0');
    GetWindowTextW(hwnd, title.data(), titleLen + 1);
    title.resize(static_cast<size_t>(titleLen));
    if (!titleMatchesBlacklist(title, *ctx->blacklist)) return TRUE;
    RECT rect{};
    if (DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)) != S_OK)
        if (!GetWindowRect(hwnd, &rect)) return TRUE;
    if (rect.right <= rect.left || rect.bottom <= rect.top) return TRUE;
    ctx->results->push_back({title, rect});
    return TRUE;
}

[[nodiscard]] std::vector<WindowMask> enumerateBlacklistedWindows(
        HWND overlayHwnd, const std::vector<std::wstring>& blacklist) {
    std::vector<WindowMask> masks;
    if (blacklist.empty()) return masks;
    WindowEnumContext ctx{overlayHwnd, &blacklist, &masks};
    EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    return masks;
}

// ─── Screen capture ──────────────────────────────────────────────────────────

struct FrameBuffer {
    int width = 0;
    int height = 0;
    // Pixels in BGRA 32bpp, top-down, left-to-right.
    // When sending via RFB Raw encoding, we convert to the negotiated pixel format.
    std::vector<uint8_t> pixels;
    bool valid() const { return width > 0 && height > 0 && !pixels.empty(); }
};

bool captureScreen(FrameBuffer& fb) {
    auto originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    auto originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    auto w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    auto h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0) return false;

    auto screenDc = GetDC(nullptr);
    if (!screenDc) return false;
    auto memDc = CreateCompatibleDC(screenDc);
    if (!memDc) { ReleaseDC(nullptr, screenDc); return false; }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    auto bmp = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    auto oldObj = SelectObject(memDc, bmp);
    bool ok = BitBlt(memDc, 0, 0, w, h, screenDc, originX, originY,
                     SRCCOPY | CAPTUREBLT) != 0;
    if (ok) {
        auto byteCount = static_cast<size_t>(w) * h * 4;
        fb.width = w;
        fb.height = h;
        fb.pixels.assign(static_cast<uint8_t*>(bits),
                         static_cast<uint8_t*>(bits) + byteCount);
    }

    SelectObject(memDc, oldObj);
    DeleteObject(bmp);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    return ok;
}

[[nodiscard]] std::vector<RECT> buildMaskRects(HWND overlayHwnd,
                                               const std::vector<std::wstring>& blacklist,
                                               int width,
                                               int height) {
    std::vector<RECT> rects;
    if (blacklist.empty() || width <= 0 || height <= 0) return rects;

    auto originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    auto originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    auto masks = enumerateBlacklistedWindows(overlayHwnd, blacklist);
    rects.reserve(masks.size());
    for (const auto& m : masks) {
        RECT adj{m.rect.left - originX, m.rect.top - originY,
                 m.rect.right - originX, m.rect.bottom - originY};
        adj.left = std::max(0L, adj.left);
        adj.top = std::max(0L, adj.top);
        adj.right = std::min(static_cast<LONG>(width), adj.right);
        adj.bottom = std::min(static_cast<LONG>(height), adj.bottom);
        if (adj.right > adj.left && adj.bottom > adj.top) rects.push_back(adj);
    }
    return rects;
}

void preserveRectsFromPreviousFrame(FrameBuffer& current,
                                    const FrameBuffer& previous,
                                    const std::vector<RECT>& rects) {
    if (!current.valid() || !previous.valid()) return;
    if (current.width != previous.width || current.height != previous.height) return;

    for (const auto& rect : rects) {
        for (LONG y = rect.top; y < rect.bottom; ++y) {
            size_t start = (static_cast<size_t>(y) * current.width + rect.left) * 4;
            size_t bytes = static_cast<size_t>(rect.right - rect.left) * 4;
            std::memcpy(current.pixels.data() + start,
                        previous.pixels.data() + start,
                        bytes);
        }
    }
}

// ─── Veyon registry & service management ─────────────────────────────────────

// Read the current VncServerPort from registry. Returns -1 on failure.
int readVncPortFromRegistry() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0,
                      KEY_READ | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return -1;
    DWORD value = 0, size = sizeof(value), type = 0;
    auto result = RegQueryValueExW(hKey, kVncPortValueName, nullptr, &type,
                                   reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(hKey);
    if (result != ERROR_SUCCESS || type != REG_DWORD) return -1;
    return static_cast<int>(value);
}

bool writeVncPortToRegistry(int port) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0,
                      KEY_WRITE | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD value = static_cast<DWORD>(port);
    auto result = RegSetValueExW(hKey, kVncPortValueName, 0, REG_DWORD,
                                 reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(hKey);
    return result == ERROR_SUCCESS;
}

bool controlService(const wchar_t* serviceName, bool start) {
    auto scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    auto svc = OpenServiceW(scm, serviceName,
                            start ? SERVICE_START : SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) { CloseServiceHandle(scm); return false; }

    bool ok = false;
    if (start) {
        ok = StartServiceW(svc, 0, nullptr) != 0 ||
             GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
    } else {
        SERVICE_STATUS status{};
        ok = ControlService(svc, SERVICE_CONTROL_STOP, &status) != 0 ||
             GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

bool waitForServiceState(const wchar_t* serviceName, DWORD desiredState, DWORD timeoutMs) {
    auto scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    auto svc = OpenServiceW(scm, serviceName, SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return false;
    }

    auto deadline = static_cast<uint64_t>(GetTickCount()) + timeoutMs;
    SERVICE_STATUS_PROCESS status{};
    DWORD bytesNeeded = 0;
    bool reached = false;
    while (static_cast<uint64_t>(GetTickCount()) <= deadline) {
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<LPBYTE>(&status), sizeof(status),
                                  &bytesNeeded)) {
            break;
        }
        if (status.dwCurrentState == desiredState) {
            reached = true;
            break;
        }
        Sleep(200);
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return reached;
}

bool waitForLoopbackPort(int port, DWORD timeoutMs) {
    auto deadline = static_cast<uint64_t>(GetTickCount()) + timeoutMs;
    while (static_cast<uint64_t>(GetTickCount()) <= deadline) {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<u_short>(port));

        bool connected = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        closeSocket(sock);
        if (connected) return true;
        Sleep(200);
    }
    return false;
}

bool restartVeyonService(int portToWaitFor) {
    std::wcout << L"Stopping VeyonService..." << std::endl;
    if (!controlService(kServiceName, false)) {
        std::wcerr << L"Failed to stop VeyonService" << std::endl;
        return false;
    }
    if (!waitForServiceState(kServiceName, SERVICE_STOPPED, 10000)) {
        std::wcerr << L"Timed out waiting for VeyonService to stop" << std::endl;
        return false;
    }

    std::wcout << L"Starting VeyonService..." << std::endl;
    if (!controlService(kServiceName, true)) {
        std::wcerr << L"Failed to start VeyonService" << std::endl;
        return false;
    }
    if (!waitForServiceState(kServiceName, SERVICE_RUNNING, 10000)) {
        std::wcerr << L"Timed out waiting for VeyonService to start" << std::endl;
        return false;
    }
    if (portToWaitFor > 0 && !waitForLoopbackPort(portToWaitFor, 10000)) {
        std::wcerr << L"Timed out waiting for VNC port " << portToWaitFor << std::endl;
        return false;
    }
    return true;
}

// ─── Shared state (thread-safe access) ───────────────────────────────────────

struct SharedState {
    std::mutex mtx;
    bool frozen = false;
    FrameBuffer frozenFrame;       // Captured at freeze time
    FrameBuffer teacherVisibleFrame; // What the master should keep seeing
    std::vector<std::wstring> blacklist;
    HWND overlayHwnd = nullptr;
    PresenceLevel presence = PresenceLevel::kNone;
    PresenceLevel overlayShowing = PresenceLevel::kNone;
    DWORD sessionId = 0;
    std::filesystem::file_time_type blacklistWriteTime{};
    std::wstring blacklistPath;
};

// ─── RFB Pixel Format ────────────────────────────────────────────────────────

struct RfbPixelFormat {
    uint8_t bitsPerPixel = 32;
    uint8_t depth = 24;
    uint8_t bigEndian = 0;
    uint8_t trueColour = 1;
    uint16_t redMax = 255;
    uint16_t greenMax = 255;
    uint16_t blueMax = 255;
    uint8_t redShift = 16;
    uint8_t greenShift = 8;
    uint8_t blueShift = 0;
    // pad: 3 bytes

    int bytesPerPixel() const { return bitsPerPixel / 8; }

    void readFrom(const uint8_t* p) {
        bitsPerPixel = p[0]; depth = p[1]; bigEndian = p[2]; trueColour = p[3];
        redMax = readU16(p + 4); greenMax = readU16(p + 6); blueMax = readU16(p + 8);
        redShift = p[10]; greenShift = p[11]; blueShift = p[12];
    }

};

// ─── RFB Proxy ───────────────────────────────────────────────────────────────
//
// The proxy sits between Veyon's VncProxyServer (client) and UltraVNC (server).
// In normal mode it transparently forwards all RFB traffic.
// In freeze/blacklist mode it intercepts FramebufferUpdateRequest from the client
// and responds with our own captured (frozen or redacted) frame.
//
// KEY SIMPLIFICATION: Instead of parsing every VNC encoding, when we need to
// send our own frame we always use Raw encoding. The client always supports Raw
// as it is mandatory in the RFB spec. We only need to parse enough of the
// server->client stream to know message boundaries for forwarding.

// Forward bytes from `src` to `dst`, reading exactly `len` bytes.
bool forwardBytes(SOCKET src, SOCKET dst, int len) {
    std::vector<uint8_t> buf(std::min(len, 65536));
    int remaining = len;
    while (remaining > 0) {
        int chunk = std::min(remaining, static_cast<int>(buf.size()));
        if (!recvAll(src, buf.data(), chunk)) return false;
        if (!sendAll(dst, buf.data(), chunk)) return false;
        remaining -= chunk;
    }
    return true;
}

// Build a FramebufferUpdate message with a single Raw rectangle covering the
// entire framebuffer. The pixel data is converted from our BGRA capture to the
// negotiated pixel format.
std::vector<uint8_t> buildRawFrameUpdate(const FrameBuffer& fb,
                                          const RfbPixelFormat& pf,
                                          uint16_t fbWidth, uint16_t fbHeight) {
    int sendW = fbWidth > 0 ? static_cast<int>(fbWidth) : fb.width;
    int sendH = fbHeight > 0 ? static_cast<int>(fbHeight) : fb.height;
    int bpp = pf.bytesPerPixel();
    if (sendW <= 0 || sendH <= 0 || bpp <= 0) return {};

    // Message header: type(1) + pad(1) + nRects(2) = 4
    // Rect header: x(2) + y(2) + w(2) + h(2) + encoding(4) = 12
    // Pixel data: sendW * sendH * bpp
    size_t pixelDataSize = static_cast<size_t>(sendW) * sendH * bpp;
    size_t totalSize = 4 + 12 + pixelDataSize;
    std::vector<uint8_t> msg(totalSize, 0);

    // Message header
    msg[0] = kRfbFramebufferUpdate; // type
    msg[1] = 0; // pad
    writeU16(msg.data() + 2, 1); // nRects = 1

    // Rect header
    writeU16(msg.data() + 4, 0); // x
    writeU16(msg.data() + 6, 0); // y
    writeU16(msg.data() + 8, static_cast<uint16_t>(sendW)); // width
    writeU16(msg.data() + 10, static_cast<uint16_t>(sendH)); // height
    writeI32(msg.data() + 12, kRfbEncodingRaw); // encoding

    // Convert BGRA pixels to negotiated format
    uint8_t* dst = msg.data() + 16;
    for (int y = 0; y < sendH; ++y) {
        for (int x = 0; x < sendW; ++x) {
            uint8_t b = 0;
            uint8_t g = 0;
            uint8_t r = 0;
            if (x < fb.width && y < fb.height) {
                size_t srcIdx = (static_cast<size_t>(y) * fb.width + x) * 4;
                b = fb.pixels[srcIdx + 0];
                g = fb.pixels[srcIdx + 1];
                r = fb.pixels[srcIdx + 2];
            }
            // Convert to negotiated pixel format
            uint32_t pixel = 0;
            pixel |= (static_cast<uint32_t>(r * pf.redMax / 255) & pf.redMax) << pf.redShift;
            pixel |= (static_cast<uint32_t>(g * pf.greenMax / 255) & pf.greenMax) << pf.greenShift;
            pixel |= (static_cast<uint32_t>(b * pf.blueMax / 255) & pf.blueMax) << pf.blueShift;

            if (bpp == 4) {
                if (pf.bigEndian) {
                    dst[0] = static_cast<uint8_t>(pixel >> 24);
                    dst[1] = static_cast<uint8_t>(pixel >> 16);
                    dst[2] = static_cast<uint8_t>(pixel >> 8);
                    dst[3] = static_cast<uint8_t>(pixel);
                } else {
                    dst[0] = static_cast<uint8_t>(pixel);
                    dst[1] = static_cast<uint8_t>(pixel >> 8);
                    dst[2] = static_cast<uint8_t>(pixel >> 16);
                    dst[3] = static_cast<uint8_t>(pixel >> 24);
                }
            } else if (bpp == 2) {
                if (pf.bigEndian) {
                    dst[0] = static_cast<uint8_t>(pixel >> 8);
                    dst[1] = static_cast<uint8_t>(pixel);
                } else {
                    dst[0] = static_cast<uint8_t>(pixel);
                    dst[1] = static_cast<uint8_t>(pixel >> 8);
                }
            } else {
                dst[0] = static_cast<uint8_t>(pixel);
            }
            dst += bpp;
        }
    }
    return msg;
}

// Perform RFB handshake with the upstream VNC server.
// Reads the server's pixel format and framebuffer dimensions.
// The internal password is generated at runtime by Veyon, so the proxy does
// not authenticate independently. It transparently forwards the handshake and
// only starts interpreting traffic after ServerInit.
//
// IMPORTANT: Since the internal VNC password is generated at runtime and
// not persisted, we take a different approach: we do NOT independently
// connect to UltraVNC. Instead, we let the downstream client (Veyon's proxy)
// do the auth. We act as a transparent byte-forwarder for the handshake,
// then intercept post-handshake traffic.
//
// The proxy flow:
//   1. Accept connection from Veyon's VncProxyServer (downstream client)
//   2. Connect to UltraVNC (upstream server)
//   3. Forward the entire handshake transparently (version, auth, init)
//   4. Sniff the ServerInit to learn framebuffer dimensions + pixel format
//   5. After handshake, enter forwarding mode with interception

// State for a single proxy connection
struct ProxySession {
    SOCKET clientSock = INVALID_SOCKET;  // Veyon's VncProxyServer
    SOCKET serverSock = INVALID_SOCKET;  // Real UltraVNC
    SharedState* shared = nullptr;
    RfbPixelFormat pixelFormat;
    uint16_t fbWidth = 0;
    uint16_t fbHeight = 0;
    std::atomic<bool> running{true};
    std::mutex clientWriteMtx; // Protects writes to clientSock
    std::thread serverThread;
    std::thread clientThread;
};

struct ProxyRuntime {
    std::atomic<bool> stopRequested{false};
    SOCKET listenSock = INVALID_SOCKET;
    std::mutex sessionsMtx;
    std::vector<std::shared_ptr<ProxySession>> sessions;
    std::unique_ptr<std::thread> thread;
};

static ProxyRuntime g_proxyRuntime;

[[nodiscard]] int activeSessionCount() {
    std::lock_guard lock(g_proxyRuntime.sessionsMtx);
    // Prune dead sessions while we're here
    g_proxyRuntime.sessions.erase(
        std::remove_if(g_proxyRuntime.sessions.begin(),
                       g_proxyRuntime.sessions.end(),
                       [](const auto& s) { return !s->running.load(); }),
        g_proxyRuntime.sessions.end());
    return static_cast<int>(g_proxyRuntime.sessions.size());
}

bool readAndMaybeForward(SOCKET src, SOCKET dst, int len, bool forward) {
    std::vector<uint8_t> buf(std::min(len, 65536));
    int remaining = len;
    while (remaining > 0) {
        int chunk = std::min(remaining, static_cast<int>(buf.size()));
        if (!recvAll(src, buf.data(), chunk)) return false;
        if (forward && !sendAll(dst, buf.data(), chunk)) return false;
        remaining -= chunk;
    }
    return true;
}

bool handleServerFramebufferUpdate(ProxySession& session, bool forward) {
    uint8_t header[3];
    if (!recvAll(session.serverSock, header, 3)) return false;
    if (forward && !sendAll(session.clientSock, header, 3)) return false;

    uint16_t nRects = readU16(header + 1);
    int bpp = session.pixelFormat.bytesPerPixel();
    if (bpp <= 0) return false;

    for (uint16_t i = 0; i < nRects; ++i) {
        uint8_t rectHeader[12];
        if (!recvAll(session.serverSock, rectHeader, 12)) return false;
        if (forward && !sendAll(session.clientSock, rectHeader, 12)) return false;

        uint16_t width = readU16(rectHeader + 4);
        uint16_t height = readU16(rectHeader + 6);
        int32_t encoding = static_cast<int32_t>(readU32(rectHeader + 8));

        switch (encoding) {
        case kRfbEncodingRaw: {
            size_t payloadSize = static_cast<size_t>(width) * height * bpp;
            if (payloadSize > static_cast<size_t>(INT_MAX)) return false;
            if (!readAndMaybeForward(session.serverSock, session.clientSock,
                                     static_cast<int>(payloadSize), forward))
                return false;
            break;
        }
        case kRfbEncodingCopyRect:
            if (!readAndMaybeForward(session.serverSock, session.clientSock, 4, forward)) {
                return false;
            }
            break;
        case kRfbEncodingLastRect:
            break;
        case kRfbEncodingNewFBSize:
            session.fbWidth = width;
            session.fbHeight = height;
            break;
        default:
            return false;
        }
    }

    return true;
}

bool handleOneServerMessage(ProxySession& session, bool forward) {
    std::unique_lock<std::mutex> clientLock(session.clientWriteMtx, std::defer_lock);
    if (forward) clientLock.lock();

    uint8_t messageType = 0;
    if (!recvAll(session.serverSock, &messageType, 1)) return false;
    if (forward && !sendAll(session.clientSock, &messageType, 1)) return false;

    switch (messageType) {
    case kRfbFramebufferUpdate:
        return handleServerFramebufferUpdate(session, forward);
    case kRfbSetColourMapEntries: {
        uint8_t header[5];
        if (!recvAll(session.serverSock, header, 5)) return false;
        if (forward && !sendAll(session.clientSock, header, 5)) return false;
        uint16_t nColours = readU16(header + 3);
        size_t payloadSize = static_cast<size_t>(nColours) * 6;
        if (payloadSize > static_cast<size_t>(INT_MAX)) return false;
        return readAndMaybeForward(session.serverSock, session.clientSock,
                                   static_cast<int>(payloadSize), forward);
    }
    case kRfbBell:
        return true;
    case kRfbServerCutText: {
        uint8_t header[7];
        if (!recvAll(session.serverSock, header, 7)) return false;
        if (forward && !sendAll(session.clientSock, header, 7)) return false;
        uint32_t textLen = readU32(header + 3);
        if (textLen > static_cast<uint32_t>(INT_MAX)) return false;
        return readAndMaybeForward(session.serverSock, session.clientSock,
                                   static_cast<int>(textLen), forward);
    }
    default:
        return false;
    }
}

void stopProxyRuntime() {
    g_proxyRuntime.stopRequested = true;
    closeSocket(g_proxyRuntime.listenSock);

    std::vector<std::shared_ptr<ProxySession>> sessions;
    {
        std::lock_guard lock(g_proxyRuntime.sessionsMtx);
        sessions = g_proxyRuntime.sessions;
    }

    for (const auto& session : sessions) {
        session->running = false;
        shutdownSocket(session->clientSock);
        shutdownSocket(session->serverSock);
    }
    for (const auto& session : sessions) {
        if (session->serverThread.joinable()) session->serverThread.join();
        if (session->clientThread.joinable()) session->clientThread.join();
        closeSocket(session->clientSock);
        closeSocket(session->serverSock);
    }

    {
        std::lock_guard lock(g_proxyRuntime.sessionsMtx);
        g_proxyRuntime.sessions.clear();
    }

    if (g_proxyRuntime.thread && g_proxyRuntime.thread->joinable()) {
        g_proxyRuntime.thread->join();
    }
    g_proxyRuntime.thread.reset();
    g_proxyRuntime.stopRequested = false;
}

// Handshake: transparently forward, but sniff ServerInit.
bool proxyHandshake(ProxySession& session) {
    // Phase 1: Protocol version (12 bytes each way)
    uint8_t versionBuf[kRfbVersionLength];

    // Server -> us -> client
    if (!recvAll(session.serverSock, versionBuf, kRfbVersionLength)) return false;
    if (!sendAll(session.clientSock, versionBuf, kRfbVersionLength)) return false;

    // Client -> us -> server
    if (!recvAll(session.clientSock, versionBuf, kRfbVersionLength)) return false;
    if (!sendAll(session.serverSock, versionBuf, kRfbVersionLength)) return false;

    // Phase 2: Security types
    // Server sends: [count: 1 byte] [type1, type2, ...]
    uint8_t secCount = 0;
    if (!recvAll(session.serverSock, &secCount, 1)) return false;
    if (!sendAll(session.clientSock, &secCount, 1)) return false;

    if (secCount == 0) {
        // Server sent error - forward the error string and bail
        uint8_t errLenBuf[4];
        if (!recvAll(session.serverSock, errLenBuf, 4)) return false;
        if (!sendAll(session.clientSock, errLenBuf, 4)) return false;
        uint32_t errLen = readU32(errLenBuf);
        if (errLen > 0 && errLen < 65536) {
            if (!forwardBytes(session.serverSock, session.clientSock, errLen)) return false;
        }
        return false;
    }

    std::vector<uint8_t> secTypes(secCount);
    if (!recvAll(session.serverSock, secTypes.data(), secCount)) return false;
    if (!sendAll(session.clientSock, secTypes.data(), secCount)) return false;

    // Client chooses a type
    uint8_t chosenType = 0;
    if (!recvAll(session.clientSock, &chosenType, 1)) return false;
    if (!sendAll(session.serverSock, &chosenType, 1)) return false;

    // Phase 3: Auth (depends on chosen type)
    if (chosenType == kRfbSecTypeVncAuth) {
        // Forward 16-byte challenge: server -> client
        if (!forwardBytes(session.serverSock, session.clientSock, kRfbChallengeSize)) return false;
        // Forward 16-byte response: client -> server
        if (!forwardBytes(session.clientSock, session.serverSock, kRfbChallengeSize)) return false;
    }
    // Other security types in this path do not require an extra auth payload.

    // Phase 4: Security result (4 bytes, server -> client)
    uint8_t resultBuf[4];
    if (!recvAll(session.serverSock, resultBuf, 4)) return false;
    if (!sendAll(session.clientSock, resultBuf, 4)) return false;
    uint32_t authResult = readU32(resultBuf);
    if (authResult != kRfbAuthOk) {
        // Auth failed - forward any error message
        // In RFB 3.8, if auth fails, server sends [4-byte error length][error string]
        uint8_t errLenBuf[4];
        if (recvAll(session.serverSock, errLenBuf, 4)) {
            sendAll(session.clientSock, errLenBuf, 4);
            uint32_t errLen = readU32(errLenBuf);
            if (errLen > 0 && errLen < 65536)
                forwardBytes(session.serverSock, session.clientSock, errLen);
        }
        return false;
    }

    // Phase 5: ClientInit (1 byte, client -> server)
    uint8_t sharedFlag = 0;
    if (!recvAll(session.clientSock, &sharedFlag, 1)) return false;
    if (!sendAll(session.serverSock, &sharedFlag, 1)) return false;

    // Phase 6: ServerInit (24 bytes + name, server -> client)
    // We sniff this to learn the framebuffer dimensions and pixel format.
    uint8_t serverInit[24];
    if (!recvAll(session.serverSock, serverInit, 24)) return false;
    session.fbWidth = readU16(serverInit);
    session.fbHeight = readU16(serverInit + 2);
    session.pixelFormat.readFrom(serverInit + 4);

    // Read the name
    uint32_t nameLen = readU32(serverInit + 20);
    if (!sendAll(session.clientSock, serverInit, 24)) return false;
    if (nameLen > 0 && nameLen < 65536) {
        if (!forwardBytes(session.serverSock, session.clientSock, nameLen)) return false;
    }

    std::wcout << L"RFB handshake complete: " << session.fbWidth << L"x"
               << session.fbHeight << L" " << (int)session.pixelFormat.bitsPerPixel
               << L"bpp" << std::endl;
    return true;
}

// Get the current frame to serve.
//
// When blacklist matches exist, we preserve the last teacher-visible pixels
// for those rectangles instead of painting them black. That makes the window
// stay hidden from the master's view rather than replaced with a black box.
FrameBuffer getInterceptFrame(ProxySession& session) {
    HWND overlayHwnd = nullptr;
    std::vector<std::wstring> blacklist;
    FrameBuffer previousTeacherVisible;
    {
        std::lock_guard lock(session.shared->mtx);
        if (session.shared->frozen && session.shared->frozenFrame.valid()) {
            return session.shared->frozenFrame;
        }
        overlayHwnd = session.shared->overlayHwnd;
        blacklist = session.shared->blacklist;
        previousTeacherVisible = session.shared->teacherVisibleFrame;
    }

    FrameBuffer fb;
    if (!captureScreen(fb) || !fb.valid()) return {};

    auto maskRects = buildMaskRects(overlayHwnd, blacklist, fb.width, fb.height);
    if (!maskRects.empty()) {
        preserveRectsFromPreviousFrame(fb, previousTeacherVisible, maskRects);
    }

    {
        std::lock_guard lock(session.shared->mtx);
        session.shared->teacherVisibleFrame = fb;
    }

    return fb;
}

// Thread: forward server->client traffic.
// Messages are parsed at the RFB message boundary so interception can stop
// forwarding entire messages without desynchronizing the stream.

void serverToClientThread(ProxySession& session) {
    while (session.running) {
        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(session.serverSock, &readFds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 50000; // 50ms

        int sel = select(0, &readFds, nullptr, nullptr, &tv);
        if (sel < 0) break;
        if (sel == 0) continue;

        bool intercepting;
        {
            std::lock_guard lock(session.shared->mtx);
            intercepting = session.shared->frozen ||
                          !session.shared->blacklist.empty();
        }

        if (!handleOneServerMessage(session, !intercepting)) break;
    }

    session.running = false;
}

// Thread: read client->server traffic.
// In normal mode: forward transparently.
// In freeze/blacklist mode: intercept FramebufferUpdateRequests and respond
// with our own frame. Other messages (key events, pointer events) are still
// forwarded to the server.
void clientToServerThread(ProxySession& session) {
    while (session.running) {
        // Peek the message type (1 byte)
        uint8_t msgType = 0;
        if (!recvAll(session.clientSock, &msgType, 1)) break;

        bool intercepting;
        {
            std::lock_guard lock(session.shared->mtx);
            intercepting = session.shared->frozen ||
                          !session.shared->blacklist.empty();
        }

        // Determine message size based on type
        switch (msgType) {
        case kRfbFramebufferUpdateRequest: {
            // 10 bytes total (1 type + 1 incremental + 8 data)
            uint8_t body[9]; // Already read type byte
            if (!recvAll(session.clientSock, body, 9)) { session.running = false; break; }

            if (intercepting) {
                // Don't forward to server. Send our own frame to client.
                auto fb = getInterceptFrame(session);
                if (fb.valid()) {
                    auto msg = buildRawFrameUpdate(fb, session.pixelFormat,
                                                   session.fbWidth, session.fbHeight);
                    std::lock_guard lock(session.clientWriteMtx);
                    if (!sendAll(session.clientSock, msg.data(),
                                static_cast<int>(msg.size()))) {
                        session.running = false;
                    }
                }
                // If no valid frame, just don't respond (client will re-request)
            } else {
                // Forward to server
                if (!sendAll(session.serverSock, &msgType, 1)) { session.running = false; break; }
                if (!sendAll(session.serverSock, body, 9)) { session.running = false; break; }
            }
            break;
        }
        case kRfbSetPixelFormat: {
            // 20 bytes total (1 type + 3 pad + 16 pixel format)
            uint8_t body[19];
            if (!recvAll(session.clientSock, body, 19)) { session.running = false; break; }
            // Update our tracked pixel format
            session.pixelFormat.readFrom(body + 3); // skip 3 pad bytes
            // Always forward
            if (!sendAll(session.serverSock, &msgType, 1)) { session.running = false; break; }
            if (!sendAll(session.serverSock, body, 19)) { session.running = false; break; }
            break;
        }
        case kRfbSetEncodings: {
            // 4 bytes header (type + pad + nEncodings), then nEncodings * 4
            uint8_t header[3]; // pad(1) + nEncodings(2), type already read
            if (!recvAll(session.clientSock, header, 3)) { session.running = false; break; }
            uint16_t nEncodings = readU16(header + 1);
            int dataLen = nEncodings * 4;
            std::vector<uint8_t> data(dataLen);
            if (dataLen > 0) {
                if (!recvAll(session.clientSock, data.data(), dataLen)) {
                    session.running = false; break;
                }
            }

            uint8_t rewrittenHeader[3]{};
            rewrittenHeader[0] = 0;
            writeU16(rewrittenHeader + 1, 1);
            uint8_t rawEncoding[4];
            writeI32(rawEncoding, kRfbEncodingRaw);

            if (!sendAll(session.serverSock, &msgType, 1)) { session.running = false; break; }
            if (!sendAll(session.serverSock, rewrittenHeader, 3)) { session.running = false; break; }
            if (!sendAll(session.serverSock, rawEncoding, 4)) { session.running = false; break; }
            break;
        }
        case kRfbKeyEvent: {
            uint8_t body[7]; // 8 total - 1 type
            if (!recvAll(session.clientSock, body, 7)) { session.running = false; break; }
            // Always forward (even when intercepting, so the user can still type)
            if (!sendAll(session.serverSock, &msgType, 1)) { session.running = false; break; }
            if (!sendAll(session.serverSock, body, 7)) { session.running = false; break; }
            break;
        }
        case kRfbPointerEvent: {
            uint8_t body[5]; // 6 total - 1 type
            if (!recvAll(session.clientSock, body, 5)) { session.running = false; break; }
            // Always forward
            if (!sendAll(session.serverSock, &msgType, 1)) { session.running = false; break; }
            if (!sendAll(session.serverSock, body, 5)) { session.running = false; break; }
            break;
        }
        case kRfbClientCutText: {
            uint8_t header[7]; // 8 total - 1 type (3 pad + 4 length)
            if (!recvAll(session.clientSock, header, 7)) { session.running = false; break; }
            uint32_t textLen = readU32(header + 3);
            // Forward header
            if (!sendAll(session.serverSock, &msgType, 1)) { session.running = false; break; }
            if (!sendAll(session.serverSock, header, 7)) { session.running = false; break; }
            // Forward text data
            if (textLen > 0 && textLen < 10 * 1024 * 1024) {
                if (!forwardBytes(session.clientSock, session.serverSock, textLen)) {
                    session.running = false;
                }
            }
            break;
        }
        default:
            // Unknown message type. We can't determine its length.
            // Forward the type byte and hope for the best, but this is risky.
            // In practice, Veyon's proxy only sends the standard types.
            if (!sendAll(session.serverSock, &msgType, 1)) { session.running = false; }
            break;
        }
    }

    session.running = false;
}

// Main proxy loop: accept connections and spawn forwarding threads.
// Tracks active sessions so sockets can be closed on shutdown.
void proxyMain(SharedState& shared, int listenPort, int upstreamPort) {
    g_proxyRuntime.stopRequested = false;
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        std::wcerr << L"Failed to create listen socket" << std::endl;
        return;
    }

    // Allow address reuse
    int reuseAddr = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuseAddr), sizeof(reuseAddr));

    sockaddr_in listenAddr{};
    listenAddr.sin_family = AF_INET;
    listenAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listenAddr.sin_port = htons(static_cast<u_short>(listenPort));

    if (bind(listenSock, reinterpret_cast<sockaddr*>(&listenAddr), sizeof(listenAddr)) != 0) {
        std::wcerr << L"Failed to bind port " << listenPort
                   << L" (error " << WSAGetLastError() << L")" << std::endl;
        closesocket(listenSock);
        return;
    }

    if (listen(listenSock, 2) != 0) {
        std::wcerr << L"Failed to listen" << std::endl;
        closesocket(listenSock);
        return;
    }

    std::wcout << L"Proxy listening on 127.0.0.1:" << listenPort << std::endl;

    g_proxyRuntime.listenSock = listenSock;

    while (!g_proxyRuntime.stopRequested) {
        SOCKET clientSock = accept(listenSock, nullptr, nullptr);
        if (clientSock == INVALID_SOCKET) {
            if (g_proxyRuntime.stopRequested) break;
            continue;
        }

        std::wcout << L"Proxy: client connected" << std::endl;

        // Connect to upstream UltraVNC (with retries — the VNC server may
        // still be starting after the service restart)
        SOCKET serverSock = INVALID_SOCKET;
        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        serverAddr.sin_port = htons(static_cast<u_short>(upstreamPort));

        constexpr int kMaxRetries = 10;
        constexpr int kRetryDelayMs = 500;
        for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
            serverSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (serverSock == INVALID_SOCKET) break;
            if (connect(serverSock, reinterpret_cast<sockaddr*>(&serverAddr),
                        sizeof(serverAddr)) == 0)
                break; // Connected
            closesocket(serverSock);
            serverSock = INVALID_SOCKET;
            if (attempt < kMaxRetries - 1) {
                std::wcout << L"Upstream VNC not ready, retry " << (attempt + 1)
                           << L"/" << kMaxRetries << std::endl;
                Sleep(kRetryDelayMs);
            }
        }
        if (serverSock == INVALID_SOCKET) {
            std::wcerr << L"Failed to connect to upstream VNC on port "
                       << upstreamPort << L" after " << kMaxRetries
                       << L" attempts" << std::endl;
            closesocket(clientSock);
            continue;
        }

        std::wcout << L"Proxy: connected to upstream VNC on port " << upstreamPort << std::endl;

        // Set up proxy session
        auto session = std::make_shared<ProxySession>();
        session->clientSock = clientSock;
        session->serverSock = serverSock;
        session->shared = &shared;

        // Track this session
        {
            std::lock_guard lock(g_proxyRuntime.sessionsMtx);
            g_proxyRuntime.sessions.push_back(session);
        }

        // Perform handshake (transparent forwarding with sniffing)
        if (!proxyHandshake(*session)) {
            std::wcerr << L"RFB handshake failed" << std::endl;
            session->running = false;
            closesocket(serverSock);
            closesocket(clientSock);
            continue;
        }

        // Spawn forwarding threads
        session->serverThread = std::thread([session]() {
            serverToClientThread(*session);
            shutdownSocket(session->clientSock);
            shutdownSocket(session->serverSock);
        });
        session->clientThread = std::thread([session]() {
            clientToServerThread(*session);
            shutdownSocket(session->clientSock);
            shutdownSocket(session->serverSock);
        });
    }

    closeSocket(g_proxyRuntime.listenSock);
}

// ─── Overlay window ──────────────────────────────────────────────────────────

void paintOverlay(HWND hwnd, PresenceLevel level) {
    PAINTSTRUCT ps{};
    auto dc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);

    COLORREF bgColor, borderColor, textColor;
    const wchar_t* text;
    if (level == PresenceLevel::kViewing) {
        bgColor = RGB(180, 0, 0);       // red — actively viewing your screen
        borderColor = RGB(255, 255, 255);
        textColor = RGB(255, 255, 255);
        text = L"MASTER VIEWING";
    } else {
        bgColor = RGB(200, 150, 0);     // amber — veyon app is open
        borderColor = RGB(60, 40, 0);
        textColor = RGB(40, 20, 0);
        text = L"VEYON ACTIVE";
    }

    auto bgBrush = CreateSolidBrush(bgColor);
    FillRect(dc, &rc, bgBrush);
    DeleteObject(bgBrush);
    auto bBrush = CreateSolidBrush(borderColor);
    FrameRect(dc, &rc, bBrush);
    DeleteObject(bBrush);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, textColor);
    auto font = CreateFontW(-28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    auto oldFont = SelectObject(dc, font);
    DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    EndPaint(hwnd, &ps);
}

void positionOverlay(HWND hwnd) {
    auto x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    auto y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    auto w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, kBannerHeight,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void updateOverlay(SharedState& shared) {
    std::lock_guard lock(shared.mtx);
    if (shared.presence == shared.overlayShowing) return;
    shared.overlayShowing = shared.presence;
    if (shared.overlayShowing != PresenceLevel::kNone) {
        positionOverlay(shared.overlayHwnd);
        ShowWindow(shared.overlayHwnd, SW_SHOWNOACTIVATE);
        InvalidateRect(shared.overlayHwnd, nullptr, TRUE);
    } else {
        ShowWindow(shared.overlayHwnd, SW_HIDE);
    }
}

[[nodiscard]] DWORD currentSessionId() {
    DWORD sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    return sid;
}

[[nodiscard]] DWORD veyonLogicalSessionId() {
    auto sid = currentSessionId();
    return sid == WTSGetActiveConsoleSessionId() ? 0 : sid;
}

// Detect if a Veyon master is remotely connected.
// We watch the external Veyon server port (11100+session). A non-loopback
// ESTABLISHED connection on that port means a master is actively viewing.
// We intentionally exclude:
//   - Our proxy port (11200+session) to avoid false positives from internal traffic
//   - Pure loopback connections (both ends 127.x.x.x) which are internal plumbing
bool detectMasterConnection(DWORD sessionId) {
    u_short veyonPort = static_cast<u_short>(11100 + sessionId);

    constexpr uint32_t kLoopbackNet = 0x7F000000; // 127.0.0.0
    constexpr uint32_t kLoopbackMask = 0xFF000000;
    auto isLoopback = [](DWORD addr) -> bool {
        uint32_t hostOrder = ntohl(addr);
        return (hostOrder & kLoopbackMask) == kLoopbackNet;
    };
    auto isIpv6Loopback = [](const UCHAR addr[16]) -> bool {
        static const UCHAR loopback[16] =
            {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        return std::memcmp(addr, loopback, 16) == 0;
    };

    DWORD size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size == 0) return false;

    std::vector<uint8_t> buf(size);
    if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR)
        return false;

    auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        auto& row = table->table[i];
        if (row.dwState != MIB_TCP_STATE_ESTAB) continue;
        auto lp = ntohs(static_cast<u_short>(row.dwLocalPort));
        if (lp != veyonPort) continue;
        // Skip loopback-to-loopback (internal proxy connections)
        if (isLoopback(row.dwLocalAddr) && isLoopback(row.dwRemoteAddr)) continue;
        // Non-loopback connection on Veyon server port = master is viewing
        return true;
    }

    size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size > 0) {
        std::vector<uint8_t> ipv6Buf(size);
        if (GetExtendedTcpTable(ipv6Buf.data(), &size, FALSE, AF_INET6,
                                TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            auto* ipv6Table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(ipv6Buf.data());
            for (DWORD i = 0; i < ipv6Table->dwNumEntries; ++i) {
                auto& row = ipv6Table->table[i];
                if (row.dwState != MIB_TCP_STATE_ESTAB) continue;
                auto lp = ntohs(static_cast<u_short>(row.dwLocalPort));
                if (lp != veyonPort) continue;
                if (isIpv6Loopback(row.ucLocalAddr) && isIpv6Loopback(row.ucRemoteAddr)) continue;
                return true;
            }
        }
    }

    return false;
}

// Timer callback for polling master presence and reloading blacklist
void onPollTimer(SharedState& shared) {
    // Determine presence level:
    //   kViewing — active RFB proxy session (master is seeing your screen)
    //   kAppOpen — TCP connection on veyon port (master app is running) but
    //              not actively viewing this specific machine
    //   kNone    — no connection at all
    bool tcpConnected = detectMasterConnection(shared.sessionId);
    bool viewing = activeSessionCount() > 0;
    {
        std::lock_guard lock(shared.mtx);
        if (viewing)
            shared.presence = PresenceLevel::kViewing;
        else if (tcpConnected)
            shared.presence = PresenceLevel::kAppOpen;
        else
            shared.presence = PresenceLevel::kNone;
    }
    updateOverlay(shared);

    // Blacklist reload
    auto path = std::filesystem::path(shared.blacklistPath);
    if (std::filesystem::exists(path)) {
        auto wt = std::filesystem::last_write_time(path);
        std::lock_guard lock(shared.mtx);
        if (shared.blacklist.empty() || wt != shared.blacklistWriteTime) {
            shared.blacklist = loadBlacklist(shared.blacklistPath);
            shared.blacklistWriteTime = wt;
        }
    } else {
        std::lock_guard lock(shared.mtx);
        shared.blacklist.clear();
    }
}

void toggleFreeze(SharedState& shared) {
    HWND overlayHwnd = nullptr;
    std::vector<std::wstring> blacklist;

    {
        std::lock_guard lock(shared.mtx);
        if (shared.frozen) {
            shared.frozen = false;
            shared.frozenFrame = FrameBuffer{};
            std::wcout << L"LIVE" << std::endl;
            return;
        }
        overlayHwnd = shared.overlayHwnd;
        blacklist = shared.blacklist;
    }

    FrameBuffer capturedFrame;
    if (!captureScreen(capturedFrame) || !capturedFrame.valid()) {
        std::wcerr << L"Failed to capture frozen frame; staying live" << std::endl;
        return;
    }

    FrameBuffer previousTeacherVisible;
    {
        std::lock_guard lock(shared.mtx);
        previousTeacherVisible = shared.teacherVisibleFrame;
    }
    auto maskRects = buildMaskRects(overlayHwnd, blacklist,
                                    capturedFrame.width, capturedFrame.height);
    if (!maskRects.empty()) {
        preserveRectsFromPreviousFrame(capturedFrame, previousTeacherVisible, maskRects);
    }

    {
        std::lock_guard lock(shared.mtx);
        shared.frozen = true;
        shared.frozenFrame = capturedFrame;
        shared.teacherVisibleFrame = std::move(capturedFrame);
    }
    std::wcout << L"FROZEN" << std::endl;
}

LRESULT CALLBACK overlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* shared = reinterpret_cast<SharedState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        shared = reinterpret_cast<SharedState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(shared));
        shared->overlayHwnd = hwnd;
    }

    switch (msg) {
    case WM_CREATE:
        RegisterHotKey(hwnd, kHotkeyFreezeId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'F');
        RegisterHotKey(hwnd, kHotkeyQuitId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');
        SetTimer(hwnd, kPollTimerId, kPollTimerIntervalMs, nullptr);
        SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(kOverlayAlpha), LWA_ALPHA);
        trySetWindowDisplayAffinity(hwnd);
        return 0;
    case WM_TIMER:
        if (shared && wParam == kPollTimerId) onPollTimer(*shared);
        return 0;
    case WM_HOTKEY:
        if (shared && wParam == kHotkeyFreezeId) toggleFreeze(*shared);
        if (wParam == kHotkeyQuitId) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_DISPLAYCHANGE:
        if (shared && shared->overlayShowing != PresenceLevel::kNone) {
            positionOverlay(hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    case WM_PAINT: {
        PresenceLevel level = PresenceLevel::kAppOpen;
        if (shared) {
            std::lock_guard lock(shared->mtx);
            level = shared->overlayShowing;
        }
        paintOverlay(hwnd, level);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kPollTimerId);
        UnregisterHotKey(hwnd, kHotkeyFreezeId);
        UnregisterHotKey(hwnd, kHotkeyQuitId);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool createOverlayWindow(HINSTANCE inst, SharedState& shared) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = overlayWndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    // Note: WS_EX_TRANSPARENT is safe from Veyon's anti-cheat because we use
    // LWA_ALPHA (not LWA_COLORKEY). Veyon only flags windows that have LWA_COLORKEY.
    auto hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
        WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kWindowClassName, L"veyoff-overlay", WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        nullptr, nullptr, inst, &shared);
    if (!hwnd) return false;
    ShowWindow(hwnd, SW_HIDE);
    return true;
}

void trySetWindowDisplayAffinity(HWND hwnd) {
    using SetWindowDisplayAffinityFn = BOOL (WINAPI*)(HWND, DWORD);
    auto user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    auto fn = reinterpret_cast<SetWindowDisplayAffinityFn>(
        GetProcAddress(user32, "SetWindowDisplayAffinity"));
    if (fn) {
        fn(hwnd, WDA_EXCLUDEFROMCAPTURE);
    }
}

// ─── Cleanup / restore ──────────────────────────────────────────────────────

struct CleanupContext {
    int originalPort;
    bool needsRestore;
    HWND quitWindow;
    DWORD sessionId;
};

static CleanupContext g_cleanup{kDefaultVncBasePort, false, nullptr, 0};

void performCleanup() {
    if (!g_cleanup.needsRestore) return;
    stopProxyRuntime();
    std::wcout << L"\nRestoring VNC port to " << g_cleanup.originalPort << std::endl;
    writeVncPortToRegistry(g_cleanup.originalPort);
    restartVeyonService(g_cleanup.originalPort + static_cast<int>(g_cleanup.sessionId));
    g_cleanup.needsRestore = false;
}

BOOL WINAPI consoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT ||
        ctrlType == CTRL_BREAK_EVENT) {
        if (g_cleanup.quitWindow) {
            PostMessageW(g_cleanup.quitWindow, WM_CLOSE, 0, 0);
            return TRUE;
        }
        performCleanup();
        return FALSE;
    }
    return FALSE;
}

}  // namespace

// ─── Main ────────────────────────────────────────────────────────────────────

int wmain(int argc, wchar_t* argv[]) {
    // Parse arguments
    std::wstring blacklistPath = L"config\\blacklist.txt";
    for (int i = 1; i < argc; ++i) {
        std::wstring_view arg = argv[i];
        if (arg == L"--blacklist" && i + 1 < argc) {
            blacklistPath = argv[++i];
        } else if (arg == L"--help" || arg == L"-h") {
            std::wcout << L"Usage: veyoff.exe [--blacklist PATH]" << std::endl;
            return 0;
        }
    }

    // Initialize Winsock
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::wcerr << L"WSAStartup failed" << std::endl;
        return 1;
    }

    DWORD sessionId = veyonLogicalSessionId();
    std::wcout << L"Veyoff starting (session " << sessionId << L")" << std::endl;

    // Step 1: Read current VNC port from registry
    int currentPort = readVncPortFromRegistry();
    if (currentPort < 0) {
        // Registry key doesn't exist, use default
        currentPort = kDefaultVncBasePort;
        std::wcout << L"No VNC port in registry, assuming default " << currentPort << std::endl;
    } else {
        std::wcout << L"Current VNC base port: " << currentPort << std::endl;
    }

    int redirectedPort = currentPort + kPortOffset;
    int listenPort = currentPort + static_cast<int>(sessionId);
    int upstreamPort = redirectedPort + static_cast<int>(sessionId);

    // Step 2: Redirect VNC port in registry
    std::wcout << L"Redirecting VNC port " << currentPort << L" -> " << redirectedPort << std::endl;
    if (!writeVncPortToRegistry(redirectedPort)) {
        std::wcerr << L"Failed to write registry (need admin privileges)" << std::endl;
        WSACleanup();
        return 1;
    }

    g_cleanup.originalPort = currentPort;
    g_cleanup.needsRestore = true;
    g_cleanup.sessionId = sessionId;
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
    // Also restore on normal exit
    atexit(performCleanup);

    // Step 3: Restart Veyon service
    if (!restartVeyonService(upstreamPort)) {
        std::wcerr << L"Failed to restart VeyonService" << std::endl;
        performCleanup();
        WSACleanup();
        return 1;
    }

    std::wcout << L"Proxy: 127.0.0.1:" << listenPort << L" -> 127.0.0.1:"
               << upstreamPort << std::endl;

    // Step 4: Set up shared state
    SharedState shared;
    shared.sessionId = sessionId;
    shared.blacklistPath = expandEnv(blacklistPath);
    shared.blacklist = loadBlacklist(shared.blacklistPath);
    if (std::filesystem::exists(shared.blacklistPath)) {
        shared.blacklistWriteTime = std::filesystem::last_write_time(shared.blacklistPath);
    }

    // Step 5: Create overlay window
    if (!createOverlayWindow(GetModuleHandleW(nullptr), shared)) {
        std::wcerr << L"Failed to create overlay window" << std::endl;
        performCleanup();
        WSACleanup();
        return 1;
    }
    g_cleanup.quitWindow = shared.overlayHwnd;

    // Step 6: Start proxy in background thread
    g_proxyRuntime.thread = std::make_unique<std::thread>([&shared, listenPort, upstreamPort]() {
        proxyMain(shared, listenPort, upstreamPort);
    });

    std::wcout << L"Veyoff active. Ctrl+Alt+F to freeze. Ctrl+Alt+Q to quit." << std::endl;

    // Step 7: Run Win32 message loop (for overlay + hotkey)
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    performCleanup();
    WSACleanup();
    return 0;
}
