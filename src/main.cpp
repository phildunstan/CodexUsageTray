#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <tlhelp32.h>
#include <process.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace Gdiplus;
using namespace std::chrono_literals;

namespace {

constexpr UINT kTrayCallback = WM_APP + 1;
constexpr UINT kUpdateReady = WM_APP + 2;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kTrayId = 1;
constexpr UINT kMenuRefresh = 1001;
constexpr UINT kMenuOpenCodex = 1002;
constexpr UINT kMenuExit = 1003;
constexpr wchar_t kWindowClass[] = L"CodexUsageTrayMessageWindow";
constexpr wchar_t kRunValue[] = L"CodexUsageTray";

struct UsageWindow {
    long long minutes = 0;
    int remaining = 0;
    int expectedRemaining = -1;
};

struct UsageSnapshot {
    UsageWindow weekly;
    std::optional<UsageWindow> shortWindow;
};

struct UpdateResult {
    bool usageOk = false;
    int remaining = 0;
    int expectedRemaining = -1;
    std::optional<UsageWindow> shortWindow;
    bool active = false;
};

HWND g_window = nullptr;
HICON g_icon = nullptr;
ULONG_PTR g_gdiplusToken = 0;
std::atomic_bool g_refreshRunning = false;
int g_remaining = 0;
bool g_hasPercentage = false;
bool g_apiAvailable = false;
bool g_active = false;
int g_expectedRemaining = -1;
std::optional<UsageWindow> g_shortWindow;
UINT g_taskbarCreated = 0;

struct HandleCloser {
    void operator()(void* value) const noexcept {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(value));
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

std::wstring GetEnvironment(const wchar_t* name) {
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (!length) return {};
    std::wstring value(length, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), length);
    if (!written) return {};
    value.resize(written);
    return value;
}

std::wstring CodexExecutable() {
    if (const std::wstring configured = GetEnvironment(L"CODEX_CLI_PATH");
        !configured.empty()) {
        return configured;
    }

    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = SearchPathW(nullptr, L"codex.exe", nullptr,
            static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (!length) return L"codex.exe";
        if (length < buffer.size()) return std::wstring(buffer.data(), length);
        buffer.resize(length + 1);
    }
}

fs::path StateFile() {
    std::wstring local = GetEnvironment(L"LOCALAPPDATA");
    if (local.empty()) local = GetEnvironment(L"TEMP");
    return fs::path(local) / L"CodexUsageTray" / L"state.txt";
}

std::optional<int> LoadLastPercentage() {
    std::ifstream input(StateFile());
    int value = -1;
    if (input >> value && value >= 0 && value <= 100) return value;
    return std::nullopt;
}

void SaveLastPercentage(int value) {
    std::error_code error;
    const fs::path path = StateFile();
    fs::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::trunc);
    if (output) output << value << '\n';
}

bool RegisterAutoStart() {
    wchar_t executable[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!length || length == ARRAYSIZE(executable)) return false;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    const std::wstring command = L"\"" + std::wstring(executable, length) + L"\"";
    const LONG result = RegSetValueExW(key, kRunValue, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool WriteUtf8Line(HANDLE pipe, std::string_view line) {
    std::string message(line);
    message.push_back('\n');
    DWORD written = 0;
    return WriteFile(pipe, message.data(), static_cast<DWORD>(message.size()), &written, nullptr) &&
           written == message.size() && FlushFileBuffers(pipe);
}

bool ReadUtf8Line(HANDLE pipe, HANDLE process, std::string& pending, std::string& line,
                  std::chrono::steady_clock::time_point deadline) {
    while (std::chrono::steady_clock::now() < deadline) {
        if (const size_t newline = pending.find('\n'); newline != std::string::npos) {
            line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }

        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available) {
            char buffer[4096];
            DWORD read = 0;
            if (!ReadFile(pipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr) || !read) {
                return false;
            }
            pending.append(buffer, read);
            continue;
        }
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) return false;
        Sleep(15);
    }
    return false;
}

std::optional<long long> ExtractInteger(std::string_view object, std::string_view key) {
    const size_t keyPosition = object.find(key);
    if (keyPosition == std::string_view::npos) return std::nullopt;
    size_t position = object.find(':', keyPosition + key.size());
    if (position == std::string_view::npos) return std::nullopt;
    ++position;
    while (position < object.size() && (object[position] == ' ' || object[position] == '\t')) ++position;
    bool negative = false;
    if (position < object.size() && object[position] == '-') {
        negative = true;
        ++position;
    }
    if (position >= object.size() || object[position] < '0' || object[position] > '9') return std::nullopt;
    long long value = 0;
    while (position < object.size() && object[position] >= '0' && object[position] <= '9') {
        value = value * 10 + (object[position++] - '0');
    }
    return negative ? -value : value;
}

std::optional<std::string_view> ExtractObject(std::string_view json, std::string_view key) {
    const size_t keyPosition = json.find(key);
    if (keyPosition == std::string_view::npos) return std::nullopt;
    size_t valuePosition = json.find(':', keyPosition + key.size());
    if (valuePosition == std::string_view::npos) return std::nullopt;
    ++valuePosition;
    while (valuePosition < json.size() &&
           (json[valuePosition] == ' ' || json[valuePosition] == '\t' ||
            json[valuePosition] == '\r' || json[valuePosition] == '\n')) {
        ++valuePosition;
    }
    if (valuePosition >= json.size() || json[valuePosition] != '{') return std::nullopt;
    const size_t start = valuePosition;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t i = start; i < json.size(); ++i) {
        const char value = json[i];
        if (inString) {
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == '"') inString = false;
            continue;
        }
        if (value == '"') inString = true;
        else if (value == '{') ++depth;
        else if (value == '}' && --depth == 0) return json.substr(start, i - start + 1);
    }
    return std::nullopt;
}

struct ParsedWindow {
    long long minutes = 0;
    long long used = 0;
    std::optional<long long> resetsAt;
};

std::vector<ParsedWindow> ParseRateLimitWindows(std::string_view limits) {
    std::vector<ParsedWindow> windows;
    for (const std::string_view key : {std::string_view("\"primary\""),
                                       std::string_view("\"secondary\"")}) {
        const auto window = ExtractObject(limits, key);
        if (!window) continue;
        const auto used = ExtractInteger(*window, "\"usedPercent\"");
        const auto minutes = ExtractInteger(*window, "\"windowDurationMins\"");
        const auto resetsAt = ExtractInteger(*window, "\"resetsAt\"");
        if (used && minutes && *minutes > 0) {
            windows.push_back({*minutes, *used, resetsAt});
        }
    }
    return windows;
}

UsageWindow MakeUsageWindow(const ParsedWindow& window) {
    UsageWindow result;
    result.minutes = window.minutes;
    result.remaining = std::clamp(100 - static_cast<int>(window.used), 0, 100);
    if (window.resetsAt && window.minutes > 0) {
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const double secondsRemaining = static_cast<double>(*window.resetsAt - now);
        const double windowSeconds = static_cast<double>(window.minutes * 60);
        result.expectedRemaining = std::clamp(
            static_cast<int>(std::lround(secondsRemaining / windowSeconds * 100.0)), 0, 100);
    }
    return result;
}

std::optional<UsageSnapshot> ParseWeeklyRemaining(std::string_view response) {
    const auto result = ExtractObject(response, "\"result\"");
    if (!result) return std::nullopt;

    std::vector<ParsedWindow> windows;
    if (const auto limits = ExtractObject(*result, "\"rateLimits\"")) {
        windows = ParseRateLimitWindows(*limits);
    }
    // A few protocol versions only populate the multi-bucket view. The
    // `codex` bucket is the one that represents this user's Codex allowance;
    // other buckets (for example Spark) are separate limits.
    if (windows.empty()) {
        if (const auto buckets = ExtractObject(*result, "\"rateLimitsByLimitId\"")) {
            if (const auto codex = ExtractObject(*buckets, "\"codex\"")) {
                windows = ParseRateLimitWindows(*codex);
            }
        }
    }
    if (windows.empty()) return std::nullopt;

    const auto weekly = std::max_element(windows.begin(), windows.end(),
        [](const ParsedWindow& left, const ParsedWindow& right) {
            return left.minutes < right.minutes;
        });
    UsageSnapshot snapshot;
    snapshot.weekly = MakeUsageWindow(*weekly);

    const auto shortWindow = std::min_element(windows.begin(), windows.end(),
        [](const ParsedWindow& left, const ParsedWindow& right) {
            return left.minutes < right.minutes;
        });
    if (windows.size() > 1 && shortWindow->minutes < weekly->minutes) {
        snapshot.shortWindow = MakeUsageWindow(*shortWindow);
    }
    return snapshot;
}

std::optional<UsageSnapshot> QueryWeeklyRemaining() {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE childOutReadRaw = nullptr, childOutWriteRaw = nullptr;
    HANDLE childInReadRaw = nullptr, childInWriteRaw = nullptr;
    if (!CreatePipe(&childOutReadRaw, &childOutWriteRaw, &attributes, 0)) return std::nullopt;
    UniqueHandle childOutRead(childOutReadRaw), childOutWrite(childOutWriteRaw);
    if (!SetHandleInformation(childOutRead.get(), HANDLE_FLAG_INHERIT, 0)) return std::nullopt;
    if (!CreatePipe(&childInReadRaw, &childInWriteRaw, &attributes, 0)) return std::nullopt;
    UniqueHandle childInRead(childInReadRaw), childInWrite(childInWriteRaw);
    if (!SetHandleInformation(childInWrite.get(), HANDLE_FLAG_INHERIT, 0)) return std::nullopt;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = childInRead.get();
    startup.hStdOutput = childOutWrite.get();
    startup.hStdError = childOutWrite.get();
    PROCESS_INFORMATION processInfo{};
    std::wstring command = L"\"" + CodexExecutable() + L"\" app-server --stdio";
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &processInfo)) {
        return std::nullopt;
    }
    UniqueHandle process(processInfo.hProcess), thread(processInfo.hThread);
    childInRead.reset();
    childOutWrite.reset();

    std::string pending, line;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    const auto stopChild = [&] {
        childInWrite.reset();
        if (WaitForSingleObject(process.get(), 1000) == WAIT_TIMEOUT) {
            TerminateProcess(process.get(), 0);
            WaitForSingleObject(process.get(), 1000);
        }
    };

    if (!WriteUtf8Line(childInWrite.get(),
        R"({"id":1,"method":"initialize","params":{"clientInfo":{"name":"codex-usage-tray","version":"1.0"},"capabilities":{"experimentalApi":true}}})")) {
        stopChild();
        return std::nullopt;
    }
    bool initialized = false;
    while (ReadUtf8Line(childOutRead.get(), process.get(), pending, line, deadline)) {
        if (line.find("\"id\":1") != std::string::npos) { initialized = true; break; }
    }
    if (!initialized || !WriteUtf8Line(childInWrite.get(), R"({"method":"initialized"})") ||
        !WriteUtf8Line(childInWrite.get(), R"({"id":2,"method":"account/read","params":{"refreshToken":false}})")) {
        stopChild();
        return std::nullopt;
    }

    std::optional<UsageSnapshot> remaining;
    bool accountRead = false;
    while (ReadUtf8Line(childOutRead.get(), process.get(), pending, line, deadline)) {
        if (!accountRead && line.find("\"id\":2") != std::string::npos) {
            if (line.find("\"error\"") != std::string::npos) {
                stopChild();
                return std::nullopt;
            }
            accountRead = true;
            if (!WriteUtf8Line(childInWrite.get(),
                    R"({"id":3,"method":"account/rateLimits/read","params":{}})")) {
                stopChild();
                return std::nullopt;
            }
            continue;
        }
        if (accountRead && line.find("\"id\":3") != std::string::npos) {
            remaining = ParseWeeklyRemaining(line);
            break;
        }
    }
    stopChild();
    return remaining;
}

bool HasRecentOpenTask(const fs::path& file,
                       std::chrono::steady_clock::time_point deadline) {
    std::error_code error;
    const uintmax_t length = fs::file_size(file, error);
    if (error || !length) return false;
    std::ifstream input(file, std::ios::binary);
    if (!input) return false;

    constexpr size_t chunkSize = 64 * 1024;
    constexpr std::string_view started = "\"type\":\"task_started\"";
    constexpr std::string_view complete = "\"type\":\"task_complete\"";
    uintmax_t end = length;
    std::string later;
    while (end > 0) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        const uintmax_t begin = end > chunkSize ? end - chunkSize : 0;
        const size_t count = static_cast<size_t>(end - begin);
        std::string block(count, '\0');
        input.clear();
        input.seekg(static_cast<std::streamoff>(begin));
        input.read(block.data(), static_cast<std::streamsize>(count));
        block.resize(static_cast<size_t>(input.gcount()));
        block += later;

        const size_t startAt = block.rfind(started);
        const size_t completeAt = block.rfind(complete);
        if (startAt != std::string::npos || completeAt != std::string::npos) {
            return startAt != std::string::npos &&
                   (completeAt == std::string::npos || startAt > completeAt);
        }
        later.assign(block.data(), std::min<size_t>(started.size(), block.size()));
        end = begin;
    }
    return false;
}

bool AnyTaskActive(std::chrono::steady_clock::time_point deadline) {
    const std::wstring profile = GetEnvironment(L"USERPROFILE");
    if (profile.empty()) return false;
    const fs::path sessions = fs::path(profile) / L".codex" / L"sessions";
    std::error_code error;
    if (!fs::exists(sessions, error)) return false;

    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24 * 2);
    fs::recursive_directory_iterator iterator(sessions,
        fs::directory_options::skip_permission_denied, error), end;
    for (; iterator != end; iterator.increment(error)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        if (error) { error.clear(); continue; }
        if (!iterator->is_regular_file(error) || iterator->path().extension() != L".jsonl") continue;
        const auto modified = iterator->last_write_time(error);
        if (!error && modified >= cutoff && HasRecentOpenTask(iterator->path(), deadline)) return true;
        error.clear();
    }
    return false;
}

void DrawUsageRing(Graphics& graphics, const RectF& ring, float width,
                   const UsageWindow& window, const Color& accent) {
    const Color usedColor(255, 205, 205, 205);
    Pen usedPen(usedColor, width);
    Pen accentPen(accent, width);
    usedPen.SetStartCap(LineCapRound); usedPen.SetEndCap(LineCapRound);
    accentPen.SetStartCap(LineCapRound); accentPen.SetEndCap(LineCapRound);

    const int used = 100 - window.remaining;
    if (used > 0) graphics.DrawArc(&usedPen, ring, -90.0f, 3.6f * used);
    if (window.remaining > 0) {
        graphics.DrawArc(&accentPen, ring, -90.0f + 3.6f * used,
            3.6f * window.remaining);
    }
}

void DrawUsagePie(Graphics& graphics, const RectF& circle,
                  const UsageWindow& window, const Color& accent) {
    const Color usedColor(255, 205, 205, 205);
    SolidBrush usedBrush(usedColor);
    SolidBrush accentBrush(accent);

    const int used = 100 - window.remaining;
    if (used > 0) graphics.FillPie(&usedBrush, circle, -90.0f, 3.6f * used);
    if (window.remaining > 0) {
        graphics.FillPie(&accentBrush, circle, -90.0f + 3.6f * used,
            3.6f * window.remaining);
    }
}

HICON CreateStatusIcon(int remaining, int expectedRemaining, bool hasPercentage,
                       bool apiAvailable, bool active,
                       const std::optional<UsageWindow>& shortWindow) {
    // Explorer renders notification icons at 16x16 logical pixels. Draw the
    // ring and inner pie at their final size so they remain distinct after
    // Explorer's notification-area scaling.
    constexpr int size = 16;
    Bitmap bitmap(size, size, PixelFormat32bppARGB);
    Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.Clear(Color(0, 0, 0, 0));

    Color accent = active
        ? Color(255, 48, 139, 245)
        : Color(255, 128, 203, 255);
    const bool weeklyBehind = expectedRemaining >= 0 && remaining < expectedRemaining;
    const bool shortBehind = shortWindow && shortWindow->expectedRemaining >= 0 &&
                             shortWindow->remaining < shortWindow->expectedRemaining;
    if (apiAvailable && (weeklyBehind || shortBehind)) {
        accent = Color(255, 168, 85, 247);
    }

    if (hasPercentage) {
        const UsageWindow weekly{10080, remaining, expectedRemaining};
        if (shortWindow) {
            // When the service reports both windows, the outer ring is the
            // short (normally five-hour) allowance and the inner pie is the
            // weekly allowance. If a future service reports another duration,
            // its shorter window is still displayed here with the same layout.
            DrawUsageRing(graphics, RectF(1.0f, 1.0f, 14.0f, 14.0f), 2.0f,
                *shortWindow, accent);
            DrawUsagePie(graphics, RectF(4.5f, 4.5f, 7.0f, 7.0f),
                weekly, accent);
        } else {
            DrawUsageRing(graphics, RectF(2.0f, 2.0f, 12.0f, 12.0f), 2.0f,
                weekly, accent);
        }
    }

    HICON icon = nullptr;
    bitmap.GetHICON(&icon);
    return icon;
}

std::wstring TooltipText() {
    if (!g_hasPercentage) return L"Codex usage\r\nWeekly usage unavailable";
    std::wstring text = L"Codex usage\r\nWeekly: " + std::to_wstring(g_remaining) +
                        L"% remaining";
    if (g_apiAvailable && g_expectedRemaining >= 0) {
        text += L" (expected " + std::to_wstring(g_expectedRemaining) + L"%)";
    }
    if (g_shortWindow) {
        text += L"\r\n5-hour: " + std::to_wstring(g_shortWindow->remaining) +
                L"% remaining";
        if (g_apiAvailable && g_shortWindow->expectedRemaining >= 0) {
            text += L" (expected " + std::to_wstring(g_shortWindow->expectedRemaining) + L"%)";
        }
    }
    if (!g_apiAvailable) text += L"\r\nData: cached; API unavailable";
    text += L"\r\nStatus: ";
    return text + (g_active ? L"active" : L"idle (last complete)");
}

void AddOrUpdateTrayIcon(bool add) {
    HICON replacement = CreateStatusIcon(g_remaining, g_expectedRemaining,
        g_hasPercentage, g_apiAvailable, g_active, g_shortWindow);
    if (!replacement) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_window;
    data.uID = kTrayId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayCallback;
    data.hIcon = replacement;
    const std::wstring tooltip = TooltipText();
    wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
    if (Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &data)) {
        if (g_icon) DestroyIcon(g_icon);
        g_icon = replacement;
    } else {
        DestroyIcon(replacement);
    }
}

void OpenCodex() {
    // ShellExecute does not reliably resolve bare executable names through the
    // user's PATH or WindowsApps aliases. Activate the installed Codex package
    // directly, then fall back to the CLI launcher if package activation is not
    // available.
    const HINSTANCE activation = ShellExecuteW(nullptr, L"open",
        L"shell:AppsFolder\\OpenAI.Codex_2p2nqsd0c76g0!App",
        nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(activation) > 32) return;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    std::wstring command = L"codex.exe app";
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

void BeginRefresh() {
    bool expected = false;
    if (!g_refreshRunning.compare_exchange_strong(expected, true)) return;
    const uintptr_t thread = _beginthreadex(nullptr, 0, [](void*) -> unsigned {
        auto result = std::make_unique<UpdateResult>();
        auto usage = QueryWeeklyRemaining();
        if (!usage) {
            Sleep(250);
            usage = QueryWeeklyRemaining();
        }
        if (usage) {
            result->usageOk = true;
            result->remaining = usage->weekly.remaining;
            result->expectedRemaining = usage->weekly.expectedRemaining;
            result->shortWindow = usage->shortWindow;
        }
        result->active = AnyTaskActive(std::chrono::steady_clock::now() + 3s);
        UpdateResult* raw = result.release();
        if (!PostMessageW(g_window, kUpdateReady, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
        return 0;
    }, nullptr, 0, nullptr);
    if (thread) CloseHandle(reinterpret_cast<HANDLE>(thread));
    else g_refreshRunning = false;
}

void ShowContextMenu() {
    POINT point{};
    GetCursorPos(&point);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kMenuRefresh, L"Refresh now");
    AppendMenuW(menu, MF_STRING, kMenuOpenCodex, L"Open Codex");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");
    SetForegroundWindow(g_window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
        point.x, point.y, 0, g_window, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == g_taskbarCreated) {
        AddOrUpdateTrayIcon(true);
        return 0;
    }
    switch (message) {
    case WM_CREATE:
        // WM_CREATE is delivered inside CreateWindowExW, before that function
        // returns and assigns its result to the global in wWinMain.
        g_window = window;
        AddOrUpdateTrayIcon(true);
        SetTimer(window, kRefreshTimer, 60'000, nullptr);
        BeginRefresh();
        return 0;
    case WM_TIMER:
        if (wParam == kRefreshTimer) BeginRefresh();
        return 0;
    case kUpdateReady: {
        std::unique_ptr<UpdateResult> result(reinterpret_cast<UpdateResult*>(lParam));
        g_refreshRunning = false;
        g_active = result->active;
        g_apiAvailable = result->usageOk;
        g_expectedRemaining = result->usageOk ? result->expectedRemaining : -1;
        if (result->usageOk) {
            g_remaining = result->remaining;
            g_shortWindow = result->shortWindow;
            g_hasPercentage = true;
            // Refresh the file timestamp as a lightweight success heartbeat,
            // even when the percentage itself has not changed.
            SaveLastPercentage(g_remaining);
        }
        AddOrUpdateTrayIcon(false);
        return 0;
    }
    case kTrayCallback:
        if (wParam == kTrayId) {
            if (lParam == WM_CONTEXTMENU || lParam == WM_RBUTTONUP) ShowContextMenu();
            else if (lParam == WM_LBUTTONDBLCLK) OpenCodex();
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kMenuRefresh: BeginRefresh(); break;
        case kMenuOpenCodex: OpenCodex(); break;
        case kMenuExit: DestroyWindow(window); break;
        }
        return 0;
    case WM_DESTROY: {
        KillTimer(window, kRefreshTimer);
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data); data.hWnd = window; data.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &data);
        if (g_icon) { DestroyIcon(g_icon); g_icon = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    UniqueHandle mutex(CreateMutexW(nullptr, FALSE, L"Local\\CodexUsageTray-7D4AF44D"));
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    GdiplusStartupInput gdiplusInput;
    if (GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr) != Ok) return 1;

    if (const auto cached = LoadLastPercentage()) {
        g_remaining = *cached;
        g_hasPercentage = true;
    }
    RegisterAutoStart();
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) {
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    // Explorer notification icons are most reliable when owned by a conventional
    // top-level window. WS_EX_TOOLWINDOW keeps this hidden owner out of Alt+Tab
    // and the taskbar; the window is never shown.
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClass, L"Codex Usage Tray", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    GdiplusShutdown(g_gdiplusToken);
    return static_cast<int>(message.wParam);
}
