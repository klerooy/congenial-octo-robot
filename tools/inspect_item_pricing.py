from pathlib import Path
import struct,json,hashlib
folder=Path(r'C:\Program Files (x86)\Steam\steamapps\common\Rob Run')
meta=(folder/'ROB RUN_Data/il2cpp_data/Metadata/global-metadata.dat').read_bytes();dll=(folder/'GameAssembly.dll').read_bytes()
assert struct.unpack_from('<II',meta)==(0xFAB11BAF,39)
so,ss,_=struct.unpack_from('<III',meta,0x20);strings=meta[so:so+ss]
to,ts,tn=struct.unpack_from('<III',meta,0xEC);fo,fs,fn=struct.unpack_from('<III',meta,0x8C)
assert ts==tn*82 and fs==fn*12
def text(i):return strings[i:strings.index(b'\0',i)].decode('utf-8',errors='replace')
nt=struct.unpack_from('<I',dll,0x3C)[0];op=nt+24;imagebase=struct.unpack_from('<Q',dll,op+24)[0];imagesize=struct.unpack_from('<I',dll,op+56)[0]
sec=[]
for i in range(struct.unpack_from('<H',dll,nt+6)[0]):
 p=op+struct.unpack_from('<H',dll,nt+20)[0]+i*40;vs,va,rs,ro=struct.unpack_from('<IIII',dll,p+8);sec.append((va,rs,ro))
def fileoff(addr):
 for va,rs,ro in sec:
  if va<=addr-imagebase<va+rs:return ro+addr-imagebase-va
 raise ValueError(hex(addr))
def q(addr):return struct.unpack_from('<Q',dll,fileoff(addr))[0]
registrations=[];pos=0
while True:
 pos=dll.find(struct.pack('<Q',tn),pos)
 if pos<0:break
 if struct.unpack_from('<Q',dll,pos+16)[0]==tn:
  try:
   ptr=struct.unpack_from('<Q',dll,pos+8)[0];offs=q(ptr+4312*8)
   if struct.unpack_from('<8i',dll,fileoff(offs))==(0x10,0x18,0x20,0x28,0x30,0x38,0x40,0x48):registrations.append(pos-80)
  except (ValueError,struct.error):pass
 pos+=1
assert len(registrations)==1
reg=registrations[0];types_count,types_array=struct.unpack_from('<QQ',dll,reg+48);fields_array=struct.unpack_from('<Q',dll,reg+88)[0]
kindnames={2:'bool',8:'int32',10:'int64',12:'float',13:'double',14:'string',17:'valuetype',18:'class',21:'generic'}
def typename(index):
 p=to+index*82;ni,ns=struct.unpack_from('<II',meta,p);return text(ns)+'.'+text(ni)
def typedesc(addr,depth=0):
 data,bits=struct.unpack_from('<QI',dll,fileoff(addr));kind=(bits>>16)&255
 result={'kind':kindnames.get(kind,hex(kind))}
 if kind in (17,18) and data<tn:result['name']=typename(data)
 if kind==21 and depth<3:
  try:
   core,inst,_=struct.unpack_from('<QQQ',dll,fileoff(data));result['definition']=typedesc(core,depth+1)
   argc=struct.unpack_from('<I',dll,fileoff(inst))[0];args=q(inst+8)
   if argc<16:result['arguments']=[typedesc(q(args+8*i),depth+1) for i in range(argc)]
  except (ValueError,struct.error):result['generic_data']=hex(data)
 return result
report={'game_assembly_sha256':hashlib.sha256(dll).hexdigest(),'metadata_version':39,'image_size':imagesize,'types':{}}
for ti in range(tn):
 name=typename(ti)
 if name not in ['Models.References.Inventory.InventoryItemDescription','Models.References.Inventory.InventoryItemSellPriceModifier','System.Delegate'] and name.rsplit('.',1)[-1] not in {'GlobalModel','PartyModel','PartyData','PlayerPartyData','PartyMemberModel','PartyMemberData','MemberBattleData','PlayerMemberBattleDataModel','PartyMemberBattleData'}:continue
 p=to+ti*82;start=struct.unpack_from('<I',meta,p+26)[0];count=struct.unpack_from('<H',meta,p+62)[0];offsets=q(fields_array+ti*8)
 rows=[]
 for j in range(count):
  ni,ty,token=struct.unpack_from('<III',meta,fo+(start+j)*12);offset=struct.unpack_from('<i',dll,fileoff(offsets)+j*4)[0]
  row={'name':text(ni),'offset':offset,'type_index':ty,'type':typedesc(q(types_array+ty*8))}
  rows.append(row)
  if 'Party' in name or 'MemberBattleData' in name or (name.endswith('.GlobalModel') and 'Party' in row['name']):print(name,row)
 report['types'][name]=rows
out=Path(r'C:\Programs\RobRunScripts\RRTool_Public_NoKey\offsets');out.mkdir(exist_ok=True)
(out/'item_pricing_fields.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('Validated pricing/Delegate field report saved')
