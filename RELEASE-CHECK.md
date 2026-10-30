# Device package verification — 2026-10-30

**Result: a buildable, packaged Cardputer Zero ARM64 beta. Physical-device release approval is still pending.** This build is distributed as a GitHub prerelease. No app-store upload or hardware verification has been performed.

Passed for this device port:

- Native Linux ELF64 AArch64 cross-build against the official BSP v0.0.4, using its GCC 14 C++ headers and runtime. This is not the Mac emulator binary.
- ELF inspection: ARM64 interpreter, PIE, non-executable stack, RELRO, immediate symbol binding, and no Homebrew/user-directory runtime paths. Dependencies are declared in the Debian control file.
- Ten Python backend tests: PKCE callback exchange and secret boundaries; wrong-state rejection; callback expiry and outage fallback; direct manual-token validation; HTTP 429 backoff; settings persistence and restrictive file permissions; invalid-server handling; paged feed/HTML conversion; posting validation with mocked HTTP; redirects disabled.
- Headless tests exercising the actual LVGL UI: both settings toggles, help and server menus, long-post scroll limits, key repeat, reading-position preservation, deduplicated pagination, end-of-feed behavior, QR/manual-token screens, token masking, draft cancellation, and sign-out.
- Physical symbol-key mapping unit assertions, including period, underscore, uppercase, and digits.
- Actual UI-to-Python process communication through private pipes. Theme, text size, and server survive helper restart in an isolated temporary profile.
- Native 320×170 rendering inspected for the dark settings screen and QR screen. The QR image was decoded by an independent macOS Vision decoder and matched the full synthetic pairing URL. This is a software decode, not a phone-camera test of a physical display.
- Fresh host build and tests from the vendored LVGL source shipped in the source archive.
- Debian archive inspection with the host `ar` reader: Debian 2.0 members, payload MD5 checksums, root-owned file metadata, permissions, launcher/icon paths, ARM64 ELF payload, and no bundled user settings or credentials.

Earlier Mac-emulator sign-in, live feed, and callback-service checks apply to the emulator/service versions tested then. They are not evidence that this new Linux executable has run on hardware. Network tests for the new Python backend use fixtures; no real test post was published.

Before a public release, install the .deb on a physical Cardputer Zero and check:

1. APT dependency resolution, launcher discovery, display orientation and colors, printed keyboard symbols, held scrolling, and exit back to the launcher.
2. Connect Wi-Fi, select the server, scan the displayed QR on a phone, approve access, and verify automatic arrival in the authenticated home feed without typing a code.
3. Page through several screens of posts; change text size and theme; restart; verify saved settings and expected sign-out on exit.
4. Verify an actual manual access token and recovery after Wi-Fi loss/server rate limits. Check local-feed behavior on the chosen instance.
5. If desired, explicitly publish and then remove a test post using the account owner's approval. Mocked posting tests do not prove live posting.
6. Record device memory use, responsiveness, and a real-device demo before submitting to the official store.

Known beta limits: ASCII text entry, partial Unicode font coverage, attachment markers rather than media rendering, 500-character public composition, 8,192-character post display limit, up to 200 loaded posts, and memory-only account sessions. The package's maintainer address is a local-build placeholder, not a public support address.
