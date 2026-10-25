#pragma once
#include "save_storage.h"
// Included after the guest memory, events and local profile helpers.
static MeSaveStorage save_storage;
static bool ensure_save_storage(){
 if(!save_storage.root.empty())return true;
 const char* root=std::getenv("ME_SAVE_ROOT");
 if(!save_storage.init(root?root:"/data/local/tmp/me_saves")){save_storage.root.clear();return false;}
 return true;
}
static void save_store64(uint32_t a,uint64_t value){put_word(a,uint32_t(value>>32));put_word(a+4,uint32_t(value));}
static std::string save_string(PPCContext& ctx,uint32_t a,unsigned limit){
 std::string value;if(!a)return value;
 for(unsigned i=0;i<=limit;++i){if(!checked_span(a+i,1,true))me_stop_import("save_string_pointer_invalid",ctx);unsigned char c=guest[a+i];if(!c)return value;if(c<32 || c>126 || i==limit)me_stop_import("save_string_invalid",ctx);value.push_back(char(c));}
 return value;
}
static void save_validate_overlapped(PPCContext& ctx,uint32_t a){
 if(!a)return;
 if(!checked_span(a,28))me_stop_import("save_overlapped_invalid",ctx);
 if(load_word(a+16))me_stop_import("save_completion_routine_not_implemented",ctx);
 const auto event=load_word(a+12);if(event && !events.find(event))me_stop_import("save_overlapped_event_invalid",ctx);
}
static uint32_t save_complete(PPCContext& ctx,uint32_t a,uint32_t result,uint32_t length=0,bool hr=false){
 if(!a)return result;save_validate_overlapped(ctx,a);
 put_word(a+8,thread_address);put_word(a+4,length);put_word(a+24,hr && result ? 0x80070000u|(result&0xffff):result);
 put_word(a,result);const auto event=load_word(a+12);if(event){uint32_t previous;events.set(event,previous);state_changed.notify_all();}
 return 997;
}
static uint32_t save_key(PPCContext& ctx,uint32_t user,uint32_t a,MeContentKey& key,std::u16string& display){
 if(user>=4)return 87;if(user || !local_player_signed_in)return 1317;
 if(!checked_span(a,308,true))return 87;
 if(load_word(a)!=1 && load_word(a)!=0)return 1167;
 if(load_word(a+4)!=1)return 50; // Saved-game containers only; no DLC/license claims.
 key={selected_local_player.xuid,0x45410850,1,{}};display.clear();
 for(unsigned i=0;i<42 && guest[a+264+i];++i)key.file_name.push_back(char(guest[a+264+i]));
 for(unsigned i=0;i<128;++i){uint16_t c=uint16_t(guest[a+8+i*2])<<8|guest[a+9+i*2];if(!c)break;display.push_back(char16_t(c));}
 return me_save::valid_name(key.file_name)?0:123;
}
static void save_content_data(uint32_t a,const MeContentInfo& info){
 std::memset(guest+a,0,308);put_word(a,1);put_word(a+4,1);
 for(unsigned i=0;i<std::min(size_t(127),info.display_name.size());++i){guest[a+8+i*2]=uint8_t(info.display_name[i]>>8);guest[a+9+i*2]=uint8_t(info.display_name[i]);}
 std::memcpy(guest+a+264,info.file_name.data(),std::min(size_t(42),info.file_name.size()));
}
struct MeContentEnumerator {bool live=true;uint32_t per_call=0;size_t index=0;std::vector<MeContentInfo> entries;};
static std::vector<MeContentEnumerator> content_enumerators;
static MeContentEnumerator* save_enumerator(uint32_t h){if(h<0x61000 || ((h-0x61000)&3))return nullptr;size_t i=(h-0x61000)/4;return i<content_enumerators.size() && content_enumerators[i].live?&content_enumerators[i]:nullptr;}
ME_IMPORT(__imp__XamShowDeviceSelectorUI){
 const auto user=ctx.r3.u32,type=ctx.r4.u32,flags=ctx.r5.u32,out=ctx.r7.u32,ov=ctx.r8.u32;const uint64_t requested=ctx.r6.u64;
 save_validate_overlapped(ctx,ov);if(!word_writable(out)){ctx.r3.u64=87;return;}
 uint32_t result=0;uint64_t total=0,free=0;
 if(user>=4 || type!=1 || (flags&~0x200u))result=87;
 else if(user || !local_player_signed_in)result=1317;
 else if(!ensure_save_storage())result=me_save::win32_from_errno(errno);
 else if(!save_storage.space(total,free))result=me_save::win32_from_errno(errno);
 else if(requested>free)result=112;
 if(!result)put_word(out,1);
 std::printf("{\"event\":\"save_device_selected\",\"result\":%u,\"free\":%llu,\"requested\":%llu}\n",result,(unsigned long long)free,(unsigned long long)requested);
 ctx.r3.u64=save_complete(ctx,ov,result,0);
}
ME_IMPORT(__imp__XamContentGetDeviceState){const auto ov=ctx.r4.u32;save_validate_overlapped(ctx,ov);uint32_t result=ctx.r3.u32==1 && ensure_save_storage()?0:1167;ctx.r3.u64=save_complete(ctx,ov,result);}
ME_IMPORT(__imp__XamContentGetDeviceData){
 const auto id=ctx.r3.u32,out=ctx.r4.u32;if(id!=1){ctx.r3.u64=1167;return;}if(!checked_span(out,80)){ctx.r3.u64=87;return;}
 uint64_t total,free;if(!ensure_save_storage() || !save_storage.space(total,free)){ctx.r3.u64=me_save::win32_from_errno(errno);return;}
 std::memset(guest+out,0,80);put_word(out,1);put_word(out+4,1);save_store64(out+8,total);save_store64(out+16,free);
 const char* name="Local saved games";for(unsigned i=0;name[i];++i)guest[out+25+i*2]=uint8_t(name[i]);ctx.r3.u64=0;
}
static void save_content_create(PPCContext& ctx,uint32_t ov){
 save_validate_overlapped(ctx,ov);const auto disposition=ctx.r7.u32,license=ctx.r8.u32;
 if((disposition && !word_writable(disposition)) || (license && !word_writable(license))){ctx.r3.u64=87;return;}
 MeContentKey key;std::u16string display;uint32_t result=save_key(ctx,ctx.r3.u32,ctx.r5.u32,key,display),opened=0;
 const auto root=save_string(ctx,ctx.r4.u32,32);if(!me_save::valid_name(root,32))result=123;
 if(!result && !ensure_save_storage())result=me_save::win32_from_errno(errno);
 if(!result)result=save_storage.create(key,display,ctx.r6.u32,root,opened);
 if(!result){if(disposition)put_word(disposition,opened);if(license)put_word(license,0);}
 std::printf("{\"event\":\"save_container_open\",\"result\":%u,\"disposition\":%u,\"root\":\"%s\"}\n",result,opened,root.c_str());
 ctx.r3.u64=save_complete(ctx,ov,result,opened,true);
}
ME_IMPORT(__imp__XamContentCreateEx){if(!checked_span(ctx.r1.u32+84,4,true))me_stop_import("content_create_stack_invalid",ctx);save_content_create(ctx,load_word(ctx.r1.u32+84));}
ME_IMPORT(__imp__XamContentCreate){save_content_create(ctx,ctx.r9.u32);}
ME_IMPORT(__imp__XamContentClose){const auto ov=ctx.r4.u32;save_validate_overlapped(ctx,ov);const auto name=save_string(ctx,ctx.r3.u32,32);ctx.r3.u64=save_complete(ctx,ov,save_storage.close_mount(name));}
ME_IMPORT(__imp__XamContentFlush){const auto ov=ctx.r4.u32;save_validate_overlapped(ctx,ov);const auto name=save_string(ctx,ctx.r3.u32,32);ctx.r3.u64=save_complete(ctx,ov,save_storage.flush_mount(name));}
ME_IMPORT(__imp__XamContentDelete){
 const auto ov=ctx.r5.u32;save_validate_overlapped(ctx,ov);MeContentKey key;std::u16string display;auto result=save_key(ctx,ctx.r3.u32,ctx.r4.u32,key,display);
 if(!result && !ensure_save_storage())result=me_save::win32_from_errno(errno);if(!result)result=save_storage.remove(key);ctx.r3.u64=save_complete(ctx,ov,result);
}
ME_IMPORT(__imp__XamContentGetCreator){
 const auto ov=ctx.r7.u32,creator=ctx.r5.u32,xuid=ctx.r6.u32;save_validate_overlapped(ctx,ov);
 if(!word_writable(creator) || (xuid && !checked_span(xuid,8))){ctx.r3.u64=87;return;}
 MeContentKey key;std::u16string display;auto result=save_key(ctx,ctx.r3.u32,ctx.r4.u32,key,display);MeContentInfo info;
 if(!result && !ensure_save_storage())result=me_save::win32_from_errno(errno);
 if(!result && !save_storage.read_meta(key,info))result=3;
 if(!result){put_word(creator,info.creator==selected_local_player.xuid);if(xuid)save_store64(xuid,info.creator);}
 ctx.r3.u64=save_complete(ctx,ov,result);
}
ME_IMPORT(__imp__XamContentCreateEnumerator){
 const auto user=ctx.r3.u32,device=ctx.r4.u32,type=ctx.r5.u32,count=ctx.r7.u32,size_out=ctx.r8.u32,handle_out=ctx.r9.u32;
 if(user>=4 || device>1 || type!=1 || !count || count>128 || !word_writable(size_out) || !word_writable(handle_out)){ctx.r3.u64=87;return;}
 if(user || !local_player_signed_in){ctx.r3.u64=1317;return;}
 if(!ensure_save_storage()){ctx.r3.u64=me_save::win32_from_errno(errno);return;}
 if(content_enumerators.size()>=256){ctx.r3.u64=8;return;}
 uint32_t error=0;auto entries=save_storage.list(selected_local_player.xuid,0x45410850,1,&error);
 if(error){ctx.r3.u64=error;return;}
 content_enumerators.push_back({true,count,0,std::move(entries)});
 put_word(size_out,count*308);put_word(handle_out,0x61000+uint32_t(content_enumerators.size()-1)*4);ctx.r3.u64=0;
}
static void enumerate_save_content(PPCContext& ctx,MeContentEnumerator& e){
 const auto buffer=ctx.r5.u32,capacity=ctx.r6.u32,returned=ctx.r7.u32,ov=ctx.r8.u32;save_validate_overlapped(ctx,ov);
 if(ctx.r4.u32 || (!ov && !returned) || (returned && !word_writable(returned))){ctx.r3.u64=87;return;}
 uint32_t result=0,count=0;if(returned)put_word(returned,0);
 if(capacity<e.per_call*308)result=122;
 else if(!checked_span(buffer,e.per_call*308))result=87;
 else if(e.index==e.entries.size())result=18;
 else{count=uint32_t(std::min(size_t(e.per_call),e.entries.size()-e.index));for(unsigned i=0;i<count;++i)save_content_data(buffer+i*308,e.entries[e.index++]);if(returned)put_word(returned,count);}
 ctx.r3.u64=save_complete(ctx,ov,result,count);
}
