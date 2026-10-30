# m pocket — Cardputer Zero device beta

Version 0.2.0-beta1. Native Linux ARM64 application for the **Cardputer Zero** with a 320×170 framebuffer. This is not firmware for the original ESP32 Cardputer or Cardputer ADV, and it is not a macOS package.

**Hardware testing is still required.** The ARM64 executable has been cross-compiled against the official Cardputer BSP v0.0.4. Automated UI, settings, networking, and packaging checks run on the Mac; they do not establish that the display, keyboard, Wi-Fi, or phone-camera QR scan work on a physical device. Do not submit this beta as a verified store release yet.

## Download

Get the `.deb` and checksums from the [v0.2.0-beta1 prerelease](https://github.com/DavisW9/m-pocket/releases/tag/v0.2.0-beta1). This repository contains the device client source and the optional [QR callback service](callback-service/README.md).

## Install on Cardputer Zero

Copy `m-pocket_0.2.0~beta1_arm64.deb` to the device. In its Linux terminal, from the folder containing the file:

```sh
sudo apt install './m-pocket_0.2.0~beta1_arm64.deb'
```

Then open **m pocket** from the Cardputer launcher. A launcher restart may be needed to discover a newly installed app. Wi-Fi must already be connected through the system settings.

Dependencies: Python 3.9 or newer, CA certificates, glibc 2.38 or newer, libstdc++6 11 or newer, and libgcc-s1. APT installs missing dependencies if the firmware's repositories provide them. The package installs no service, adds no user permissions, and makes no changes to other apps. The launcher user needs access to the framebuffer and keyboard (normally the `video` and `input` groups).

Remove with `sudo apt remove m-pocket`. User settings remain in `~/.config/m-pocket/` (or `$XDG_CONFIG_HOME/m-pocket/`).

## Sign in

Press **S** to select your Mastodon server, then **L** for a QR code. Scan it on your phone, continue to your server, and approve access. The app finishes automatically: **no authorization string to type**. The physical app has no browser-launch option; that remains specific to the Mac emulator.

If the callback service is unavailable or reaches its limit, press **A**, or select the displayed access-token fallback. On your server's website, create an application under Preferences → Development with `read:statuses` and `write:statuses`, then enter its access token. Tokens are masked while typed. Do not enter your account password.

Cloudflare handles only QR pairing and the short-lived authorization-code callback. Reading feeds, validating manual tokens, and publishing go directly to your selected Mastodon server over HTTPS. OAuth uses state validation and PKCE S256. Passwords, access tokens, client secrets, and PKCE verifiers are not sent to the callback service. The server must support PKCE S256 for QR sign-in.

Access tokens stay in memory and are forgotten when the app exits or you sign out. App registration credentials and preferences are kept in files readable only by your Linux user; device storage is not encrypted by this app. Sign-out clears this session; revoke the app on your server if you want to invalidate its token remotely.

## Controls and settings

| Key | Action |
| --- | --- |
| Z / C | Previous / next post; next loads another page at the end |
| F / X | Scroll up / down; hold to repeat |
| R | Refresh current feed |
| T | Home / local feed |
| N | Compose a public post; Enter reviews, Y publishes |
| S / L / A | Server / QR sign-in / access token |
| O | Settings: 1 theme, 2 text size, 3 server, 4 help, 5 sign out |
| Esc | Back or cancel; from the feed, exit to the launcher |

Both themes stay purple. Settings persist across launches. Changing text size resets a post's scroll so its first line remains readable. Refresh preserves the selected post and scroll position if that post is still in the returned page. Pages are deduplicated and the newest 200 loaded posts are retained to bound memory.

The firmware's printed symbol-key mappings are supported, plus ordinary USB keyboard punctuation and Shift. Text entry is ASCII in this beta. Server content is UTF-8, but built-in fonts do not cover every language or emoji. Media shows as an attachment marker. Very long posts are limited to 8,192 characters with a truncation marker. Local feeds depend on the instance's access policy. No notifications, media viewer, boosts, favourites, reply threads, multi-account support, or saved login yet.

## Build from source

Prerequisites: CMake 3.20+, an `aarch64-linux-gnu-gcc/g++` cross compiler, Python 3.9+, LVGL 9.5.0, and the official BSP v0.0.4. Get the BSP from the CardputerZero/M5CardputerZero-UserDemo GitHub release and extract it to a local directory. Get LVGL from https://github.com/lvgl/lvgl at tag `v9.5.0` (the source archive includes it under `vendor/lvgl`).

```sh
cmake -S . -B build-arm64 \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/cardputer-toolchain.cmake" \
  -DCM0_SDK_ROOT=/absolute/path/to/sdk_bsp \
  -DCM0_ALLOW_FETCH_DEPS=OFF \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-arm64 -j 4
DESTDIR="$PWD/stage" cmake --install build-arm64
python3 packaging/build_deb.py stage m-pocket_0.2.0~beta1_arm64.deb
```

Optional display/input overrides: `M_POCKET_FB=/dev/fb0` and `M_POCKET_INPUT=/dev/input/eventN`. These help diagnose a firmware with different device names. No root background process is needed.

Host-side tests:

```sh
python3 -m unittest discover -s tests -v
cmake -S . -B build-tests -DPOCKET_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tests -j 4
ctest --test-dir build-tests --output-on-failure
```

See [RELEASE-CHECK.md](RELEASE-CHECK.md) for this build's verification and remaining hardware checks.
