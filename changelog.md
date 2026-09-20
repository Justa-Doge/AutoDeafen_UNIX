# v2.0.5
- Made AutoDeafen prefer a separate numbered Discord IPC endpoint under Wine so it can coexist with Eclipse Rich Presence.
- Continued scanning other Discord IPC endpoints when one pipe is busy instead of failing the entire connection attempt.
- Published a matching multi-client rpc-bridge fork that exposes `discord-ipc-0` through `discord-ipc-9` using isolated worker processes.
- Stopped the AutoDeafen bridge from writing raw Discord IPC payloads and access tokens to its logs.
- Updated the Linux guide and settings labels for the public multi-IPC bridge, its per-endpoint logs, and automatic Discord socket retries.
- Removed the temporary Linux setup-reminder test button while keeping the automatic first-run prompt.

# v2.0.4
- Added a Linux-only settings button that opens rpc-bridge's active Proton log folder in the desktop's default file manager.
- The button uses the current Proton prefix directly, so default and custom Steam library locations require no separate path detection.
- Added an auto-dismissing warning and diagnostic log entry when the bridge log folder does not exist or cannot be opened.

# v2.0.3
- Added privacy-safe `ADIPC-*` diagnostics that identify the exact transport, handshake, authentication, response, voice-command, or idle-connection stage that failed.
- Added named-pipe endpoint, probe count, retry count, and Win32 I/O error details without logging Discord tokens, OAuth secrets, or raw IPC payloads.
- Added an ordered Steam/Proton troubleshooting checklist and a strict bridge-log allowlist designed to exclude raw Discord IPC frames.
- Added package-specific Linux IPC guidance for native distro packages, Flatpak, Snap, and Vesktop, including Steam/Proton bridge launch steps.
- Made Linux IPC warnings distinguish a missing, busy, or access-denied Discord/bridge pipe and clarified that OAuth credentials cannot create a missing pipe.

# v2.0.2
- Added a bundled plain-text guide with distro-neutral Linux runtime, Discord IPC, authorization, and troubleshooting instructions.
- Added an Open Linux Guide button to the mod settings that appears only when the Win64 build is running through Wine on a Linux host.
- Added a one-time Linux setup prompt on the first supported launch, plus a settings button that can display the prompt again for testing.
- Replaced the blocking missing-pipe popup with an auto-dismissing warning that points to the packaged guide in mod settings.

# v2.0.1
- Made Linux/Wine IPC explicitly compatible with standard Discord named-pipe bridges.
- Added an in-game Linux setup message when Discord or its IPC bridge is unavailable.
- Documented Linux runtime dependencies, supported Discord configurations, and Geode's Win64-on-Wine build model.

# v2.0.0
- Added Windows support and a universal macOS build for Apple silicon and Intel Macs.
- Added Linux support through Geode's Win64 Wine/Proton setup, using the bridge-compatible named-pipe path and an in-game setup message when Linux Discord IPC is unavailable.
- Replaced shell commands and direct clipboard/browser APIs with Geode's cross-platform utilities.
- Reworked Discord IPC communication with ordered voice updates, safer frame handling, and automatic reconnects.
- Added Discord-compliant HTTP identification and actionable OAuth error messages.
- Made Windows IPC tolerate busy or slow Discord startup and retry failed initial connections automatically.
- Undeafens while paused, then deafens again after resuming when the player is still above the threshold.
- Fixed OAuth token refreshes and improved callback validation, setup errors, and credential handling.
- Made per-level settings save immediately and migrate older saved settings automatically.
- Cleaned up naming, project structure, documentation, and outdated comments.

# v1.5.0
- Cleaned up the codebase and configuration flow.
- Made Discord deafening more consistent without disconnecting from Discord.

# v1.0.1
- Cleaned up the project files and organization.

# v1.0.0
- Ported to UNIX systems
