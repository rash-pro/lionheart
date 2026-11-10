# XZ Embedded

- Origin: https://github.com/tukaani-project/xz-embedded
- Release tag: v2024-12-30, commit ae63ae3a36ed01724674e8f3d750dc47bf125410
- Licence: 0BSD (see COPYING)
- Files, unmodified, flattened from their upstream folders: `linux/include/linux/xz.h`,
  `userspace/xz_config.h`, and from `linux/lib/xz/`: `xz_private.h`, `xz_stream.h`, `xz_lzma2.h`,
  `xz_crc32.c`, `xz_crc64.c`, `xz_dec_stream.c` and `xz_dec_lzma2.c`.
- The BCJ filters, SHA-256 check, test programs and kernel glue are omitted. The build defines
  `XZ_USE_CRC64`, the check the `xz` tool writes by default.

The player decodes the official Linux compiler archive (`.tar.xz`) with it while installing that compiler.
