#define VK_ENABLE_BETA_EXTENSIONS
#include <vulkan/vulkan.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>

#include <cstdio>
#include <cstring>
int main() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;
    VkInstance instance{}; if(vkCreateInstance(&ci,nullptr,&instance))return 1;
    uint32_t count=1;VkPhysicalDevice physical{};if(vkEnumeratePhysicalDevices(instance,&count,&physical)||!count)return 2;
    uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);
    std::vector<VkQueueFamilyProperties> families(n);vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,families.data());
    uint32_t family=UINT32_MAX;for(uint32_t i=0;i<n;i++)if(families[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){family=i;break;}
    if(family==UINT32_MAX)return 3;
    float priority=1;VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queue.queueFamilyIndex=family;queue.queueCount=1;queue.pQueuePriorities=&priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&queue;
    VkDevice device{};if(vkCreateDevice(physical,&di,nullptr,&device))return 4;
    // Own synthetic Xenos CF pair: EXEC_END with zero ALU/fetch instructions.
    // It checks the translation path, not Mirror's Edge shader compatibility.
    const uint32_t ucode[]={0,0x2000,0};
    for(unsigned stage=0;stage<2;stage++) {
        rex::graphics::SpirvShaderTranslator::Features features(false);
        rex::graphics::SpirvShaderTranslator translator(features,false,false,false);
        rex::graphics::SpirvShader shader(stage?rex::graphics::xenos::ShaderType::kPixel:rex::graphics::xenos::ShaderType::kVertex,0,ucode,3,std::endian::native);
        rex::string::StringBuffer disassembly;shader.AnalyzeUcode(disassembly);
        auto modification=stage?translator.GetDefaultPixelShaderModification(0):translator.GetDefaultVertexShaderModification(0);
        auto* translation=shader.GetOrCreateTranslation(modification);
        if(!translator.TranslateAnalyzedShader(*translation)||!translation->is_valid())return 5;
        const auto& binary=translation->translated_binary();
        if(binary.size()<20 || binary.size()%4)return 6;
        std::vector<uint32_t> words(binary.size()/4);std::memcpy(words.data(),binary.data(),binary.size());
        if(words[0]!=0x07230203)return 7;
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};si.codeSize=binary.size();si.pCode=words.data();
        VkShaderModule module{};auto status=vkCreateShaderModule(device,&si,nullptr,&module);
        std::printf("{\"event\":\"synthetic_xenos_shader\",\"stage\":\"%s\",\"spirv_bytes\":%zu,\"shader_module_status\":%d,\"game_shader\":false}\n",stage?"pixel":"vertex",binary.size(),status);
        if(status)return 8;vkDestroyShaderModule(device,module,nullptr);
    }
    vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);
    return 0;
}
