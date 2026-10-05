#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <winhttp.h>
#include <tlhelp32.h>
#include <process.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <cstdlib>
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
constexpr auto kGrokRefreshInterval = 30min;
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
    std::optional<long long> resetsAt;
};

struct UsageSnapshot {
    UsageWindow weekly;
    std::optional<UsageWindow> shortWindow;
};

struct GrokUsageSnapshot {
    std::optional<int> subscriptionRemaining;
    int expectedRemaining = -1;
    std::optional<long long> resetsAt;
    std::optional<long long> extraCreditsCents;
};

struct XaiApiCreditsSnapshot {
    std::optional<long long> paidCreditsCents;
    std::optional<long long> freeCreditsCents;
};

struct UpdateResult {
    bool usageOk = false;
    int remaining = 0;
    int expectedRemaining = -1;
    std::optional<long long> resetsAt;
    std::optional<UsageWindow> shortWindow;
    bool active = false;
    bool grokQueried = false;
    std::optional<GrokUsageSnapshot> grok;
    bool xaiApiConfigured = false;
    std::optional<long long> xaiApiCreditsCents;
    std::optional<long long> xaiApiFreeCreditsCents;
};

HWND g_window = nullptr;
HICON g_icon = nullptr;
ULONG_PTR g_gdiplusToken = 0;
std::atomic_bool g_refreshRunning = false;
std::atomic_bool g_shuttingDown = false;
int g_remaining = 0;
bool g_hasPercentage = false;
bool g_apiAvailable = false;
bool g_active = false;
int g_expectedRemaining = -1;
std::optional<long long> g_resetsAt;
std::optional<UsageWindow> g_shortWindow;
bool g_grokAvailable = false;
std::optional<int> g_grokSubscriptionRemaining;
int g_grokExpectedRemaining = -1;
std::optional<long long> g_grokResetsAt;
std::optional<long long> g_grokExtraCreditsCents;
bool g_xaiApiConfigured = false;
bool g_xaiApiAvailable = false;
std::optional<long long> g_xaiApiCreditsCents;
std::optional<long long> g_xaiApiFreeCreditsCents;
UINT g_taskbarCreated = 0;

struct HandleCloser {
    void operator()(void* value) const noexcept {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(value));
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;
UniqueHandle g_refreshThread;

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

std::wstring GrokExecutable() {
    if (const std::wstring configured = GetEnvironment(L"GROK_CLI_PATH");
        !configured.empty()) {
        return configured;
    }

    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = SearchPathW(nullptr, L"grok.exe", nullptr,
            static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (!length) return L"grok.exe";
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
    while (!g_shuttingDown && std::chrono::steady_clock::now() < deadline) {
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
    while (position < object.size() && (object[position] == ' ' || object[position] == '\t' ||
                                        object[position] == '\r' || object[position] == '\n')) {
        ++position;
    }
    const bool quoted = position < object.size() && object[position] == '"';
    if (quoted) ++position;
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
    if (quoted && (position >= object.size() || object[position] != '"')) return std::nullopt;
    return negative ? -value : value;
}

std::optional<double> ExtractNumber(std::string_view object, std::string_view key) {
    const size_t keyPosition = object.find(key);
    if (keyPosition == std::string_view::npos) return std::nullopt;
    size_t position = object.find(':', keyPosition + key.size());
    if (position == std::string_view::npos) return std::nullopt;
    ++position;
    while (position < object.size() && (object[position] == ' ' || object[position] == '\t' ||
                                        object[position] == '\r' || object[position] == '\n')) {
        ++position;
    }
    const bool quoted = position < object.size() && object[position] == '"';
    if (quoted) ++position;
    const size_t start = position;
    if (position < object.size() && (object[position] == '-' || object[position] == '+')) ++position;
    while (position < object.size() && object[position] >= '0' && object[position] <= '9') ++position;
    if (position < object.size() && object[position] == '.') {
        ++position;
        while (position < object.size() && object[position] >= '0' && object[position] <= '9') ++position;
    }
    if (position < object.size() && (object[position] == 'e' || object[position] == 'E')) {
        ++position;
        if (position < object.size() && (object[position] == '-' || object[position] == '+')) ++position;
        while (position < object.size() && object[position] >= '0' && object[position] <= '9') ++position;
    }
    if (position == start || (quoted && (position >= object.size() || object[position] != '"'))) {
        return std::nullopt;
    }
    char* end = nullptr;
    const std::string text(object.substr(start, position - start));
    const double result = std::strtod(text.c_str(), &end);
    if (!end || end != text.c_str() + text.size()) return std::nullopt;
    return result;
}

std::optional<std::string_view> ExtractString(std::string_view object, std::string_view key) {
    const size_t keyPosition = object.find(key);
    if (keyPosition == std::string_view::npos) return std::nullopt;
    size_t position = object.find(':', keyPosition + key.size());
    if (position == std::string_view::npos) return std::nullopt;
    ++position;
    while (position < object.size() && (object[position] == ' ' || object[position] == '\t' ||
                                        object[position] == '\r' || object[position] == '\n')) {
        ++position;
    }
    if (position >= object.size() || object[position] != '"') return std::nullopt;
    const size_t start = ++position;
    bool escaped = false;
    for (; position < object.size(); ++position) {
        const char value = object[position];
        if (escaped) {
            escaped = false;
        } else if (value == '\\') {
            escaped = true;
        } else if (value == '"') {
            return object.substr(start, position - start);
        }
    }
    return std::nullopt;
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

std::optional<long long> ParseRfc3339Timestamp(std::string_view value) {
    const auto digits = [value](size_t offset, size_t count) -> std::optional<int> {
        if (offset > value.size() || count > value.size() - offset) return std::nullopt;
        int result = 0;
        for (size_t index = 0; index < count; ++index) {
            const char character = value[offset + index];
            if (character < '0' || character > '9') return std::nullopt;
            result = result * 10 + static_cast<int>(character - '0');
        }
        return result;
    };
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' ||
        (value[10] != 'T' && value[10] != 't') || value[13] != ':' ||
        value[16] != ':') {
        return std::nullopt;
    }
    const auto year = digits(0, 4);
    const auto month = digits(5, 2);
    const auto day = digits(8, 2);
    const auto hour = digits(11, 2);
    const auto minute = digits(14, 2);
    const auto second = digits(17, 2);
    if (!year || !month || !day || !hour || !minute || !second ||
        *month < 1 || *month > 12 || *day < 1 || *day > 31 ||
        *hour > 23 || *minute > 59 || *second > 59) {
        return std::nullopt;
    }

    size_t position = 19;
    if (position < value.size() && value[position] == '.') {
        ++position;
        const size_t fractionStart = position;
        while (position < value.size() && value[position] >= '0' && value[position] <= '9') {
            ++position;
        }
        if (position == fractionStart) return std::nullopt;
    }

    int offsetMinutes = 0;
    if (position < value.size() && (value[position] == 'Z' || value[position] == 'z')) {
        ++position;
    } else if (position < value.size() && (value[position] == '+' || value[position] == '-')) {
        const int sign = value[position] == '+' ? 1 : -1;
        ++position;
        const auto offsetHours = digits(position, 2);
        if (!offsetHours) return std::nullopt;
        position += 2;
        if (position < value.size() && value[position] == ':') ++position;
        const auto offsetMinutesPart = digits(position, 2);
        if (!offsetMinutesPart || *offsetHours > 23 || *offsetMinutesPart > 59) {
            return std::nullopt;
        }
        position += 2;
        offsetMinutes = sign * (*offsetHours * 60 + *offsetMinutesPart);
    } else {
        return std::nullopt;
    }
    if (position != value.size()) return std::nullopt;

    std::tm utc{};
    utc.tm_year = *year - 1900;
    utc.tm_mon = *month - 1;
    utc.tm_mday = *day;
    utc.tm_hour = *hour;
    utc.tm_min = *minute;
    utc.tm_sec = *second;
    const __time64_t timestamp = _mkgmtime64(&utc);
    if (timestamp == static_cast<__time64_t>(-1)) return std::nullopt;
    return static_cast<long long>(timestamp) - static_cast<long long>(offsetMinutes) * 60;
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

int CalculateExpectedRemaining(long long start, long long end) {
    if (end <= start) return -1;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const double secondsRemaining = static_cast<double>(end - now);
    const double windowSeconds = static_cast<double>(end - start);
    return std::clamp(
        static_cast<int>(std::lround(secondsRemaining / windowSeconds * 100.0)), 0, 100);
}

UsageWindow MakeUsageWindow(const ParsedWindow& window) {
    UsageWindow result;
    result.minutes = window.minutes;
    result.remaining = std::clamp(100 - static_cast<int>(window.used), 0, 100);
    result.resetsAt = window.resetsAt;
    if (window.resetsAt && window.minutes > 0) {
        result.expectedRemaining = CalculateExpectedRemaining(
            *window.resetsAt - window.minutes * 60, *window.resetsAt);
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

std::optional<GrokUsageSnapshot> ParseGrokBilling(std::string_view response) {
    if (response.find("\"error\"") != std::string_view::npos) return std::nullopt;
    const auto result = ExtractObject(response, "\"result\"");
    if (!result) return std::nullopt;
    const auto config = ExtractObject(*result, "\"config\"");
    if (!config) return std::nullopt;

    GrokUsageSnapshot snapshot;
    std::optional<long long> periodStart;
    std::optional<long long> periodEnd;
    if (const auto period = ExtractObject(*config, "\"currentPeriod\"")) {
        if (const auto start = ExtractString(*period, "\"start\"")) {
            periodStart = ParseRfc3339Timestamp(*start);
        }
        if (const auto end = ExtractString(*period, "\"end\"")) {
            periodEnd = ParseRfc3339Timestamp(*end);
        }
    }
    // Older billing responses expose the same timestamps at the config level.
    if (!periodStart) {
        if (const auto start = ExtractString(*config, "\"billingPeriodStart\"")) {
            periodStart = ParseRfc3339Timestamp(*start);
        }
    }
    if (!periodEnd) {
        if (const auto end = ExtractString(*config, "\"billingPeriodEnd\"")) {
            periodEnd = ParseRfc3339Timestamp(*end);
        }
    }
    if (periodEnd) {
        snapshot.resetsAt = periodEnd;
        const long long start = periodStart.value_or(*periodEnd - 7 * 24 * 60 * 60);
        snapshot.expectedRemaining = CalculateExpectedRemaining(start, *periodEnd);
    }
    if (const auto usedPercent = ExtractNumber(*config, "\"creditUsagePercent\"")) {
        snapshot.subscriptionRemaining = std::clamp(
            static_cast<int>(std::lround(100.0 - *usedPercent)), 0, 100);
    } else {
        const auto monthlyLimit = ExtractObject(*config, "\"monthlyLimit\"");
        const auto used = ExtractObject(*config, "\"used\"");
        if (monthlyLimit && used) {
            const auto limitCents = ExtractInteger(*monthlyLimit, "\"val\"");
            const auto usedCents = ExtractInteger(*used, "\"val\"");
            if (limitCents && *limitCents > 0 && usedCents) {
                snapshot.subscriptionRemaining = std::clamp(
                    static_cast<int>(std::lround(
                        (1.0 - static_cast<double>(*usedCents) / *limitCents) * 100.0)),
                    0, 100);
            }
        }
        // The credits service omits a zero-valued usage percentage from its
        // protobuf JSON. A unified weekly pool with no usage fields therefore
        // represents a full allowance, not a failed query.
        if (!snapshot.subscriptionRemaining &&
            config->find("\"isUnifiedBillingUser\":true") != std::string_view::npos &&
            ExtractObject(*config, "\"currentPeriod\"")) {
            snapshot.subscriptionRemaining = 100;
        }
    }

    if (const auto prepaid = ExtractObject(*config, "\"prepaidBalance\"")) {
        snapshot.extraCreditsCents = ExtractInteger(*prepaid, "\"val\"");
    }
    if (!snapshot.subscriptionRemaining && !snapshot.extraCreditsCents) return std::nullopt;
    return snapshot;
}

bool IsJsonRpcResponseFor(std::string_view line, int id) {
    return ExtractInteger(line, "\"id\"") == id;
}

class GrokRefreshSchedule {
public:
    bool Begin(std::chrono::steady_clock::time_point now, bool force) {
        if (!force && now < nextRefresh_) return false;
        // Failed attempts also wait 30 minutes, rather than retrying every minute.
        nextRefresh_ = now + kGrokRefreshInterval;
        return true;
    }
private:
    std::chrono::steady_clock::time_point nextRefresh_{};
};

GrokRefreshSchedule g_grokSchedule;

bool IsSafeTeamId(std::wstring_view value) {
    if (value.empty()) return false;
    for (const wchar_t character : value) {
        if (!((character >= L'a' && character <= L'z') ||
              (character >= L'A' && character <= L'Z') ||
              (character >= L'0' && character <= L'9') ||
              character == L'-' || character == L'_')) {
            return false;
        }
    }
    return true;
}

bool XaiApiConfigured() {
    const std::wstring managementKey = GetEnvironment(L"XAI_MANAGEMENT_API_KEY");
    const std::wstring teamId = GetEnvironment(L"XAI_TEAM_ID");
    return !managementKey.empty() && IsSafeTeamId(teamId) &&
           managementKey.find_first_of(L"\r\n") == std::wstring::npos;
}

std::optional<long long> ExtractCreditValue(std::string_view json, std::string_view key) {
    if (const auto object = ExtractObject(json, key)) {
        if (const auto value = ExtractInteger(*object, "\"val\"")) return value;
    }
    return ExtractInteger(json, key);
}

std::optional<XaiApiCreditsSnapshot> ParseXaiApiCredits(
    std::string_view prepaidResponse, std::string_view previewResponse) {
    XaiApiCreditsSnapshot snapshot;
    snapshot.paidCreditsCents = ExtractCreditValue(prepaidResponse, "\"total\"");

    const auto freeFrom = [](std::string_view response) -> std::optional<long long> {
        // The postpaid invoice preview calls this the current default credit
        // balance. Older responses and console variants use one of the more
        // explicit free/promotional names, so accept those too.
        for (const std::string_view key : {
                std::string_view("\"defaultCredits\""),
                std::string_view("\"freeCreditsRemaining\""),
                std::string_view("\"freeCredits\""),
                std::string_view("\"promotionalCreditsRemaining\""),
                std::string_view("\"promotionalCredits\""),
                std::string_view("\"promoCreditsRemaining\""),
                std::string_view("\"promoCredits\"")}) {
            if (const auto value = ExtractCreditValue(response, key)) return value;
        }
        return std::nullopt;
    };
    if (const auto defaultCredits = ExtractCreditValue(previewResponse,
            "\"defaultCredits\"")) {
        // The current free balance is composed of the default credits and
        // signed invoice adjustments. Proto3 omits zero-valued fields, so a
        // missing adjustment is equivalent to zero.
        long long freeCredits = *defaultCredits;
        for (const std::string_view key : {
                std::string_view("\"defaultCreditsIssued\""),
                std::string_view("\"autoCreditsIssued\""),
                std::string_view("\"prepaidCredits\"")}) {
            if (const auto value = ExtractCreditValue(previewResponse, key)) {
                freeCredits += *value;
            }
        }
        if (const auto value = ExtractCreditValue(previewResponse,
                "\"prepaidCreditsUsed\"")) {
            freeCredits -= *value;
        }
        snapshot.freeCreditsCents = freeCredits;
    } else {
        snapshot.freeCreditsCents = freeFrom(previewResponse);
    }
    if (!snapshot.freeCreditsCents) snapshot.freeCreditsCents = freeFrom(prepaidResponse);
    if (!snapshot.paidCreditsCents && !snapshot.freeCreditsCents) return std::nullopt;
    return snapshot;
}

struct InternetHandleCloser {
    void operator()(void* handle) const noexcept {
        if (handle) WinHttpCloseHandle(handle);
    }
};
using UniqueInternetHandle = std::unique_ptr<void, InternetHandleCloser>;

struct HttpJsonResponse {
    DWORD status = 0;
    std::string body;
};

std::optional<HttpJsonResponse> GetJsonHttps(const wchar_t* host,
        const wchar_t* path, const std::wstring& headers) {
    if (g_shuttingDown) return std::nullopt;
    UniqueInternetHandle session(WinHttpOpen(L"CodexUsageTray/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return std::nullopt;
    UniqueInternetHandle connection(WinHttpConnect(session.get(), host,
        INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection) return std::nullopt;
    UniqueInternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", path,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) return std::nullopt;

    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
    DWORD compression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
        !WinHttpSetOption(request.get(), WINHTTP_OPTION_DECOMPRESSION, &compression, sizeof(compression)) ||
        !WinHttpSetTimeouts(request.get(), 3000, 3000, 5000, 5000) ||
        !WinHttpAddRequestHeaders(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE) ||
        !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.get(), nullptr)) {
        return std::nullopt;
    }
    HttpJsonResponse response;
    DWORD statusSize = sizeof(response.status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        return std::nullopt;
    }
    // Preserve the status for authentication recovery; discard error bodies.
    if (response.status < 200 || response.status >= 300) return response;
    for (;;) {
        if (g_shuttingDown) return std::nullopt;
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) return std::nullopt;
        if (!available) break;
        if (response.body.size() + available > 1'000'000) return std::nullopt;
        const size_t start = response.body.size();
        response.body.resize(start + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), response.body.data() + start, available, &read)) {
            return std::nullopt;
        }
        response.body.resize(start + read);
        if (!read) break;
    }
    return response;
}

std::optional<std::string> QueryXaiManagementApi(std::wstring_view path,
                                               const std::wstring& managementKey) {
    const std::wstring requestPath(path);
    auto response = GetJsonHttps(L"management-api.x.ai", requestPath.c_str(),
        L"Authorization: Bearer " + managementKey);
    if (!response || response->status < 200 || response->status >= 300) return std::nullopt;
    return std::move(response->body);
}

// Return only immediate object members, so credentials cannot mix fields from
// different account entries. String views stay within the bounded source file.
using JsonFields = std::vector<std::pair<std::string_view, std::string_view>>;
std::optional<JsonFields> JsonObjectFields(std::string_view json) {
    size_t at = 0;
    const auto skipSpace = [&] {
        while (at < json.size() && (json[at] == ' ' || json[at] == '\t' ||
                json[at] == '\r' || json[at] == '\n')) ++at;
    };
    const auto scanString = [&]() -> bool {
        if (at >= json.size() || json[at++] != '"') return false;
        while (at < json.size()) {
            const char c = json[at++];
            if (static_cast<unsigned char>(c) < 32) return false;
            if (c == '"') return true;
            if (c == '\\') {
                if (at == json.size()) return false;
                ++at;
            }
        }
        return false;
    };
    skipSpace();
    if (at == json.size() || json[at++] != '{') return std::nullopt;
    JsonFields fields;
    skipSpace();
    if (at < json.size() && json[at] == '}') {
        ++at;
        skipSpace();
        return at == json.size() ? std::optional<JsonFields>(fields) : std::nullopt;
    }
    for (;;) {
        skipSpace();
        const size_t keyStart = at;
        if (!scanString()) return std::nullopt;
        const auto key = json.substr(keyStart + 1, at - keyStart - 2);
        skipSpace();
        if (at == json.size() || json[at++] != ':') return std::nullopt;
        skipSpace();
        const size_t valueStart = at;
        if (at == json.size()) return std::nullopt;
        if (json[at] == '"') {
            if (!scanString()) return std::nullopt;
        } else if (json[at] == '{' || json[at] == '[') {
            std::string closers;
            do {
                const char c = json[at];
                if (c == '"') {
                    if (!scanString()) return std::nullopt;
                    continue;
                }
                ++at;
                if (c == '{') closers += '}';
                else if (c == '[') closers += ']';
                else if (c == '}' || c == ']') {
                    if (closers.empty() || closers.back() != c) return std::nullopt;
                    closers.pop_back();
                }
                if (closers.size() > 32) return std::nullopt;
            } while (!closers.empty() && at < json.size());
            if (!closers.empty()) return std::nullopt;
        } else {
            while (at < json.size() && json[at] != ',' && json[at] != '}') ++at;
            if (at == valueStart) return std::nullopt;
        }
        if (std::any_of(fields.begin(), fields.end(), [key](const auto& field) { return field.first == key; })) {
            return std::nullopt;
        }
        fields.emplace_back(key, json.substr(valueStart, at - valueStart));
        skipSpace();
        if (at == json.size()) return std::nullopt;
        const char separator = json[at++];
        if (separator == '}') {
            skipSpace();
            return at == json.size() ? std::optional<JsonFields>(fields) : std::nullopt;
        }
        if (separator != ',') return std::nullopt;
    }
}

std::optional<std::string_view> JsonPlainString(const JsonFields& fields, std::string_view key) {
    for (const auto& [name, raw] : fields) {
        if (name != key) continue;
        if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') return std::nullopt;
        const auto value = raw.substr(1, raw.size() - 2);
        // Credential keys, IDs, scopes and timestamps contain no JSON escapes.
        if (value.find('\\') != std::string_view::npos) return std::nullopt;
        return value;
    }
    return std::nullopt;
}

bool SafeHeaderValue(std::string_view value) {
    return !value.empty() && value.size() <= 16'384 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 33 && c <= 126; });
}

std::optional<std::string_view> JsonField(const JsonFields& fields, std::string_view key) {
    for (const auto& [name, value] : fields) {
        if (name == key) {
            auto trimmed = value;
            while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t' ||
                    trimmed.back() == '\r' || trimmed.back() == '\n')) trimmed.remove_suffix(1);
            return trimmed;
        }
    }
    return std::nullopt;
}

std::optional<long long> JsonInteger(const JsonFields& fields, std::string_view key) {
    const auto raw = JsonField(fields, key);
    if (!raw) return std::nullopt;
    const auto value = *raw;
    long long result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return std::nullopt;
    return result;
}

std::optional<std::string> DecodeBase64Url(std::string_view value) {
    if (value.empty() || value.size() > 16'384 || value.size() % 4 == 1) return std::nullopt;
    std::string decoded;
    unsigned int accumulator = 0;
    int bits = 0;
    for (const char c : value) {
        const size_t digit = std::string_view(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_").find(c);
        if (digit == std::string_view::npos) return std::nullopt;
        accumulator = (accumulator << 6) | static_cast<unsigned int>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded += static_cast<char>((accumulator >> bits) & 255);
        }
    }
    if (bits && (accumulator & ((1u << bits) - 1u))) return std::nullopt;
    return decoded;
}

struct CodexCredentials {
    std::string token;
    std::string accountId;
    long long expiresAt = 0;
};

std::optional<CodexCredentials> ParseCodexCredentials(std::string_view json) {
    const auto root = JsonObjectFields(json);
    if (!root) return std::nullopt;
    // Legacy ChatGPT auth files omit auth_mode. API-key and external-token
    // modes must not be refreshed using Codex's managed OAuth lifecycle.
    if (JsonField(*root, "auth_mode") && JsonPlainString(*root, "auth_mode") != "chatgpt") {
        return std::nullopt;
    }
    const auto rawTokens = JsonField(*root, "tokens");
    if (!rawTokens) return std::nullopt;
    const auto tokens = JsonObjectFields(*rawTokens);
    if (!tokens) return std::nullopt;
    const auto token = JsonPlainString(*tokens, "access_token");
    const auto accountId = JsonPlainString(*tokens, "account_id");
    if (!token || !accountId || !SafeHeaderValue(*token) || !SafeHeaderValue(*accountId)) {
        return std::nullopt;
    }
    const size_t first = token->find('.');
    const size_t second = first == std::string_view::npos ? first : token->find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos ||
            first == 0 || second + 1 == token->size() || token->find('.', second + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    auto payload = DecodeBase64Url(token->substr(first + 1, second - first - 1));
    if (!payload) return std::nullopt;
    const auto claims = JsonObjectFields(*payload);
    const auto expiry = claims ? JsonInteger(*claims, "exp") : std::nullopt;
    SecureZeroMemory(payload->data(), payload->size());
    // JWT decoding is only an expiry hint; the HTTPS service authenticates it.
    if (!expiry || *expiry <= 0) return std::nullopt;
    return CodexCredentials{std::string(*token), std::string(*accountId), *expiry};
}

fs::path CodexAuthFile() {
    const auto configured = GetEnvironment(L"CODEX_HOME");
    if (!configured.empty()) return fs::path(configured) / L"auth.json";
    const auto profile = GetEnvironment(L"USERPROFILE");
    return profile.empty() ? fs::path{} : fs::path(profile) / L".codex" / L"auth.json";
}

std::optional<CodexCredentials> LoadCodexCredentials() {
    std::ifstream input(CodexAuthFile(), std::ios::binary | std::ios::ate);
    if (!input) return std::nullopt;
    const auto size = input.tellg();
    if (size <= 0 || size > 1'000'000) return std::nullopt;
    input.seekg(0);
    std::string json(static_cast<size_t>(size), '\0');
    const bool read = static_cast<bool>(input.read(json.data(), static_cast<std::streamsize>(json.size())));
    auto credentials = read ? ParseCodexCredentials(json) : std::nullopt;
    SecureZeroMemory(json.data(), json.size());
    return credentials;
}

std::optional<HttpJsonResponse> RequestCodexUsage(const CodexCredentials& credentials) {
    if (!SafeHeaderValue(credentials.token) || !SafeHeaderValue(credentials.accountId)) return std::nullopt;
    std::wstring headers = L"Authorization: Bearer " +
        std::wstring(credentials.token.begin(), credentials.token.end()) +
        L"\r\nChatGPT-Account-Id: " + std::wstring(credentials.accountId.begin(), credentials.accountId.end()) +
        L"\r\nAccept: application/json";
    auto response = GetJsonHttps(L"chatgpt.com", L"/backend-api/wham/usage", headers);
    SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));
    return response;
}

std::optional<UsageSnapshot> ParseCodexUsage(std::string_view response) {
    const auto root = JsonObjectFields(response);
    if (!root) return std::nullopt;
    const auto rawLimits = JsonField(*root, "rate_limit");
    if (!rawLimits) return std::nullopt;
    const auto limits = JsonObjectFields(*rawLimits);
    if (!limits) return std::nullopt;
    std::vector<ParsedWindow> windows;
    for (const auto key : {"primary_window", "secondary_window"}) {
        const auto rawWindow = JsonField(*limits, key);
        if (!rawWindow || *rawWindow == "null") continue;
        const auto window = JsonObjectFields(*rawWindow);
        if (!window) return std::nullopt;
        const auto used = JsonInteger(*window, "used_percent");
        const auto seconds = JsonInteger(*window, "limit_window_seconds");
        const auto resetsAt = JsonInteger(*window, "reset_at");
        if (!used || *used < 0 || *used > 100 || !seconds || *seconds <= 0 ||
                *seconds % 60 != 0 || *seconds > 365LL * 24 * 60 * 60 || !resetsAt || *resetsAt <= 0) {
            return std::nullopt;
        }
        windows.push_back({*seconds / 60, *used, resetsAt});
    }
    if (windows.empty()) return std::nullopt;
    std::sort(windows.begin(), windows.end(), [](const ParsedWindow& left, const ParsedWindow& right) {
        return left.minutes < right.minutes;
    });
    UsageSnapshot snapshot;
    snapshot.weekly = MakeUsageWindow(windows.back());
    if (windows.size() > 1 && windows.front().minutes < windows.back().minutes) {
        snapshot.shortWindow = MakeUsageWindow(windows.front());
    }
    return snapshot;
}

struct GrokCredentials {
    std::string token;
    std::string userId;
    std::optional<long long> expiresAt;
};

std::optional<GrokCredentials> ParseGrokCredentials(std::string_view json) {
    const auto scopes = JsonObjectFields(json);
    if (!scopes) return std::nullopt;
    std::optional<GrokCredentials> selected;
    for (const auto& [scope, entry] : *scopes) {
        // Send only the first-party cached login to the first-party billing host.
        if (!scope.starts_with("https://auth.x.ai::")) continue;
        const auto fields = JsonObjectFields(entry);
        if (!fields) continue;
        const auto mode = JsonPlainString(*fields, "auth_mode");
        const auto token = JsonPlainString(*fields, "key");
        const auto userId = JsonPlainString(*fields, "user_id");
        if (!mode || *mode != "oidc" || !token || !userId ||
                !SafeHeaderValue(*token) || !SafeHeaderValue(*userId)) continue;
        GrokCredentials credentials{std::string(*token), std::string(*userId), std::nullopt};
        if (const auto expiry = JsonPlainString(*fields, "expires_at")) {
            credentials.expiresAt = ParseRfc3339Timestamp(*expiry);
            if (!credentials.expiresAt) continue;
        } else if (const auto created = JsonPlainString(*fields, "create_time")) {
            if (const auto timestamp = ParseRfc3339Timestamp(*created)) {
                credentials.expiresAt = *timestamp + 30LL * 24 * 60 * 60;
            } else continue;
        }
        if (!selected || credentials.expiresAt.value_or(0) > selected->expiresAt.value_or(0)) {
            selected = std::move(credentials);
        }
    }
    return selected;
}

fs::path GrokAuthFile() {
    const std::wstring configured = GetEnvironment(L"GROK_HOME");
    if (!configured.empty()) return fs::path(configured) / L"auth.json";
    const std::wstring profile = GetEnvironment(L"USERPROFILE");
    return profile.empty() ? fs::path{} : fs::path(profile) / L".grok" / L"auth.json";
}

std::optional<GrokCredentials> LoadGrokCredentials() {
    std::ifstream input(GrokAuthFile(), std::ios::binary | std::ios::ate);
    if (!input) return std::nullopt;
    const std::streamoff size = input.tellg();
    if (size <= 0 || size > 1'000'000) return std::nullopt;
    std::string json(static_cast<size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(json.data(), size)) return std::nullopt;
    auto credentials = ParseGrokCredentials(json);
    SecureZeroMemory(json.data(), json.size());
    return credentials;
}

std::optional<HttpJsonResponse> RequestGrokBilling(const GrokCredentials& credentials) {
    if (!SafeHeaderValue(credentials.token) || !SafeHeaderValue(credentials.userId)) return std::nullopt;
    const std::wstring token(credentials.token.begin(), credentials.token.end());
    const std::wstring userId(credentials.userId.begin(), credentials.userId.end());
    std::wstring headers = L"Authorization: Bearer " + token +
        L"\r\nX-XAI-Token-Auth: xai-grok-cli\r\nx-userid: " + userId +
        L"\r\nAccept: application/json";
    auto response = GetJsonHttps(L"cli-chat-proxy.grok.com", L"/v1/billing?format=credits", headers);
    SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));
    return response;
}

bool RefreshGrokCredentials() {
    // Let the CLI own OAuth rotation and auth.json locking. This is used only
    // near token expiry or after a rejected token, never for normal polls.
    if (g_shuttingDown) return false;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE outReadRaw = nullptr, outWriteRaw = nullptr, inReadRaw = nullptr, inWriteRaw = nullptr;
    if (!CreatePipe(&outReadRaw, &outWriteRaw, &attributes, 0)) return false;
    UniqueHandle outRead(outReadRaw), outWrite(outWriteRaw);
    if (!SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0)) return false;
    if (!CreatePipe(&inReadRaw, &inWriteRaw, &attributes, 0)) return false;
    UniqueHandle inRead(inReadRaw), inWrite(inWriteRaw);
    if (!SetHandleInformation(inWrite.get(), HANDLE_FLAG_INHERIT, 0)) return false;
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        return false;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = inRead.get();
    startup.hStdOutput = outWrite.get();
    startup.hStdError = outWrite.get();
    PROCESS_INFORMATION info{};
    std::wstring command = L"\"" + GrokExecutable() + L"\" agent --no-leader stdio";
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &info)) return false;
    UniqueHandle process(info.hProcess), thread(info.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        TerminateProcess(process.get(), 0);
        WaitForSingleObject(process.get(), 1000);
        return false;
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) return false;
    inRead.reset();
    outWrite.reset();
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    std::string pending, line;
    const auto exchange = [&](int id, std::string_view rpc) {
        std::string message(rpc);
        message += '\n';
        DWORD written = 0;
        if (!WriteFile(inWrite.get(), message.data(), static_cast<DWORD>(message.size()), &written, nullptr) ||
                written != message.size()) return false;
        while (ReadUtf8Line(outRead.get(), process.get(), pending, line, deadline)) {
            if (IsJsonRpcResponseFor(line, id)) return line.find("\"error\"") == std::string::npos;
            if (pending.size() > 1'000'000) return false;
        }
        return false;
    };
    const bool ready = exchange(1,
        R"({"id":1,"method":"initialize","params":{"protocolVersion":1,"clientCapabilities":{},"clientInfo":{"name":"codex-usage-tray","version":"1.0"}}})") &&
        exchange(2, R"({"id":2,"method":"_x.ai/billing","params":{}})");
    inWrite.reset();
    if (WaitForSingleObject(process.get(), 1000) == WAIT_TIMEOUT) {
        TerminateJobObject(job.get(), 0);
        WaitForSingleObject(process.get(), 1000);
    }
    return ready;
}

class GrokBillingClient {
public:
    using Loader = std::function<std::optional<GrokCredentials>()>;
    using Request = std::function<std::optional<HttpJsonResponse>(const GrokCredentials&)>;
    using Refresh = std::function<bool()>;

    GrokBillingClient(Loader load = LoadGrokCredentials, Request request = RequestGrokBilling,
                     Refresh refresh = RefreshGrokCredentials)
        : load_(std::move(load)), request_(std::move(request)), refresh_(std::move(refresh)) {}

    std::optional<GrokUsageSnapshot> Query(long long now = static_cast<long long>(std::time(nullptr))) {
        if (g_shuttingDown) return std::nullopt;
        auto credentials = load_();
        if (!credentials) return std::nullopt;
        bool refreshed = false;
        const auto nearExpiry = [now](const GrokCredentials& value) {
            return value.expiresAt && now >= *value.expiresAt - 300;
        };
        if (nearExpiry(*credentials)) {
            if (!refresh_()) return std::nullopt;
            refreshed = true;
            credentials = load_();
            if (!credentials || nearExpiry(*credentials)) return std::nullopt;
        }
        auto response = request_(*credentials);
        if (response && (response->status == 401 || response->status == 403) && !refreshed) {
            // Another Grok process may already have rotated the shared login.
            auto current = load_();
            if (!current || current->token == credentials->token || nearExpiry(*current)) {
                if (!refresh_()) return std::nullopt;
                current = load_();
            }
            if (!current || nearExpiry(*current)) return std::nullopt;
            response = request_(*current);
        }
        if (!response || response->status < 200 || response->status >= 300) return std::nullopt;
        // The HTTPS endpoint returns the billing object without an ACP result envelope.
        return ParseGrokBilling("{\"result\":" + response->body + "}");
    }

private:
    Loader load_;
    Request request_;
    Refresh refresh_;
};

GrokBillingClient g_grokClient;

std::optional<XaiApiCreditsSnapshot> QueryXaiApiCredits() {
    const std::wstring managementKey = GetEnvironment(L"XAI_MANAGEMENT_API_KEY");
    const std::wstring teamId = GetEnvironment(L"XAI_TEAM_ID");
    if (!XaiApiConfigured()) {
        return std::nullopt;
    }
    const std::wstring prepaidPath = L"/v1/billing/teams/" + teamId + L"/prepaid/balance";
    const auto prepaid = QueryXaiManagementApi(prepaidPath, managementKey);
    const std::wstring previewPath =
        L"/v1/billing/teams/" + teamId + L"/postpaid/invoice/preview";
    const auto preview = QueryXaiManagementApi(previewPath, managementKey);
    // The two billing endpoints are independent. Keep a valid preview result
    // when the prepaid endpoint is unavailable (and vice versa), so a
    // transient failure in one balance does not erase the other from the UI.
    const std::string_view prepaidResponse = prepaid
        ? std::string_view(*prepaid) : std::string_view{};
    const std::string_view previewResponse = preview
        ? std::string_view(*preview) : std::string_view{};
    return ParseXaiApiCredits(prepaidResponse, previewResponse);
}

bool RefreshCodexCredentials() {
    // Codex owns token rotation and shared credential-store writes. No model
    // requests or usage reads are needed for this occasional auth refresh.
    if (g_shuttingDown) return false;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE childOutReadRaw = nullptr, childOutWriteRaw = nullptr;
    HANDLE childInReadRaw = nullptr, childInWriteRaw = nullptr;
    if (!CreatePipe(&childOutReadRaw, &childOutWriteRaw, &attributes, 0)) return false;
    UniqueHandle childOutRead(childOutReadRaw), childOutWrite(childOutWriteRaw);
    if (!SetHandleInformation(childOutRead.get(), HANDLE_FLAG_INHERIT, 0)) return false;
    if (!CreatePipe(&childInReadRaw, &childInWriteRaw, &attributes, 0)) return false;
    UniqueHandle childInRead(childInReadRaw), childInWrite(childInWriteRaw);
    if (!SetHandleInformation(childInWrite.get(), HANDLE_FLAG_INHERIT, 0)) return false;
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        return false;
    }

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
            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &processInfo)) {
        return false;
    }
    UniqueHandle process(processInfo.hProcess), thread(processInfo.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        TerminateProcess(process.get(), 0);
        WaitForSingleObject(process.get(), 1000);
        return false;
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) return false;
    childInRead.reset();
    childOutWrite.reset();

    std::string pending, line;
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    const auto stopChild = [&] {
        childInWrite.reset();
        if (WaitForSingleObject(process.get(), 1000) == WAIT_TIMEOUT) {
            TerminateJobObject(job.get(), 0);
            WaitForSingleObject(process.get(), 1000);
        }
    };

    if (!WriteUtf8Line(childInWrite.get(),
        R"({"id":1,"method":"initialize","params":{"clientInfo":{"name":"codex-usage-tray","version":"1.0"},"capabilities":{"experimentalApi":true}}})")) {
        stopChild();
        return false;
    }
    bool initialized = false;
    while (ReadUtf8Line(childOutRead.get(), process.get(), pending, line, deadline)) {
        if (IsJsonRpcResponseFor(line, 1)) {
            initialized = line.find("\"error\"") == std::string::npos;
            break;
        }
    }
    if (!initialized || !WriteUtf8Line(childInWrite.get(), R"({"method":"initialized"})") ||
        !WriteUtf8Line(childInWrite.get(), R"({"id":2,"method":"account/read","params":{"refreshToken":true}})")) {
        stopChild();
        return false;
    }

    bool refreshed = false;
    while (ReadUtf8Line(childOutRead.get(), process.get(), pending, line, deadline)) {
        if (IsJsonRpcResponseFor(line, 2)) {
            const auto result = ExtractObject(line, "\"result\"");
            const auto account = result ? ExtractObject(*result, "\"account\"") : std::nullopt;
            const auto fields = account ? JsonObjectFields(*account) : std::nullopt;
            refreshed = line.find("\"error\"") == std::string::npos && fields &&
                JsonPlainString(*fields, "type") == "chatgpt";
            break;
        }
        if (pending.size() > 1'000'000) break;
    }
    stopChild();
    return refreshed;
}

class CodexUsageClient {
public:
    using Loader = std::function<std::optional<CodexCredentials>()>;
    using Request = std::function<std::optional<HttpJsonResponse>(const CodexCredentials&)>;
    using Refresh = std::function<bool()>;

    CodexUsageClient(Loader load = LoadCodexCredentials, Request request = RequestCodexUsage,
                     Refresh refresh = RefreshCodexCredentials)
        : load_(std::move(load)), request_(std::move(request)), refresh_(std::move(refresh)) {}

    std::optional<UsageSnapshot> Query(long long now = static_cast<long long>(std::time(nullptr))) {
        if (g_shuttingDown) return std::nullopt;
        auto credentials = load_();
        if (!credentials) return std::nullopt;
        const auto nearExpiry = [now](const CodexCredentials& value) {
            return now >= value.expiresAt - 300;
        };
        bool refreshed = false;
        if (nearExpiry(*credentials)) {
            if (!refresh_()) return std::nullopt;
            refreshed = true;
            credentials = load_();
            if (!credentials || nearExpiry(*credentials)) return std::nullopt;
        }
        auto response = request_(*credentials);
        if (response && response->status == 401 && !refreshed) {
            // Prefer a login already rotated by the desktop app or another CLI.
            auto current = load_();
            if (!current || current->token == credentials->token || nearExpiry(*current)) {
                if (!refresh_()) return std::nullopt;
                current = load_();
            }
            if (!current || nearExpiry(*current)) return std::nullopt;
            response = request_(*current);
        }
        if (!response || response->status < 200 || response->status >= 300) return std::nullopt;
        return ParseCodexUsage(response->body);
    }

private:
    Loader load_;
    Request request_;
    Refresh refresh_;
};

CodexUsageClient g_codexClient;

std::optional<UsageSnapshot> QueryWeeklyRemaining() {
    return g_codexClient.Query();
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

std::wstring FormatUsdCents(long long cents) {
    const long long magnitude = cents < 0 ? -cents : cents;
    std::wstring result = L"$" + std::to_wstring(magnitude / 100) + L".";
    const int remainder = static_cast<int>(magnitude % 100);
    if (remainder < 10) result.push_back(L'0');
    result += std::to_wstring(remainder);
    return result;
}

std::wstring FormatResetTime(const std::optional<long long>& resetsAt, bool includeWeekday) {
    if (!resetsAt) return {};
    const std::time_t timestamp = static_cast<std::time_t>(*resetsAt);
    std::tm localTime{};
    if (localtime_s(&localTime, &timestamp) != 0) return {};

    wchar_t buffer[32]{};
    const wchar_t* format = includeWeekday ? L"reset %A %H:%M" : L"reset %H:%M";
    if (!std::wcsftime(buffer, _countof(buffer), format, &localTime)) return {};
    return buffer;
}

std::wstring TooltipText() {
    std::wstring text = L"Codex 7d: ";
    if (g_hasPercentage) {
        text += std::to_wstring(g_remaining) + L"%";
        if (g_apiAvailable && g_expectedRemaining >= 0) {
            text += L" (expected " + std::to_wstring(g_expectedRemaining) + L"%)";
        }
        if (g_apiAvailable) {
            if (const std::wstring reset = FormatResetTime(g_resetsAt, true);
                !reset.empty()) {
                text += L" - " + reset;
            }
        }
    } else {
        text += L"n/a";
    }
    if (g_shortWindow) {
        text += L"\r\nCodex 5h: " + std::to_wstring(g_shortWindow->remaining) + L"%";
        if (const std::wstring reset = FormatResetTime(g_shortWindow->resetsAt, false);
            !reset.empty()) {
            text += L" - " + reset;
        }
    }
    text += L"\r\nxAI credits: ";
    if (!g_xaiApiConfigured) {
        text += L"not set";
    } else if (g_xaiApiAvailable && g_xaiApiFreeCreditsCents) {
        text += FormatUsdCents(*g_xaiApiFreeCreditsCents);
    } else {
        text += L"n/a";
    }
    return text;
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

void BeginRefresh(bool forceGrok = false) {
    if (g_shuttingDown) return;
    bool expected = false;
    if (!g_refreshRunning.compare_exchange_strong(expected, true)) return;
    const uintptr_t thread = _beginthreadex(nullptr, 0, [](void* context) -> unsigned {
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
            result->resetsAt = usage->weekly.resetsAt;
            result->shortWindow = usage->shortWindow;
        }
        if (!g_shuttingDown && g_grokSchedule.Begin(std::chrono::steady_clock::now(), context != nullptr)) {
            result->grokQueried = true;
            result->grok = g_grokClient.Query();
        }
        result->xaiApiConfigured = XaiApiConfigured();
        if (result->xaiApiConfigured) {
            if (const auto xai = QueryXaiApiCredits()) {
                result->xaiApiCreditsCents = xai->paidCreditsCents;
                result->xaiApiFreeCreditsCents = xai->freeCreditsCents;
            }
        }
        result->active = AnyTaskActive(std::chrono::steady_clock::now() + 3s);
        UpdateResult* raw = result.release();
        if (!PostMessageW(g_window, kUpdateReady, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
        return 0;
    }, forceGrok ? reinterpret_cast<void*>(1) : nullptr, 0, nullptr);
    if (thread) g_refreshThread.reset(reinterpret_cast<HANDLE>(thread));
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
        g_resetsAt = result->usageOk ? result->resetsAt : std::nullopt;
        if (result->usageOk) {
            g_remaining = result->remaining;
            g_shortWindow = result->shortWindow;
            g_hasPercentage = true;
            // Refresh the file timestamp as a lightweight success heartbeat,
            // even when the percentage itself has not changed.
            SaveLastPercentage(g_remaining);
        }
        if (result->grokQueried && result->grok) {
            g_grokAvailable = true;
            g_grokSubscriptionRemaining = result->grok->subscriptionRemaining;
            g_grokExpectedRemaining = result->grok->expectedRemaining;
            g_grokResetsAt = result->grok->resetsAt;
            g_grokExtraCreditsCents = result->grok->extraCreditsCents;
        } else if (result->grokQueried) {
            g_grokAvailable = false;
            g_grokExpectedRemaining = -1;
            g_grokResetsAt.reset();
        }
        g_xaiApiConfigured = result->xaiApiConfigured;
        g_xaiApiAvailable = result->xaiApiConfigured &&
            (result->xaiApiCreditsCents || result->xaiApiFreeCreditsCents);
        if (result->xaiApiConfigured &&
            (result->xaiApiCreditsCents || result->xaiApiFreeCreditsCents)) {
            g_xaiApiCreditsCents = result->xaiApiCreditsCents;
            g_xaiApiFreeCreditsCents = result->xaiApiFreeCreditsCents;
        } else if (!result->xaiApiConfigured) {
            g_xaiApiCreditsCents.reset();
            g_xaiApiFreeCreditsCents.reset();
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
        case kMenuRefresh: BeginRefresh(true); break;
        case kMenuOpenCodex: OpenCodex(); break;
        case kMenuExit: DestroyWindow(window); break;
        }
        return 0;
    case WM_DESTROY: {
        g_shuttingDown = true;
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
    if (g_refreshThread) WaitForSingleObject(g_refreshThread.get(), INFINITE);
    GdiplusShutdown(g_gdiplusToken);
    return static_cast<int>(message.wParam);
}
