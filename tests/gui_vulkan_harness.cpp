// Synthetic presentation only: no guest, game data, SDL window or network.
#include "gui_vulkan.h"
#include "imgui.h"
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <cstring>

void ck(VkResult r) { if (r!=VK_SUCCESS) throw std::runtime_error("VkResult "+std::to_string(r)); }
int main() try {
    ck(volkInitialize());
    VkApplicationInfo app={}; app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO; app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci={}; ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ci.pApplicationInfo=&app;
    VkInstance instance; ck(vkCreateInstance(&ci,nullptr,&instance)); volkLoadInstance(instance);
    uint32_t count=0; ck(vkEnumeratePhysicalDevices(instance,&count,nullptr)); std::vector<VkPhysicalDevice> physical(count);
    ck(vkEnumeratePhysicalDevices(instance,&count,physical.data())); if (!count) return 2;
    VkPhysicalDevice gpu=physical[0];
    vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,nullptr); std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,families.data()); unsigned family=0;
    while (!(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT)) ++family;
    float priority=1; VkDeviceQueueCreateInfo qi={}; qi.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di={}; di.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi;
    VkDevice device; ck(vkCreateDevice(gpu,&di,nullptr,&device)); volkLoadDevice(device);
    VkQueue queue; vkGetDeviceQueue(device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties memories; vkGetPhysicalDeviceMemoryProperties(gpu,&memories);
    auto memory=[&](unsigned mask,VkMemoryPropertyFlags required) {
        for (unsigned i=0;i<memories.memoryTypeCount;++i) {
            if ((mask&(1u<<i)) && (memories.memoryTypes[i].propertyFlags&required)==required) return i;
        }
        throw std::runtime_error("memory");
    };
    VkCommandPoolCreateInfo pi={}; pi.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pi.queueFamilyIndex=family;
    pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; VkCommandPool commands; ck(vkCreateCommandPool(device,&pi,nullptr,&commands));
    VkCommandBufferAllocateInfo ai={}; ai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ai.commandPool=commands;
    ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1; VkCommandBuffer command; ck(vkAllocateCommandBuffers(device,&ai,&command));
    ImGui::CreateContext(); ImGui::GetIO().IniFilename=nullptr; ImGui::GetIO().Fonts->AddFontDefault();
    HostGuiVulkan::configure(instance,gpu,device,family,queue);
    unsigned checked=0, reinitializations=0;
    for (unsigned round=0;round<12;++round) {
        if (round==4 || round==8) {
            host_gui_vulkan_shutdown();
            if (host_gui_vulkan_active()) throw std::runtime_error("Vulkan GUI did not detach");
            ImGui::DestroyContext();
            ImGui::CreateContext(); ImGui::GetIO().IniFilename=nullptr;
            ImGui::GetIO().Fonts->AddFontDefault();
            HostGuiVulkan::configure(instance,gpu,device,family,queue);
            ++reinitializations;
        }
        unsigned width=round>=6?1920:1280,height=round>=6?1080:720;
        if (round==6) { host_gui_vulkan_invalidate_fonts(); ImGui::GetIO().Fonts->Clear(); ImGui::GetIO().Fonts->AddFontDefault(); }
        if (!host_gui_vulkan_prepare(width,height)) return 3;
        ImGui::GetIO().DisplaySize=ImVec2(float(width),float(height)); ImGui::GetIO().DeltaTime=1.0f/60;
        ImGui::NewFrame(); ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(20,30),ImVec2(60,80),IM_COL32(159,180,216,255));
        ImGui::GetBackgroundDrawList()->AddText(ImVec2(90,40),IM_COL32_WHITE,"Atlas text"); ImGui::Render();
        VkFormat format=round%2?VK_FORMAT_B8G8R8A8_UNORM:VK_FORMAT_R8G8B8A8_UNORM;
        VkImageCreateInfo ii={}; ii.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType=VK_IMAGE_TYPE_2D; ii.format=format;
        ii.extent={width,height,1}; ii.mipLevels=ii.arrayLayers=1; ii.samples=VK_SAMPLE_COUNT_1_BIT; ii.tiling=VK_IMAGE_TILING_OPTIMAL;
        ii.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VkImage image; ck(vkCreateImage(device,&ii,nullptr,&image)); VkMemoryRequirements req; vkGetImageMemoryRequirements(device,image,&req);
        VkMemoryAllocateInfo mi={}; mi.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; mi.allocationSize=req.size; mi.memoryTypeIndex=memory(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory image_mem; ck(vkAllocateMemory(device,&mi,nullptr,&image_mem)); ck(vkBindImageMemory(device,image,image_mem,0));
        VkImageViewCreateInfo vi={}; vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image=image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
        vi.format=format; vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; VkImageView view; ck(vkCreateImageView(device,&vi,nullptr,&view));
        VkBufferCreateInfo bi={}; bi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bi.size=size_t(width)*height*4; bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buffer; ck(vkCreateBuffer(device,&bi,nullptr,&buffer)); vkGetBufferMemoryRequirements(device,buffer,&req);
        mi.allocationSize=req.size; mi.memoryTypeIndex=memory(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory buffer_mem; ck(vkAllocateMemory(device,&mi,nullptr,&buffer_mem)); ck(vkBindBufferMemory(device,buffer,buffer_mem,0));
        ck(vkResetCommandBuffer(command,0)); VkCommandBufferBeginInfo begin={}; begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; ck(vkBeginCommandBuffer(command,&begin));
        VkImageMemoryBarrier barrier={}; barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; barrier.image=image;
        barrier.subresourceRange=vi.subresourceRange; barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkClearColorValue black={}; black.float32[3]=1; vkCmdClearColorImage(command,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&vi.subresourceRange);
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        HostGuiVulkan::render(command,image,view,{width,height},format);
        VkBufferImageCopy copy={}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent=ii.extent;
        vkCmdCopyImageToBuffer(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy); ck(vkEndCommandBuffer(command));
        VkSubmitInfo submit={}; submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO; submit.commandBufferCount=1; submit.pCommandBuffers=&command;
        ck(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); ck(vkQueueWaitIdle(queue));
        void *mapped; ck(vkMapMemory(device,buffer_mem,0,bi.size,0,&mapped)); auto *rgb=static_cast<unsigned char *>(mapped);
        size_t pixel=(40*width+30)*4; unsigned r=round%2?2:0,b=round%2?0:2;
        if (rgb[pixel+r]!=159 || rgb[pixel+1]!=180 || rgb[pixel+b]!=216 || rgb[0]!=0 || rgb[1]!=0 || rgb[2]!=0) throw std::runtime_error("pixel mismatch");
        unsigned text_pixels=0; for (unsigned y=40;y<55;++y) for (unsigned x=90;x<160;++x) if (rgb[(y*width+x)*4]) ++text_pixels;
        if (text_pixels<50) throw std::runtime_error("atlas absent");
        vkUnmapMemory(device,buffer_mem); HostGuiVulkan::forget_image(image);
        vkDestroyBuffer(device,buffer,nullptr); vkFreeMemory(device,buffer_mem,nullptr); vkDestroyImageView(device,view,nullptr);
        vkDestroyImage(device,image,nullptr); vkFreeMemory(device,image_mem,nullptr); ++checked;
    }
    host_gui_vulkan_shutdown(); host_gui_vulkan_shutdown(); ImGui::DestroyContext(); vkDestroyCommandPool(device,commands,nullptr); vkDestroyDevice(device,nullptr); vkDestroyInstance(instance,nullptr);
    std::printf("{\"passed\":true,\"frames\":%u,\"formats\":[\"RGBA\",\"BGRA\"],\"font_rebuild\":true,\"resize\":true,\"texture_text\":true,\"reinitializations\":%u}\n",checked,reinitializations);
    return 0;
} catch (const std::exception &e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
