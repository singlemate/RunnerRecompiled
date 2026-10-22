#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
// Title achievement metadata from the XEX XDBF (SPA) resource. Unlock state is not
// stored here: a new local profile has none, and nothing is reported as achieved.
struct MeAchievement {uint32_t id=0,image_id=0,gamerscore=0,flags=0;std::u16string label,description,unachieved;};
inline uint32_t me_be32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
inline uint16_t me_be16(const uint8_t* p){return uint16_t(p[0]<<8|p[1]);}
inline bool me_utf8_to_utf16(const uint8_t* p,size_t n,std::u16string& out){
 out.clear();
 for(size_t i=0;i<n;){uint32_t c=p[i],extra=c<0x80?0:(c>>5)==6?1:(c>>4)==14?2:(c>>3)==30?3:4;if(extra>3 || i+extra>=n+(extra?0:1))return false;
  if(extra){c&=0x3f>>extra;for(uint32_t k=1;k<=extra;++k){if((p[i+k]&0xc0)!=0x80)return false;c=c<<6|(p[i+k]&0x3f);}
   static const uint32_t minimum[4]={0,0x80,0x800,0x10000};if(c<minimum[extra] || c>0x10ffff || (c>=0xd800 && c<=0xdfff))return false;}
  i+=extra+1;
  if(c>=0x10000){c-=0x10000;out.push_back(char16_t(0xd800+(c>>10)));out.push_back(char16_t(0xdc00+(c&0x3ff)));}else out.push_back(char16_t(c));}
 return true;
}
struct MeXdbf {
 const uint8_t* data=nullptr;size_t size=0,content=0;uint32_t used=0;
 bool open(const uint8_t* d,size_t n){
  if(!d || n<24 || std::memcmp(d,"XDBF",4))return false;
  const uint64_t entries=me_be32(d+8),free_entries=me_be32(d+16);used=me_be32(d+12);
  const uint64_t start=24+entries*18+free_entries*8;
  if(used>entries || start>n)return false;data=d;size=n;content=size_t(start);return true;}
 bool entry(uint16_t section,uint64_t id,const uint8_t*& block,uint32_t& length) const{
  for(uint32_t i=0;i<used;++i){const uint8_t* e=data+24+i*18;uint64_t eid=0;for(unsigned k=2;k<10;++k)eid=eid<<8|e[k];
   if(me_be16(e)!=section || eid!=id)continue;const uint64_t offset=content+uint64_t(me_be32(e+10));length=me_be32(e+14);
   if(offset+length>size)return false;block=data+offset;return true;}
  return false;}
 // XSTR string table of one language; empty result means the id is absent.
 bool string(uint32_t language,uint16_t id,std::u16string& out,bool& found) const{
  const uint8_t* b;uint32_t n;found=false;out.clear();if(!entry(3,language,b,n))return false;
  if(n<14 || std::memcmp(b,"XSTR",4) || me_be32(b+4)!=1)return false;
  size_t at=14;for(uint32_t i=0,count=me_be16(b+12);i<count;++i){if(at+4>n)return false;const auto sid=me_be16(b+at),len=me_be16(b+at+2);at+=4;
   if(at+len>n)return false;if(sid==id){found=true;return me_utf8_to_utf16(b+at,len,out);}at+=len;}
  return true;}
 bool default_language(uint32_t& language) const{const uint8_t* b;uint32_t n;
  if(!entry(1,0x58535443,b,n))return false;if(n<16 || std::memcmp(b,"XSTC",4))return false;language=me_be32(b+12);return true;}
 // Same rule as the SDK: a language counts only if the title string 0x8000 exists.
 bool resolve_language(uint32_t requested,uint32_t& language) const{
  std::u16string title;bool found=false;if(string(requested,0x8000,title,found) && found){language=requested;return true;}
  return default_language(language) && string(language,0x8000,title,found) && found;}
 bool achievements(uint32_t requested_language,std::vector<MeAchievement>& out,uint32_t& language) const{
  out.clear();const uint8_t* b;uint32_t n;
  if(!resolve_language(requested_language,language) || !entry(1,0x58414348,b,n))return false;
  if(n<14 || std::memcmp(b,"XACH",4) || me_be32(b+4)!=1)return false;
  const uint32_t count=me_be16(b+12);if(14+uint64_t(count)*36>n)return false;
  // String id 0xFFFF marks an intentionally absent text (observed for unachieved
  // descriptions); it becomes an empty string. Any other missing id is invalid data.
  auto text=[&](uint16_t id,std::u16string& out){bool found=false;if(id==0xffff){out.clear();return true;}return string(language,id,out,found) && found;};
  for(uint32_t i=0;i<count;++i){const uint8_t* e=b+14+i*36;MeAchievement a;
   a.id=me_be16(e);a.image_id=me_be32(e+8);a.gamerscore=me_be16(e+12);a.flags=me_be32(e+16);
   if(!text(me_be16(e+2),a.label) || !text(me_be16(e+4),a.description) || !text(me_be16(e+6),a.unachieved))return false;
   out.push_back(std::move(a));}
  return true;}
};
// Enumerator objects for XamUserCreateAchievementEnumerator/XamEnumerate/NtClose.
struct MeAchievementEnumerators {
 static constexpr uint32_t detail_size=36,string_budget=464; // SDK X_ACHIEVEMENT_DETAILS layout.
 struct Object {uint32_t user=0,flags=0,per_call=0,cursor=0;bool live=true;};
 std::vector<Object> objects;
 static uint32_t item_size(uint32_t flags){return detail_size+((flags&7)?string_budget:0);}
 Object* find(uint32_t h){if(h<0x60000 || (h&3))return nullptr;auto i=(h-0x60000)/4;return i<objects.size()&&objects[i].live?&objects[i]:nullptr;}
 uint32_t create(uint32_t user,uint32_t flags,uint32_t per_call,uint32_t offset){if(objects.size()>=256)return 0;objects.push_back({user,flags,per_call,offset,true});return 0x60000+uint32_t(objects.size()-1)*4;}
 bool close(uint32_t h){auto* e=find(h);if(!e)return false;e->live=false;return true;}
 // Writes up to per_call items into a host view of the guest buffer at guest_address.
 // Returns a Win32 code: 0, 18 NO_MORE_FILES, 122 INSUFFICIENT_BUFFER, 1627 strings overflow.
 static uint32_t write(Object& e,const std::vector<MeAchievement>& items,uint8_t* buffer,uint32_t guest_address,uint32_t capacity,uint32_t& written){
  written=0;if(e.cursor>=items.size())return 18;
  if(uint64_t(capacity)<uint64_t(e.per_call)*item_size(e.flags))return 122;
  const uint32_t count=std::min<uint32_t>(e.per_call,uint32_t(items.size()-e.cursor));
  uint32_t strings=e.per_call*detail_size;const uint32_t strings_end=strings+count*((e.flags&7)?string_budget:0);
  auto put=[&](uint32_t at,uint32_t v){buffer[at]=uint8_t(v>>24);buffer[at+1]=uint8_t(v>>16);buffer[at+2]=uint8_t(v>>8);buffer[at+3]=uint8_t(v);};
  auto append=[&](const std::u16string& s)->uint32_t{const uint32_t bytes=uint32_t(s.size()+1)*2;if(strings+bytes>strings_end)return 0;const uint32_t at=strings;
   for(size_t i=0;i<s.size();++i){buffer[at+i*2]=uint8_t(s[i]>>8);buffer[at+i*2+1]=uint8_t(s[i]);}buffer[at+s.size()*2]=buffer[at+s.size()*2+1]=0;strings+=bytes;return guest_address+at;};
  for(uint32_t i=0;i<count;++i){const auto& a=items[e.cursor+i];const uint32_t d=i*detail_size;uint32_t label=0,description=0,unachieved=0;
   if((e.flags&1) && !(label=append(a.label)))return 1627;
   if((e.flags&2) && !(description=append(a.description)))return 1627;
   if((e.flags&4) && !(unachieved=append(a.unachieved)))return 1627;
   put(d,a.id);put(d+4,label);put(d+8,description);put(d+12,unachieved);put(d+16,a.image_id);put(d+20,a.gamerscore);
   put(d+24,0);put(d+28,0);put(d+32,a.flags);}  // No unlock time: nothing is achieved.
  e.cursor+=count;written=count;return 0;}
};
