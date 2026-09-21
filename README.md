# not working on this anymore, gonna work on macos only now

# AutoDeafen

AutoDeafen is a Geode mod for Geometry Dash that deafens Discord once you reach a chosen percentage in a level. It restores audio when you die, complete the level, or leave the attempt.

## Platform support

- **macOS:** native universal build for Apple silicon and Intel Macs (macOS 11 or newer).
- **Windows:** native 64-bit build using Discord's named-pipe transport.
- **Linux:** the Win64 build runs with Geometry Dash through Wine or Proton, matching Geode's Linux setup. It supports Discord in the same Wine prefix and native Linux Discord through a compatible IPC bridge.

Geode 5.7.1 does not define a native Linux mod target, so there is no separate Linux `.so`. Stock Wine and Proton also do not translate Discord's Linux Unix socket into a Windows named pipe. See the distro-neutral [plain-text Linux setup guide](LINUX_SETUP.txt) for Linux runtime setup, Discord IPC, authorization, and troubleshooting. Linux users can also open that bundled text file from AutoDeafen's mod settings. The mod ID remains `justa_doge.autodeafen_unix` to preserve updates, settings, and saved credentials from earlier releases.

The Linux guide distinguishes supported official Discord installations from unsupported wrappers. Native distro packages such as pacman/APT/DNF, Flatpak Discord, and Snap Discord use socket layouts handled by the [AutoDeafen multi-IPC bridge](https://github.com/Justa-Doge/rpc-bridge-autodeafen/tree/multi-ipc). Vesktop/arRPC is currently unsupported because Rich Presence socket compatibility does not include the authenticated voice-setting commands AutoDeafen needs.

## Setup

Open AutoDeafen from a level's pause menu, then follow the Discord setup guide. The mod stores the Client ID, Client Secret, and refresh token locally so you only need to authorize it once.

## Configuration

Each level can use its own enabled state and deafen percentage. Levels without a custom setting use the defaults from the Geode mod settings. Practice mode support is optional.

## Building

Build the current desktop target with `geode build`. The checked-in workflow builds and combines Geode's `MacOS` and `Win64` targets; the Win64 artifact is also the one used by Linux/Wine installations.

This project is a fork of the original AutoDeafen mod by Lynxdeer, published with permission.
