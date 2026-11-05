"""Stage player executable and notices; Linux uses system shared libraries."""
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def licences(dist, paths=None, prefix=None):
    dest = dist / 'licences'
    dest.mkdir(exist_ok=True)
    for source, name in [
        ('third_party/parallel-psx/COPYING', 'parallel-psx-ISC.txt'),
        ('third_party/rcheevos/LICENSE', 'rcheevos-MIT.txt'),
        ('third_party/spleen/LICENSE', 'Spleen-BSD-2.txt'),
        ('third_party/imgui/LICENSE.txt', 'Dear-ImGui-MIT.txt'),
        ('third_party/nativefiledialog-extended/LICENSE', 'nativefiledialog-zlib.txt'),
        ('third_party/chakra-petch/OFL.txt', 'Chakra-Petch-OFL.txt'),
    ]:
        shutil.copyfile(ROOT / source, dest / name)
    for source, name in [
        ('LICENSE', 'Lionheart-MIT.txt'),
        ('THIRD_PARTY_NOTICES.md', 'THIRD_PARTY_NOTICES.md'),
        ('licenses/curl.txt', 'curl.txt'),
        ('third_party/parallel-psx/volk/volk.h', 'volk-MIT.h'),
    ]:
        shutil.copyfile(ROOT / source, dest / name)
    if paths:
        shutil.copyfile(paths['SDL2'] / 'LICENSE.txt', dest / 'SDL2-zlib.txt')
        shutil.copyfile(paths['ffmpeg'] / 'COPYING.LGPLv2.1', dest / 'FFmpeg-LGPL-2.1.txt')
        shutil.copyfile(paths['zlib'] / 'LICENSE', dest / 'zlib.txt')
        parts = (prefix / 'ffmpeg-configure.txt').read_text().split()
        parts[0] = '$FFMPEG_SOURCE/configure'
        parts = ['--prefix=${PREFIX}' if item.startswith('--prefix=') else item for item in parts]
        (dest / 'FFmpeg-configure.txt').write_text(' '.join(parts) + '\n')
        shutil.copyfile(paths['llvm-mingw'] / 'LICENSE.TXT', dest / 'llvm-mingw-runtime.txt')
        for name in ('COPYING.MinGW-w64-runtime.txt', 'COPYING.winpthreads.txt'):
            shutil.copyfile(paths['llvm-mingw'] / 'x86_64-w64-mingw32/share/mingw32' / name, dest / name)
        (dest / 'FFmpeg-source.txt').write_text(
            'FFmpeg 7.1.1, unmodified source: https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz\n'
            'LGPL shared libraries; no GPL, nonfree or version3 enabled.\n'
            'Rebuild with tools/build_windows.py; exact configure line is adjacent.\n'
            'You may replace the DLLs with compatible modified versions. No static FFmpeg linkage.\n'
            'Distributors must preserve LGPL notices and make corresponding source available;\n'
            'this source URL and the checked-in pinned fetch/build recipe accompany this distribution.\n')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, default=ROOT / 'build/linux')
    args = p.parse_args()
    subprocess.run(['cmake', '-S', str(ROOT), '-B', str(args.build), '-DFF8_PLAYER=ON',
                    '-DCMAKE_BUILD_TYPE=Release'], check=True)
    subprocess.run(['cmake', '--build', str(args.build), '--target', 'ff8', '-j8'], check=True)
    dist = args.build / 'dist-linux'
    dist.mkdir(exist_ok=True)
    shutil.copy2(args.build / 'lionheart', dist / 'lionheart')
    licences(dist)
