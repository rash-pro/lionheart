# Cases are loaded before the runner target exists. Attach its standard thread
# dependency after CMake has declared it, without changing the shared project.
include_guard(GLOBAL)
find_package(Threads REQUIRED)
function(lh_link_network_support)
  if(TARGET lionheart_tests)
    target_link_libraries(lionheart_tests PRIVATE Threads::Threads)
  endif()
endfunction()
cmake_language(DEFER CALL lh_link_network_support)
