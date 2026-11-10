# Paths are relative so the same generator has one identity in every checkout.
file(GLOB_RECURSE LHG_ID_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/src/*.c" "${CMAKE_CURRENT_LIST_DIR}/src/*.h"
  "${CMAKE_CURRENT_LIST_DIR}/include/*.h")
list(SORT LHG_ID_SOURCES)
set(LHG_ID "lionheart-generator-cache-v1;")
foreach(source IN LISTS LHG_ID_SOURCES)
  file(RELATIVE_PATH name "${CMAKE_CURRENT_LIST_DIR}" "${source}")
  file(SHA256 "${source}" digest)
  string(APPEND LHG_ID "${name}:${digest};")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
endforeach()
string(SHA256 LIONHEART_GENERATOR_FINGERPRINT "${LHG_ID}")
