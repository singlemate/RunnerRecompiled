from pathlib import Path
import subprocess
root=Path.cwd();p=root/'work/references/rexglue-sdk/src/graphics/vulkan/command_processor.cpp';s=p.read_text()
anchor='  static bool external_background_dumped=false;'
if 'ME_DIAGNOSTIC_DUMP_DEPTH_WRITER' not in s:
 block='''  static bool depth_writer_dumped=false;
  if(!depth_writer_dumped && draw_vs_hash==0xBF8AD3FD7A465985ull && std::getenv("ME_DIAGNOSTIC_DUMP_DEPTH_WRITER")){
    depth_writer_dumped=true;
    if(FILE* f=std::fopen("/data/local/tmp/me_depth_writer.txt","wb")){
      const auto& disasm=vertex_shader->ucode_disassembly();std::fwrite(disasm.data(),1,disasm.size(),f);
      std::fprintf(f,"\\nindices=%u depth=%08X normalized=%08X\\n",index_count,regs.Get<reg::RB_DEPTHCONTROL>().value,normalized_depth_control.value);
      for(unsigned i=0;i<256;++i){const unsigned base=XE_GPU_REG_SHADER_CONSTANT_000_X+i*4;std::fprintf(f,"vc%u %08X %08X %08X %08X\\n",i,regs[base],regs[base+1],regs[base+2],regs[base+3]);}
      for(const auto& binding:vertex_shader->vertex_bindings()){const auto fetch=regs.GetVertexFetch(binding.fetch_constant);std::fprintf(f,"fetch=%u raw=%08X,%08X\\n",binding.fetch_constant,fetch.dword_0,fetch.dword_1);}
      std::fclose(f);
    }
    REXGPU_INFO("external_depth_writer_dumped vs={:016X}",draw_vs_hash);
  }
'''
 assert s.count(anchor)==1;s=s.replace(anchor,block+anchor);p.write_text(s)
s=p.read_text()
if 'depth_geometry_v2' not in s:
 start=s.index('  static bool depth_writer_dumped=false;');end=s.index('  static bool external_background_dumped=false;',start)
 block=s[start:end]
 anchor='      std::fclose(f);'
 extra=r"""      // depth_geometry_v2: bounded reads of source geometry and viewport state.
      for(unsigned regnum: {unsigned(XE_GPU_REG_PA_CL_CLIP_CNTL),unsigned(XE_GPU_REG_PA_CL_VTE_CNTL),unsigned(XE_GPU_REG_VGT_INDX_OFFSET),unsigned(XE_GPU_REG_VGT_DMA_BASE),unsigned(XE_GPU_REG_VGT_DMA_SIZE)})std::fprintf(f,"reg=%04X value=%08X\n",regnum,regs[regnum]);
      for(unsigned regnum=0x210F;regnum<=0x2114;++regnum)std::fprintf(f,"reg=%04X value=%08X\n",regnum,regs[regnum]);
      auto dump_words=[&](uint32_t address,unsigned count){
        if(uint64_t(address)+count*4>0x20000000ull || !memory_->IsPhysicalRangeValid(address,count*4))return;
        const auto* bytes=memory_->TranslatePhysical<uint8_t*>(address);if(!bytes)return;
        for(unsigned i=0;i<count;++i){uint32_t word;std::memcpy(&word,bytes+i*4,4);std::fprintf(f,"mem=%08X raw=%08X\n",address+i*4,word);}
      };
      for(const auto& binding:vertex_shader->vertex_bindings()){const auto fetch=regs.GetVertexFetch(binding.fetch_constant);dump_words(fetch.address<<2,std::min(unsigned(fetch.size),192u));}
      dump_words(regs[XE_GPU_REG_VGT_DMA_BASE]&0x1FFFFFFCu,16);
"""
 assert block.count(anchor)==1;block=block.replace(anchor,extra+anchor);s=s[:start]+block+s[end:];p.write_text(s)
s=p.read_text()
if 'depth_index_v3' not in s:
 s=s.replace('dump_words(regs[XE_GPU_REG_VGT_DMA_BASE]&0x1FFFFFFCu,16);','// depth_index_v3: six indices fit in the mapped allocation tail.\n      std::fprintf(f,"processed type=%u base=%08X format=%u count=%u\\n",unsigned(primitive_processing_result.index_buffer_type),primitive_processing_result.guest_index_base,unsigned(primitive_processing_result.host_index_format),primitive_processing_result.host_draw_vertex_count);\n      dump_words(regs[XE_GPU_REG_VGT_DMA_BASE]&0x1FFFFFFCu,3);')
 p.write_text(s)
s=p.read_text()
if 'depth_sync_v4' not in s:
 anchor='  // Update dynamic graphics pipeline state.'
 block=r"""  // depth_sync_v4
  if(draw_vs_hash==0xBF8AD3FD7A465985ull && std::getenv("ME_DIAGNOSTIC_DUMP_DEPTH_WRITER")){
    static unsigned sync_count=0;
    if(sync_count++<8){
      REXGPU_INFO("external_depth_writer_viewport frame={} ndc_scale={},{},{} ndc_offset={},{},{} z_bounds={},{}",frame_current_,viewport_info.ndc_scale[0],viewport_info.ndc_scale[1],viewport_info.ndc_scale[2],viewport_info.ndc_offset[0],viewport_info.ndc_offset[1],viewport_info.ndc_offset[2],viewport_info.z_min,viewport_info.z_max);
      if(sync_count==1){if(FILE* f=std::fopen("/data/local/tmp/me_depth_writer.txt","ab")){std::fprintf(f,"frame=%llu\nndc_scale=%.9g,%.9g,%.9g\nndc_offset=%.9g,%.9g,%.9g\nz_bounds=%.9g,%.9g\n",(unsigned long long)frame_current_,viewport_info.ndc_scale[0],viewport_info.ndc_scale[1],viewport_info.ndc_scale[2],viewport_info.ndc_offset[0],viewport_info.ndc_offset[1],viewport_info.ndc_offset[2],viewport_info.z_min,viewport_info.z_max);std::fclose(f);}}
    }
  }
"""
 assert s.count(anchor)==1;s=s.replace(anchor,block+anchor);p.write_text(s)
s=p.read_text()
if 'depth_writer_snapshot_count' not in s:
 s=s.replace('static bool depth_writer_dumped=false;','static unsigned depth_writer_snapshot_count=0;')
 s=s.replace('if(!depth_writer_dumped && draw_vs_hash==0xBF8AD3FD7A465985ull','if(depth_writer_snapshot_count<48 && draw_vs_hash==0xBF8AD3FD7A465985ull')
 s=s.replace('    depth_writer_dumped=true;','    const unsigned snapshot=depth_writer_snapshot_count++;\n    char depth_path[128];std::snprintf(depth_path,sizeof(depth_path),"/data/local/tmp/me_depth_writer_%u.txt",snapshot);')
 s=s.replace('std::fopen("/data/local/tmp/me_depth_writer.txt","wb")','std::fopen(depth_path,"wb")')
 s=s.replace('      const auto& disasm=vertex_shader->ucode_disassembly();','      std::fprintf(f,"snapshot=%u frame=%llu\\n",snapshot,(unsigned long long)frame_current_);\n      const auto& disasm=vertex_shader->ucode_disassembly();',1)
 s=s.replace('REXGPU_INFO("external_depth_writer_dumped vs={:016X}",draw_vs_hash);','REXGPU_INFO("external_depth_writer_dumped vs={:016X} snapshot={} frame={} indices={}",draw_vs_hash,snapshot,frame_current_,index_count);')
 p.write_text(s)
scope={};s=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(s[:s.index('def build')],scope)
q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(p),'-o',str(root/'work/gpu-port/objects/vulkan_command_processor.cpp.o')],capture_output=True)
if q.returncode:print(q.stderr.decode(errors='replace')[-4000:])
raise SystemExit(q.returncode)
