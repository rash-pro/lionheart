# RetroAchievements

The client is off by default. The menu warning and sign-in enable it; `ra=1`
allows a saved token to sign in on the next launch. Hardcore is disabled when
each client is created. There are no leaderboard controls or submissions.

Lionheart is not an approved RetroAchievements client. The owner must contact
the RetroAchievements administrators before any public release. Local mock
verification does not establish acceptance by the real service. Before signing
in there, read and accept the in-app warning; the service can reject this client.

The loaded PlayStation disc is hashed on the HTTP worker. Disc changes are
reported to `rc_client`; they do not assume that all discs have the same hash.
Evaluation runs once per VBlank against RAM and scratchpad through the existing
read-only achievement memory adapter. The paused achievements view calls
`rc_client_idle`, without evaluating a frame or advancing the guest.

Linux uses dynamically linked libcurl; Windows uses WinHTTP. The worker owns
network I/O, disc hashing, badge reads, PNG decoding and badge writes. The host
thread delivers completed callbacks. Requests identify Lionheart and rcheevos
versions and the operating system, verify HTTPS certificates, and do not follow
redirects. An explicit service rejection ends the session and deletes its token.

Only the username and token are saved in `<data>/ra/credentials`, with atomic
replacement and mode 0600 on Linux. Windows always uses `%APPDATA%\Lionheart\ra`,
including portable installs. Sign out removes credentials. Badge PNGs live in
`ra/badges`; a missing or corrupt image is downloaded again. Pending unlocks and
the loaded set remain in the current client session during a connection loss.
They are not a persistent offline queue across shutdown or sign-out.

## Local verification

Never use real credentials or contact the service during automated testing.
`tests/ra_mock.py` serves synthetic achievements and badges on loopback, and
`tests/test_ra_client.py` covers authentication, refusal, timeouts, disc changes,
unlock retries, token permissions and badge caching.

`LIONHEART_RA_TEST_HOST=http://127.0.0.1:PORT` redirects **all** API and image
requests to the local fixture and bypasses network proxies. Other host values
fail closed. This diagnostic override is not an ini setting or GUI control.
`LIONHEART_RA_TEST_TIMEOUT_MS=50..5000` changes the request deadline only with a
valid diagnostic host.

```sh
cmake --build build --target ff8 ra_gui_contract -j4
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy python -m pytest -q tests/test_ra_client.py tests/test_ra_gui.py
```

`FF8_PLAYER_HOST` and `LIONHEART_RA_GUI_HOST` can select existing build binaries.
`LIONHEART_RA_GUI_SHOTS` selects the directory for the GUI test PNGs. These tests
exercise actual SDL input and the Soft and Vulkan presenters without a window.
The client harness also supports `FF8_TEST_SANITIZE=1` with ASan preloaded into
the test process.

The client and API files are pinned in `third_party/rcheevos/SOURCE.md`. The
display adapter reads the pinned internal trigger's measured target because the
public progress string omits zero progress. Recheck that layout and the zero
progress test when updating rcheevos.
