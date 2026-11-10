# The module ABI id: the headers module code is compiled against, plus the
# build settings that shape it. The host and every module builder run this same
# function over the same files, so disagreeing inputs produce different ids.
# The compiler itself is deliberately absent: the player and the modules are
# built by different compilers by design, and the layout traits compiled into
# both sides are what actually have to agree.
function(lh_native_module_abi_id output root stats)
  set(text "")
  foreach(name IN ITEMS cpu.h perf_stats.h gte.h native_module.h native_module_api.h)
    file(SHA256 "${root}/runtime/${name}" digest)
    # A header edit must reach both sides, so it reruns every configure that used it.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${root}/runtime/${name}")
    string(APPEND text "${name}=${digest}\n")
  endforeach()
  if(stats)
    string(APPEND text "stats=1\n")
  else()
    string(APPEND text "stats=0\n")
  endif()
  string(SHA256 id "${text}")
  set(${output} "${id}" PARENT_SCOPE)
endfunction()
