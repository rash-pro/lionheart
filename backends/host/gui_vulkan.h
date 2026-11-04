#ifndef FF8_HOST_GUI_VULKAN_H
#define FF8_HOST_GUI_VULKAN_H
#ifdef __cplusplus
extern "C" {
#endif
/* Called by the toolkit adapter outside any render pass, before ImGui::NewFrame. */
int host_gui_vulkan_prepare(unsigned width, unsigned height);
void host_gui_vulkan_invalidate_fonts(void);
void host_gui_vulkan_shutdown(void);
int host_gui_vulkan_active(void);
#ifdef __cplusplus
}
#include "volk/volk.h"
namespace HostGuiVulkan {
void configure(VkInstance, VkPhysicalDevice, VkDevice, unsigned queue_family, VkQueue);
/* Forget before replacing the target/view. Resize is rare and drains its old work. */
void forget_image(VkImage);
bool render(VkCommandBuffer, VkImage, VkImageView, VkExtent2D, VkFormat);
}
#endif
#endif
