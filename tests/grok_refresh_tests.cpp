#include "../src/main.cpp"
#include <iostream>
#include <stdexcept>

void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

const std::string billing = R"({"config":{"creditUsagePercent":25,"prepaidBalance":{"val":500}}})";

int wmain(int argc, wchar_t** argv) {
    try {
        const long long now = *ParseRfc3339Timestamp("2026-10-05T12:00:00Z");
        GrokRefreshSchedule schedule;
        const auto start = std::chrono::steady_clock::now();
        Check(schedule.Begin(start, false), "Startup must fetch Grok");
        for (int minute = 1; minute < 30; ++minute) {
            Check(!schedule.Begin(start + std::chrono::minutes(minute), false),
                "Minute updates must not fetch Grok before 30 minutes");
        }
        Check(schedule.Begin(start + 30min, false), "30-minute update must fetch Grok");
        Check(schedule.Begin(start + 31min, true), "Manual refresh must fetch immediately");
        Check(!schedule.Begin(start + 60min, false), "Manual refresh must reset the interval");
        Check(schedule.Begin(start + 61min, false), "Next update must be 30 minutes after manual refresh");
        std::cout << "PASS: startup, 30-minute schedule, and manual override\n";

        const auto credentials = ParseGrokCredentials(R"({
          "https://external.example::client":{"auth_mode":"oidc","key":"wrong","user_id":"wrong"},
          "https://auth.x.ai::older":{"auth_mode":"oidc","key":"old","user_id":"old-id","expires_at":"2026-10-05T10:00:00Z"},
          "https://auth.x.ai::current":{"auth_mode":"oidc","key":"current","user_id":"current-id","expires_at":"2026-10-05T18:00:00Z"}
        })");
        Check(credentials && credentials->token == "current" && credentials->userId == "current-id" &&
            credentials->expiresAt == now + 6 * 60 * 60, "Credentials must stay within the correct first-party scope");
        Check(!ParseGrokCredentials(R"({"https://auth.x.ai::client":{"auth_mode":"api_key","key":"api-key","user_id":"id"}})"),
            "API keys must not be treated as subscription tokens");
        Check(!ParseGrokCredentials(R"({"https://auth.x.ai::client":{"auth_mode":"oidc","key":"value\r\nInjected:yes","user_id":"id"}})"),
            "Escaped control characters must not enter request headers");
        Check(!ParseGrokCredentials(R"({"https://auth.x.ai::client":{"auth_mode":"oidc","key":"token","nested":{"user_id":"id"}}})"),
            "Nested fields must not be mistaken for account fields");
        Check(!ParseGrokCredentials(R"({"https://auth.x.ai::client":{"auth_mode":"oidc","key":"token","key":"other","user_id":"id"}})"),
            "Duplicate fields must be rejected");
        Check(!ParseGrokCredentials(R"({"https://auth.x.ai::client":{"auth_mode":"oidc","key":"token","user_id":"id","expires_at":"invalid"}})"),
            "Invalid expiry must not silently become a valid credential");
        Check(!ParseGrokCredentials("{broken"), "Truncated auth files must fail cleanly");
        const auto legacy = ParseGrokCredentials(R"({"https://auth.x.ai::client":{"auth_mode":"oidc","key":"token","user_id":"id","create_time":"2026-10-01T12:00:00Z"}})");
        Check(legacy && legacy->expiresAt == now + 26LL * 24 * 60 * 60, "Missing expiry must use the documented 30-day fallback");
        std::cout << "PASS: scoped credential parsing, malformed data, and expiry\n";

        int loads = 0, requests = 0, refreshes = 0;
        GrokCredentials cached{"cached", "user", now + 6 * 60 * 60};
        GrokBillingClient direct(
            [&]() -> std::optional<GrokCredentials> { ++loads; return cached; },
            [&](const GrokCredentials& value) -> std::optional<HttpJsonResponse> {
                ++requests;
                Check(value.token == "cached", "Direct request must use the cached token");
                return HttpJsonResponse{200, billing};
            },
            [&] { ++refreshes; return true; });
        const auto snapshot = direct.Query(now);
        Check(snapshot && snapshot->subscriptionRemaining == 75 && snapshot->extraCreditsCents == 500,
            "Direct billing body must parse without an ACP envelope");
        Check(loads == 1 && requests == 1 && refreshes == 0, "Normal polling must make one direct request and never launch the CLI");

        cached.expiresAt = now + 299;
        GrokBillingClient expired(
            [&]() -> std::optional<GrokCredentials> { return cached; },
            [&](const GrokCredentials& value) -> std::optional<HttpJsonResponse> {
                Check(value.token == "renewed", "Expired token must be refreshed before a direct request");
                return HttpJsonResponse{200, billing};
            },
            [&] { cached = {"renewed", "user", now + 6 * 60 * 60}; return true; });
        Check(expired.Query(now).has_value(), "Near-expiry credentials must recover");

        requests = refreshes = 0;
        cached = {"before", "user", now + 6 * 60 * 60};
        GrokBillingClient rejected(
            [&]() -> std::optional<GrokCredentials> { return cached; },
            [&](const GrokCredentials& value) -> std::optional<HttpJsonResponse> {
                ++requests;
                return HttpJsonResponse{value.token == "before" ? 401u : 200u, billing};
            },
            [&] { ++refreshes; cached.token = "after"; return true; });
        Check(rejected.Query(now).has_value() && requests == 2 && refreshes == 1,
            "Rejected token must refresh and retry once");

        loads = requests = refreshes = 0;
        GrokBillingClient rotated(
            [&]() -> std::optional<GrokCredentials> {
                return GrokCredentials{++loads == 1 ? "old" : "already-rotated", "user", now + 3600};
            },
            [&](const GrokCredentials& value) -> std::optional<HttpJsonResponse> {
                ++requests;
                return HttpJsonResponse{value.token == "old" ? 401u : 200u, billing};
            },
            [&] { ++refreshes; return false; });
        Check(rotated.Query(now).has_value() && requests == 2 && refreshes == 0,
            "A token rotated by another process should be reloaded without launching the CLI");

        requests = refreshes = 0;
        GrokBillingClient unauthorized(
            [&]() -> std::optional<GrokCredentials> { return cached; },
            [&](const GrokCredentials&) -> std::optional<HttpJsonResponse> {
                ++requests; return HttpJsonResponse{403, {}};
            },
            [&] { ++refreshes; return true; });
        Check(!unauthorized.Query(now) && requests == 2 && refreshes == 1,
            "Repeated authentication errors must not loop");
        for (const DWORD status : {302u, 429u, 500u}) {
            refreshes = 0;
            GrokBillingClient failed(
                [&]() -> std::optional<GrokCredentials> { return cached; },
                [status](const GrokCredentials&) -> std::optional<HttpJsonResponse> { return HttpJsonResponse{status, {}}; },
                [&] { ++refreshes; return true; });
            Check(!failed.Query(now) && refreshes == 0, "Redirects, rate limits and server errors must not refresh credentials");
        }
        GrokBillingClient missing(
            []() -> std::optional<GrokCredentials> { return std::nullopt; },
            [](const GrokCredentials&) -> std::optional<HttpJsonResponse> { throw std::runtime_error("Unexpected HTTP request"); },
            []() -> bool { throw std::runtime_error("Missing login must not trigger interactive authentication"); });
        Check(!missing.Query(now), "Missing credentials must fail cleanly");
        g_shuttingDown = true;
        Check(!direct.Query(now), "Shutdown must not start another request");
        g_shuttingDown = false;
        std::cout << "PASS: direct billing, cached tokens, authentication recovery, and bounded retries\n";

        Check(argc == 3, "Expected fake executable and event-log paths");
        SetEnvironmentVariableW(L"GROK_CLI_PATH", argv[1]);
        SetEnvironmentVariableW(L"GROK_TEST_EVENTS", argv[2]);
        SetEnvironmentVariableW(L"GROK_TEST_MODE", nullptr);
        std::ofstream(fs::path(argv[2]), std::ios::trunc).close();
        Check(RefreshGrokCredentials(), "CLI refresh handshake failed");
        std::ifstream events{fs::path(argv[2])};
        int starts = 0, sessions = 0, bills = 0;
        DWORD pid = 0;
        for (std::string event; std::getline(events, event);) {
            if (event.starts_with("start ")) { ++starts; pid = static_cast<DWORD>(std::stoul(event.substr(6))); }
            if (event == "session") ++sessions;
            if (event == "billing") ++bills;
        }
        Check(starts == 1 && sessions == 0 && bills == 1, "Credential refresh must not create an agent session");
        UniqueHandle child(OpenProcess(SYNCHRONIZE, FALSE, pid));
        Check(!child || WaitForSingleObject(child.get(), 1000) == WAIT_OBJECT_0,
            "Credential refresh must close its child process");
        std::cout << "PASS: CLI refresh handshake and child-process cleanup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
