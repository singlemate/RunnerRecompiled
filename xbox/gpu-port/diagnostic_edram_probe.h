#pragma once
#include <cstdint>
#include <rex/ui/vulkan/api.h>
// Shared by the two SDK translation units; GPU thread only, opt-in diagnosis.
namespace rex::graphics::vulkan {
struct MeEdramProbeState {
 VkBuffer buffer=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE;
 uint32_t memory_type=UINT32_MAX, count=0, offsets[8]{}, color=0, msaa=0, word_count=0, select=0;
 uint64_t frame=0;
};
inline MeEdramProbeState me_edram_probe;
struct MeHostProbeState {
 VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;
 VkPipeline pipeline=VK_NULL_HANDLE;VkPipelineLayout layout=VK_NULL_HANDLE;
 uint32_t memory_type=UINT32_MAX,count=0,host_ids[2]{},depth=0;uint64_t frame=0,vs=0,ps=0;bool after_draw=false;
};
inline MeHostProbeState me_host_probe;
inline MeHostProbeState me_host_depth_probe;
}
