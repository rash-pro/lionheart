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
bool fonts_uploaded=false;
uint64_t font_gen=1;
std::vector<Renderer> renderers;
std::vector<Target> targets;

static uint64_t hash_bytes(uint64_t h,const void *data,size_t len) {
    const auto *p=static_cast<const uint8_t *>(data);
    for (size_t i=0;i<len;++i) h=(h^p[i])*UINT64_C(0x100000001b3);
    return h;
}

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
void reset_resources() {
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
    pool=VK_NULL_HANDLE; owner=nullptr; fonts_uploaded=false; ++font_gen;
}
}

namespace HostGuiVulkan {
uint64_t font_generation() { return font_gen; }
uint64_t draw_hash() {
    if (!owner || !device.logical) return 0;
    ImDrawData *draw=ImGui::GetDrawData();
    if (!draw || !draw->Valid || draw->TotalVtxCount==0) return 0;
    uint64_t h=UINT64_C(0xcbf29ce484222325);
    h=hash_bytes(h,&draw->DisplayPos,sizeof(draw->DisplayPos));
    h=hash_bytes(h,&draw->DisplaySize,sizeof(draw->DisplaySize));
    h=hash_bytes(h,&draw->FramebufferScale,sizeof(draw->FramebufferScale));
    h=hash_bytes(h,&draw->CmdListsCount,sizeof(draw->CmdListsCount));
    for (int i=0;i<draw->CmdListsCount;++i) {
        const ImDrawList *list=draw->CmdLists[i];
        int nv=list->VtxBuffer.Size, ni=list->IdxBuffer.Size, nc=list->CmdBuffer.Size;
        h=hash_bytes(h,&nv,sizeof(nv));
        h=hash_bytes(h,&ni,sizeof(ni));
        h=hash_bytes(h,&nc,sizeof(nc));
        if (nv>0) h=hash_bytes(h,list->VtxBuffer.Data,size_t(nv)*sizeof(ImDrawVert));
        if (ni>0) h=hash_bytes(h,list->IdxBuffer.Data,size_t(ni)*sizeof(ImDrawIdx));
        for (int c=0;c<nc;++c) {
            const ImDrawCmd &cmd=list->CmdBuffer[c];
            h=hash_bytes(h,&cmd.ClipRect,sizeof(cmd.ClipRect));
            h=hash_bytes(h,&cmd.TextureId,sizeof(cmd.TextureId));
            h=hash_bytes(h,&cmd.VtxOffset,sizeof(cmd.VtxOffset));
            h=hash_bytes(h,&cmd.IdxOffset,sizeof(cmd.IdxOffset));
            h=hash_bytes(h,&cmd.ElemCount,sizeof(cmd.ElemCount));
            uintptr_t cb=reinterpret_cast<uintptr_t>(cmd.UserCallback);
            h=hash_bytes(h,&cb,sizeof(cb));
        }
    }
    return h ? h : 1;
}
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
    barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
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
    ImGuiContext *current=ImGui::GetCurrentContext();
    if (!device.logical || !current) return 0;
    if (owner && owner!=current) reset_resources();
    if (!owner) {
        try {
            VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,32};
            VkDescriptorPoolCreateInfo info={}; info.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            info.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            info.maxSets=32; info.poolSizeCount=1; info.pPoolSizes=&size;
            require(vkCreateDescriptorPool(device.logical,&info,nullptr,&pool));
            // These passes have format-specific pipelines with identically defined
            // descriptor layouts, so they share one font atlas and texture.
            renderer(VK_FORMAT_R8G8B8A8_UNORM);
            renderer(VK_FORMAT_B8G8R8A8_UNORM);
            renderer(VK_FORMAT_A8B8G8R8_UNORM_PACK32);
            ImGui::GetIO().BackendFlags|=ImGuiBackendFlags_RendererHasVtxOffset;
            owner=current;
        } catch (...) {
            reset_resources();
            throw;
        }
    }
    CurrentContext restore;
    if (fonts_uploaded && !ImGui::GetIO().Fonts->TexID) {
        vkDeviceWaitIdle(device.logical);
        for (const auto &backend:renderers) {
            ImGui::SetCurrentContext(backend.context);
            ImGui_ImplVulkan_DestroyFontsTexture();
        }
        fonts_uploaded=false;
        ++font_gen;
    }
    ImGui::SetCurrentContext(renderers.front().context);
    bool had_fonts=fonts_uploaded;
    ImGui_ImplVulkan_NewFrame(); // Own staging commands, never the guest pass.
    fonts_uploaded=ImGui::GetIO().Fonts->TexID!=0;
    if (fonts_uploaded && !had_fonts) ++font_gen;
    return 1;
} catch (const std::exception &error) {
    std::fprintf(stderr,"%s\n",error.what()); return 0;
}
extern "C" void host_gui_vulkan_invalidate_fonts(void) {
    if (!device.logical || renderers.empty()) return;
    vkDeviceWaitIdle(device.logical);
    CurrentContext restore;
    for (const auto &backend:renderers) {
        ImGui::SetCurrentContext(backend.context);
        ImGui_ImplVulkan_DestroyFontsTexture();
    }
    fonts_uploaded=false;
    ++font_gen;
}
extern "C" void host_gui_vulkan_shutdown(void) {
    if (!device.logical) return;
    reset_resources();
    device={};
}
