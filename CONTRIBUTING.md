# Contributing

Thanks for your interest in PWR-REACTOR.

## Ground rules

- The whole app is one C file (`main.c`) plus a Makefile - please keep
  it that way. No new build systems, no new runtime dependencies
  without discussion. Optional integrations should degrade gracefully
  when the tool they talk to is missing (see the adb / NUT / KDE
  Connect scanners for the pattern).
- Kernel style-ish C99/gnu99: 4-space indent, snake_case, no typedef'd
  pointers, explicit error handling. Build must stay warning-free with
  `-Wall -Wextra`.
- ASCII punctuation only in code and strings (the VFD font covers
  0x20..0x5F).
- Colors come from the `Theme` struct only - never hardcode a color in
  drawing code.
- External command output is untrusted input: validate anything that
  goes into a shell command (see `serial_ok`, `sanitize_text`).

## Workflow

1. Fork, branch from `main`.
2. `make` and `make asan` must both build clean; run the asan binary
   for a minute and watch for reports. Known issue: on GNOME the
   system `libayatana-appindicator` can SEGV under ASan (inside the
   library's legacy fallback path, only when the window is visible);
   the normal build is unaffected. Test scanning/drawing logic with
   `./power_reactor_asan --hidden` if you hit it.
3. If you touched scanning, test with the sources you have and note
   which ones you could not test in the PR description.
4. Screenshots for visual changes, please.

## Ideas that would be welcome

- More battery sources (OpenRGB peripherals, BLE direct, Windows/macOS
  data layers behind an ifdef).
- Persisted history across restarts.
- Wayland layer-shell widget positioning.
- Packaging for other distros (AUR, Flatpak, RPM).
