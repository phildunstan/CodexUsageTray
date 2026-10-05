# Codex Usage Tray

A small native Windows 11 notification-area utility that shows remaining Codex usage and whether any local Codex task is active.

## Technology and behaviour

- **Native C++20 and Win32:** no Electron, Python, .NET runtime, web view, or visible window. The idle process consists of a hidden tool window, a notification icon, and a one-minute Windows timer.
- **Direct Codex usage:** once per minute the utility makes a compressed WinHTTP GET to `https://chatgpt.com/backend-api/wham/usage` using the existing managed ChatGPT login in `~/.codex/auth.json` (or `CODEX_HOME/auth.json`). Normal polls start no Codex process. Within five minutes of access-token expiry, or after HTTP 401, it briefly starts `codex app-server --stdio` and calls `account/read` with `refreshToken: true`, then reloads Codex's credential file and retries the direct request once. Codex owns token rotation; the tray never writes or copies tokens to its state file. File-based managed ChatGPT credentials are required; API-key, external-token, and keyring-only logins are not read by this utility. It classifies the longest reported Codex window as weekly and, when a shorter window is present, displays it as the five-hour allowance with its local reset time. The existing one-time retry after a failed refresh and local state-file timestamp updates are retained. The multiline tooltip reports the Codex weekly actual percentage, expected percentage, and local reset day/time, assuming usage is spread evenly until the next reset time. This usage endpoint is an internal ChatGPT backend and can change independently of OpenAI's public API.
- **Task state:** Windows Codex keeps live `thread/status` state inside its private stdio app-server connection, which another process cannot attach to. The utility therefore reads the final `task_started` or `task_complete` event from recently changed local Codex session streams. The scan has a strict three-second budget so it can never prevent later usage refreshes. These events are written as work starts and completes; it does not scrape the Codex UI or depend on a periodically refreshed percentage cache.
- **Dynamic icon:** the usage indicator is drawn in memory with GDI+. When a five-hour window is available, it is the outer ring and the weekly window is a filled inner pie chart; without a five-hour window, the weekly window uses the original single ring. Unused segments are blue when any task is active and light blue when idle. Either window turns the displayed indicator purple when actual remaining usage is below its time-adjusted expected value; purple takes priority over task state. Used segments are light grey, and exact actual/expected percentages for both windows remain available in the tooltip. If the API is unavailable, the last successful values are retained.
- **Grok billing:** the utility makes one direct, compressed WinHTTP GET to `https://cli-chat-proxy.grok.com/v1/billing?format=credits` at startup and every 30 minutes. It reads the existing first-party login from `~/.grok/auth.json` (or `GROK_HOME/auth.json`) into memory and sends no model prompts. No Grok agent runs between polls. Near token expiry or after an authentication rejection, it briefly starts the installed CLI to let Grok refresh its own credentials, then reloads the file and retries the direct request once. The tray never writes tokens or copies them to its state file. **Refresh now** fetches Grok immediately and restarts the 30-minute interval. The last Grok reading stays visible between polls. This subscription billing endpoint is an internal CLI backend and can change independently of the public xAI API.
- **xAI API billing:** when both `XAI_MANAGEMENT_API_KEY` and `XAI_TEAM_ID` are set in the current user's environment, the utility makes read-only WinHTTP requests to the xAI Management API's prepaid-balance and postpaid-invoice-preview endpoints. The tooltip shows the free balance (`xAI free`), calculated as `defaultCredits + defaultCreditsIssued + autoCreditsIssued + prepaidCredits - prepaidCreditsUsed`; omitted zero-valued fields count as zero. The management key is read only at runtime and is never written to the repository. If these variables are not set, the tooltip says `not set`.
- **Automatic startup:** every launch writes a per-user `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` entry pointing at the current executable. No administrator rights are required.

## Build and run

Visual Studio 2026 needs the **Desktop development with C++** workload.

1. Open `CodexUsageTray.sln` in Visual Studio and choose **Release / x64**, then **Build Solution**; or right-click `build.ps1` and run it with PowerShell.
2. Run `x64\Release\CodexUsageTray.exe` once. It immediately registers itself to start when you next sign in.
3. If Windows places it in the notification overflow menu, drag it beside the clock or enable it under **Settings > Personalization > Taskbar > Other system tray icons**.

Right-click the icon for **Refresh now**, **Open Codex**, and **Exit**. Double-clicking opens Codex through its installed Windows app identity, with the Codex CLI launcher as a fallback.

Run `tests\run.ps1` to verify the 30-minute Grok polling schedule, Codex and Grok cached-credential parsing, direct-response parsing, authentication recovery, retry limits, and CLI refresh cleanup. These tests use fixture credentials and local fake CLIs and make no network requests.

### Optional xAI API credits

The Grok subscription and extra-credit lines work automatically when `grok.exe` is installed and signed in. The separate xAI API balance needs a management key and team ID. Set them as per-user environment variables (never commit the key):

```powershell
[Environment]::SetEnvironmentVariable('XAI_MANAGEMENT_API_KEY', '<management-key>', 'User')
[Environment]::SetEnvironmentVariable('XAI_TEAM_ID', '<team-id>', 'User')
```

Restart the tray utility after changing either value. The tray tooltip uses the full labels (`Codex 7d`, `Codex 5h`, `Grok 7d`, and `xAI free`).

The subscription usage endpoints are internal, and the Codex app-server protocol used for credential refresh is experimental. If a future update changes them, the icon keeps the last known values and marks the tooltip as cached rather than showing an incorrect fresh value.
