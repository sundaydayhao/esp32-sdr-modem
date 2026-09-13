"""Build an attributed offline Shenzhen/Pearl River estuary vector map.

OSM source data is downloaded once from Overpass (no raster tile scraping).
The source JSON, its checksum and ODbL attribution are retained for reproducibility.
Run again with --refresh to deliberately update the regional extract.
"""
import argparse, gzip, hashlib, json, math, pathlib, urllib.request, urllib.parse
ROOT=pathlib.Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--refresh',action='store_true');a=p.parse_args()
source=ROOT/'build/shenzhen-osm-source.json'
bbox=(22.40,113.75,22.85,114.45)
query='''[out:json][timeout:90];(
way[highway~"^(motorway|trunk|primary|secondary|tertiary)$"](22.40,113.75,22.85,114.45);
way[natural=coastline](22.40,113.75,22.85,114.45);
way[waterway=river](22.40,113.75,22.85,114.45);
way[aeroway=runway](22.40,113.75,22.85,114.45);
node[place~"^(city|town|suburb)$"](22.40,113.75,22.85,114.45);
);out tags geom;'''
endpoint='https://overpass-api.de/api/interpreter'
if a.refresh or not source.exists():
    req=urllib.request.Request(endpoint,data=urllib.parse.urlencode({'data':query}).encode(),headers={'User-Agent':'SDRStudioMapBuilder/1.0','Accept-Encoding':'gzip'})
    with urllib.request.urlopen(req,timeout=155) as r:
        data=r.read(80*1024*1024+1)
        if r.headers.get('Content-Encoding')=='gzip':data=gzip.decompress(data)
    if len(data)>80*1024*1024:raise RuntimeError('Extract exceeds the build limit')
    parsed=json.loads(data)
    if not parsed.get('elements') or 'remark' in parsed:raise RuntimeError(parsed.get('remark','Empty map extract'))
    source.write_bytes(data)
data=source.read_bytes();osm=json.loads(data)
def simplify(points,eps=.00012):
    if len(points)<3:return points
    x,y=points[0];dx,dy=points[-1][0]-x,points[-1][1]-y;d=dx*dx+dy*dy
    index=0;best=0
    for i,(a,b) in enumerate(points[1:-1],1):
        t=max(0,min(1,((a-x)*dx+(b-y)*dy)/d)) if d else 0
        dist=(a-x-t*dx)**2+(b-y-t*dy)**2
        if dist>best:best=dist;index=i
    return simplify(points[:index+1],eps)[:-1]+simplify(points[index:],eps) if best>eps*eps else [points[0],points[-1]]
paths=[];places=[]
for e in osm['elements']:
    tags=e.get('tags',{})
    if e['type']=='node':
        name=tags.get('name:zh',tags.get('name',''))
        if name:places.append({'name':name,'lat':e['lat'],'lon':e['lon'],'rank':0 if tags.get('place')=='city' else 1 if tags.get('place')=='town' else 2})
    elif e.get('geometry'):
        kind=0 if tags.get('natural')=='coastline' else 1 if tags.get('waterway') else 2 if tags.get('aeroway') else 3 if tags.get('highway') in ['motorway','trunk'] else 4 if tags.get('highway')=='primary' else 5
        xy=simplify([(g['lon'],g['lat']) for g in e['geometry']])
        if len(xy)>1:paths.append({'kind':kind,'name':tags.get('name:zh',tags.get('name','')),'points':[[round(x*100000),round(y*100000)] for x,y in xy]})
# Landmarks use published aerodrome coordinates; data positions stay WGS84.
places.extend([{'name':'深圳宝安机场','lat':22.6393,'lon':113.8107,'rank':0},
               {'name':'香港国际机场','lat':22.3080,'lon':113.9185,'rank':0}])
places.sort(key=lambda v:(v['rank'],v['name']))
meta={'source':'OpenStreetMap contributors','license':'ODbL-1.0','attribution':'© OpenStreetMap contributors',
      'url':'https://www.openstreetmap.org/copyright','endpoint':endpoint,'query':query,
      'source_sha256':hashlib.sha256(data).hexdigest(),'timestamp':osm.get('osm3s',{}).get('timestamp_osm_base'),
      'bbox':bbox,'scale':100000,'paths':paths,'places':places}
out=ROOT/'main/sdr_map_region.json';out.write_text(json.dumps(meta,ensure_ascii=False,separators=(',',':')),encoding='utf-8')
(ROOT/'main/sdr_map_places.json').write_text(json.dumps(places,ensure_ascii=False,indent=2),encoding='utf-8')
vertices=[];features=[]
for path in paths:
    pts=path['points'];features.append((len(vertices),len(pts),path['kind'],min(p[0] for p in pts),min(p[1] for p in pts),max(p[0] for p in pts),max(p[1] for p in pts)))
    vertices.extend(pts)
header='/* Generated from OSM by tools/build_regional_map.py; ODbL-1.0. See sdr_map_region.json. */\n'
header+='static const int32_t regional_points[][2]={\n'+''.join('{%d,%d},'%tuple(p) for p in vertices)+'\n};\n'
header+='static const struct {uint32_t start;uint16_t count;uint8_t kind;int32_t west,south,east,north;} regional_features[]={\n'
header+=''.join('{%d,%d,%d,%d,%d,%d,%d},\n'%f for f in features)+'};\n'
header+='static const sdr_map_place_t regional_places[]={\n'
header+=''.join('{%s,%.6f,%.6f,%d},\n'%(json.dumps(p['name'],ensure_ascii=False),p['lat'],p['lon'],p['rank']) for p in places)+'};\n'
(ROOT/'main/sdr_map_region.inc').write_text(header,encoding='utf-8')
print(json.dumps({k:v for k,v in meta.items() if k not in ['paths','places','query']},ensure_ascii=True))
print('features',len(features),'vertices',len(vertices),'places',len(places),'json_bytes',out.stat().st_size)
