#ifndef FF8_VULKAN_WSI_HPP
#define FF8_VULKAN_WSI_HPP
#include "volk/volk.h"
#include <algorithm>
#include <vector>
#include <stdexcept>
#include <string>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <cstdio>

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
    // The watcher (watch()): the swapchain is externally synchronized, so it
    // and the game thread take turns.
    std::mutex lock;
    std::thread watcher;
    std::atomic<bool> stopping{false};
    std::deque<uint64_t> watching;
    std::deque<std::pair<uint64_t,double>> sightings;
    double (*now_seconds)()=nullptr;
    uint64_t retry_after=0;
    // Reports of past presents the swapchain keeps, and the host after it.
    static constexpr unsigned timing_queue=64;
    struct {
        bool on=false, absolute=false;
        VkTimeDomainKHR domain=VK_TIME_DOMAIN_DEVICE_KHR;
        uint64_t domain_id=0, refresh=0, counter=0;
        // A time domain of the swapchain's own: its times are the host clock's
        // (domain) plus offset, calibrated now and then (calibrate()).
        bool local=false;
        VkTimeDomainKHR local_domain=VK_TIME_DOMAIN_DEVICE_KHR;
        int64_t offset=0;
        unsigned since_calibrated=0;
        // Presents since their reports were last taken up (gather()).
        unsigned unread=0;
        uint32_t stage=0;
        std::deque<std::pair<uint64_t,uint64_t>> results;
    } timing;
    void refresh_properties() {
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
        if (!timing.on) return;
        VkSwapchainTimingPropertiesEXT properties={}; properties.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT;
        uint64_t counter=0;
        VkResult result=timing_api.properties(device,active.handle,&properties,&counter);
        if (result==VK_SUCCESS) { timing.refresh=properties.refreshDuration; timing.counter=counter; }
#endif
    }
    // The offset between the swapchain's own time domain and the host clock.
    bool calibrate() {
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
        if (!timing.local) return true;
        if (!timing_api.calibrated) return false;
        VkSwapchainCalibratedTimestampInfoEXT swapchain={}; swapchain.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT;
        swapchain.swapchain=active.handle; swapchain.presentStage=timing.stage; swapchain.timeDomainId=timing.domain_id;
        VkCalibratedTimestampInfoKHR infos[2]={};
        infos[0].sType=infos[1].sType=VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
        infos[0].pNext=&swapchain; infos[0].timeDomain=timing.local_domain; infos[1].timeDomain=timing.domain;
        uint64_t stamps[2]={}, deviation=0;
        if (timing_api.calibrated(device,2,infos,stamps,&deviation)!=VK_SUCCESS) return false;
        timing.offset=int64_t(stamps[0])-int64_t(stamps[1]);
        timing.since_calibrated=0;
#endif
        return true;
    }
    // Takes up what the presentation engine reports of past presents.
    void gather() {
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
        if (!timing.on) return;
        timing.unread=0;
        if (++timing.since_calibrated>=64) calibrate();
        VkPastPresentationTimingInfoEXT info={}; info.sType=VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT;
        info.swapchain=active.handle;
        VkPresentStageTimeEXT stages[8][1]={};
        VkPastPresentationTimingEXT past[8]={};
        for (int round=0;round<4;++round) {
            for (unsigned i=0;i<8;++i) {
                past[i]={}; past[i].sType=VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT;
                past[i].presentStageCount=1; past[i].pPresentStages=stages[i];
            }
            VkPastPresentationTimingPropertiesEXT properties={}; properties.sType=VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT;
            properties.presentationTimingCount=8; properties.pPresentationTimings=past;
            VkResult result=timing_api.past(device,&info,&properties);
            if (result!=VK_SUCCESS && result!=VK_INCOMPLETE) return;
            for (unsigned i=0;i<properties.presentationTimingCount;++i)
                if (past[i].reportComplete && past[i].presentStageCount && past[i].pPresentStages[0].time)
                    timing.results.push_back({past[i].presentId,uint64_t(int64_t(past[i].pPresentStages[0].time)-timing.offset)});
            // A host that stopped asking (shown()) leaves the oldest behind.
            while (timing.results.size()>timing_queue) timing.results.pop_front();
            if (properties.timingPropertiesCounter!=timing.counter) refresh_properties();
            if (result!=VK_INCOMPLETE) return;
        }
#endif
    }
    // Sets the active swapchain up for present timing, where it was made with it.
    void time_chain(bool requested) {
        timing.on=false; timing.results.clear(); timing.unread=0;
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
        if (!requested) return;
        if (timing_api.queue_size(device,active.handle,timing_queue)!=VK_SUCCESS) return;
        uint64_t counter=0;
        VkSwapchainTimeDomainPropertiesEXT domains={}; domains.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT;
        if (timing_api.domains(device,active.handle,&domains,&counter)!=VK_SUCCESS || !domains.timeDomainCount) return;
        std::vector<VkTimeDomainKHR> kinds(domains.timeDomainCount);
        std::vector<uint64_t> ids(domains.timeDomainCount);
        domains.pTimeDomains=kinds.data(); domains.pTimeDomainIds=ids.data();
        if (timing_api.domains(device,active.handle,&domains,&counter)!=VK_SUCCESS) return;
        // Times on a host clock in nanoseconds: one the swapchain offers, or its
        // own, calibrated against one.
        bool found=false;
        timing.local=false; timing.offset=0;
        for (VkTimeDomainKHR wanted:{VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR,VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR})
            for (uint32_t i=0;i<domains.timeDomainCount && !found;++i)
                if (kinds[i]==wanted) { timing.domain=kinds[i]; timing.domain_id=ids[i]; found=true; }
        for (uint32_t i=0;i<domains.timeDomainCount && !found;++i)
            if (kinds[i]==VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT || kinds[i]==VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT) {
                timing.local=true; timing.local_domain=kinds[i]; timing.domain_id=ids[i];
                for (VkTimeDomainKHR host:{VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR,VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR}) {
                    timing.domain=host;
                    if (calibrate()) { found=true; break; }
                }
            }
        if (!found) return;
        timing.on=true;
        refresh_properties();
#else
        (void)requested;
#endif
    }
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
        watching.clear();
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
    // The host paces its own presents (pace()); the device can show the latest
    // ready picture at each refresh (VK_EXT_present_mode_fifo_latest_ready).
    bool paced=false, latest_ready=false;
    VkPresentModeKHR mode=VK_PRESENT_MODE_FIFO_KHR;
    VkPresentModeKHR announced=VK_PRESENT_MODE_FIFO_KHR;
    bool announced_timing=false;
    void pace(bool on) { std::lock_guard<std::mutex> hold(lock); if (on!=paced) { paced=on; retry=true; retry_after=0; } }
    // Present ids (VK_KHR_present_wait), when the device has them: the id of
    // the latest present, 0 before any. watch() starts a thread that sees each
    // present reach the display, as soon as it does, whatever the game thread
    // is busy with; seen() hands those sightings over, clock() seconds.
    PFN_vkWaitForPresentKHR wait_present=nullptr;
    uint64_t last_id=0, next_id=0;
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
    // Present timing (VK_EXT_present_timing), where the device and the surface
    // have it: when each picture was shown, as the presentation engine reports
    // it, the display's refresh period, and presents held until a given time.
    // The device's entry points are set by the host; timed() says whether the
    // active swapchain was made with them.
    struct {
        PFN_vkSetSwapchainPresentTimingQueueSizeEXT queue_size=nullptr;
        PFN_vkGetSwapchainTimingPropertiesEXT properties=nullptr;
        PFN_vkGetSwapchainTimeDomainPropertiesEXT domains=nullptr;
        PFN_vkGetPastPresentationTimingEXT past=nullptr;
        PFN_vkGetCalibratedTimestampsKHR calibrated=nullptr;
        bool absolute=false;
    } timing_api;
#endif
    bool timed() const noexcept { return timing.on; }
    // The time domain of shown() and of present()'s targets.
    VkTimeDomainKHR time_domain() const noexcept { return timing.domain; }
    // Presents can be held until a time (present()'s target).
    bool targets() const noexcept { return timing.on && timing.absolute; }
    // The display's refresh period in nanoseconds as the presentation engine
    // knows it, 0 when it does not.
    uint64_t refresh() {
        std::lock_guard<std::mutex> hold(lock);
        refresh_properties();
        return timing.refresh;
    }
    // The next present seen on the display since the last call: its id and
    // when its first pixel went out, in time_domain().
    bool shown(uint64_t &id,uint64_t &time) {
        std::lock_guard<std::mutex> hold(lock);
        gather();
        if (timing.results.empty()) return false;
        id=timing.results.front().first; time=timing.results.front().second; timing.results.pop_front();
        return true;
    }
    void watch(double (*clock)()) {
        if (!wait_present || watcher.joinable()) return;
        now_seconds=clock; stopping=false;
        watcher=std::thread([this] {
            while (!stopping) {
                bool found=false;
                {
                    std::lock_guard<std::mutex> hold(lock);
                    if (!watching.empty() && active.handle) {
                        VkResult result=wait_present(device,active.handle,watching.front(),0);
                        if (result==VK_SUCCESS) { sightings.push_back({watching.front(),now_seconds()}); found=true; }
                        if (result!=VK_TIMEOUT) watching.pop_front();
                    }
                }
                if (!found) std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
    }
    bool seen(uint64_t &id,double &when) {
        std::lock_guard<std::mutex> hold(lock);
        if (sightings.empty()) return false;
        id=sightings.front().first; when=sightings.front().second; sightings.pop_front();
        return true;
    }
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
        if (watcher.joinable()) { stopping=true; watcher.join(); }
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
        std::lock_guard<std::mutex> hold(lock);
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
#ifdef VK_EXT_present_mode_fifo_latest_ready
        // A host that paces its presents itself wants the latest picture ready at
        // each refresh, without presents blocking, where the surface offers it.
        if (paced && latest_ready) {
            uint32_t modes=0;
            if (vkGetPhysicalDeviceSurfacePresentModesKHR(gpu,surface,&modes,nullptr)==VK_SUCCESS && modes) {
                std::vector<VkPresentModeKHR> list(modes);
                if (vkGetPhysicalDeviceSurfacePresentModesKHR(gpu,surface,&modes,list.data())==VK_SUCCESS &&
                    std::find(list.begin(),list.end(),VK_PRESENT_MODE_FIFO_LATEST_READY_EXT)!=list.end())
                    ci.presentMode=VK_PRESENT_MODE_FIFO_LATEST_READY_EXT;
            }
        }
#endif
        // Present timing where the surface has it, for a host that paces its presents.
        bool timed_chain=false;
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
        if (paced && timing_api.past && vkGetPhysicalDeviceSurfaceCapabilities2KHR) {
            VkPhysicalDeviceSurfaceInfo2KHR surface_info={}; surface_info.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
            surface_info.surface=surface;
            VkPresentTimingSurfaceCapabilitiesEXT timing_caps={}; timing_caps.sType=VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT;
            VkSurfaceCapabilitiesPresentId2KHR id_caps={}; id_caps.sType=VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR;
            id_caps.pNext=&timing_caps;
            VkSurfaceCapabilities2KHR caps2={}; caps2.sType=VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR; caps2.pNext=&id_caps;
            if (vkGetPhysicalDeviceSurfaceCapabilities2KHR(gpu,&surface_info,&caps2)==VK_SUCCESS && id_caps.presentId2Supported &&
                timing_caps.presentTimingSupported) {
                uint32_t stage=(timing_caps.presentStageQueries&VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT) ? uint32_t(VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT) :
                    (timing_caps.presentStageQueries&VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT) ? uint32_t(VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT) : 0u;
                if (stage) {
                    timed_chain=true; timing.stage=stage;
                    timing.absolute=timing_api.absolute && timing_caps.presentAtAbsoluteTimeSupported;
                    ci.flags|=VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT|VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
                }
            }
        }
#endif
        // Pictures held until their refresh go to the compositor as they are drawn,
        // each held there until its time; FIFO would also have each wait for the
        // one before to be shown, which some compositors only do when something
        // else on the output changes. With room for the pictures held.
        if (timed_chain && timing.absolute) {
            uint32_t modes=0;
            if (vkGetPhysicalDeviceSurfacePresentModesKHR(gpu,surface,&modes,nullptr)==VK_SUCCESS && modes) {
                std::vector<VkPresentModeKHR> list(modes);
                if (vkGetPhysicalDeviceSurfacePresentModesKHR(gpu,surface,&modes,list.data())==VK_SUCCESS &&
                    std::find(list.begin(),list.end(),VK_PRESENT_MODE_MAILBOX_KHR)!=list.end()) {
                    ci.presentMode=VK_PRESENT_MODE_MAILBOX_KHR;
                    ci.minImageCount=caps.minImageCount+5;
                    if (caps.maxImageCount) ci.minImageCount=std::min(ci.minImageCount,caps.maxImageCount);
                }
            }
        }
        mode=ci.presentMode;
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
        time_chain(timed_chain);
        requested=size; extent=next; this->format=ci.imageFormat; retry=false;
        if (mode!=announced || timing.on!=announced_timing) {
            announced=mode; announced_timing=timing.on;
            std::fprintf(stderr,"Vulkan: %s swapchain%s\n",mode==VK_PRESENT_MODE_FIFO_KHR ? "FIFO" : mode==VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "latest-ready FIFO",
                timing.on ? (timing.absolute ? ", present timing with target times" : ", present timing") : "");
        }
        return true;
    }
    bool acquire(VkSemaphore semaphore,uint32_t &index,uint64_t frame,uint64_t now) {
        std::lock_guard<std::mutex> hold(lock);
        VkResult result=api.acquire(device,active.handle,0,semaphore,VK_NULL_HANDLE,&index);
        if (result==VK_TIMEOUT || result==VK_NOT_READY || recover(result,frame,now)) return false;
        if (result==VK_SUBOPTIMAL_KHR) suboptimal(now); else require(result,"Acquire Vulkan swapchain image");
        return true;
    }
    VkImage image(unsigned index) const { return active.images.at(index); }
    VkSemaphore ready(unsigned index) const { return active.ready.at(index); }
    // target: with present timing, the time (in time_domain()) before which the
    // picture is not shown; 0 for none.
    void present(unsigned index,uint64_t frame,uint64_t now,uint64_t target=0) {
        std::lock_guard<std::mutex> hold(lock);
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
        // Each present's id, for a host that asks when it reached the display.
        VkPresentIdKHR id_info={}; id_info.sType=VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
        if (wait_present && !timing.on) {
            last_id=++next_id;
            id_info.swapchainCount=1; id_info.pPresentIds=&last_id;
            id_info.pNext=info.pNext; info.pNext=&id_info;
        }
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
        VkPresentId2KHR id2_info={}; id2_info.sType=VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR;
        VkPresentTimingInfoEXT timing_info={}; timing_info.sType=VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT;
        VkPresentTimingsInfoEXT timings_info={}; timings_info.sType=VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT;
        if (timing.on) {
            // The swapchain keeps a report for each present until it is taken
            // up, and refuses presents once its queue is full: where the host
            // is not asking for them (a menu drawn by another loop), they are
            // taken up here.
            if (++timing.unread>=timing_queue/4) gather();
            last_id=++next_id;
            id2_info.swapchainCount=1; id2_info.pPresentIds=&last_id;
            id2_info.pNext=info.pNext; info.pNext=&id2_info;
            timing_info.presentStageQueries=timing.stage;
            if (target && timing.absolute) { timing_info.targetTime=uint64_t(int64_t(target)+timing.offset); timing_info.timeDomainId=timing.domain_id; }
            timings_info.swapchainCount=1; timings_info.pTimingInfos=&timing_info;
            timings_info.pNext=info.pNext; info.pNext=&timings_info;
        }
#else
        (void)target;
#endif
        VkResult result=api.present(queue,&info);
        presented=true;
        if (wait_present && !timing.on && watcher.joinable() && (result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR)) watching.push_back(last_id);
        if (fence) {
            if (result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR) active.presents.push_back(fence);
            else active.uncertain.push_back(fence);
        }
        // Reclaim completed active-chain fences too; no CPU wait during play. A
        // host that paces its presents asks only after the oldest, a few
        // presents back, which have usually completed: asking after a present
        // fence can take most of a millisecond on some drivers.
        if (paced) {
            while (active.presents.size()>6 && api.fence_status(device,active.presents.front())==VK_SUCCESS) {
                api.destroy_fence(device,active.presents.front(),nullptr);
                active.presents.erase(active.presents.begin());
            }
        } else for (auto it=active.presents.begin();it!=active.presents.end();) {
            if (api.fence_status(device,*it)==VK_SUCCESS) { api.destroy_fence(device,*it,nullptr); it=active.presents.erase(it); }
            else ++it;
        }
        if (recover(result,frame,now)) return;
        if (result==VK_SUBOPTIMAL_KHR) suboptimal(now); else require(result,"Queue Vulkan present");
    }
};
}
#endif
