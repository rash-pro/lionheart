#include "gui_vulkan.h"
#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Device {
    VkInstance instance=VK_NULL_HANDLE;
    VkPhysicalDevice physical=VK_NULL_HANDLE;
    VkDevice logical=VK_NULL_HANDLE;
    unsigned family=0;
    VkQueue queue=VK_NULL_HANDLE;
} device;
struct Renderer {
    VkFormat format;
    VkRenderPass pass;
    ImGuiContext *context;
};
struct Target {
    VkImage image;
    VkFramebuffer framebuffer;
};
ImGuiContext *owner=nullptr;
VkDescriptorPool pool=VK_NULL_HANDLE;
std::vector<Renderer> renderers;
std::vector<Target> targets;

struct CurrentContext {
    ImGuiContext *previous=ImGui::GetCurrentContext();
    ~CurrentContext() { ImGui::SetCurrentContext(previous); }
};
void require(VkResult result) {
    if (result!=VK_SUCCESS)
        throw std::runtime_error("GUI Vulkan: VkResult="+std::to_string(int(result)));
}
Renderer &renderer(VkFormat format) {
    auto found=std::find_if(renderers.begin(),renderers.end(),
                           [=](const Renderer &r) { return r.format==format; });
    if (found!=renderers.end()) return *found;
    VkAttachmentDescription colour={};
    colour.format=format; colour.samples=VK_SAMPLE_COUNT_1_BIT;
    colour.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD; colour.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    colour.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colour.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colour.initialLayout=colour.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference attachment={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={}; subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount=1; subpass.pColorAttachments=&attachment;
    VkRenderPassCreateInfo pass={}; pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass.attachmentCount=1; pass.pAttachments=&colour; pass.subpassCount=1; pass.pSubpasses=&subpass;
    VkRenderPass handle=VK_NULL_HANDLE;
    require(vkCreateRenderPass(device.logical,&pass,nullptr,&handle));
    CurrentContext restore;
    ImGuiContext *context=ImGui::CreateContext(ImGui::GetIO().Fonts);
    ImGui::SetCurrentContext(context);
    try {
        ImGui_ImplVulkan_InitInfo info={};
        info.ApiVersion=VK_API_VERSION_1_1; info.Instance=device.instance;
        info.PhysicalDevice=device.physical; info.Device=device.logical;
        info.QueueFamily=device.family; info.Queue=device.queue;
        info.DescriptorPool=pool; info.RenderPass=handle;
        // A guest frame may compose both window and capture targets. Eight buffers
        // cover two calls per frame across all three Granite contexts without reuse.
        info.MinImageCount=3; info.ImageCount=8; info.MSAASamples=VK_SAMPLE_COUNT_1_BIT;
        info.CheckVkResultFn=require;
        if (!ImGui_ImplVulkan_Init(&info)) throw std::runtime_error("GUI Vulkan backend initialization failed");
    } catch (...) {
        if (ImGui::GetIO().BackendRendererUserData) ImGui_ImplVulkan_Shutdown();
        ImGui::DestroyContext(context);
        vkDestroyRenderPass(device.logical,handle,nullptr);
        throw;
    }
    renderers.push_back({format,handle,context});
    return renderers.back();
}
VkFramebuffer framebuffer(VkImage image,VkImageView view,VkExtent2D extent,VkRenderPass pass) {
    auto found=std::find_if(targets.begin(),targets.end(),[=](const Target &t) { return t.image==image; });
    if (found!=targets.end()) return found->framebuffer;
    VkFramebufferCreateInfo info={}; info.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass=pass; info.attachmentCount=1; info.pAttachments=&view;
    info.width=extent.width; info.height=extent.height; info.layers=1;
    VkFramebuffer result=VK_NULL_HANDLE;
    require(vkCreateFramebuffer(device.logical,&info,nullptr,&result));
    targets.push_back({image,result});
    return result;
}
}

namespace HostGuiVulkan {
void configure(VkInstance instance,VkPhysicalDevice physical,VkDevice logical,unsigned family,VkQueue queue) {
    host_gui_vulkan_shutdown();
    device={instance,physical,logical,family,queue};
}
void forget_image(VkImage image) {
    auto found=std::find_if(targets.begin(),targets.end(),[=](const Target &t) { return t.image==image; });
    if (found==targets.end()) return;
    // This runs before ensure_image releases the old image/view, while neither
    // target is inside a pass. Do not retire a framebuffer ahead of its commands.
    require(vkDeviceWaitIdle(device.logical));
    vkDestroyFramebuffer(device.logical,found->framebuffer,nullptr);
    targets.erase(found);
}
bool render(VkCommandBuffer command,VkImage image,VkImageView view,VkExtent2D extent,VkFormat format) {
    if (!owner || !device.logical) return false;
    ImDrawData *draw=ImGui::GetDrawData();
    if (!draw || !draw->Valid || draw->TotalVtxCount==0) return false;
    CurrentContext restore;
    auto &backend=renderer(format);
    ImGui::SetCurrentContext(backend.context);
    VkFramebuffer target=framebuffer(image,view,extent,backend.pass);
    VkImageMemoryBarrier barrier={}; barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.image=image; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0,0,nullptr,0,nullptr,1,&barrier);
    VkRenderPassBeginInfo pass={}; pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass=backend.pass; pass.framebuffer=target; pass.renderArea.extent=extent;
    vkCmdBeginRenderPass(command,&pass,VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(draw,command);
    vkCmdEndRenderPass(command);
    barrier.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,0,nullptr,0,nullptr,1,&barrier);
    // Granite's following OSD begin_render_pass() calls begin_graphics(), which
    // invalidates every cached pipeline, descriptor, vertex, viewport and scissor.
    return true;
}
}

extern "C" int host_gui_vulkan_active(void) { return device.logical!=VK_NULL_HANDLE; }
extern "C" int host_gui_vulkan_prepare(unsigned width,unsigned height) try {
    (void)width; (void)height;
    if (!device.logical || !ImGui::GetCurrentContext()) return 0;
    if (!owner) {
        owner=ImGui::GetCurrentContext();
        VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,32};
        VkDescriptorPoolCreateInfo info={}; info.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        info.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets=32; info.poolSizeCount=1; info.pPoolSizes=&size;
        require(vkCreateDescriptorPool(device.logical,&info,nullptr,&pool));
        // These passes have format-specific pipelines but share the atlas and its
        // descriptor layout. The official backend remains entirely unmodified.
        renderer(VK_FORMAT_R8G8B8A8_UNORM);
        renderer(VK_FORMAT_B8G8R8A8_UNORM);
        renderer(VK_FORMAT_A8B8G8R8_UNORM_PACK32);
        ImGui::GetIO().BackendFlags|=ImGuiBackendFlags_RendererHasVtxOffset;
    }
    CurrentContext restore;
    ImGui::SetCurrentContext(renderer(VK_FORMAT_R8G8B8A8_UNORM).context);
    ImGui_ImplVulkan_NewFrame(); // Own staging commands, never the guest pass.
    return 1;
} catch (const std::exception &error) {
    std::fprintf(stderr,"%s\n",error.what()); return 0;
}
extern "C" void host_gui_vulkan_invalidate_fonts(void) {
    if (!device.logical || renderers.empty()) return;
    vkDeviceWaitIdle(device.logical);
    CurrentContext restore;
    ImGui::SetCurrentContext(renderer(VK_FORMAT_R8G8B8A8_UNORM).context);
    ImGui_ImplVulkan_DestroyFontsTexture();
}
extern "C" void host_gui_vulkan_shutdown(void) {
    if (!device.logical) return;
    vkDeviceWaitIdle(device.logical);
    CurrentContext restore;
    for (const auto &target:targets) vkDestroyFramebuffer(device.logical,target.framebuffer,nullptr);
    targets.clear();
    for (const auto &backend:renderers) {
        ImGui::SetCurrentContext(backend.context);
        ImGui_ImplVulkan_Shutdown();
        ImGui::DestroyContext(backend.context);
        vkDestroyRenderPass(device.logical,backend.pass,nullptr);
    }
    renderers.clear();
    if (pool) vkDestroyDescriptorPool(device.logical,pool,nullptr);
    pool=VK_NULL_HANDLE; owner=nullptr; device={};
}
