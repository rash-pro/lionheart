# Builds the Linux release package, Lionheart-VERSION-x86_64.AppImage, from the
# checkout and the sources pinned in tools/linux-deps.json. Run it in the build
# image (packaging/build-environment.sh linux): the C library and compilers of
# that image are what the package needs at least and what makes it repeatable.
#   cmake -DJOBS=4 -P tools/build_appimage.cmake
# -DDEPS, -DCACHE, -DBUILD and -DOUT move the dependency, download, build and
# output folders; -DOFFLINE=ON uses only verified cached downloads.
include("${CMAKE_CURRENT_LIST_DIR}/fetch_windows_deps.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/stage_player.cmake")
release_default(DEPS "${RELEASE_ROOT}/build/linux-deps")
release_default(CACHE "${RELEASE_ROOT}/build/downloads")
release_default(BUILD "${RELEASE_ROOT}/build/appimage")
release_default(OUT "${RELEASE_ROOT}/build/packages")
release_jobs()
foreach(path DEPS CACHE BUILD OUT)
  get_filename_component(${path} "${${path}}" ABSOLUTE)
endforeach()
set(lock "${RELEASE_ROOT}/tools/linux-deps.json")
fetch_locked_deps("${lock}" "${DEPS}" "${CACHE}")

# A component both packages carry is the same version in both, and the same
# file wherever both download the same address (SDL2 differs: source here,
# the prebuilt MinGW libraries there).
file(READ "${lock}" linux_lock)
file(READ "${RELEASE_ROOT}/tools/windows-deps.json" windows_lock)
string(JSON linux_count LENGTH "${linux_lock}")
string(JSON windows_count LENGTH "${windows_lock}")
math(EXPR linux_last "${linux_count}-1")
math(EXPR windows_last "${windows_count}-1")
foreach(i RANGE ${linux_last})
  string(JSON name GET "${linux_lock}" ${i} name)
  foreach(j RANGE ${windows_last})
    string(JSON other GET "${windows_lock}" ${j} name)
    if(name STREQUAL other)
      string(JSON a GET "${linux_lock}" ${i} url)
      string(JSON b GET "${windows_lock}" ${j} url)
      set(keys version)
      if(a STREQUAL b)
        list(APPEND keys sha256 size)
      endif()
      foreach(key IN LISTS keys)
        string(JSON a GET "${linux_lock}" ${i} ${key})
        string(JSON b GET "${windows_lock}" ${j} ${key})
        if(NOT a STREQUAL b)
          message(FATAL_ERROR "${name} differs between the Linux and Windows locks (${key})")
        endif()
      endforeach()
    endif()
  endforeach()
endforeach()

file(READ "${RELEASE_ROOT}/CMakeLists.txt" top)
if(NOT top MATCHES "project\\(lionheart VERSION ([0-9]+[.][0-9]+[.][0-9]+)")
  message(FATAL_ERROR "No project version in CMakeLists.txt")
endif()
set(VERSION "${CMAKE_MATCH_1}")
# File times inside the package are the commit's, not the build's.
if(DEFINED ENV{SOURCE_DATE_EPOCH})
  set(epoch "$ENV{SOURCE_DATE_EPOCH}")
else()
  execute_process(COMMAND git -C "${RELEASE_ROOT}" log -1 --format=%ct
    OUTPUT_VARIABLE epoch OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE status)
  if(NOT status EQUAL 0 OR NOT epoch MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Set SOURCE_DATE_EPOCH outside a git checkout")
  endif()
endif()
set(ENV{SOURCE_DATE_EPOCH} "${epoch}")
set(ENV{TZ} UTC)
set(ENV{LC_ALL} C)

# Where a checkout or the dependency folder lives must not reach the bytes:
# compiled-in file names are made relative to these roots.
set(maps "-ffile-prefix-map=${RELEASE_ROOT}=. -ffile-prefix-map=${DEPS}=deps -ffile-prefix-map=${BUILD}=build")
set(PREFIX "${DEPS}/install")
set(ENV{PKG_CONFIG_PATH} "${PREFIX}/lib/pkgconfig")

# Reuse an installed prefix only if it is exactly what this lock and recipe
# produced before, as the Windows build does.
file(SHA256 "${lock}" lock_hash)
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" recipe_hash)
set(identity "${lock_hash}\n${recipe_hash}\n${maps}\n")
function(prefix_receipt output)
  file(GLOB_RECURSE files LIST_DIRECTORIES FALSE RELATIVE "${PREFIX}" "${PREFIX}/*")
  list(REMOVE_ITEM files SOURCE.sha256)
  list(SORT files)
  set(receipt "${identity}")
  foreach(file IN LISTS files)
    if(IS_SYMLINK "${PREFIX}/${file}")
      file(READ_SYMLINK "${PREFIX}/${file}" target)
      string(APPEND receipt "link ${target}  ${file}\n")
    else()
      file(SHA256 "${PREFIX}/${file}" hash)
      string(APPEND receipt "${hash}  ${file}\n")
    endif()
  endforeach()
  set(${output} "${receipt}" PARENT_SCOPE)
endfunction()
if(EXISTS "${PREFIX}")
  if(NOT EXISTS "${PREFIX}/SOURCE.sha256")
    message(FATAL_ERROR "Missing install provenance at ${PREFIX}; rebuild in an empty dependency root")
  endif()
  prefix_receipt(actual)
  file(READ "${PREFIX}/SOURCE.sha256" expected)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "Different install provenance at ${PREFIX}")
  endif()
else()
  set(common -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=${PREFIX}" -DCMAKE_INSTALL_LIBDIR=lib
    "-DCMAKE_C_FLAGS=${maps}" -DCMAKE_SKIP_RPATH=ON)
  # zlib links into the player itself, as on Windows.
  release_run("${CMAKE_COMMAND}" -S "${DEP_zlib}" -B "${DEPS}/build-zlib" ${common}
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
  release_run("${CMAKE_COMMAND}" --build "${DEPS}/build-zlib" --parallel "${JOBS}")
  release_run("${CMAKE_COMMAND}" --install "${DEPS}/build-zlib")
  file(GLOB shared "${PREFIX}/lib/libz.so*")
  file(REMOVE ${shared})

  # SDL2 opens the display, audio and input libraries of the system at run
  # time, so it needs only the C library to load.
  release_run("${CMAKE_COMMAND}" -S "${DEP_SDL2}" -B "${DEPS}/build-sdl2" ${common}
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_TESTS=OFF -DSDL_INSTALL_TESTS=OFF
    -DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_WAYLAND_LIBDECOR=ON -DSDL_KMSDRM=ON -DSDL_OPENGL=ON -DSDL_OPENGLES=ON
    -DSDL_VULKAN=ON -DSDL_ALSA=ON -DSDL_PULSEAUDIO=ON -DSDL_PIPEWIRE=ON -DSDL_JACK=OFF -DSDL_SNDIO=OFF
    -DSDL_NAS=OFF -DSDL_ESD=OFF -DSDL_ARTS=OFF -DSDL_LIBSAMPLERATE=OFF -DSDL_IBUS=OFF -DSDL_RPATH=OFF)
  release_run("${CMAKE_COMMAND}" --build "${DEPS}/build-sdl2" --parallel "${JOBS}")
  release_run("${CMAKE_COMMAND}" --install "${DEPS}/build-sdl2")

  # HTTP and HTTPS only. TLS and its certificates are the system's: OpenSSL
  # is not ours to bundle, and the system knows where its certificates are.
  release_run("${CMAKE_COMMAND}" -S "${DEP_curl}" -B "${DEPS}/build-curl" ${common}
    -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF
    -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF -DHTTP_ONLY=ON
    -DCURL_USE_OPENSSL=ON -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DUSE_NGTCP2=OFF
    -DUSE_QUICHE=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF
    -DCURL_USE_GSSAPI=OFF -DCURL_DISABLE_LDAP=ON -DENABLE_ARES=OFF
    -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none -DCURL_CA_FALLBACK=ON)
  release_run("${CMAKE_COMMAND}" --build "${DEPS}/build-curl" --parallel "${JOBS}")
  release_run("${CMAKE_COMMAND}" --install "${DEPS}/build-curl")

  # The renderer needs newer Vulkan headers than this image has: the version
  # the Windows build compiles against.
  foreach(headers vulkan vk_video)
    file(COPY "${DEP_vulkan-headers}/include/${headers}" DESTINATION "${PREFIX}/include")
  endforeach()

  # The Windows configuration, for the same LGPL libraries. configure finds the
  # sources through a link named src, the one way it keeps their path relative,
  # and installs under /usr, so neither the source folder nor the prefix enters
  # the libraries' file names or configuration string.
  set(ffbuild "${DEP_ffmpeg}/build-linux")
  file(REMOVE_RECURSE "${ffbuild}")
  file(MAKE_DIRECTORY "${ffbuild}")
  file(CREATE_LINK "${DEP_ffmpeg}" "${ffbuild}/src" SYMBOLIC)
  set(configure src/configure --prefix=/usr --libdir=/usr/lib --arch=x86_64 --target-os=linux
    --disable-x86asm --disable-autodetect --disable-programs --disable-doc --disable-debug
    --disable-static --enable-shared --disable-network --disable-avdevice --disable-avfilter
    --disable-swresample --disable-encoders --disable-muxers --disable-filters --disable-devices)
  execute_process(COMMAND ${configure} WORKING_DIRECTORY "${ffbuild}" RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "FFmpeg configure failed: ${status}")
  endif()
  execute_process(COMMAND make "-j${JOBS}" WORKING_DIRECTORY "${ffbuild}" RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "FFmpeg build failed: ${status}")
  endif()
  execute_process(COMMAND make install "DESTDIR=${DEPS}/ffmpeg-root" WORKING_DIRECTORY "${ffbuild}" RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "FFmpeg install failed: ${status}")
  endif()
  foreach(sub include lib)
    file(COPY "${DEPS}/ffmpeg-root/usr/${sub}" DESTINATION "${PREFIX}")
  endforeach()
  file(REMOVE_RECURSE "${DEPS}/ffmpeg-root")
  string(JOIN " " line ${configure})
  file(WRITE "${PREFIX}/ffmpeg-configure.txt" "${line}\n")
  file(GLOB metadata "${PREFIX}/lib/pkgconfig/*.pc")
  foreach(pc IN LISTS metadata)
    file(READ "${pc}" value)
    string(REGEX REPLACE "(^|\n)prefix=[^\n]*" "\\1prefix=${PREFIX}" value "${value}")
    string(REGEX REPLACE "(^|\n)libdir=/usr/lib" "\\1libdir=${PREFIX}/lib" value "${value}")
    string(REGEX REPLACE "(^|\n)includedir=/usr/include" "\\1includedir=${PREFIX}/include" value "${value}")
    file(WRITE "${pc}" "${value}")
  endforeach()
  prefix_receipt(receipt)
  file(WRITE "${PREFIX}/SOURCE.sha256" "${receipt}")
endif()

# The player finds its libraries beside it through its own run path, never
# through LD_LIBRARY_PATH, which every program it starts would inherit.
file(REMOVE_RECURSE "${BUILD}")
set(flags "${maps} -isystem ${PREFIX}/include")
release_run("${CMAKE_COMMAND}" -S "${RELEASE_ROOT}" -B "${BUILD}/player" -DCMAKE_BUILD_TYPE=Release -DFF8_PLAYER=ON
  "-DCMAKE_C_FLAGS=${flags}" "-DCMAKE_CXX_FLAGS=${flags}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  "-DCMAKE_INSTALL_RPATH=\$ORIGIN/../lib" -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON
  -DCMAKE_EXE_LINKER_FLAGS=-Wl,--enable-new-dtags "-DCMAKE_PREFIX_PATH=${PREFIX}"
  "-DZLIB_LIBRARY=${PREFIX}/lib/libz.a" "-DZLIB_INCLUDE_DIR=${PREFIX}/include")
release_run("${CMAKE_COMMAND}" --build "${BUILD}/player" --target ff8 --parallel "${JOBS}")

set(APPDIR "${BUILD}/AppDir")
release_run("${CMAKE_COMMAND}" --install "${BUILD}/player" --prefix "${APPDIR}/usr")

# Bundle every library the player loads that the build prefix provides, and
# theirs in turn; whatever else it needs is the system's (the package check
# holds the list of what may come from there).
function(needed binary output)
  execute_process(COMMAND objdump -p "${binary}" OUTPUT_VARIABLE dump RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "objdump failed on ${binary}")
  endif()
  string(REGEX MATCHALL "NEEDED +[^\n]+" lines "${dump}")
  set(names)
  foreach(line IN LISTS lines)
    string(REGEX REPLACE "NEEDED +" "" name "${line}")
    list(APPEND names "${name}")
  endforeach()
  set(${output} "${names}" PARENT_SCOPE)
endfunction()
file(MAKE_DIRECTORY "${APPDIR}/usr/lib")
set(pending "${APPDIR}/usr/bin/lionheart")
set(bundled)
while(pending)
  list(POP_FRONT pending binary)
  needed("${binary}" names)
  foreach(name IN LISTS names)
    if(EXISTS "${PREFIX}/lib/${name}" AND NOT name IN_LIST bundled)
      file(REAL_PATH "${PREFIX}/lib/${name}" real)
      configure_file("${real}" "${APPDIR}/usr/lib/${name}" COPYONLY)
      list(APPEND bundled "${name}")
      list(APPEND pending "${APPDIR}/usr/lib/${name}")
    endif()
  endforeach()
endwhile()
list(SORT bundled)
message(STATUS "Bundled: ${bundled}")

set(PLATFORM appimage)
set(doc "${APPDIR}/usr/share/doc/lionheart")
player_licences("${doc}")
configure_file("${lock}" "${doc}/dependencies.json" COPYONLY)

# The AppImage's own entry and icon, read by desktops that integrate it.
file(READ "${APPDIR}/usr/share/applications/lionheart.desktop" entry)
string(STRIP "${entry}" entry)
file(WRITE "${APPDIR}/usr/share/applications/lionheart.desktop" "${entry}\nX-AppImage-Version=${VERSION}\n")
configure_file("${APPDIR}/usr/share/applications/lionheart.desktop" "${APPDIR}/lionheart.desktop" COPYONLY)
configure_file("${RELEASE_ROOT}/assets/logo/icon-256.png" "${APPDIR}/lionheart.png" COPYONLY)
file(CREATE_LINK lionheart.png "${APPDIR}/.DirIcon" SYMBOLIC)
separate_arguments(map_arguments UNIX_COMMAND "${maps}")
release_run(cc -std=c99 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror ${map_arguments}
  "${RELEASE_ROOT}/packaging/linux/apprun.c" -o "${APPDIR}/AppRun")

# Modes do not depend on the checkout's umask: programs 0755, the rest 0644.
file(GLOB_RECURSE files LIST_DIRECTORIES TRUE "${APPDIR}/*")
foreach(path IN LISTS files)
  if(IS_SYMLINK "${path}")
    continue()
  elseif(IS_DIRECTORY "${path}" OR path STREQUAL "${APPDIR}/AppRun" OR path STREQUAL "${APPDIR}/usr/bin/lionheart")
    file(CHMOD "${path}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
  else()
    file(CHMOD "${path}" PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
  endif()
endforeach()

file(MAKE_DIRECTORY "${OUT}")
set(image "${BUILD}/AppDir.squashfs")
set(package "${OUT}/Lionheart-${VERSION}-x86_64.AppImage")
# Fix old timestamps too: SOURCE_DATE_EPOCH alone only clamps newer files.
release_run("${CMAKE_COMMAND}" -E env --unset=SOURCE_DATE_EPOCH mksquashfs "${APPDIR}" "${image}"
  -mkfs-time "${epoch}" -all-time "${epoch}" -noappend -no-xattrs -all-root -comp zstd
  -processors "${JOBS}" -no-progress -quiet)
# An AppImage is its runtime followed by the file system it mounts.
execute_process(COMMAND cat "${DEP_appimage-runtime}/runtime-x86_64" "${image}"
  OUTPUT_FILE "${package}" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Cannot write ${package}")
endif()
file(CHMOD "${package}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
file(SHA256 "${package}" hash)
file(SIZE "${package}" bytes)
message(STATUS "${package}: ${bytes} bytes SHA-256 ${hash}")
