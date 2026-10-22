#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/device.h>
#include <cstdio>
int main() {
    auto instance = rex::ui::vulkan::VulkanInstance::Create(false, false);
    if (!instance) { std::puts("{\"event\":\"rex_vulkan_instance_failed\"}"); return 1; }
    std::vector<VkPhysicalDevice> physical;
    instance->EnumeratePhysicalDevices(physical);
    for (auto candidate : physical) {
        auto device = rex::ui::vulkan::VulkanDevice::CreateIfSupported(instance.get(), candidate, true, false);
        if (!device) continue;
        const auto& properties=device->properties();
        std::printf("{\"event\":\"rex_vulkan_device_created\",\"gpu\":\"%s\",\"api\":\"%u.%u.%u\",\"max_storage_buffer_range\":%u,\"presentation\":false,\"command_processor\":false}\n",properties.deviceName,VK_VERSION_MAJOR(properties.apiVersion),VK_VERSION_MINOR(properties.apiVersion),VK_VERSION_PATCH(properties.apiVersion),properties.maxStorageBufferRange);
        return 0;
    }
    std::puts("{\"event\":\"rex_vulkan_device_failed\"}"); return 2;
}
