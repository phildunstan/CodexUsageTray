#include "../src/main.cpp"
#include <iostream>
#include <stdexcept>

void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

const std::string usage = R"({"rate_limit":{"primary_window":{"used_percent":21,"limit_window_seconds":604800,"reset_at":1791581263},"secondary_window":{"used_percent":40,"limit_window_seconds":18000,"reset_at":1791230000}},"code_review_rate_limit":{"primary_window":{"used_percent":99,"limit_window_seconds":604800,"reset_at":1791581263}}})";
const std::string token = "header.eyJleHAiOjE3OTEyMDAwMDB9.signature"; // {"exp":1791200000}

int wmain(int argc, wchar_t** argv) {
    try {
        const long long now = 1791190000;
        const std::string auth = "{\"auth_mode\":\"chatgpt\",\"tokens\":{\"access_token\":\"" + token + "\",\"account_id\":\"account\"}}";
        const auto parsed = ParseCodexCredentials(auth);
        Check(parsed && parsed->token == token && parsed->accountId == "account" && parsed->expiresAt == 1791200000,
            "Managed ChatGPT tokens and JWT expiry must parse together");
        Check(ParseCodexCredentials("{\"tokens\":{\"access_token\":\"" + token + "\",\"account_id\":\"account\"}}").has_value(),
            "Legacy auth files may omit auth_mode");
        for (const char* malformed : {
            R"({"auth_mode":"apikey","tokens":{"access_token":"secret","account_id":"account"}})",
            R"({"auth_mode":"chatgptAuthTokens","tokens":{"access_token":"secret","account_id":"account"}})",
            R"({"tokens":{"access_token":"not-a-jwt","account_id":"account"}})",
            R"({"tokens":{"access_token":"header.eyJleHAiOjE3OTEyMDAwMDB9.signature","nested":{"account_id":"wrong"}}})",
            R"({"tokens":{"access_token":"header.eyJleHAiOjE3OTEyMDAwMDB9.signature","account_id":"id\r\nInjected:yes"}})",
            R"({"tokens":{"access_token":"header.eyJleHAiOiJpbnZhbGlkIn0.signature","account_id":"account"}})",
            R"({"tokens":{"access_token":"header.eyJleHAiOjE3OTEyMDAwMDB9.signature","account_id":"one","account_id":"two"}})",
            R"({"tokens":null})", "{broken"}) {
            Check(!ParseCodexCredentials(malformed), "Malformed or incompatible credentials must fail cleanly");
        }
        Check(!DecodeBase64Url("a") && !DecodeBase64Url("ab!") && !DecodeBase64Url("ab"),
            "Invalid base64url and nonzero trailing bits must fail");
        std::cout << "PASS: managed credentials, legacy files, JWT expiry, and malformed data\n";

        const auto snapshot = ParseCodexUsage(usage);
        Check(snapshot && snapshot->weekly.remaining == 79 && snapshot->weekly.minutes == 10080 &&
            snapshot->weekly.resetsAt == 1791581263 && snapshot->shortWindow &&
            snapshot->shortWindow->remaining == 60 && snapshot->shortWindow->minutes == 300,
            "Longest Codex window must remain weekly and unrelated buckets must be ignored");
        const auto single = ParseCodexUsage(R"({"rate_limit":{"primary_window":{"used_percent":0,"limit_window_seconds":604800,"reset_at":1791581263},"secondary_window":null}})");
        Check(single && single->weekly.remaining == 100 && !single->shortWindow, "A single weekly window must work");
        const auto pretty = ParseCodexUsage(R"({
          "rate_limit": {
            "primary_window": {
              "used_percent": 21,
              "limit_window_seconds": 604800,
              "reset_at": 1791581263
            },
            "secondary_window": null
          }
        })");
        Check(pretty && pretty->weekly.remaining == 79 && !pretty->shortWindow,
            "Pretty JSON must allow whitespace after a null secondary window");
        const auto reversed = ParseCodexUsage(R"({"rate_limit":{"primary_window":{"used_percent":30,"limit_window_seconds":18000,"reset_at":1791230000},"secondary_window":{"used_percent":80,"limit_window_seconds":604800,"reset_at":1791581263}}})");
        Check(reversed && reversed->weekly.remaining == 20 && reversed->shortWindow->remaining == 70,
            "Window names must not determine their duration");
        for (const char* malformed : {"{}", "{broken",
            R"({"additional_rate_limits":{"rate_limit":{"primary_window":{"used_percent":21,"limit_window_seconds":604800,"reset_at":1791581263}}}})",
            R"({"rate_limit":{"primary_window":{"used_percent":101,"limit_window_seconds":604800,"reset_at":1791581263}}})",
            R"({"rate_limit":{"primary_window":{"used_percent":20,"limit_window_seconds":0,"reset_at":1791581263}}})",
            R"({"rate_limit":{"primary_window":{"used_percent":20,"limit_window_seconds":604800}}})"}) {
            Check(!ParseCodexUsage(malformed), "Invalid usage must preserve cached values rather than invent a percentage");
        }
        std::cout << "PASS: native usage body, weekly and five-hour windows, and invalid responses\n";

        CodexCredentials cached{token, "account", now + 10000};
        int loads = 0, requests = 0, refreshes = 0;
        CodexUsageClient direct(
            [&]() -> std::optional<CodexCredentials> { ++loads; return cached; },
            [&](const CodexCredentials& credentials) -> std::optional<HttpJsonResponse> {
                ++requests;
                Check(credentials.token == token && credentials.accountId == "account", "Cached login must be used directly");
                return HttpJsonResponse{200, usage};
            }, [&] { ++refreshes; return true; });
        Check(direct.Query(now).has_value() && loads == 1 && requests == 1 && refreshes == 0,
            "Normal polling must never launch Codex");
        cached.expiresAt = now + 299;
        requests = refreshes = 0;
        CodexUsageClient expired(
            [&]() -> std::optional<CodexCredentials> { return cached; },
            [&](const CodexCredentials& credentials) -> std::optional<HttpJsonResponse> {
                ++requests; Check(credentials.token == "renewed", "Near-expiry tokens must be refreshed first");
                return HttpJsonResponse{200, usage};
            }, [&] { ++refreshes; cached = {"renewed", "account", now + 3600}; return true; });
        Check(expired.Query(now).has_value() && refreshes == 1 && requests == 1, "Expiry refresh must occur once");
        cached = {"before", "account", now + 3600};
        requests = refreshes = 0;
        CodexUsageClient rejected(
            [&]() -> std::optional<CodexCredentials> { return cached; },
            [&](const CodexCredentials& credentials) -> std::optional<HttpJsonResponse> {
                ++requests; return HttpJsonResponse{credentials.token == "before" ? 401u : 200u, usage};
            }, [&] { ++refreshes; cached.token = "after"; return true; });
        Check(rejected.Query(now).has_value() && requests == 2 && refreshes == 1, "401 must refresh and retry once");
        loads = requests = refreshes = 0;
        CodexUsageClient rotated(
            [&]() -> std::optional<CodexCredentials> { return CodexCredentials{++loads == 1 ? "old" : "rotated", "account", now + 3600}; },
            [&](const CodexCredentials& credentials) -> std::optional<HttpJsonResponse> {
                ++requests; return HttpJsonResponse{credentials.token == "old" ? 401u : 200u, usage};
            }, [&] { ++refreshes; return false; });
        Check(rotated.Query(now).has_value() && requests == 2 && refreshes == 0, "Concurrent token rotation must avoid CLI refresh");
        requests = refreshes = 0;
        CodexUsageClient unauthorized(
            [&]() -> std::optional<CodexCredentials> { return cached; },
            [&](const CodexCredentials&) -> std::optional<HttpJsonResponse> { ++requests; return HttpJsonResponse{401, {}}; },
            [&] { ++refreshes; return true; });
        Check(!unauthorized.Query(now) && requests == 2 && refreshes == 1, "Authentication retries must be bounded");
        for (DWORD status : {302u, 403u, 429u, 500u}) {
            refreshes = 0;
            CodexUsageClient failed(
                [&]() -> std::optional<CodexCredentials> { return cached; },
                [status](const CodexCredentials&) -> std::optional<HttpJsonResponse> { return HttpJsonResponse{status, {}}; },
                [&] { ++refreshes; return true; });
            Check(!failed.Query(now) && refreshes == 0, "Other HTTP failures must not rotate credentials");
        }
        cached.expiresAt = now - 1;
        requests = refreshes = 0;
        Check(!unauthorized.Query(now) && refreshes == 1 && requests == 0, "An unchanged expired cache must not be sent");
        CodexUsageClient missing([]() -> std::optional<CodexCredentials> { return std::nullopt; },
            [](const CodexCredentials&) -> std::optional<HttpJsonResponse> { throw std::runtime_error("Unexpected request"); },
            []() -> bool { throw std::runtime_error("Missing login must not start interactive authentication"); });
        Check(!missing.Query(now), "Missing login must fail cleanly");
        g_shuttingDown = true;
        Check(!direct.Query(now), "Shutdown must prevent new polling");
        g_shuttingDown = false;
        std::cout << "PASS: direct polling, expiry, concurrent rotation, and bounded authentication retries\n";

        Check(argc == 3, "Expected fake executable and event-log paths");
        SetEnvironmentVariableW(L"CODEX_CLI_PATH", argv[1]);
        SetEnvironmentVariableW(L"CODEX_TEST_EVENTS", argv[2]);
        for (const wchar_t* mode : {L"success", L"error", L"missing", L"exit"}) {
            SetEnvironmentVariableW(L"CODEX_TEST_MODE", mode);
            std::ofstream(fs::path(argv[2]), std::ios::trunc).close();
            Check(RefreshCodexCredentials() == (std::wstring_view(mode) == L"success"), "Refresh must report errors and missing accounts");
            std::ifstream events{fs::path(argv[2])};
            int starts = 0, initialized = 0, refreshesSeen = 0;
            DWORD pid = 0;
            for (std::string event; std::getline(events, event);) {
                if (event.starts_with("start ")) { ++starts; pid = static_cast<DWORD>(std::stoul(event.substr(6))); }
                else if (event == "initialized") ++initialized;
                else if (event == "refresh") ++refreshesSeen;
                else Check(false, "Refresh helper must send no usage, model, or login requests");
            }
            Check(starts == 1 && initialized == 1 && refreshesSeen == 1, "Refresh must perform one initialized handshake and force token renewal");
            UniqueHandle child(OpenProcess(SYNCHRONIZE, FALSE, pid));
            Check(!child || WaitForSingleObject(child.get(), 1000) == WAIT_OBJECT_0, "Refresh must close the child process");
        }
        std::cout << "PASS: native CLI refresh, spaced response IDs, failure handling, and child cleanup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
