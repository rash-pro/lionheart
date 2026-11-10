find_package(PkgConfig REQUIRED)
pkg_check_modules(LH_SDL REQUIRED sdl2)
set(LH_SDL_FLAGS ${LH_SDL_CFLAGS})
set(LH_SDL_LINK ${LH_SDL_LDFLAGS})
