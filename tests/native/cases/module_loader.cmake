# The host side links the runtime the call table points into; the modules link
# nothing at all, which is the property the loader relies on.
lh_harness(loader FLAGS -std=c99 -O1 -Wall -Wextra INCLUDES runtime backends/host RUNTIME_ALL
  SOURCES tests/native/cases/module_loader_host.c LIBS ${CMAKE_DL_LIBS})
set(module_flags -std=c99 -O1 -Wall -Wextra -DFF8_NATIVE_MODULE "-DLH_MODULE_ABI_ID=\"lionheart-loader-test\"")
set(module_link -nostdlib -Wl,--no-undefined)
foreach(variant IN ITEMS main main_code_mismatch overlay overlay_missing_import overlay_other_exe overlay_no_descriptor)
  string(TOUPPER "MODULE_${variant}" define)
  set(defines "-D${define}")
  if(variant MATCHES "^main")
    list(APPEND defines -DMODULE_MAIN)
  endif()
  lh_harness(${variant} LIBRARY FLAGS ${module_flags} ${defines} INCLUDES runtime LINK_FLAGS ${module_link}
    SOURCES tests/native/cases/module_loader_module.c)
endforeach()
lh_harness(overlay_other_abi LIBRARY FLAGS -std=c99 -O1 -DFF8_NATIVE_MODULE "-DLH_MODULE_ABI_ID=\"lionheart-other-headers\""
  INCLUDES runtime LINK_FLAGS ${module_link} SOURCES tests/native/cases/module_loader_module.c)
# Same headers and id, other settings: statistics change the CPU layout.
lh_harness(overlay_other_flags LIBRARY FLAGS ${module_flags} -DFF8_STATS
  INCLUDES runtime LINK_FLAGS ${module_link} SOURCES tests/native/cases/module_loader_module.c)
