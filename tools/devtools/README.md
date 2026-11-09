# Native developer tools

Configure with `cmake -S tools/devtools -B build/devtools` and build with
`cmake --build build/devtools -j4`. This standalone host project needs C++17
and does not discover or invoke an interpreter.

`pad-support` folds the controller verification helpers into a native header.
Include `pad_support.h` for schedules, field samples, movement comparisons and
protocol checks. The former module had no command-line entry point.
The five identities in `test_host_pad_verification` retain the original inputs
and assertions, including skipped polls and centered idle axes.

`build/devtools/iro-extract ARCHIVE --list` inspects the IROS directory.
Use `--output DIRECTORY` to extract movies and cameras outside the repository.
Unknown compression, overlapping/truncated records, unsafe paths, duplicate
basenames and symlink escapes fail before unsafe extraction. Existing files are
compared before reuse; replacement uses an exclusively created `.part` file.

The five IROS identities in `test_movie_hd` are native. Its other eight
identities explicitly skip until a native Vulkan fixture exposes the separately
built `movie_contract` target and synthetic FFmpeg clips. Those skips differ from
the passing baseline; this file has not reached full parity.
