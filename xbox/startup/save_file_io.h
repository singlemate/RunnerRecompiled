#pragma once
#include <linux/falloc.h>
static bool checked_file_buffer(uint32_t address,uint32_t size);
struct MeSavePath {MeSaveStorage::Mount* mount=nullptr;std::string relative;bool recognized=false;};
static MeSavePath save_map_path(uint32_t root,const std::string& path){
 MeSavePath mapped;
 if(auto* file=save_storage.find(root)){
  mapped.recognized=true;if(!file->directory || file->mount>=save_storage.mounts.size())return mapped;
  mapped.mount=&save_storage.mounts[file->mount];if(!mapped.mount->live){mapped.mount=nullptr;return mapped;}
  if(file->path.size()>mapped.mount->dir.size())mapped.relative=file->path.substr(mapped.mount->dir.size()+1)+"/";
  mapped.relative+=path;return mapped;
 }
 if(root && root!=0xfffffffd)return mapped;
 const auto colon=path.find(':');if(colon==std::string::npos)return mapped;
 mapped.mount=save_storage.find_mount(path.substr(0,colon));if(!mapped.mount)return mapped;
 mapped.recognized=true;mapped.relative=path.substr(colon+1);return mapped;
}
static void save_iosb(PPCContext& ctx,uint32_t a,uint32_t status,uint32_t length){
 if(!checked_span(a,8))me_stop_import("save_iosb_invalid",ctx);put_word(a,status);put_word(a+4,length);ctx.r3.u64=status;
}
static bool save_open_guest(PPCContext& ctx,uint32_t disposition,uint32_t options){
 const auto attrs=ctx.r5.u32;if(!checked_span(attrs,12,true))return false;const auto name=load_word(attrs+4);if(!checked_span(name,8,true))return false;
 const auto length=load_word(name)>>16,address=load_word(name+4);if(length>1024 || !checked_span(address,length,true))return false;
 const auto path=std::string(reinterpret_cast<char*>(guest+address),length);auto mapped=save_map_path(load_word(attrs),path);if(!mapped.recognized)return false;
 const auto out=ctx.r3.u32,iosb=ctx.r6.u32,access=ctx.r4.u32;
 if(!word_writable(out) || !checked_span(iosb,8))me_stop_import("save_open_output_invalid",ctx);
 uint32_t handle=0,information=0,status=0;
 if(!mapped.mount)status=0xc0000008;
 else if(options&~0x107fu)status=0xc000000d;
 else status=save_storage.open(*mapped.mount,mapped.relative,disposition,(access&(0x40000000u|0x10000u|0x106u))!=0,options,handle,information);
 if(!status)put_word(out,handle);save_iosb(ctx,iosb,status,status?0:information);
 std::printf("{\"event\":\"save_file_open\",\"handle\":%u,\"status\":%u,\"disposition\":%u}\n",handle,status,disposition);return true;
}
ME_IMPORT(__imp__NtOpenFile){if(!save_open_guest(ctx,1,ctx.r8.u32))me_stop_import("nt_open_file_unmapped",ctx);}
static bool save_read_guest(PPCContext& ctx){
 auto* file=save_storage.find(ctx.r3.u32);if(!file)return false;
 const auto event=ctx.r4.u32,iosb=ctx.r7.u32,buffer=ctx.r8.u32,length=ctx.r9.u32,offset_ptr=ctx.r10.u32;
 if(ctx.r5.u32 || ctx.r6.u32)me_stop_import("save_read_apc_unsupported",ctx);
 if(!checked_span(iosb,8) || !checked_file_buffer(buffer,length) || (offset_ptr && !checked_span(offset_ptr,8,true)) || (event && !events.find(event)))me_stop_import("save_read_arguments_invalid",ctx);
 uint64_t offset=offset_ptr?(uint64_t(load_word(offset_ptr))<<32)|load_word(offset_ptr+4):UINT64_MAX;if(offset>=UINT64_MAX-1)offset=file->position;
 me_gpu_write_watch.host_write(buffer,length);
 const auto n=save_storage.read(*file,guest+buffer,length,offset);uint32_t status=n<0?me_save::ntstatus_from_errno(errno):(length && !n?0xc0000011:0);
 save_iosb(ctx,iosb,status,n<0?0:uint32_t(n));if(event){uint32_t previous;events.set(event,previous);state_changed.notify_all();}return true;
}
ME_IMPORT(__imp__NtWriteFile){
 auto* file=save_storage.find(ctx.r3.u32);if(!file)me_stop_import("write_file_unmapped_handle",ctx);
 const auto event=ctx.r4.u32,iosb=ctx.r7.u32,buffer=ctx.r8.u32,length=ctx.r9.u32,offset_ptr=ctx.r10.u32;
 if(ctx.r5.u32 || ctx.r6.u32)me_stop_import("save_write_apc_unsupported",ctx);
 if(!checked_span(iosb,8) || !checked_file_buffer(buffer,length) || (offset_ptr && !checked_span(offset_ptr,8,true)) || (event && !events.find(event)))me_stop_import("save_write_arguments_invalid",ctx);
 uint64_t offset=offset_ptr?(uint64_t(load_word(offset_ptr))<<32)|load_word(offset_ptr+4):UINT64_MAX-1;
 uint32_t status=0;if(offset==UINT64_MAX){struct stat st{};if(fstat(file->fd,&st))status=me_save::ntstatus_from_errno(errno);else offset=uint64_t(st.st_size);}else if(offset==UINT64_MAX-1)offset=file->position;
 if(!status && !save_storage.write(*file,guest+buffer,length,offset))status=me_save::ntstatus_from_errno(errno);
 save_iosb(ctx,iosb,status,status?0:length);if(event){uint32_t previous;events.set(event,previous);state_changed.notify_all();}
 std::printf("{\"event\":\"save_file_write\",\"bytes\":%u,\"status\":%u}\n",status?0:length,status);
}
ME_IMPORT(__imp__NtFlushBuffersFile){auto* file=save_storage.find(ctx.r3.u32);if(!file)me_stop_import("save_flush_handle_invalid",ctx);const auto out=ctx.r4.u32;if(!checked_span(out,8))me_stop_import("save_flush_iosb_invalid",ctx);const auto status=fsync(file->fd)?me_save::ntstatus_from_errno(errno):0;save_iosb(ctx,out,status,0);}
static bool save_query_guest(PPCContext& ctx){
 const auto* file=save_storage.find(ctx.r3.u32);if(!file)return false;const auto iosb=ctx.r4.u32,out=ctx.r5.u32,len=ctx.r6.u32,kind=ctx.r7.u32;
 uint32_t needed=0;switch(kind){case 4:needed=40;break;case 5:needed=24;break;case 14:needed=8;break;case 34:needed=56;break;default:me_stop_import("save_query_information_class_unsupported",ctx);}
 if(!checked_span(iosb,8))me_stop_import("save_query_iosb_invalid",ctx);if(len<needed){save_iosb(ctx,iosb,0xc0000004,0);return true;}if(!checked_span(out,needed))me_stop_import("save_query_output_invalid",ctx);
 struct stat st{};if(fstat(file->fd,&st)){save_iosb(ctx,iosb,me_save::ntstatus_from_errno(errno),0);return true;}
 std::memset(guest+out,0,needed);const uint32_t attrs=file->directory?0x10:0x20;
 if(kind==4)put_word(out+32,attrs);
 if(kind==5){save_store64(out,uint64_t(st.st_blocks)*512);save_store64(out+8,uint64_t(st.st_size));put_word(out+16,uint32_t(st.st_nlink));guest[out+20]=file->delete_on_close;guest[out+21]=file->directory;}
 if(kind==14)save_store64(out,file->position);
 if(kind==34){save_store64(out+32,uint64_t(st.st_blocks)*512);save_store64(out+40,uint64_t(st.st_size));put_word(out+48,attrs);}
 save_iosb(ctx,iosb,0,needed);return true;
}
static bool save_set_guest(PPCContext& ctx){
 auto* file=save_storage.find(ctx.r3.u32);if(!file)return false;const auto iosb=ctx.r4.u32,in=ctx.r5.u32,len=ctx.r6.u32,kind=ctx.r7.u32;
 if(!checked_span(iosb,8) || len!=8 || !checked_span(in,8,true)){save_iosb(ctx,iosb,0xc000000d,0);return true;}
 const uint64_t value=(uint64_t(load_word(in))<<32)|load_word(in+4);uint32_t status=0;
 if(value>INT64_MAX)status=0xc000000d;
 else if(kind==14)file->position=value;
 else if(kind==20){if(!file->writable)status=0xc0000022;else if(ftruncate(file->fd,off_t(value)))status=me_save::ntstatus_from_errno(errno);}
 else if(kind==19){
  if(!file->writable)status=0xc0000022;
  else if(file->directory)status=0xc000000d;
  else {
   struct stat st{};
   if(fstat(file->fd,&st))status=me_save::ntstatus_from_errno(errno);
   else if(value && fallocate(file->fd,FALLOC_FL_KEEP_SIZE,0,off_t(value)))status=me_save::ntstatus_from_errno(errno);
   else if(value<uint64_t(st.st_size) && ftruncate(file->fd,off_t(value)))status=me_save::ntstatus_from_errno(errno);
  }
 }
 else me_stop_import("save_set_information_class_unsupported",ctx);
 save_iosb(ctx,iosb,status,0);return true;
}
static bool save_full_attributes(PPCContext& ctx){
 const auto attrs=ctx.r3.u32,out=ctx.r4.u32;if(!checked_span(attrs,12,true))return false;const auto desc=load_word(attrs+4);if(!checked_span(desc,8,true))return false;
 const auto length=load_word(desc)>>16,address=load_word(desc+4);if(length>1024 || !checked_span(address,length,true))return false;auto mapped=save_map_path(load_word(attrs),std::string(reinterpret_cast<char*>(guest+address),length));if(!mapped.recognized)return false;
 if(!checked_span(out,56))me_stop_import("save_full_attributes_output_invalid",ctx);std::string path;uint32_t status=mapped.mount?save_storage.resolve(*mapped.mount,mapped.relative,path):0xc0000008;
 struct stat st{};if(!status && lstat(path.c_str(),&st))status=me_save::ntstatus_from_errno(errno);
 if(!status){std::memset(guest+out,0,56);save_store64(out+32,uint64_t(st.st_blocks)*512);save_store64(out+40,uint64_t(st.st_size));put_word(out+48,S_ISDIR(st.st_mode)?0x10:0x20);}ctx.r3.u64=status;return true;
}
