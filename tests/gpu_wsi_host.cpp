// Driver-independent contracts for WSI recovery and present-resource retirement.
#include "vulkan_wsi.hpp"
#include "vulkan/device_selection.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
using namespace HostGPU;
namespace {
unsigned checks=0,creates=0,destroys=0,sem_destroys=0,fence_destroys=0,created_ok=0,sem_created=0,fence_created=0;
uintptr_t next_handle=100;
VkResult caps_result=VK_SUCCESS,create_result=VK_SUCCESS,acquire_result=VK_SUCCESS,present_result=VK_SUCCESS;
VkExtent2D current={UINT32_MAX,UINT32_MAX};
bool fences_ready=false;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(#x); } while (0)
template<class T> T handle() { return reinterpret_cast<T>(++next_handle); }
VKAPI_ATTR VkResult VKAPI_CALL caps(VkPhysicalDevice,VkSurfaceKHR,VkSurfaceCapabilitiesKHR *c) {
    *c={}; c->currentExtent=current; c->minImageExtent={64,64}; c->maxImageExtent={2048,2048};
    c->minImageCount=2; c->maxImageCount=3; c->supportedUsageFlags=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    c->supportedCompositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    return caps_result;
}
VKAPI_ATTR VkResult VKAPI_CALL formats(VkPhysicalDevice,VkSurfaceKHR,uint32_t *n,VkSurfaceFormatKHR *f) {
    *n=1; if (f) *f={VK_FORMAT_B8G8R8A8_UNORM,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice,const VkSwapchainCreateInfoKHR *i,const VkAllocationCallbacks *,VkSwapchainKHR *out) {
    ++creates; CHECK(i->imageExtent.width && i->imageExtent.height);
    if (create_result==VK_SUCCESS) { *out=handle<VkSwapchainKHR>(); ++created_ok; }
    return create_result;
}
VKAPI_ATTR void VKAPI_CALL destroy(VkDevice,VkSwapchainKHR,const VkAllocationCallbacks *) { ++destroys; }
VKAPI_ATTR VkResult VKAPI_CALL images(VkDevice,VkSwapchainKHR,uint32_t *n,VkImage *out) {
    *n=3; if (out) for (unsigned i=0;i<3;++i) out[i]=handle<VkImage>(); return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL acquire(VkDevice,VkSwapchainKHR,uint64_t timeout,VkSemaphore,VkFence,uint32_t *i) {
    CHECK(timeout==0); *i=0; return acquire_result;
}
VKAPI_ATTR VkResult VKAPI_CALL present(VkQueue,const VkPresentInfoKHR *i) {
    CHECK(i->waitSemaphoreCount==1 && i->swapchainCount==1);
    return present_result;
}
VKAPI_ATTR VkResult VKAPI_CALL semaphore(VkDevice,const VkSemaphoreCreateInfo *,const VkAllocationCallbacks *,VkSemaphore *s) { *s=handle<VkSemaphore>(); ++sem_created; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL destroy_sem(VkDevice,VkSemaphore,const VkAllocationCallbacks *) { ++sem_destroys; }
VKAPI_ATTR VkResult VKAPI_CALL fence(VkDevice,const VkFenceCreateInfo *,const VkAllocationCallbacks *,VkFence *f) { *f=handle<VkFence>(); ++fence_created; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL destroy_fence(VkDevice,VkFence,const VkAllocationCallbacks *) { ++fence_destroys; }
VKAPI_ATTR VkResult VKAPI_CALL fence_status(VkDevice,VkFence) { return fences_ready?VK_SUCCESS:VK_NOT_READY; }
VKAPI_ATTR VkResult VKAPI_CALL wait_fences(VkDevice,uint32_t,const VkFence *,VkBool32,uint64_t) { return VK_SUCCESS; }
WsiApi api={caps,formats,create,destroy,images,acquire,present,semaphore,destroy_sem,fence,destroy_fence,fence_status,wait_fences};
}
int main() try {
    auto gpu=handle<VkPhysicalDevice>(); auto device=handle<VkDevice>(); auto queue=handle<VkQueue>();
    {
        Swapchain w(gpu,device,queue,false,api); w.surface=handle<VkSurfaceKHR>();
        CHECK(!w.prepare({0,800},0,0)); CHECK(creates==0);
        current={0,0}; CHECK(!w.prepare({800,600},0,0)); CHECK(creates==0);
        current={UINT32_MAX,UINT32_MAX}; CHECK(w.prepare({12,12},0,0));
        CHECK(w.extent.width==64 && w.extent.height==64);
        unsigned count=creates;
        for (unsigned i=0;i<12;++i) CHECK(w.prepare({12,12},i,10*i));
        CHECK(creates==count); // requested size differs from chosen extent: no recreate loop.
        uint32_t index=0;
        acquire_result=VK_TIMEOUT; CHECK(!w.acquire(handle<VkSemaphore>(),index,12,120));
        acquire_result=VK_SUBOPTIMAL_KHR; CHECK(w.acquire(handle<VkSemaphore>(),index,12,120));
        w.present(index,12,120); CHECK(w.prepare({12,12},13,130)); CHECK(creates==count);
        CHECK(w.prepare({12,12},14,400)); CHECK(creates==count+1);
        CHECK(sem_destroys==0); w.collect(30,500); CHECK(sem_destroys==0);
        w.collect(40,800); CHECK(sem_destroys==3);
        create_result=VK_ERROR_OUT_OF_DATE_KHR;
        CHECK(!w.prepare({900,700},41,810)); CHECK(!w.prepare({900,700},42,820));
        create_result=VK_SUCCESS; CHECK(w.prepare({900,700},43,830));
        caps_result=VK_ERROR_SURFACE_LOST_KHR;
        CHECK(!w.prepare({901,700},44,840)); CHECK(w.surface_lost);
        caps_result=VK_SUCCESS; w.surface=handle<VkSurfaceKHR>(); w.surface_lost=false;
        CHECK(w.prepare({901,700},45,850));
        acquire_result=VK_ERROR_OUT_OF_DATE_KHR;
        CHECK(!w.acquire(handle<VkSemaphore>(),index,46,860));
        acquire_result=VK_SUCCESS; CHECK(w.prepare({901,700},47,870));
        for (auto result:{VK_SUBOPTIMAL_KHR,VK_ERROR_OUT_OF_DATE_KHR,VK_ERROR_SURFACE_LOST_KHR}) {
            present_result=result;
            CHECK(w.acquire(handle<VkSemaphore>(),index,48,880)); w.present(index,48,880);
            w.surface_lost=false; CHECK(w.prepare({901,700},49,890));
        }
        present_result=VK_ERROR_DEVICE_LOST;
        bool lost=false;
        try { w.present(0,50,900); } catch (const std::exception &) { lost=true; }
        CHECK(lost); // the C boundary converts this fatal Vulkan condition to soft fallback.
        present_result=VK_SUCCESS;
    }
    {
        unsigned before=sem_destroys;
        Swapchain w(gpu,device,queue,true,api); w.surface=handle<VkSurfaceKHR>();
        CHECK(w.prepare({800,600},0,0));
        w.present(0,0,0);
        CHECK(w.prepare({900,600},1,20));
        w.collect(100,5000); CHECK(sem_destroys==before); // no premature destroy with a pending present fence.
        fences_ready=true; w.collect(101,5010); CHECK(sem_destroys==before+3);
    }
    using Vulkan::choose_host_device;
    std::vector<VkPhysicalDeviceType> types={VK_PHYSICAL_DEVICE_TYPE_CPU,VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU};
    CHECK(choose_host_device(types,{true,true,true},nullptr)==2);
    CHECK(choose_host_device(types,{true,true,false},nullptr)==1);
    CHECK(choose_host_device(types,{true,false,false},nullptr)==0);
    CHECK(choose_host_device(types,{true,true,true},"0")==0);
    CHECK(choose_host_device(types,{true,true,false},"2")==-1);
    for (auto index:{"-1","3","oops","0garbage",""}) CHECK(choose_host_device(types,{true,true,true},index)==-1);
    CHECK(destroys==created_ok); CHECK(sem_destroys==sem_created); CHECK(fence_destroys==fence_created);
    std::printf("{\"wsi\":\"pass\",\"checks\":%u,\"swapchains\":%u,\"retired\":%u,\"fences_retired\":%u}\n",checks,creates,destroys,fence_destroys);
    return 0;
} catch (const std::exception &e) { std::fprintf(stderr,"WSI contract: %s\n",e.what()); return 1; }
