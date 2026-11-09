# Native developer tools

Configure with `cmake -S tools/devtools -B build/devtools` and build with
`cmake --build build/devtools -j4`. This standalone host project needs C++17
and does not discover or invoke an interpreter.

`pad-support` folds the controller verification helpers into a native header.
Include `pad_support.h` for schedules, field samples, movement comparisons and
protocol checks. The former module had no command-line entry point.
The five identities in `test_host_pad_verification` retain the original inputs
and assertions, including skipped polls and centered idle axes.
