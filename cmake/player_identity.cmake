get_filename_component(identity_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
# An archive has no checkout identity, even if extracted inside another repo.
set(LH_PLAYER_IDENTITY "Lionheart ${PROJECT_VERSION}")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --show-toplevel
    WORKING_DIRECTORY "${identity_root}" OUTPUT_VARIABLE git_root
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(git_root STREQUAL identity_root)
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --always --abbrev=12 --dirty --exclude=*
      WORKING_DIRECTORY "${identity_root}" OUTPUT_VARIABLE git_identity
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE git_result)
    if(git_result EQUAL 0 AND git_identity MATCHES "^[0-9a-f]+(-dirty)?$")
      string(APPEND LH_PLAYER_IDENTITY " (${git_identity})")
    endif()
  endif()
endif()
configure_file("${identity_root}/backends/host/player_identity.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/player_identity.h" @ONLY)
