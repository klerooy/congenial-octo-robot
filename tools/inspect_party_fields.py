import contextlib,io,runpy,struct,json
from pathlib import Path
root=Path(__file__).resolve().parents[1]
with contextlib.redirect_stdout(io.StringIO()):data=runpy.run_path(str(Path(__file__).with_name('inspect_item_pricing.py')))
report={'game_assembly_sha256':data['report']['game_assembly_sha256'],'types':{}}
wanted={'Game.Battle.BattleModel','Game.Battle.Party.ClientPartyMemberAccessModel','Game.Global.Party.PartyModel','Game.Global.Party.PartyMemberModel','Models.Data.Party.BattlePartyMemberData'}
for i in range(data['tn']):
 name=data['typename'](i)
 if name not in wanted:continue
 p=data['to']+i*82;start=struct.unpack_from('<I',data['meta'],p+26)[0];count=struct.unpack_from('<H',data['meta'],p+62)[0];offsets=data['q'](data['fields_array']+i*8)
 fields=[]
 for j in range(count):
  ni,typ,_=struct.unpack_from('<III',data['meta'],data['fo']+(start+j)*12)
  fields.append({'name':data['text'](ni),'offset':struct.unpack_from('<i',data['dll'],data['fileoff'](offsets)+j*4)[0],'type':data['typedesc'](data['q'](data['types_array']+typ*8))})
 report['types'][name]=fields
(root/'offsets/party_fields.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('Verified BattleModel -> Party access -> actual members -> entity ID fields')
