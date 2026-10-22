"""Bounded unlit world-material ablation; not a shadow-only switch."""
from pathlib import Path
import subprocess
root=Path.cwd();p=root/'work/references/rexglue-sdk/src/graphics/pipeline/shader/spirv_translator_rb.cpp';s=p.read_text()
if '#include <cstdio>' not in s:s='#include <cstdio>\n'+s;p.write_text(s)
if 'ME_DIAGNOSTIC_UNLIT_WORLD' not in s:
 anchor='        builder_->createStore(color, color_variable);'
 block=r'''        if(std::getenv("ME_DIAGNOSTIC_UNLIT_WORLD") && color_target_index==0){
          const uint64_t hash=current_shader().ucode_data_hash();
          if(hash==0x89670FB6FAB9E7C6ull || hash==0x67175986A6155884ull || hash==0x268E406A96386146ull || hash==0x5EBE814012217F48ull){
            std::vector<spv::Id> channels{builder_->makeFloatConstant(8.0f),builder_->makeFloatConstant(8.0f),builder_->makeFloatConstant(8.0f),builder_->makeFloatConstant(1.0f)};
            color=builder_->createCompositeConstruct(type_float4_,channels);
            std::fprintf(stderr,"external_unlit_world_shader ps=%016llX not_shadow_only=true not_a_fix=true\n",(unsigned long long)hash);
          }
        }
'''
 assert s.count(anchor)==1;s=s.replace(anchor,block+anchor);p.write_text(s)
scope={};s=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(s[:s.index('def build')],scope)
q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(p),'-o',str(root/'work/gpu-port/objects/pipeline_shader_spirv_translator_rb.cpp.o')],capture_output=True)
if q.returncode:print(q.stderr.decode(errors='replace')[-4000:])
raise SystemExit(q.returncode)
