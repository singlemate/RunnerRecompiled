from pathlib import Path
import subprocess,json,os
r=Path.cwd(); sdk=r/'work/references/rexglue-sdk'; out=r/'work/gpu-port/packet-check';out.mkdir(exist_ok=True)
s=(sdk/'src/graphics/command_processor.cpp').read_text()
def method(name):
 start=s.index(name); start=s.rfind('\n',0,start)+1; brace=s.index('{',start); depth=1;end=brace+1
 while depth:
  depth += (s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]
code=r'''
#include <rex/memory/ring_buffer.h>
#include <cstdio>
#include <cstdlib>
#define SCOPE_profile_cpu_f(...)
#define REXGPU_ERROR(...)
namespace memory=rex::memory;
struct Mem {void SynchronizePhysicalWrites() {} uint8_t data[64]{}; uint8_t* TranslatePhysical(uint32_t){return data;}};
struct CommandProcessor {
 Mem mem;Mem* memory_=&mem;uint32_t primary_buffer_ptr_=0,primary_buffer_size_=64;
 bool packet_fault_=false;int writes=0,ends=0;
 void WriteRegister(uint32_t,uint32_t){++writes;}
 void OnPrimaryBufferEnd(){++ends;}
 bool ExecutePacket(memory::RingBuffer* r){if(r->read_count()<4)return false;auto h=r->ReadAndSwap<uint32_t>();if(h==0)return true;return ExecutePacketType1(r,h);}
 bool ExecutePacketType1(memory::RingBuffer*,uint32_t);
 uint32_t ExecutePrimaryBuffer(uint32_t,uint32_t);
 void word(int i,uint32_t v){v=rex::byte_swap(v);__builtin_memcpy(mem.data+i*4,&v,4);}
};
'''+method('uint32_t CommandProcessor::ExecutePrimaryBuffer(')+'\n'+method('bool CommandProcessor::ExecutePacketType1(')+r'''
int main(){int n=0;auto check=[&](bool b){if(!b)std::abort();++n;};
 {CommandProcessor c;check(c.ExecutePrimaryBuffer(0,0)==0 && c.writes==0 && !c.packet_fault_);}
 {CommandProcessor c;c.word(0,0x40000000);check(c.ExecutePrimaryBuffer(0,1)==0 && c.writes==0 && c.packet_fault_ && c.ends==0);}
 {CommandProcessor c;c.word(0,0x40000000);c.word(1,12);check(c.ExecutePrimaryBuffer(0,2)==0 && c.writes==0 && c.packet_fault_);}
 {CommandProcessor c;c.word(0,0x40000000);c.word(1,12);c.word(2,13);check(c.ExecutePrimaryBuffer(0,3)==3 && c.writes==2 && !c.packet_fault_ && c.ends==1);}
 {CommandProcessor c;c.word(15,0x40000000);c.word(0,12);c.word(1,13);check(c.ExecutePrimaryBuffer(15,2)==2 && c.writes==2 && !c.packet_fault_);}
 {CommandProcessor c;c.word(0,0);c.word(1,0x40000000);check(c.ExecutePrimaryBuffer(0,2)==1 && c.writes==0 && c.packet_fault_ && c.ends==0);}
 printf("packet_boundary_checks=%d passed\n",n);
}
'''
(out/'check.cpp').write_text(code)
cc=r/'work/toolchains/llvm-mingw/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
q=subprocess.run([str(cc),'-std=c++23','-DNDEBUG','-I'+str(sdk/'include'),str(out/'check.cpp'),str(sdk/'src/core/ring_buffer.cpp'),'-o',str(out/'check.exe')],capture_output=True,text=True)
if q.returncode:print(q.stderr);raise SystemExit(q.returncode)
env=dict(os.environ);env['PATH']=str(cc.parent)+os.pathsep+env['PATH']
q=subprocess.run([str(out/'check.exe')],capture_output=True,text=True,env=env)
print(q.stdout,q.stderr)
(r/'outputs/mirrors-edge-native/xbox/gpu-port/packet-check-result.json').write_text(json.dumps(dict(exit=q.returncode,checks=6,output=q.stdout,scope='Host test of extracted SDK primary-ring and Type1 methods with actual SDK RingBuffer; packet dispatch and memory provider are harness fixtures; not full CommandProcessor or device execution.'),indent=2))
raise SystemExit(q.returncode)
