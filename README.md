# Codex Usage Tray

A small native Windows 11 notification-area utility that shows remaining Codex usage and whether any local Codex task is active.

## Technology and behaviour

- **Native C++20 and Win32:** no Electron, Python, .NET runtime, web view, or visible window. The idle process consists of a hidden tool window, a notification icon, and a one-minute Windows timer.
- **Local Codex app-server API:** once per minute the utility briefly starts `codex app-server --stdio`, performs the protocol handshake (including `account/read` so current Codex releases load the existing login), calls `account/rateLimits/read`, and exits the child process. It classifies the longest reported Codex window as weekly and, when a shorter window is present, displays it as the five-hour allowance. It retries once after a transient failure and records each successful read by refreshing the local state-file timestamp. No API key is stored by this utility; Codex uses its existing login. The multiline tooltip reports each window and task status separately, and calculates the expected remaining allowance for each reported window at the current point in that window, assuming usage is spread evenly until the API's next reset time.
- **Task state:** Windows Codex keeps live `thread/status` state inside its private stdio app-server connection, which another process cannot attach to. The utility therefore reads the final `task_started` or `task_complete` event from recently changed local Codex session streams. The scan has a strict three-second budget so it can never prevent later usage refreshes. These events are written as work starts and completes; it does not scrape the Codex UI or depend on a periodically refreshed percentage cache.
- **Dynamic icon:** the usage indicator is drawn in memory with GDI+. When a five-hour window is available, it is the outer ring and the weekly window is a filled inner pie chart; without a five-hour window, the weekly window uses the original single ring. Unused segments are blue when any task is active and light blue when idle. Either window turns the displayed indicator purple when actual remaining usage is below its time-adjusted expected value; purple takes priority over task state. Used segments are light grey, and exact actual/expected percentages for both windows remain available in the tooltip. If the API is unavailable, the last successful values are retained.
- **Automatic startup:** every launch writes a per-user `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` entry pointing at the current executable. No administrator rights are required.

## Build and run

Visual Studio 2026 needs the **Desktop development with C++** workload.

1. Open `CodexUsageTray.sln` in Visual Studio and choose **Release / x64**, then **Build Solution**; or right-click `build.ps1` and run it with PowerShell.
2. Run `x64\Release\CodexUsageTray.exe` once. It immediately registers itself to start when you next sign in.
3. If Windows places it in the notification overflow menu, drag it beside the clock or enable it under **Settings > Personalization > Taskbar > Other system tray icons**.

Right-click the icon for **Refresh now**, **Open Codex**, and **Exit**. Double-clicking opens Codex through its installed Windows app identity, with the Codex CLI launcher as a fallback.

The Codex app-server protocol is currently marked experimental. If a future Codex update changes it, the icon keeps the last known values and marks the tooltip as cached rather than showing an incorrect fresh value.
