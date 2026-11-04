"""Build pinned UCRT dependencies and the GUI player; stage a self-contained DLL folder."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
from fetch_windows_deps import DEFAULT, ROOT, fetch


def run(command, env=None, cwd=None):
    print(' '.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, env=env, cwd=cwd)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, default=ROOT / 'build/windows')
    p.add_argument('--deps', type=Path, default=DEFAULT)
    args = p.parse_args()
    paths = fetch(args.deps)
    build = args.build.resolve()
    prefix = args.deps / 'install-ucrt'
    toolchain = paths['llvm-mingw']
    env = {**os.environ, 'PATH': f"{toolchain}/bin:{os.environ['PATH']}",
           'LLVM_MINGW_ROOT': str(toolchain), 'WINDOWS_DEPS_ROOT': str(prefix)}
    prefix.mkdir(parents=True, exist_ok=True)
    sdl = paths['SDL2'] / 'x86_64-w64-mingw32'
    for sub in ('bin', 'include', 'lib', 'share'):
        if (sdl / sub).exists():
            shutil.copytree(sdl / sub, prefix / sub, dirs_exist_ok=True)
    pc = prefix / 'lib/pkgconfig/sdl2.pc'
    text = pc.read_text().splitlines()
    pc.write_text('\n'.join(f'prefix={prefix}' if x.startswith('prefix=') else x for x in text) + '\n')
    if not (prefix / 'lib/libz.a').exists():
        run(['cmake', '-S', paths['zlib'], '-B', paths['zlib'] / 'build-ucrt',
             f'-DCMAKE_TOOLCHAIN_FILE={ROOT}/cmake/toolchains/windows-llvm-mingw.cmake',
             f'-DCMAKE_INSTALL_PREFIX={prefix}', '-DCMAKE_BUILD_TYPE=Release'], env)
        run(['cmake', '--build', paths['zlib'] / 'build-ucrt', '-j8'], env)
        run(['cmake', '--install', paths['zlib'] / 'build-ucrt'], env)
        shutil.copyfile(prefix / 'lib/libzlibstatic.a', prefix / 'lib/libz.a')
        for dll in (prefix / 'bin').glob('*zlib*.dll'):
            dll.unlink()
        for lib in (prefix / 'lib').glob('*zlib.dll.a'):
            lib.unlink()
    for header_dir in ('vulkan', 'vk_video'):
        shutil.copytree(Path('/usr/include') / header_dir, prefix / 'include' / header_dir, dirs_exist_ok=True)
    # Relocate a cached dependency prefix without retaining machine paths.
    for metadata in (prefix / 'lib/pkgconfig').glob('*.pc'):
        value = metadata.read_text()
        match = __import__('re').search(r'^prefix=(.+)$', value, __import__('re').M)
        if match:
            metadata.write_text(value.replace(match[1], str(prefix)))
    ffbuild = paths['ffmpeg'] / 'build-ucrt'
    ffbuild.mkdir(exist_ok=True)
    configure = [str(paths['ffmpeg'] / 'configure'), f'--prefix={prefix}',
                 '--enable-cross-compile', '--target-os=mingw32', '--arch=x86_64',
                 '--cc=x86_64-w64-mingw32-clang', '--cxx=x86_64-w64-mingw32-clang++',
                 '--ar=llvm-ar', '--ranlib=llvm-ranlib', '--nm=llvm-nm', '--strip=llvm-strip', '--cross-prefix=x86_64-w64-mingw32-', '--windres=x86_64-w64-mingw32-windres',
                 '--disable-x86asm', '--disable-autodetect', '--disable-programs', '--disable-doc',
                 '--disable-debug', '--disable-static', '--enable-shared', '--disable-network',
                 '--disable-avdevice', '--disable-avfilter', '--disable-swresample',
                 '--disable-encoders', '--disable-muxers', '--disable-filters', '--disable-devices']
    if not (prefix / 'lib/libavformat.dll.a').exists():
        run(configure, env, ffbuild)
        run(['make', '-j8'], env, ffbuild)
        run(['make', 'install'], env, ffbuild)
        (prefix / 'ffmpeg-configure.txt').write_text(' '.join(configure) + '\n')
    run(['cmake', '-S', ROOT, '-B', build,
         f'-DCMAKE_TOOLCHAIN_FILE={ROOT}/cmake/toolchains/windows-llvm-mingw.cmake',
         '-DCMAKE_BUILD_TYPE=Release', '-DFF8_PLAYER=ON',
         '-DFF8_STATE_BRIDGE=OFF', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
         f'-DZLIB_LIBRARY={prefix}/lib/libz.a', f'-DZLIB_INCLUDE_DIR={prefix}/include'], env)
    run(['cmake', '--build', build, '--target', 'ff8', '-j8'], env)
    dist = build / 'dist-windows'
    dist.mkdir(exist_ok=True)
    shutil.copyfile(build / 'Lionheart.exe', dist / 'Lionheart.exe')
    for dll in ['SDL2.dll', 'avformat-61.dll', 'avcodec-61.dll', 'avutil-59.dll', 'swscale-8.dll']:
        shutil.copyfile(prefix / 'bin' / dll, dist / dll)
    for dll in ('libc++.dll', 'libunwind.dll', 'libwinpthread-1.dll'):
        source = toolchain / 'x86_64-w64-mingw32/bin' / dll
        if source.exists():
            shutil.copyfile(source, dist / dll)
    from stage_player import licences
    licences(dist, paths, prefix)
    (dist / 'dependencies.json').write_text((ROOT / 'tools/windows-deps.json').read_text())
    print(f'Distribution: {dist}; {sum(p.stat().st_size for p in dist.rglob("*") if p.is_file())} bytes')


if __name__ == '__main__':
    main()
