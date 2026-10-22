"""Opt-in raw lightmap output, with ordinary depth state and write sentinel."""
from pathlib import Path
import subprocess
root=Path.cwd();sdk=root/'work/references/rexglue-sdk';inc=sdk/'include/rex/graphics/pipeline/shader/diagnostic_lightmaps.h'
inc.write_text('#pragma once\n#include <array>\n#include <cstdint>\n#include <cstdlib>\nnamespace rex::graphics { inline thread_local std::array<uint32_t,3> me_lightmap_values{}; }\n')
main=sdk/'src/graphics/pipeline/shader/spirv_translator.cpp';fetch=sdk/'src/graphics/pipeline/shader/spirv_translator_fetch.cpp';rb=sdk/'src/graphics/pipeline/shader/spirv_translator_rb.cpp'
for p in [main,fetch,rb]:
 s=p.read_text()
 if 'diagnostic_lightmaps.h' not in s:s='#include <rex/graphics/pipeline/shader/diagnostic_lightmaps.h>\n'+s
 if p==main and 'me_lightmap_values.fill' not in s:s=s.replace('void SpirvShaderTranslator::Reset() {','void SpirvShaderTranslator::Reset() {\n  me_lightmap_values.fill(0);')
 if p==fetch and 'me_lightmap_values[instr.operands[1].storage_index]' not in s:
  anchor='  StoreResult(instr.result, result_vector);'
  block='''  if(is_pixel_shader() && current_shader().ucode_data_hash()==0x89670FB6FAB9E7C6ull && std::getenv("ME_DIAGNOSTIC_MATERIAL_SAMPLES") && instr.operands[1].storage_index<3){
    std::vector<spv::Id> channels{result[0],result[1],result[2],builder_->makeFloatConstant(1.0f)};
    me_lightmap_values[instr.operands[1].storage_index]=builder_->createCompositeConstruct(type_float4_,channels);
  }
'''
  assert s.count(anchor)==1;s=s.replace(anchor,block+anchor)
 if p==rb and 'me_lightmap_values[slot]' not in s:
  anchor='        builder_->createStore(color, color_variable);'
  block='''        if(current_shader().ucode_data_hash()==0x89670FB6FAB9E7C6ull && color_target_index==0){
          if(const char* option=std::getenv("ME_DIAGNOSTIC_MATERIAL_SAMPLES")){
            const unsigned slot=unsigned(option[0]-'0');
            if(slot<3 && me_lightmap_values[slot])color=me_lightmap_values[slot];
          }
        }
'''
  assert s.count(anchor)==1;s=s.replace(anchor,block+anchor)
 p.write_text(s)
rtc=sdk/'src/graphics/vulkan/render_target_cache.cpp';s=rtc.read_text()
if 'ME_DIAGNOSTIC_MATERIAL_SAMPLES' not in s:
 a=s.index('void VulkanRenderTargetCache::DiagnosticHostAfterDraw');b=s.index('bool VulkanRenderTargetCache::DumpRenderTargets',a);block=s[a:b]
 block=block.replace('const int32_t push[]={128,468,640,180,','const bool lightmaps=std::getenv("ME_DIAGNOSTIC_MATERIAL_SAMPLES")!=nullptr;\n      const int32_t push[]={128,468,lightmaps?128:640,lightmaps?550:180,');s=s[:a]+block+s[b:];rtc.write_text(s)
cp=sdk/'src/graphics/vulkan/command_processor.cpp';s=cp.read_text()
if 'ME_DIAGNOSTIC_MATERIAL_SAMPLES' not in s:
 s=s.replace('if(std::getenv("ME_DIAGNOSTIC_PAIR_DEPTH") && draw_ps_hash==0x89670FB6FAB9E7C6ull){','if((std::getenv("ME_DIAGNOSTIC_PAIR_DEPTH") || std::getenv("ME_DIAGNOSTIC_MATERIAL_SAMPLES")) && draw_ps_hash==0x89670FB6FAB9E7C6ull){')
 s=s.replace('rects[1].rect={{640,180},{1,1}};','rects[1].rect=std::getenv("ME_DIAGNOSTIC_MATERIAL_SAMPLES") ? VkRect2D{{128,550},{1,1}} : VkRect2D{{640,180},{1,1}};')
 s=s.replace('if(paired && draw_ps_hash!=0x89670FB6FAB9E7C6ull)break;','if((paired || std::getenv("ME_DIAGNOSTIC_MATERIAL_SAMPLES")) && draw_ps_hash!=0x89670FB6FAB9E7C6ull)break;');cp.write_text(s)
s=rtc.read_text().replace('lightmaps?128:640,lightmaps?550:180','lightmaps?256:640,lightmaps?560:180');rtc.write_text(s)
s=cp.read_text().replace('VkRect2D{{128,550},{1,1}}','VkRect2D{{256,560},{1,1}}');cp.write_text(s)
scope={};s=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(s[:s.index('def build')],scope)
for p,name in [(main,'pipeline_shader_spirv_translator.cpp.o'),(fetch,'pipeline_shader_spirv_translator_fetch.cpp.o'),(rb,'pipeline_shader_spirv_translator_rb.cpp.o'),(rtc,'vulkan_render_target_cache.cpp.o'),(cp,'vulkan_command_processor.cpp.o')]:
 q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(p),'-o',str(root/'work/gpu-port/objects'/name)],capture_output=True)
 if q.returncode:print(q.stderr.decode(errors='replace')[-4000:]);raise SystemExit(q.returncode)
