find_program(desktop_database NAMES update-desktop-database)
if(desktop_database)
  # Keep cache updates inside the installation, including package staging roots.
  execute_process(COMMAND "${desktop_database}"
    "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/applications"
    COMMAND_ERROR_IS_FATAL ANY)
endif()
