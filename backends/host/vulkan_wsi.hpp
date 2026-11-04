#ifndef FF8_VULKAN_WSI_HPP
#define FF8_VULKAN_WSI_HPP
#include "volk/volk.h"
#include <algorithm>
#include <vector>
#include <stdexcept>
#include <string>

namespace HostGPU {
struct WsiApi {
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR capabilities;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR formats;
    PFN_vkCreateSwapchainKHR create;
    PFN_vkDestroySwapchainKHR destroy;
    PFN_vkGetSwapchainImagesKHR images;
    PFN_vkAcquireNextImageKHR acquire;
    PFN_vkQueuePresentKHR present;
    PFN_vkCreateSemaphore create_semaphore;
    PFN_vkDestroySemaphore destroy_semaphore;
    PFN_vkCreateFence create_fence;
    PFN_vkDestroyFence destroy_fence;
    PFN_vkGetFenceStatus fence_status;
    PFN_vkWaitForFences wait_fences;
    static WsiApi current() { return {vkGetPhysicalDeviceSurfaceCapabilitiesKHR,vkGetPhysicalDeviceSurfaceFormatsKHR,
        vkCreateSwapchainKHR,vkDestroySwapchainKHR,vkGetSwapchainImagesKHR,vkAcquireNextImageKHR,vkQueuePresentKHR,
        vkCreateSemaphore,vkDestroySemaphore,vkCreateFence,vkDestroyFence,vkGetFenceStatus,vkWaitForFences}; }
};
// Retryable WSI failures never cross into guest execution. Device/resource errors do.
inline void require(VkResult result,const char *operation) {
    if (result!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+": VkResult="+std::to_string(int(result))+(result==VK_ERROR_DEVICE_LOST ? " (device lost)" : ""));
}
class Swapchain {
    struct Chain {
        VkSwapchainKHR handle=VK_NULL_HANDLE;
        std::vector<VkImage> images;
        std::vector<VkSemaphore> ready;
        std::vector<VkFence> presents;
        std::vector<VkFence> uncertain;
        uint64_t frame=0, milliseconds=0;
    } active;
    VkPhysicalDevice gpu;
    VkDevice device;
    VkQueue queue;
    WsiApi api;
    bool maintenance;
    std::vector<Chain> retired;
    bool retry=true, presented=false;
    uint64_t retry_after=0;
    void suboptimal(uint64_t now) { if (!retry) { retry=true; retry_after=now+250; } }
    VkExtent2D requested={};
    void release(Chain &chain) noexcept {
        // Destroy the retired chain before its presentation synchronization.
        if (chain.handle) api.destroy(device,chain.handle,nullptr);
        for (auto sem:chain.ready) api.destroy_semaphore(device,sem,nullptr);
        for (auto fence:chain.presents) api.destroy_fence(device,fence,nullptr);
        for (auto fence:chain.uncertain) api.destroy_fence(device,fence,nullptr);
        chain={};
    }
    void retire(uint64_t frame,uint64_t now) {
        if (active.handle) { active.frame=frame; active.milliseconds=now; retired.push_back(std::move(active)); active={}; }
        retry=true; retry_after=0;
    }
    bool recover(VkResult result,uint64_t frame,uint64_t now) {
        if (result==VK_ERROR_SURFACE_LOST_KHR) { surface_lost=true; retire(frame,now); return true; }
        if (result==VK_ERROR_OUT_OF_DATE_KHR) { retire(frame,now); return true; }
        return false;
    }
public:
    VkSurfaceKHR surface=VK_NULL_HANDLE; // The backend owns the surface lifetime.
    VkExtent2D extent={};
    VkFormat format=VK_FORMAT_UNDEFINED;
    bool surface_lost=false;
    Swapchain(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,bool maintenance,WsiApi api=WsiApi::current())
        :gpu(gpu),device(device),queue(queue),api(api),maintenance(maintenance) {}
    Swapchain(const Swapchain &)=delete;
    Swapchain &operator=(const Swapchain &)=delete;
    ~Swapchain() { shutdown(); }
    void collect(uint64_t frame,uint64_t now) {
        for (auto it=retired.begin();it!=retired.end();) {
            bool ready=true;
            if (maintenance) {
                for (auto fence:it->presents) if (api.fence_status(device,fence)!=VK_SUCCESS) ready=false;
                if (!it->uncertain.empty()) ready = ready && frame>=it->frame+6 && now>=it->milliseconds+250;
            } else ready=frame>=it->frame+6 && now>=it->milliseconds+250;
            if (ready) { release(*it); it=retired.erase(it); } else ++it;
        }
    }
    bool needs_present_grace() const noexcept { return presented && !maintenance; }
    void shutdown() noexcept {
        // Called after the backend drains device work. Present fences also
        // cover presentation-engine semaphore use, which DeviceWaitIdle does not.
        if (maintenance) {
            for (auto *chain: {&active}) if (!chain->presents.empty())
                api.wait_fences(device,uint32_t(chain->presents.size()),chain->presents.data(),VK_TRUE,UINT64_MAX);
            for (auto &chain:retired) if (!chain.presents.empty())
                api.wait_fences(device,uint32_t(chain.presents.size()),chain.presents.data(),VK_TRUE,UINT64_MAX);
        }
        release(active); for (auto &chain:retired) release(chain); retired.clear();
    }
    bool prepare(VkExtent2D size,uint64_t frame,uint64_t now) {
        collect(frame,now);
        if (!size.width || !size.height || !surface || surface_lost) return false;
        if (active.handle && (!retry || now<retry_after) && size.width==requested.width && size.height==requested.height) return true;
        VkSurfaceCapabilitiesKHR caps={};
        VkResult result=api.capabilities(gpu,surface,&caps);
        if (recover(result,frame,now)) return false;
        require(result,"Vulkan surface capabilities");
        VkExtent2D next=caps.currentExtent;
        if (next.width==UINT32_MAX) next={std::min(std::max(size.width,caps.minImageExtent.width),caps.maxImageExtent.width),
            std::min(std::max(size.height,caps.minImageExtent.height),caps.maxImageExtent.height)};
        if (!next.width || !next.height) return false;
        unsigned count=0;
        result=api.formats(gpu,surface,&count,nullptr);
        if (recover(result,frame,now)) return false;
        require(result,"Vulkan surface formats");
        std::vector<VkSurfaceFormatKHR> formats(count);
        result=api.formats(gpu,surface,&count,formats.data());
        if (recover(result,frame,now)) return false;
        require(result,"Vulkan surface formats");
        if (!count || !(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT)) throw std::runtime_error("Surface cannot receive GPU scanout");
        auto format=formats.front();
        if (format.format==VK_FORMAT_UNDEFINED) format.format=VK_FORMAT_B8G8R8A8_UNORM;
        for (auto candidate:formats) if (candidate.format==VK_FORMAT_B8G8R8A8_UNORM) format=candidate;
        VkSwapchainCreateInfoKHR ci={}; ci.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface=surface; ci.minImageCount=caps.minImageCount+1;
        if (caps.maxImageCount) ci.minImageCount=std::min(ci.minImageCount,caps.maxImageCount);
        ci.imageFormat=format.format; ci.imageColorSpace=format.colorSpace; ci.imageExtent=next; ci.imageArrayLayers=1;
        ci.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform=caps.currentTransform;
        ci.compositeAlpha=VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha&(~caps.supportedCompositeAlpha+1));
        ci.presentMode=VK_PRESENT_MODE_FIFO_KHR; ci.clipped=VK_TRUE; ci.oldSwapchain=active.handle;
        Chain chain;
        result=api.create(device,&ci,nullptr,&chain.handle);
        // Even a failed create can retire oldSwapchain. Never acquire it again.
        if (recover(result,frame,now)) return false;
        require(result,"Create Vulkan swapchain");
        retire(frame,now);
        active=std::move(chain);
        result=api.images(device,active.handle,&count,nullptr);
        if (recover(result,frame,now)) return false;
        require(result,"Swapchain images"); active.images.resize(count);
        result=api.images(device,active.handle,&count,active.images.data());
        if (recover(result,frame,now)) return false;
        require(result,"Swapchain images");
        VkSemaphoreCreateInfo sem={}; sem.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (unsigned i=0;i<count;++i) {
            VkSemaphore handle=VK_NULL_HANDLE;
            require(api.create_semaphore(device,&sem,nullptr,&handle),"Present semaphore"); active.ready.push_back(handle);
        }
        requested=size; extent=next; this->format=ci.imageFormat; retry=false;
        return true;
    }
    bool acquire(VkSemaphore semaphore,uint32_t &index,uint64_t frame,uint64_t now) {
        VkResult result=api.acquire(device,active.handle,0,semaphore,VK_NULL_HANDLE,&index);
        if (result==VK_TIMEOUT || result==VK_NOT_READY || recover(result,frame,now)) return false;
        if (result==VK_SUBOPTIMAL_KHR) suboptimal(now); else require(result,"Acquire Vulkan swapchain image");
        return true;
    }
    VkImage image(unsigned index) const { return active.images.at(index); }
    VkSemaphore ready(unsigned index) const { return active.ready.at(index); }
    void present(unsigned index,uint64_t frame,uint64_t now) {
        VkPresentInfoKHR info={}; info.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        VkSemaphore semaphore=ready(index);
        info.waitSemaphoreCount=1; info.pWaitSemaphores=&semaphore;
        info.swapchainCount=1; info.pSwapchains=&active.handle; info.pImageIndices=&index;
        VkFence fence=VK_NULL_HANDLE;
        VkSwapchainPresentFenceInfoEXT fence_info={}; fence_info.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
        if (maintenance) {
            VkFenceCreateInfo fi={}; fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            require(api.create_fence(device,&fi,nullptr,&fence),"Present completion fence");
            fence_info.swapchainCount=1; fence_info.pFences=&fence; info.pNext=&fence_info;
        }
        VkResult result=api.present(queue,&info);
        presented=true;
        if (fence) {
            if (result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR) active.presents.push_back(fence);
            else active.uncertain.push_back(fence);
        }
        // Reclaim completed active-chain fences too; no CPU wait during play.
        for (auto it=active.presents.begin();it!=active.presents.end();) {
            if (api.fence_status(device,*it)==VK_SUCCESS) { api.destroy_fence(device,*it,nullptr); it=active.presents.erase(it); }
            else ++it;
        }
        if (recover(result,frame,now)) return;
        if (result==VK_SUBOPTIMAL_KHR) suboptimal(now); else require(result,"Queue Vulkan present");
    }
};
}
#endif
