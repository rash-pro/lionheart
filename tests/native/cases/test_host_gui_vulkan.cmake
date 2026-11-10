lh_harness(gui CXX_FLAGS -std=c++14 -O0 -DIMGUI_IMPL_VULKAN_USE_VOLK -DVK_NO_PROTOTYPES
  INCLUDES backends/host third_party/imgui third_party/parallel-psx third_party/parallel-psx/volk
  SOURCES tests/gui_vulkan_harness.cpp backends/host/gui_vulkan.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_widgets.cpp third_party/imgui/imgui_tables.cpp
    third_party/parallel-psx/volk/volk.c LINK_FLAGS -ldl)
