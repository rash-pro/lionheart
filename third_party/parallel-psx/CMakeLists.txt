# Only the independently licensed parallel-psx directory is compiled.
file(GLOB VULKAN_SOURCES CONFIGURE_DEPENDS vulkan/*.cpp)
add_library(parallel_psx STATIC ${VULKAN_SOURCES} volk/volk.c atlas/atlas.cpp renderer/renderer.cpp)
target_include_directories(parallel_psx SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/util ${CMAKE_CURRENT_SOURCE_DIR}/volk
    ${CMAKE_CURRENT_SOURCE_DIR}/glsl/prebuilt)
target_compile_features(parallel_psx PUBLIC cxx_std_14)
target_compile_definitions(parallel_psx PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(parallel_psx PUBLIC ${CMAKE_DL_LIBS})
