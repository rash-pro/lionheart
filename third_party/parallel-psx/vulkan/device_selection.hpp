#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <cstdlib>
#include <cerrno>
namespace Vulkan {
inline int choose_host_device(const std::vector<VkPhysicalDeviceType> &types,
                              const std::vector<bool> &compatible, const char *override_index) {
    if (override_index) {
        char *end=nullptr; errno=0;
        unsigned long index=std::strtoul(override_index,&end,0);
        if (errno || end==override_index || *end || *override_index=='-' ||
            index>=types.size() || !compatible[index]) return -1;
        return int(index);
    }
    int best=-1, score=-1;
    for (unsigned i=0;i<types.size();++i) if (compatible[i]) {
        int candidate=types[i]==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 4 :
                      types[i]==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3 :
                      types[i]==VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 2 :
                      types[i]==VK_PHYSICAL_DEVICE_TYPE_CPU ? 1 : 0;
        if (candidate>score) { score=candidate; best=int(i); }
    }
    return best;
}
}
