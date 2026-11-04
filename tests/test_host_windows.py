"""Player session behavior and bounded portable hashing, without private media."""
import hashlib
import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def session_binary(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("player-session")
    stub = tmp / "startup.c"
    stub.write_text(r'''
#include <SDL.h>
#include <stdio.h>
#include <string.h>
int host_player_main(int argc, char **argv) {
    (void)argc; (void)argv;
    fputs("fatal startup: supply disc 1\n",stderr);
    for (int i=0;i<10000;++i) fputc('x',stderr);
    return 2;
}
int __wrap_SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *text, SDL_Window *window) {
    (void)flags; (void)window;
    printf("messagebox title=%s text=%.27s\n",title,text);
    return 0;
}
''')
    flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "sdl2"], text=True).split()
    binary = tmp / "lionheart"
    subprocess.run(["cc", "-std=c99", "-DFF8_PLAYER", "-Wall", "-Wextra", "-Werror",
                    "-Ibackends/host", "backends/host/player_session.c", "backends/host/player_paths.c",
                    str(stub), "-Wl,--wrap=SDL_ShowSimpleMessageBox", *flags, "-o", str(binary)],
                   cwd=ROOT, check=True)
    return binary


def test_session_tee_rotation_and_fatal_box(session_binary, tmp_path):
    env = {**os.environ, "HOME": str(tmp_path), "XDG_DATA_HOME": str(tmp_path / "data"),
           "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}
    expected = "fatal startup: supply disc 1\n" + "x" * 10000
    for n in range(7):
        proc = subprocess.run([str(session_binary), *(["--headless"] if n < 6 else [])],
                              env=env, capture_output=True, text=True, check=False)
        assert proc.returncode == 2 and proc.stderr == expected
        assert ("messagebox title=Lionheart text=fatal startup" in proc.stdout) == (n == 6)
    logs = tmp_path / "data/lionheart/logs"
    assert sorted(p.name for p in logs.iterdir()) == [
        "lionheart.1.log", "lionheart.2.log", "lionheart.3.log", "lionheart.4.log", "lionheart.log"]
    assert all(p.read_text() == expected for p in logs.iterdir())


def test_gui_diagnostic_errors_never_open_native_fatal_dialog(session_binary, tmp_path):
    result = subprocess.run([str(session_binary), '--gui-test', 'unused-script.txt'],
                            env={**os.environ, 'XDG_DATA_HOME': str(tmp_path / 'data')},
                            capture_output=True, text=True)
    assert result.returncode == 2 and 'fatal startup' in result.stderr
    assert 'messagebox' not in result.stdout


def test_streaming_sha1_padding_and_chunks(tmp_path):
    source = tmp_path / "sha.c"
    source.write_text(r'''
#include "sha1_stream.h"
#include <stdio.h>
int main(void) {
    host_sha1_t s; unsigned char data[37], out[20]; size_t n;
    host_sha1_init(&s);
    while ((n=fread(data,1,sizeof(data),stdin))) host_sha1_update(&s,data,n);
    host_sha1_final(&s,out);
    for (unsigned i=0;i<20;++i) printf("%02x",out[i]);
    return ferror(stdin) ? 1 : 0;
}
''')
    binary = tmp_path / "sha"
    subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-Ibackends/host",
                    str(source), "-o", str(binary)], cwd=ROOT, check=True)
    for size in [0, 1, 55, 56, 63, 64, 65, 65535, 65536, 1048583]:
        data = bytes(i % 251 for i in range(size))
        result = subprocess.run([str(binary)], input=data, capture_output=True, check=True)
        assert result.stdout.decode() == hashlib.sha1(data).hexdigest()


def test_log_directory_failure_has_fatal_diagnostic(session_binary, tmp_path):
    blocked = tmp_path / "not-a-directory"
    blocked.write_text("owned test file")
    proc = subprocess.run([str(session_binary)], capture_output=True, text=True,
                          env={**os.environ, "XDG_DATA_HOME": str(blocked),
                               "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"})
    assert proc.returncode == 2
    assert proc.stderr == "Cannot create Lionheart log directory\n"
    assert "messagebox title=Lionheart" in proc.stdout
    assert blocked.read_text() == "owned test file"
