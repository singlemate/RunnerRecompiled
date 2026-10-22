"""Append original XEX resources to a private MEBOOT01 bundle. No game data in outputs/."""
from pathlib import Path
import struct,hashlib,json
root=Path.cwd();private=root/'work/xbox-game-private'
image=(private/'full-decoded-image.bin').read_bytes()
original=(private/'startup-sections.bin').read_bytes()
assert original[:8]==b'MEBOOT01'
count=struct.unpack_from('<I',original,8)[0];at=12
for _ in range(count):
    address,size=struct.unpack_from('<II',original,at);at+=8+size
assert at+4<=len(original)
header=(private/'xex-headers.bin').read_bytes();assert header[:4]==b'XEX2'
opts=dict(struct.unpack_from('>II',header,24+i*8)for i in range(struct.unpack_from('>I',header,20)[0]))
r=opts[0x2ff];length=struct.unpack_from('>I',header,r)[0]
assert length>=4 and (length-4)%16==0 and r+length<=len(header)
extra=bytearray();rows=[]
for pos in range(r+4,r+length,16):
    address,size=struct.unpack_from('>II',header,pos+8);start=address-0x82000000
    assert size and start>=0 and start+size<=len(image) and address+size<=0x83170000
    data=image[start:start+size]
    extra+=struct.pack('<II',address,size)+data
    rows.append(dict(address=hex(address),size=size,sha256=hashlib.sha256(data).hexdigest()))
assert count+len(rows)<=32
result=original[:8]+struct.pack('<I',count+len(rows))+original[12:at]+extra+original[at:]
dest=private/'startup-sections-resources.bin'
if dest.exists():assert dest.read_bytes()==result,'Existing private bundle differs'
else:
    with dest.open('xb')as f:f.write(result)
(root/'outputs/mirrors-edge-native/xbox/gpu-port/resource-bundle-result.json').write_text(json.dumps(dict(original_sections=count,new_sections=count+len(rows),resource_ranges=rows,private_bundle_bytes=len(result)),indent=2))
print('resource bundle checked',len(result),'bytes')
