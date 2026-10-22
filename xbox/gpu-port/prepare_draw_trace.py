"""Bounded draw observations; optional single-shader omission for diagnosis only."""
from pathlib import Path
import os,subprocess,sys
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path.cwd()
os.chdir(root)
source=root/'work/references/rexglue-sdk/src/graphics/vulkan/command_processor.cpp'
text=source.read_text()
if 'external_draw_state' not in text:
    text=text.replace('#include <array>','#include <array>\n#include <set>',1)
    anchor='  texture_cache_->RequestTextures(used_texture_mask);'
    assert text.count(anchor)==1
    trace='''
  const uint64_t draw_vs_hash=vertex_shader->ucode_data_hash();
  const uint64_t draw_ps_hash=pixel_shader ? pixel_shader->ucode_data_hash() : 0;
  const uint32_t draw_depth=regs.Get<reg::RB_DEPTHCONTROL>().value;
  const uint32_t draw_color=regs.Get<reg::RB_COLORCONTROL>().value;
  const uint32_t draw_blend=regs[reg::RB_BLENDCONTROL::rt_register_indices[0]];
  const uint32_t draw_target=regs.Get<reg::RB_COLOR_INFO>().value;
  static std::set<std::array<uint64_t,8>> external_draw_states;
  const std::array<uint64_t,8> draw_key={draw_vs_hash,draw_ps_hash,draw_depth,draw_color,draw_blend,draw_target,normalized_color_mask,used_texture_mask};
  if(external_draw_states.size()<512 && external_draw_states.insert(draw_key).second){
    const auto state=external_draw_states.size();
    REXGPU_INFO("external_draw_state id={} frame={} vs={:016X} ps={:016X} primitive={} indices={} depth={:08X} color={:08X} blend0={:08X} target0={:08X} mask={:08X} textures={:08X}",state,frame_current_,draw_vs_hash,draw_ps_hash,uint32_t(prim_type),index_count,draw_depth,draw_color,draw_blend,draw_target,normalized_color_mask,used_texture_mask);
    for(uint32_t slot=0;slot<32;++slot)if(used_texture_mask&(1u<<slot)){
      const auto fetch=regs.GetTextureFetch(slot);
      REXGPU_INFO("external_draw_fetch id={} slot={} raw={:08X},{:08X},{:08X},{:08X},{:08X},{:08X}",state,slot,fetch.dword_0,fetch.dword_1,fetch.dword_2,fetch.dword_3,fetch.dword_4,fetch.dword_5);
    }
  }
'''
    text=text.replace(anchor,anchor+trace)
if 'ME_DIAGNOSTIC_SKIP_PS' not in text:
    text=text.replace('#include <cstring>','#include <cstring>\n#include <cstdlib>',1)
    anchor='  const uint64_t draw_ps_hash=pixel_shader ? pixel_shader->ucode_data_hash() : 0;'
    assert text.count(anchor)==1
    block='''
  static const uint64_t diagnostic_skip_ps=[]{
    const char* value=std::getenv("ME_DIAGNOSTIC_SKIP_PS");
    if(!value || std::strlen(value)!=16)return uint64_t(0);
    char* end=nullptr;const auto hash=std::strtoull(value,&end,16);
    return end && !*end ? uint64_t(hash) : uint64_t(0);
  }();
  if(diagnostic_skip_ps && draw_ps_hash==diagnostic_skip_ps){
    static uint64_t skipped=0;
    if(++skipped<=4)REXGPU_WARN("external_diagnostic_draw_omitted ps={:016X} indices={} not_a_fix=true",draw_ps_hash,index_count);
    return true;
  }
'''
    text=text.replace(anchor,anchor+block)
if 'ME_DIAGNOSTIC_SKIP_VS' not in text:
    anchor='  if(diagnostic_skip_ps && draw_ps_hash==diagnostic_skip_ps){'
    assert text.count(anchor)==1
    text=text.replace(anchor,'''  static const uint64_t diagnostic_skip_vs=[]{
    const char* value=std::getenv("ME_DIAGNOSTIC_SKIP_VS");
    if(!value || std::strlen(value)!=16)return uint64_t(0);
    char* end=nullptr;const auto hash=std::strtoull(value,&end,16);
    return end && !*end ? uint64_t(hash) : uint64_t(0);
  }();
  if((diagnostic_skip_ps && draw_ps_hash==diagnostic_skip_ps) ||
     (diagnostic_skip_vs && draw_vs_hash==diagnostic_skip_vs)){''')
if 'ME_DIAGNOSTIC_DUMP_SHADER' not in text:
    text=text.replace('#include <cstring>','#include <cstring>\n#include <cstdio>',1)
    anchor='  const uint32_t draw_depth=regs.Get<reg::RB_DEPTHCONTROL>().value;'
    assert text.count(anchor)==1
    block='''
  static bool external_background_dumped=false;
  if(!external_background_dumped && draw_ps_hash==0x5EBE814012217F48ull && std::getenv("ME_DIAGNOSTIC_DUMP_SHADER")){
    external_background_dumped=true;
    auto dump=[](const char* path,const std::string& value){if(FILE* file=std::fopen(path,"wb")){std::fwrite(value.data(),1,value.size(),file);std::fclose(file);}};
    dump("/data/local/tmp/me_background_vs.txt",vertex_shader->ucode_disassembly());
    dump("/data/local/tmp/me_background_ps.txt",pixel_shader->ucode_disassembly());
    if(FILE* file=std::fopen("/data/local/tmp/me_background_constants.txt","wb")){
      for(unsigned i=0;i<32;++i){
        const unsigned base=XE_GPU_REG_SHADER_CONSTANT_000_X+(256+i)*4;
        std::fprintf(file,"c%u %08X %08X %08X %08X\\n",i,regs[base],regs[base+1],regs[base+2],regs[base+3]);
      }std::fclose(file);
    }
    REXGPU_INFO("external_background_shader_dumped ps={:016X}",draw_ps_hash);
  }
'''
    text=text.replace(anchor,block+anchor)
text=text.replace('for(unsigned i=0;i<32;++i){\n        const unsigned base=XE_GPU_REG_SHADER_CONSTANT_000_X+(256+i)*4;',
                  'for(unsigned i=0;i<256;++i){\n        const unsigned base=XE_GPU_REG_SHADER_CONSTANT_000_X+(256+i)*4;')
if 'ME_DIAGNOSTIC_VERTEX_DATA' not in text:
    anchor='  const uint32_t draw_depth=regs.Get<reg::RB_DEPTHCONTROL>().value;'
    assert text.count(anchor)==1
    text=text.replace(anchor,'''  static unsigned external_vertex_snapshots=0;
  if(draw_vs_hash==0x302B2B9F18E0017Eull && external_vertex_snapshots<3 && std::getenv("ME_DIAGNOSTIC_VERTEX_DATA")){
    const unsigned snapshot=external_vertex_snapshots++;
    char path[128];std::snprintf(path,sizeof(path),"/data/local/tmp/me_vertex_data_%u.txt",snapshot);
    if(FILE* file=std::fopen(path,"wb")){
      std::fprintf(file,"snapshot=%u indices=%u first_words_not_indexed_vertices=true\\n",snapshot,index_count);
      for(const auto& binding:vertex_shader->vertex_bindings()){
        const unsigned slot=binding.fetch_constant;
        const auto fetch=regs.GetVertexFetch(slot);
        const uint64_t address=uint64_t(fetch.address)<<2;
        const unsigned words=std::min(unsigned(fetch.size),48u);
        std::fprintf(file,"slot=%u raw=%08X,%08X address=%08llX endian=%u size_words=%u\\n",slot,fetch.dword_0,fetch.dword_1,(unsigned long long)address,unsigned(fetch.endian),unsigned(fetch.size));
        if(!words || address+uint64_t(words)*4>0x20000000ull || !memory_->IsPhysicalRangeValid(uint32_t(address),words*4)){std::fprintf(file,"range_unavailable=true\\n");continue;}
        const auto* data=memory_->TranslatePhysical<uint8_t*>(uint32_t(address));
        if(!data)continue;
        for(unsigned i=0;i<words;++i){
          uint32_t raw;std::memcpy(&raw,data+i*4,4);
          std::fprintf(file,"word=%u raw_le=%08X\\n",i,raw);
        }
      }
      for(unsigned i=0;i<16;++i){
        const unsigned base=XE_GPU_REG_SHADER_CONSTANT_000_X+i*4;
        std::fprintf(file,"vc%u %08X %08X %08X %08X\\n",i,regs[base],regs[base+1],regs[base+2],regs[base+3]);
      }
      std::fclose(file);
    }
  }
'''+anchor)
if 'ME_DIAGNOSTIC_DUMP_PS' not in text:
    anchor='  const uint32_t draw_depth=regs.Get<reg::RB_DEPTHCONTROL>().value;'
    assert text.count(anchor)==1
    text=text.replace(anchor,'''  static bool external_material_dumped=false;
  const char* material_dump_hash=std::getenv("ME_DIAGNOSTIC_DUMP_PS");
  if(!external_material_dumped && material_dump_hash && std::strlen(material_dump_hash)==16){
    char* end=nullptr;const auto wanted=std::strtoull(material_dump_hash,&end,16);
    if(end && !*end && draw_ps_hash==wanted && pixel_shader){
      external_material_dumped=true;
      auto dump=[](const char* path,const std::string& value){if(FILE* file=std::fopen(path,"wb")){std::fwrite(value.data(),1,value.size(),file);std::fclose(file);}};
      dump("/data/local/tmp/me_material_vs.txt",vertex_shader->ucode_disassembly());
      dump("/data/local/tmp/me_material_ps.txt",pixel_shader->ucode_disassembly());
      if(FILE* file=std::fopen("/data/local/tmp/me_material_constants.txt","wb")){
        std::fprintf(file,"vs=%016llX ps=%016llX indices=%u\\n",(unsigned long long)draw_vs_hash,(unsigned long long)draw_ps_hash,index_count);
        for(unsigned stage=0;stage<2;++stage)for(unsigned i=0;i<256;++i){
          const unsigned base=XE_GPU_REG_SHADER_CONSTANT_000_X+(stage*256+i)*4;
          std::fprintf(file,"%sc%u %08X %08X %08X %08X\\n",stage?"p":"v",i,regs[base],regs[base+1],regs[base+2],regs[base+3]);
        }std::fclose(file);
      }
      REXGPU_INFO("external_material_shader_dumped vs={:016X} ps={:016X} indices={}",draw_vs_hash,draw_ps_hash,index_count);
    }
  }
'''+anchor)
source.write_text(text)
scope={};compiler=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text()
exec(compiler[:compiler.index('def build')],scope)
result=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(source),'-o',str(root/'work/gpu-port/objects/vulkan_command_processor.cpp.o')],capture_output=True)
if result.returncode:print(result.stderr.decode(errors='replace')[-4000:])
raise SystemExit(result.returncode)
