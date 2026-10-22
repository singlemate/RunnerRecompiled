"""Opt-in single-shader color-path isolation. Never a rendering fix."""
from pathlib import Path
import subprocess,sys
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path.cwd()
source=root/'work/references/rexglue-sdk/src/graphics/pipeline/shader/spirv_translator.cpp'
s=source.read_text()
if 'ME_DIAGNOSTIC_PS_INPUT6' not in s:
 s=s.replace('#include <cstddef>','#include <cstddef>\n#include <cstdlib>',1)
 start=s.index('void SpirvShaderTranslator::StoreResult(')
 anchor='  builder_->createStore(value_to_store, target_pointer);'
 pos=s.index(anchor,start)
 block='''  // Diagnostic: expose incoming interpolator 6 for one background PS only.
  // Changes shader output deliberately; disabled by default, not a fix.
  if(is_pixel_shader() && current_shader().ucode_data_hash()==0x5EBE814012217F48ull &&
     result.storage_target==InstructionStorageTarget::kColor && result.storage_index==0 &&
     std::getenv("ME_DIAGNOSTIC_PS_INPUT6") && input_output_interpolators_[6]!=spv::NoResult){
    const auto incoming=builder_->createLoad(input_output_interpolators_[6],spv::NoPrecision);
    std::vector<spv::Id> channels;
    for(unsigned c=0;c<3;++c)channels.push_back(std::getenv("ME_DIAGNOSTIC_PS_INPUT6")[0]=='2' ? builder_->makeFloatConstant(c==1?0.0f:0.25f) : builder_->createCompositeExtract(incoming,type_float_,c));
    channels.push_back(builder_->makeFloatConstant(1.0f));
    value_to_store=builder_->createCompositeConstruct(type_float4_,channels);
  }
'''
 s=s[:pos]+block+s[pos:]
 source.write_text(s)
s=source.read_text()
if 'ME_DIAGNOSTIC_DEPTH_COLOR_SAMPLE' not in s:
 start=s.index('void SpirvShaderTranslator::StoreResult(')
 anchor='  builder_->createStore(value_to_store, target_pointer);'
 pos=s.index(anchor,start)
 block='''  // Diagnostic only: bypass depth-weighted RGB with an existing HDR sample.
  // r6.xyz is last written by tfetch1 offset(0,3); retain original output alpha.
  if(is_pixel_shader() && current_shader().ucode_data_hash()==0x8F5851C1A202047Bull &&
     result.storage_target==InstructionStorageTarget::kColor && result.storage_index==0 &&
     std::getenv("ME_DIAGNOSTIC_DEPTH_COLOR_SAMPLE")){
    std::vector<spv::Id> index{builder_->makeIntConstant(6)};
    const auto reg6=builder_->createLoad(builder_->createAccessChain(spv::StorageClassFunction,var_main_registers_,index),spv::NoPrecision);
    std::vector<spv::Id> channels;
    for(unsigned c=0;c<3;++c)channels.push_back(builder_->createCompositeExtract(reg6,type_float_,c));
    channels.push_back(builder_->createCompositeExtract(value_to_store,type_float_,3));
    value_to_store=builder_->createCompositeConstruct(type_float4_,channels);
  }
'''
 s=s[:pos]+block+s[pos:];source.write_text(s)
scope={};compiler=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(compiler[:compiler.index('def build')],scope)
result=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(source),'-o',str(root/'work/gpu-port/objects/pipeline_shader_spirv_translator.cpp.o')],capture_output=True)
if result.returncode:print(result.stderr.decode(errors='replace')[-4000:])
raise SystemExit(result.returncode)
