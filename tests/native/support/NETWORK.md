# Verify the native network cases

Build the native runner as described in [the test guide](../../README.md).
Build the GUI contract in a separate top-level build and supply its path:

```sh
nice -n 10 cmake -S . -B build/ra-gui -G Ninja -DFF8_PLAYER=ON -DCMAKE_BUILD_TYPE=Release
nice -n 10 cmake --build build/ra-gui --target ra_gui_contract -j4
export LIONHEART_RA_GUI_HOST="$PWD/build/ra-gui/ra_gui_contract"
nice -n 10 ctest --test-dir build/native-tests -j4 --output-on-failure
```

To use an existing player build, set `FF8_PLAYER_HOST`. The native GUI cases use
`ra_gui_contract` beside that binary unless `LIONHEART_RA_GUI_HOST` overrides it.
The Vulkan GUI case requires `/usr/share/vulkan/icd.d/lvp_icd.json`.

## Audit socket addresses

On Linux x86_64, compile the syscall auditor and run the cases through it:

```sh
nice -n 10 c++ -std=c++17 tests/native/support/check_loopback.cpp -o build/check_loopback
nice -n 10 build/check_loopback ctest --test-dir build/native-tests -j4 \
  -R '^test_(ra_client|ra_gui|state_bridge)::' -LE 'game|realtime' --output-on-failure
```

The auditor follows processes and threads through exec. It inspects `bind`,
`connect`, `sendto`, `sendmsg`, and `sendmmsg` before execution. A non-loopback
IPv4 or IPv6 address fails the audit and stops the test processes. Unix sockets
and kernel netlink sockets stay local. The final line reports the address count,
process and thread count, and command exit status.

To check the auditor, run it around a program that binds `0.0.0.0`. The auditor
must reject that address before the syscall executes.
