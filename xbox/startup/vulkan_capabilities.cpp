#include <vulkan/vulkan.h>
#include <cstdio>
#include <vector>
#include <cstring>

int main() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Mirror's Edge runtime capability check";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    auto status = vkCreateInstance(&info, nullptr, &instance);
    if (status) { std::printf("{\"error\":\"vkCreateInstance\",\"status\":%d}\n", status); return 1; }
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) || !count) return 2;
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(instance, &count, devices.data())) return 2;
    for (auto physical : devices) {
        VkPhysicalDeviceProperties properties{};
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        vkGetPhysicalDeviceFeatures(physical, &features);
        uint32_t ext_count = 0;
        if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &ext_count, nullptr)) return 3;
        std::vector<VkExtensionProperties> extensions(ext_count);
        if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &ext_count, extensions.data())) return 3;
        bool interlock = false;
        for (const auto& extension : extensions)
            if (!std::strcmp(extension.extensionName, "VK_EXT_fragment_shader_interlock")) interlock = true;
        const VkFormat formats[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_R16G16_SNORM,
            VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16_SFLOAT,
            VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM};
        for (const auto format : formats) {
            VkFormatProperties support{};
            vkGetPhysicalDeviceFormatProperties(physical, format, &support);
            const auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                (format == VK_FORMAT_D24_UNORM_S8_UINT ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
            std::printf("{\"format\":%u,\"sampled_attachment_supported\":%s}\n", unsigned(format),
                (support.optimalTilingFeatures & required) == required ? "true" : "false");
        }
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
        uint32_t family = UINT32_MAX;
        for (unsigned i = 0; i < family_count; ++i)
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
        const bool required = features.independentBlend && features.fragmentStoresAndAtomics &&
            features.vertexPipelineStoresAndAtomics && features.geometryShader && features.fillModeNonSolid;
        VkResult create_status = VK_ERROR_FEATURE_NOT_PRESENT;
        if (required && family != UINT32_MAX) {
            float priority = 1;
            VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = family; queue.queueCount = 1; queue.pQueuePriorities = &priority;
            VkPhysicalDeviceFeatures enabled{};
            enabled.independentBlend = enabled.fragmentStoresAndAtomics = enabled.vertexPipelineStoresAndAtomics =
                enabled.geometryShader = enabled.fillModeNonSolid = VK_TRUE;
            VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            device_info.queueCreateInfoCount = 1; device_info.pQueueCreateInfos = &queue; device_info.pEnabledFeatures = &enabled;
            VkDevice device = VK_NULL_HANDLE;
            create_status = vkCreateDevice(physical, &device_info, nullptr, &device);
            if (!create_status) vkDestroyDevice(device, nullptr);
        }
        std::printf("{\"gpu\":\"%s\",\"api\":\"%u.%u.%u\",\"vendor\":%u,\"independentBlend\":%u,\"fragmentStoresAndAtomics\":%u,\"vertexPipelineStoresAndAtomics\":%u,\"geometryShader\":%u,\"fillModeNonSolid\":%u,\"tessellationShader\":%u,\"fragment_interlock_extension\":%s,\"baseline_device_create_status\":%d,\"xbox_gpu_initialized\":false}\n",
            properties.deviceName, VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion),
            properties.vendorID, features.independentBlend, features.fragmentStoresAndAtomics,
            features.vertexPipelineStoresAndAtomics, features.geometryShader, features.fillModeNonSolid,
            features.tessellationShader, interlock ? "true" : "false", create_status);
    }
    vkDestroyInstance(instance, nullptr);
}
