#pragma once
#ifdef _WIN32
#include <windows.h>
#define RTLD_NOW 0
#define RTLD_LOCAL 0
inline void *dlopen(const char *path, int) { return LoadLibraryA(path); }
inline void *dlsym(void *module, const char *symbol) { return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(module), symbol)); }
inline const char *dlerror() { return "Windows library operation failed"; }
#else
#include <dlfcn.h>
#endif
