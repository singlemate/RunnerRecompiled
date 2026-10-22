"""Encode fragment input depth bits as exact byte-valued RGBA; diagnostic only."""
from pathlib import Path
import subprocess
root=Path.cwd();sdk=root/'work/references/rexglue-sdk';main=sdk/'src/graphics/pipeline/shader/spirv_translator.cpp';rb=sdk/'src/graphics/pipeline/shader/spirv_translator_rb.cpp';pc=sdk/'src/graphics/vulkan/pipeline_cache.cpp'
condition='(std::getenv("ME_DIAGNOSTIC_INCOMING_DEPTH") && current_shader().ucode_data_hash()==0x89670FB6FAB9E7C6ull)'
s=main.read_text()
if 'ME_DIAGNOSTIC_INCOMING_DEPTH' not in s:
 s=s.replace('float24_depth_from_fragment_coordinates || alpha_to_coverage_possible)', 'float24_depth_from_fragment_coordinates || alpha_to_coverage_possible || '+condition+')')
 s=s.replace('IsSampleRate()', '(IsSampleRate() || '+condition+')')
 main.write_text(s)
s=rb.read_text()
if 'ME_DIAGNOSTIC_INCOMING_DEPTH' not in s:
 s='#include <cstdlib>\n'+s
 anchor='        builder_->createStore(color, color_variable);'
 block=r'''        if(current_shader().ucode_data_hash()==0x89670FB6FAB9E7C6ull && color_target_index==0 && std::getenv("ME_DIAGNOSTIC_INCOMING_DEPTH")){
          std::vector<spv::Id> idx{builder_->makeIntConstant(2)};
          const auto z=builder_->createLoad(builder_->createAccessChain(spv::StorageClassInput,input_fragment_coordinates_,idx),spv::NoPrecision);
          const auto bits=builder_->createUnaryOp(spv::OpBitcast,type_uint_,z);
          std::vector<spv::Id> bytes;
          for(unsigned i=0;i<4;++i){
            auto byte=builder_->createBinOp(spv::OpBitwiseAnd,type_uint_,builder_->createBinOp(spv::OpShiftRightLogical,type_uint_,bits,builder_->makeUintConstant(i*8)),builder_->makeUintConstant(255));
            bytes.push_back(builder_->createUnaryOp(spv::OpConvertUToF,type_float_,byte));
          }
          if(input_sample_id_!=spv::NoResult)builder_->createLoad(input_sample_id_,spv::NoPrecision);
          color=builder_->createCompositeConstruct(type_float4_,bytes);
        }
'''
 assert s.count(anchor)==1;s=s.replace(anchor,block+anchor);rb.write_text(s)
s=pc.read_text()
if 'ME_DIAGNOSTIC_INCOMING_DEPTH' not in s:
 anchor='  description_out.render_pass_key = render_pass_key;'
 block='''  if(pixel_shader && pixel_shader->shader().ucode_data_hash()==0x89670FB6FAB9E7C6ull && std::getenv("ME_DIAGNOSTIC_INCOMING_DEPTH"))description_out.sample_rate_shading=1;
'''
 assert s.count(anchor)==1;s=s.replace(anchor,block+anchor);pc.write_text(s)
cp=sdk/'src/graphics/vulkan/command_processor.cpp';s=cp.read_text()
if 'external_incoming_depth_compare_always' not in s:
 anchor='  uint32_t ps_param_gen_pos = UINT32_MAX;'
 block='''  if(pixel_shader && pixel_shader->ucode_data_hash()==0x89670FB6FAB9E7C6ull && std::getenv("ME_DIAGNOSTIC_INCOMING_DEPTH")){
    normalized_depth_control.z_enable=1;
    normalized_depth_control.z_write_enable=0;
    normalized_depth_control.zfunc=xenos::CompareFunction::kAlways;
    normalized_depth_control.stencil_enable=0;
    static unsigned incoming_count=0;
    if(++incoming_count<=4)REXGPU_INFO("external_incoming_depth_compare_always viewport_depth_enabled=true writes=false");
  }
'''
 assert s.count(anchor)==1;s=s.replace(anchor,block+anchor);cp.write_text(s)
scope={};s=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(s[:s.index('def build')],scope)
for p,name in [(main,'pipeline_shader_spirv_translator.cpp.o'),(rb,'pipeline_shader_spirv_translator_rb.cpp.o'),(pc,'vulkan_pipeline_cache.cpp.o'),(cp,'vulkan_command_processor.cpp.o')]:
 q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(p),'-o',str(root/'work/gpu-port/objects'/name)],capture_output=True)
 if q.returncode:print(q.stderr.decode(errors='replace')[-4000:]);raise SystemExit(q.returncode)
