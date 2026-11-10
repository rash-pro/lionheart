if(NOT CMAKE_CROSSCOMPILING)
  # The generator tools test_generator_parity.cmake builds, under this file's names.
  add_executable(test_generator_rules.lionheart-gen-spec ALIAS lionheart-gen-spec)
  add_executable(test_generator_rules.lionheart-gen ALIAS lionheart-gen)
  set_property(GLOBAL APPEND PROPERTY LH_HARNESSES test_generator_rules.lionheart-gen-spec test_generator_rules.lionheart-gen)
endif()
