# Codex Usage Tray

A small native Windows 11 notification-area utility that shows remaining weekly Codex usage and whether any local Codex task is active.

## Technology and behaviour

- **Native C++20 and Win32:** no Electron, Python, .NET runtime, web view, or visible window. The idle process consists of a hidden tool window, a notification icon, and a one-minute Windows timer.
- **Local Codex app-server API:** once per minute the utility briefly starts `codex app-server --stdio`, performs the documented protocol handshake, calls `account/rateLimits/read`, selects the longest reported Codex window (the weekly limit), and exits the child process. No API key is stored by this utility; Codex uses its existing login. The tooltip also calculates the expected remaining allowance at the current point in the week, assuming usage is spread evenly until the API's next reset time.
- **Task state:** Windows Codex keeps live `thread/status` state inside its private stdio app-server connection, which another process cannot attach to. The utility therefore reads the final `task_started` or `task_complete` event from recently changed local Codex session streams. These events are written as work starts and completes; it does not scrape the Codex UI or depend on a periodically refreshed percentage cache.
- **Dynamic icon:** the usage ring is drawn in memory with GDI+. Its unused segment is blue when any task is active and light blue when idle. It turns red when actual remaining usage is below the time-adjusted expected remaining value; red takes priority over task state. The used segment is light grey, and exact actual/expected percentages remain available in the tooltip. If the API is unavailable, the last successful percentage is retained.
- **Automatic startup:** every launch writes a per-user `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` entry pointing at the current executable. No administrator rights are required.

## Build and run

Visual Studio 2026 needs the **Desktop development with C++** workload.

1. Open `CodexUsageTray.sln` in Visual Studio and choose **Release / x64**, then **Build Solution**; or right-click `build.ps1` and run it with PowerShell.
2. Run `x64\Release\CodexUsageTray.exe` once. It immediately registers itself to start when you next sign in.
3. If Windows places it in the notification overflow menu, drag it beside the clock or enable it under **Settings > Personalization > Taskbar > Other system tray icons**.

Right-click the icon for **Refresh now**, **Open Codex**, and **Exit**. Double-clicking opens Codex.

The Codex app-server protocol is currently marked experimental. If a future Codex update changes it, the icon deliberately becomes grey and keeps the last known percentage rather than showing an incorrect fresh value.
