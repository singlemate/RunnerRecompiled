from pathlib import Path
import re,struct,tomllib,json,bisect
r=Path.cwd();g=r/'work/xbox-game-private/generated';private=r/'work/xbox-game-private';code=(private/'code.bin').read_bytes();base=0x82230000;end=base+len(code)
config=tomllib.loads((private/'mirrors-edge.toml').read_text());tables=tomllib.loads((private/'switches.toml').read_text())['switch']
pdata=next(private.glob(f'section.{0x821d1400}.bin')).read_bytes();pdstarts=sorted({struct.unpack_from('>I',pdata,i)[0] for i in range(0,len(pdata),8)})
def word(a):
 assert base<=a<end and a%4==0
 return struct.unpack_from('>I',code,a-base)[0]
def signed(n,bits):return n-(1<<bits) if n&(1<<(bits-1)) else n
switches={}
for t in tables:
 if not base<=t['base']<end:continue
 for a in range(t['base'],min(t['base']+128,end),4):
  w=word(a)
  if w>>26==19 and (w>>1)&1023==528 and not w&1:
   assert a not in switches or switches[a]==t['labels']
   switches[a]=t['labels'];break
# Find current function owning each failing switch, not just the maximum target.
owners={}
for p in g.glob('ppc_recomp.*.cpp'):
 text=p.read_text();matches=list(re.finditer(r'PPC_FUNC_IMPL\(__imp__(\w+)\) \{',text))
 for i,m in enumerate(matches):
  body=text[m.end():matches[i+1].start() if i+1<len(matches) else len(text)]
  if '// unresolved switch target' not in body:continue
  # The block's guest address is carried by the generated function symbol.
  assert m[1].startswith('sub_'),m[1]
  owners[int(m[1][4:],16)]=sorted(set(int(x,16) for x in re.findall(r'// ERROR: 0x([0-9A-F]+)',body)))
proposals=[];rejected=[]
for start,targets in sorted(owners.items()):
 seen=set();pending=[start];external=[];unresolved=[]
 # Existing pdata function starts are hard boundaries for adjacent routines.
 nextpd=next((x for x in pdstarts if x>start),end);limit=min(end,start+0x20000,nextpd)
 try:
  while pending:
   a=pending.pop()
   if a in seen:continue
   if a<start:external.append(a);continue
   if a>=limit:raise ValueError(f'edge crosses hard bound: {a:08X}')
   w=word(a);seen.add(a);op=w>>26;link=w&1
   if w==0:continue
   if op==18 and not link:
    dest=signed(w&0x3fffffc,26);dest=(dest if w&2 else a+dest)&0xffffffff
    if dest in pdstarts and dest!=start:external.append(dest)
    else:pending.append(dest)
   elif op==16 and not link:
    dest=signed(w&0xfffc,16);dest=(dest if w&2 else a+dest)&0xffffffff
    if dest in pdstarts and dest!=start:external.append(dest)
    else:pending.append(dest)
    pending.append(a+4)
   elif op==19 and ((w>>1)&1023) in (16,528) and not link:
    xo=(w>>1)&1023;bo=(w>>21)&31
    if xo==528:
     if a in switches:pending.extend(switches[a])
     else:unresolved.append(a)
    if (bo&20)!=20:pending.append(a+4)
   else:pending.append(a+4)
  assert all(t in seen for t in targets),'switch target not reached by recovered CFG'
  assert not unresolved,'unknown indirect jump in CFG'
  high=max(seen)+4
  assert not any(start<x<high for x in pdstarts),'crossed pdata function start'
  proposals.append(dict(address=start,size=high-start,reachable_instructions=len(seen),external_tail_targets=sorted(set(external)),switch_targets_checked=len(targets),hard_limit=limit))
 except (ValueError,AssertionError) as e:rejected.append(dict(address=start,reason=str(e)))
result=dict(method='Bounded CFG traversal with decoded direct edges and recovered switch edges; no crossing next pdata entry; unknown indirect jumps rejected',proposals=proposals,rejected=rejected)
(r/'outputs/mirrors-edge-native/xbox/cfg-boundary-proposals.json').write_text(json.dumps(result,indent=2));print(json.dumps(dict(owners=len(owners),accepted=len(proposals),rejected=rejected),indent=2))
# Save proposals for review; application is a separate step.
(private/'cfg-proposals.toml').write_text('\n'.join(f'[[main.functions]]\naddress = 0x{x["address"]:08x}\nsize = 0x{x["size"]:x}\n' for x in proposals))
