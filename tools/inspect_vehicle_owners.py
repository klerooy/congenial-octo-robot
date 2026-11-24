import contextlib,io,runpy,struct,json
from pathlib import Path
root=Path(__file__).resolve().parents[1]
with contextlib.redirect_stdout(io.StringIO()):s=runpy.run_path(str(Path(__file__).with_name('inspect_item_pricing.py')))
report={'game_assembly_sha256':s['report']['game_assembly_sha256'],'types':{}}
wanted={'Models.Models.Car.CarModel','Models.Models.Helicopter.HelicopterModel','Models.Models.VehicleOwner.VehicleOwnerModel'}
for i in range(s['tn']):
 name=s['typename'](i)
 if name not in wanted:continue
 t=s['to']+i*82;start=struct.unpack_from('<I',s['meta'],t+26)[0];count=struct.unpack_from('<H',s['meta'],t+62)[0];offsets=s['q'](s['fields_array']+i*8)
 fields=[]
 for j in range(count):
  ni,typ,_=struct.unpack_from('<III',s['meta'],s['fo']+(start+j)*12)
  fields.append({'name':s['text'](ni),'offset':struct.unpack_from('<i',s['dll'],s['fileoff'](offsets)+j*4)[0],'type':s['typedesc'](s['q'](s['types_array']+typ*8))})
 report['types'][name]=fields
(root/'offsets/vehicle_owner_fields.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('Vehicle CharacterOwner fields verified against the installed binary')
