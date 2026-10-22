from pathlib import Path
import re,json,sys,collections,math
source=json.loads(Path(sys.argv[1]).read_text(encoding='utf-8-sig'));s=Path(sys.argv[2]).read_text(errors='replace');by=collections.defaultdict(list)
for b in source['blocks']:by[b['slot']].append(b)
rows=[]
for line in s.splitlines():
 if 'external_lightmap_image ' not in line:continue
 d=dict(re.findall(r'(\w+)=([^ ]+)',line));slot,index=int(d['slot']),int(d['index']);actual=[float(x) for x in d['rgba'].split(',')];expected=by[slot][index]['first_texel'];error=max(abs(a-b) for a,b in zip(actual,expected));rows.append(dict(slot=slot,index=index,actual=actual,expected=expected,error=error,view_null=d['view_null']))
summary={}
for slot in range(3):
 a=[r for r in rows if r['slot']==slot];summary[slot]=dict(records=len(a),max_error=max((r['error'] for r in a),default=None),over_tolerance=sum(r['error']>0.005 for r in a),nonblack=sum(max(r['actual'][:3])>0 for r in a),null_views=sum(r['view_null'] in ['true','1'] for r in a))
print(json.dumps(dict(exit=re.findall(r'probe_exit=\d+',s),summary=summary,mismatches=[r for r in rows if r['error']>0.005][:12],limitations='Mip0 integer texelFetch at64 points, layer0,unsigned active views; does not validate guest UV, filtered sampling or lighting ALU.'),indent=2))
