#!/usr/bin/env python3
"""Check runner prerequisites; install missing packages on Debian/Ubuntu runners."""
import argparse
import shutil
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('platform', choices=['linux', 'windows'])
    a = p.parse_args()
    tools = ['cc', 'c++', 'cmake', 'make', 'pkg-config', 'tar', 'xz', 'glslc']
    modules = ['vulkan']
    packages = ['build-essential', 'cmake', 'pkg-config', 'libvulkan-dev', 'glslc', 'xz-utils']
    if a.platform == 'linux':
        tools += ['ffmpeg']
        modules += ['sdl2', 'zlib', 'libavformat', 'libavcodec', 'libavutil', 'libswscale', 'dbus-1']
        packages += ['libsdl2-dev', 'zlib1g-dev', 'libavformat-dev', 'libavcodec-dev',
                     'libavutil-dev', 'libswscale-dev', 'libdbus-1-dev', 'ffmpeg']
    def ready():
        return (all(shutil.which(tool) for tool in tools) and
                subprocess.run(['pkg-config', '--exists', *modules]).returncode == 0)
    if not ready():
        if not shutil.which('apt-get'):
            p.error('install these build requirements with your package manager: ' + ', '.join(packages))
        subprocess.run(['sudo', 'apt-get', 'update'], check=True)
        subprocess.run(['sudo', 'apt-get', 'install', '-y', *packages], check=True)
        if not ready(): p.error('build prerequisites are still unavailable')
    print(a.platform + ': all build prerequisites are available')


if __name__ == '__main__': main()
