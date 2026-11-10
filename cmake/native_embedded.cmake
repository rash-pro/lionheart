# Writes the C source that carries, inside the release player, what it needs to
# build its own modules: the disc catalog the generator reads and the headers
# module code compiles against. Assembler includes keep megabytes of JSON out
# of C initializers; sizes and the catalog digest are fixed at configure time,
# so the configure reruns whenever one of these files changes.
function(lh_native_embedded output root windows)
  set(catalog overlays.json overlays-all.json files-disc1.json files-disc2.json files-disc3.json files-disc4.json)
  set(headers cpu.h perf_stats.h gte.h native_module.h native_module_api.h)
  if(windows)
    set(section ".section .rdata,\\\"dr\\\"")
  else()
    set(section ".section .rodata")
  endif()
  set(text "/* Written by cmake/native_embedded.cmake. */\n#include \"native_player.h\"\n\n")
  set(digest "")
  set(number 0)
  foreach(kind IN ITEMS catalog headers)
    set(rows "")
    foreach(name IN LISTS ${kind})
      if(kind STREQUAL "catalog")
        set(path "${root}/games/ff8-usa/${name}")
        file(SHA256 "${path}" sha)
        string(APPEND digest "${name}=${sha}\n")
      else()
        set(path "${root}/runtime/${name}")
      endif()
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
      file(SIZE "${path}" size)
      string(REPLACE "\\" "\\\\" quoted "${path}")
      string(REPLACE "\"" "\\\"" quoted "${quoted}")
      string(APPEND text "__asm__(\"${section}\\n.balign 16\\n.globl lh_native_embed_${number}\\nlh_native_embed_${number}:\\n"
        ".incbin \\\"${quoted}\\\"\\n.byte 0\\n.text\\n\");\n"
        "extern const unsigned char lh_native_embed_${number}[];\n")
      string(APPEND rows "    {\"${name}\", lh_native_embed_${number}, ${size}},\n")
      math(EXPR number "${number} + 1")
    endforeach()
    list(LENGTH ${kind} count)
    string(APPEND text "const host_embedded_file_t host_native_${kind}[] = {\n${rows}};\n"
      "const size_t host_native_${kind}_count = ${count};\n\n")
  endforeach()
  string(SHA256 digest "${digest}")
  string(APPEND text "const char host_native_catalog_digest[] = \"${digest}\";\n")
  file(CONFIGURE OUTPUT "${output}" CONTENT "${text}" @ONLY)
endfunction()
