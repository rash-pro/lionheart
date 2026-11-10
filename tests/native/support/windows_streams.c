/* Harness protocols use bytes; the Windows CRT otherwise translates newlines
 * and treats control-Z as end of input. */
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
__attribute__((constructor)) static void binary_streams(void) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}
