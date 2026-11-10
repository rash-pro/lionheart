/* Drives the host loader over synthetic modules: boots a 64-byte executable,
 * opens each module named on the command line in order, registers whatever was
 * accepted, and reports what the registry holds afterwards. */
#define LH_MODULE_ABI_ID "lionheart-loader-test"
#include "../../../backends/host/native_modules.c"

static const char *status_name(native_module_status_t status) {
    static const char *const names[] = {"ok", "not-configured", "unreadable", "truncated", "load-failed",
                                        "no-descriptor", "bad-abi", "bad-image", "bad-tables", "code-mismatch",
                                        "missing-import"};
    return (unsigned)status < sizeof(names) / sizeof(*names) ? names[status] : "unknown";
}

int main(int argc, char **argv) {
    static uint8_t text[64];
    cpu_t *c = calloc(1, sizeof(*c));
    int i;
    if (!c) return 2;
    for (i = 0; i < 64; ++i) text[i] = (uint8_t)(i * 37 + 11);
    if (!native_modules_configure(0x35e25b05u, 0x80010000u, sizeof(text), text)) return 3;
    for (i = 1; i < argc; ++i) {
        native_module_t *module;
        char reason[256] = "";
        native_module_status_t status = native_module_open(argv[i], &module, reason, sizeof(reason));
        printf("%s %s\n", status_name(status), status ? reason : "");
        if (status == NATIVE_MODULE_OK) {
            native_module_publish(module);
            native_modules_commit(c);
        }
    }
    printf("registered %zu main %zu overlays %zu\n", native_modules_registered(), c->entry_count, c->overlay_count);
    /* Run what was registered: the overlay's body calls main through its import
     * slot and stops the CPU through the call table. */
    if (c->overlay_count) {
        c->overlays[0].entries[0].fn(c);
        printf("r2 %u stop %s\n", (unsigned)c->r[2], c->stop_reason);
    }
    native_modules_shutdown(c);
    printf("after shutdown %zu\n", native_modules_registered());
    free(c);
    return 0;
}
