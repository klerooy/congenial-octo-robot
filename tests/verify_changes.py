"""Offline regression checks. Run after build_port.bat from an x64 VS Developer shell."""
from pathlib import Path
from html.parser import HTMLParser
import json
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1] / '2_src_port'

def run(*args, cwd=root):
    subprocess.run(args, cwd=cwd, check=True)

def function(text, signature):
    start = text.index(signature)
    body = text.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

class Inputs(HTMLParser):
    def __init__(self):
        super().__init__()
        self.inputs = []
    def handle_starttag(self, tag, attrs):
        if tag in {'input', 'select', 'button'}:
            self.inputs.append(dict(attrs))

html = (root / 'menu.html').read_text(encoding='utf-8')
parser = Inputs()
parser.feed(html)
settings = set(re.findall(r'X\((\w+),', (root / 'menu_bridge.h').read_text()))
for control in parser.inputs:
    if 'data-key' in control:
        assert control['data-key'] in settings, control
for key in ('aim_humanize', 'aim_humanize_strength', 'aim_humanize_speed',
            'aim_bone_random', 'aim_bone_interval', 'aa_los_on', 'trigger_los'):
    assert any(control.get('data-key') == key for control in parser.inputs), key
for name, key in [('show_loot_ents', 'show_loot'), ('show_cops', 'show_cops'),
                  ('show_robbers', 'show_robbers'), ('show_civilians', 'show_civilians'),
                  ('aim_enabled','aim_enabled'), ('aim_fov','aim_fov'), ('aim_smooth','aim_smooth'),
                  ('trigger_enabled','trigger_enabled'), ('trigger_key','trigger_key'),
                  ('trigger_delay','trigger_delay'), ('show_spread_visualizer','show_spread_visualizer'),
                  ('esp_health_bar','show_health'),('esp_weapon_icon','esp_weapon_icon'),
                  ('esp_role_label','esp_role_label'),('esp_nickname','show_name'),('esp_distance','show_distance'),
                  ('esp_box_thickness','line_w'),('esp_text_distance','esp_text_distance'),
                  ('show_npcs','show_npcs'),('show_prisoners','show_prisoners'),
                  ('esp_weapon_mk','esp_weapon_mk'),('show_loot_price','show_loot_price'),
                  ('loot_price_source','loot_price_source'),('show_party','show_party'),
                  ('show_vehicle_owner','show_vehicle_owner')]:
    matches = [c for c in parser.inputs if c.get('id') == name]
    assert len(matches) == 1 and matches[0]['data-key'] == key

# Every translation unit the overlay build compiles must also appear in the
# selftest link list, or tests\run_checks.bat fails with unresolved symbols.
build_text = (root / 'build_port.bat').read_text(encoding='utf-8')
built = set(re.findall(r'/c\s+(\w+)\.c\b', build_text)) | set(re.findall(r'/c\s+\w+\s+(\w+)\.cpp\b', build_text))
listed = set(re.findall(r"'(\w+)'", next(
    line for line in Path(__file__).read_text(encoding='utf-8').splitlines()
    if line.strip().startswith('objects = '))))
assert built, 'no compiled sources found in build_port.bat'
assert not (built - listed), f'build_port.bat sources missing from selftest objects: {sorted(built - listed)}'

# The internal menu is a second, hand-written schema: every new aim setting
# needs a field, a menu row and an ini save/load key there as well.
internal = (root / 'rr_int.cpp').read_text(encoding='utf-8')
for key in ('aim_humanize', 'aim_humanize_strength', 'aim_humanize_speed',
            'aim_bone_random', 'aim_bone_interval', 'aim_los'):
    assert f'g_cfg.{key}' in internal, key
    assert f'"{key}"' in internal, key

source = (root / 'rr_game.c').read_text(encoding='utf-8')
cache_defs = source[source.index('#define RR_MAX_LOOT_ENTS'):source.index('static void cache_loot_entities(void);')]
c_functions = function(source, 'static void cache_loot_entities(void) {') + '\n' + function(source, 'static void snapshot_read_spread(uint64_t character, RR_PLAYER* out) {') + '\n' + function(source, 'static void cache_roster_npcs(ENT_CACHE* stage, int count) {') + '\n' + function(source, 'static int loot_delegate_price(uint64_t function, int* out) {') + '\n' + function(source, 'static void loot_read_prices(uint64_t inventory, int* base, int* npc, unsigned* flags) {') + '\n' + function(source, 'static void cache_roster_party(ENT_CACHE* stage, int count) {')
c_functions += '\n' + function(source, 'static int vehicle_owner_id(uint64_t object, int kind, int64_t* out) {')
c_functions += '\n' + function(source, 'static void cache_vehicle_owner_names(VEH_CACHE* vehicles, int count) {')
with tempfile.TemporaryDirectory(prefix='rr_checks_', dir=root.parent / 'tests') as tmp:
    work = Path(tmp)
    script = re.search(r'<script>([\s\S]*?)</script>', html).group(1)
    (work / 'menu.js').write_text(script, encoding='utf-8')
    run('node', '--check', str(work / 'menu.js'))
    js = '''const assert = require('assert');
const sent = [];
const send = message => sent.push(message);
const controls = [
 {type:'checkbox', checked:false, dataset:{key:'show_cops'}},
 {type:'range', min:'0', max:'2000', value:'150', dataset:{key:'esp_text_distance',scale:'100',unit:' m'}},
 {type:'checkbox',checked:false,dataset:{key:'aim_targets',bit:'1'}},
 {type:'checkbox',checked:false,dataset:{key:'aim_targets',bit:'2'}},
 {type:'checkbox',checked:false,dataset:{key:'trigger_targets',bit:'1'}},
 {type:'checkbox',checked:false,dataset:{key:'trigger_targets',bit:'2'}}
];
for (const c of controls) {
 c.style={setProperty(){}};
 c.parentElement={querySelector(){return null;}};
 c.addEventListener=(event, callback)=>{c.fire=callback;};
}
const swatches=[];
'''
    js += '\n'.join(function(script, signature) for signature in ['function paintRange(input)', 'function wire(input)', 'function applyState(config)'])
    js += '''
controls.forEach(wire);
controls[0].checked=true; controls[0].fire();
controls[1].value='250'; controls[1].fire();
assert.deepStrictEqual(sent, ['set:show_cops:1','set:esp_text_distance:25000']);
applyState({show_cops:0,esp_text_distance:0});
assert.strictEqual(controls[0].checked,false);
assert.strictEqual(controls[1].value,0);
controls[2].checked=controls[3].checked=true; controls[2].fire();
assert.strictEqual(sent[sent.length-1],'set:aim_targets:3');
controls[3].checked=false; controls[3].fire();
assert.strictEqual(sent[sent.length-1],'set:aim_targets:1');
applyState({aim_targets:2,trigger_targets:1});
assert.strictEqual(controls[2].checked,false); assert.strictEqual(controls[3].checked,true);
assert.strictEqual(controls[4].checked,true); assert.strictEqual(controls[5].checked,false);
console.log('MENU CHECKS OK: schema, toggles, label range, target masks and state synchronization');
'''
    (work / 'menu_check.js').write_text(js, encoding='utf-8')
    run('node', str(work / 'menu_check.js'))

    harness = r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "rr_game.h"
#include "menu_bridge.h"
#include "robrun_offsets.h"
#undef RR_OFF_CONTAINER_POS
#undef RR_OFF_VEHICLE_LOOT_POS
#include "robrun_sdk_offsets.h"
#define RR_OFF_PRIM_VALUE 0x18
#define RR_OFF_REF_VALUE 0x18
#define RR_OFF_DESC_NAME 0x10
typedef struct { int32_t id; int64_t public_id; int is_npc, in_party, local; char role[20], name[RR_NAME_MAX]; } ENT_CACHE;
typedef struct { int64_t owner_public_id; int owner_known; char owner_name[RR_NAME_MAX]; } VEH_CACHE;
static ENT_CACHE g_cache[4];
static int g_ncache;
static unsigned char mem[0x40000];
#define PL(a) ((uint64_t)(a) >= 0x100 && (uint64_t)(a) < sizeof(mem))
static uint64_t g_world = 0x1000, g_k_world = 0x200, g_k_char = 0x201, g_battle = 0x18000;
static uint64_t local_addr = 0x2000, failed_write;
static uint64_t g_module=0x28000,g_battle_model=0x31000;
static int writes, g_cs_ready;
static CRITICAL_SECTION g_cache_cs;
static BR_Config config;
static struct { uint64_t addr; const char* name; } classes[] = {
 {0x3000,"BattleCharacterPrivateModel"}, {0x4000,"PrimitiveModel`1"}, {0x5000,"BoolData"},
 {0x6000,"PrimitiveModel`1"}, {0x7000,"BoolData"},
 {0x10000,"RandomLootObjectModel"}, {0x9000,"RandomLootObjectDescription"}, {0x9100,"BoolData"},
 {0x11000,"CashLootNodeModel"}, {0x12000,"LootModel"}, {0x13000,"VehicleLootModel"},
 {0x14000,"PositionModel"}, {0x15000,"InteractPositionModel"},
 {0x18000,"BattleEntitiesModel"}, {0x19000,"PlayerEntityModel"}, {0x1A000,"PlayerClientModel"},
 {0x1B000,"PlayerFakeSpreadModel"}, {0x1C000,"PrimitiveModel`1"}, {0x1D000,"FloatData"},
 {0x1E000,"LocationNpcDescription"},
 {0x22000,"Func`1"},{0x22100,"Func`1"},{0x24000,"InventoryItemDescription"},
 {0x31000,"BattleModel"},{0x32000,"ClientPartyMemberAccessModel"},{0x33000,"PartyModel"},
 {0x34000,"PartyMemberModel"},{0x35000,"BattlePartyMemberData"},{0x36000,"IntData"},
 {0x37000,"CarModel"},{0x38000,"HelicopterModel"},{0x39000,"VehicleOwnerModel"},
 {0x21000,"BattleCharacterSlotData"}
};
static struct { uint64_t addr, rows; int count, stride; } dicts[] = {
 {0x8000,0xA000,1,0x18}, {0x8100,0xA100,1,0x18},
 {0x8200,0xA200,1,0x18}, {0x8300,0xA300,1,0x18}, {0x8400,0xA400,1,0x18}, {0x8500,0xA500,1,0x18},
 {0x8600,0xA600,1,0x18}
};
static void put64(uint64_t at,uint64_t v) { memcpy(mem+at,&v,8); }
static void put32(uint64_t at,int v) { memcpy(mem+at,&v,4); }
static void putpos(uint64_t at,float x,float y,float z) { RR_V3 v={x,y,z}; memcpy(mem+at,&v,sizeof(v)); }
static uint64_t rr_u64(uint64_t at) { uint64_t v=0; if(PL(at)&&at+8<=sizeof(mem))memcpy(&v,mem+at,8); return v; }
static int rr_i32(uint64_t at) { int v=0; if(PL(at)&&at+4<=sizeof(mem))memcpy(&v,mem+at,4); return v; }
static int rr_read(uint64_t at,void* out,size_t n) { if(!PL(at)||at+n>sizeof(mem)||n>0x10000)return 0; memcpy(out,mem+at,n); return 1; }
static int rr_write(uint64_t at,const void* value,size_t n) { if(!PL(at)||at+n>sizeof(mem)||at==failed_write)return 0; memcpy(mem+at,value,n); ++writes; return 1; }
static int u_is(uint64_t addr,const char* name) { for(unsigned i=0;i<sizeof(classes)/sizeof(classes[0]);++i)if(classes[i].addr==addr)return !strcmp(classes[i].name,name); return 0; }
static int u_string(uint64_t addr,char* out,size_t n) { if(!PL(addr))return 0; snprintf(out,n,"%s",(char*)mem+addr); return 1; }
static int u_dict(uint64_t addr,int* n,uint64_t* rows,int* stride) { for(unsigned i=0;i<sizeof(dicts)/sizeof(dicts[0]);++i)if(dicts[i].addr==addr){*n=dicts[i].count;*rows=dicts[i].rows;*stride=dicts[i].stride;return 1;}return 0; }
BR_Config* br_get(void) { return &config; }
uint64_t rr_local_player(void) { return local_addr; }
'''
    harness += cache_defs + '\n' + c_functions
    harness += r'''
int main(void) {
 put64(local_addr,g_k_char);put64(local_addr+RR_OFF_CHAR_PRIVATE,0x3000);
 put64(g_world,g_k_world);
 put64(g_world+RR_OFF_WEM_RANDOM_LOOT,0x8000); put64(g_world+RR_OFF_WEM_CASH_LOOT,0x8100);
 put64(g_world+RR_OFF_WEM_LOOTS,0x8200); put64(g_world+RR_OFF_WEM_VEHICLE_LOOT,0x8300);
 for(int i=0;i<4;++i){put32(dicts[i].rows+8,21+i);put64(dicts[i].rows+0x10,0x10000+i*0x1000);}
 put64(0x10010,0x9100); put64(0x10020,0x9000); put32(0x9020,21); putpos(0x9024,10,20,30);
 putpos(0x11010,40,50,60);
 put64(0x12000+RR_OFF_DROPPED_LOOT_POS,0x14000); putpos(0x14010,70,80,90);
 put64(0x13000+RR_OFF_VEHICLE_LOOT_POS,0x15000); putpos(0x15010,100,110,120);
 config.show_loot=config.show_loot_nodes=1;
 cache_loot_entities(); assert(g_nloot_ents==4&&g_loot_ents_world==g_world);
 assert(g_loot_ents[0].kind==1&&g_loot_ents[0].pos.x==10&&g_loot_ents[0].obj==0x10000);
 assert(g_loot_ents[1].kind==2&&g_loot_ents[1].pos.x==40);
 assert(g_loot_ents[2].kind==5&&g_loot_ents[2].pos.x==70);
 assert(g_loot_ents[3].kind==6&&g_loot_ents[3].pos.x==100);
 mem[0x9118]=1; cache_loot_entities(); assert(g_nloot_ents==3); mem[0x9118]=0;
 putpos(0x9024,NAN,0,0); cache_loot_entities(); assert(g_nloot_ents==3); putpos(0x9024,10,20,30);
 dicts[0].stride=0x20; cache_loot_entities(); assert(g_nloot_ents==3); dicts[0].stride=0x18;
 put32(0xA000,-1); cache_loot_entities(); assert(g_nloot_ents==3); put32(0xA000,0);
 dicts[0].count=4097; cache_loot_entities(); assert(g_nloot_ents==3); dicts[0].count=1;
 config.show_loot=0; cache_loot_entities(); assert(g_nloot_ents==1&&g_loot_ents[0].kind==2);
 puts("LOOT CHECKS OK: independent sources, renderer kinds, taken/deleted entries, NaN, bounds and filters");

 static RR_PLAYER spread;
 int before_reads=writes;
 put64(g_battle+RR_OFF_BATTLE_WORLD,g_world); put64(g_battle+RR_OFF_BATTLE_PLAYER,0x19000);
 put64(0x19000+RR_OFF_PLAYER_SERVER,local_addr); put64(0x19000+RR_OFF_PLAYER_CLIENT,0x1A000);
 put64(0x1A000+RR_OFF_CLIENT_FAKE_SPREAD,0x1B000); putpos(0x1B000+RR_OFF_FAKE_SPREAD_VALUES,0.03f,0.004f,0.005f);
 put64(0x3000+RR_OFF_PRIV_WEAPON_SPREAD,0x1C000); put64(0x1C010,0x1D000); putpos(0x1D018,0.02f,0,0);
 snapshot_read_spread(local_addr,&spread);
 assert(spread.has_spread_data&&spread.has_fake_spread_data&&fabsf(spread.spread_value-0.02f)<0.00001f);
 assert(fabsf(spread.spread_air-0.004f)<0.00001f&&fabsf(spread.spread_move-0.005f)<0.00001f);
 /* The nospread pass writes where the values came from, so the reader has to
  * publish those addresses and clear them when a link fails. */
 assert(spread.spread_addr==0x1D018&&spread.fake_spread_addr==0x1B000+RR_OFF_FAKE_SPREAD_VALUES);
 putpos(0x1D018,NAN,0,0); snapshot_read_spread(local_addr,&spread);
 assert(spread.has_spread_data&&fabsf(spread.spread_value-0.03f)<0.00001f);
 assert(spread.spread_addr==0&&spread.fake_spread_addr==0x1B000+RR_OFF_FAKE_SPREAD_VALUES);
 put64(0x19000+RR_OFF_PLAYER_SERVER,0x999); snapshot_read_spread(local_addr,&spread);
 assert(!spread.has_spread_data&&!spread.has_fake_spread_data);
 assert(!spread.spread_addr&&!spread.fake_spread_addr);
 put64(0x19000+RR_OFF_PLAYER_SERVER,local_addr); putpos(0x1B000+RR_OFF_FAKE_SPREAD_VALUES,NAN,0,0);
 snapshot_read_spread(local_addr,&spread); assert(!spread.has_spread_data&&!spread.has_fake_spread_data);
 assert(writes==before_reads);
 puts("SPREAD CHECKS OK: correct entity/client chain, Spread/Air/Move layout, NaN, fallback and stale identity; zero writes");
 ENT_CACHE roster[2]={{0}};roster[0].id=7;roster[1].id=8;
 snprintf(roster[0].role,sizeof(roster[0].role),"cop");snprintf(roster[1].role,sizeof(roster[1].role),"civilian");
 put64(g_world+RR_OFF_WEM_NPC_DESCRIPTIONS,0x8400);put32(0xA408,7);put64(0xA410,0x1E000);
 cache_roster_npcs(roster,2);assert(roster[0].is_npc&&!roster[1].is_npc);
 put32(0xA400,-1);cache_roster_npcs(roster,2);assert(!roster[0].is_npc&&!roster[1].is_npc);
 memset(mem+0xA400,0,2731*0x18);dicts[4].count=2731;
 put32(0xA400+2730*0x18+8,7);put64(0xA400+2730*0x18+0x10,0x1E000);
 cache_roster_npcs(roster,2);assert(roster[0].is_npc&&!roster[1].is_npc);
 puts("NPC CHECKS OK: dictionary membership, ordinary remote player exclusion, deleted entries and chunked reads");
 put64(0x31000+RR_OFF_BATTLE_PARTY_ACCESS,0x32000);
 put64(0x32000+RR_OFF_PARTY_ACCESS_WORLD,g_world);put64(0x32000+RR_OFF_PARTY_ACCESS_MODEL,0x33000);
 mem[0x33000+RR_OFF_PARTY_HAS_GROUP]=1;put64(0x33000+RR_OFF_PARTY_ACTUAL_MEMBERS,0x8500);
 roster[0].public_id=777;roster[1].public_id=888;
 put32(0xA500,0);put64(0xA508,777);put64(0xA510,0x34000);put64(0x34000+RR_OFF_PARTY_MEMBER_DATA,0x35000);
 put64(0x35000+RR_OFF_PARTY_MEMBER_ENTITY,0x36000);put32(0x36018,7);
 cache_roster_party(roster,2);assert(roster[0].in_party&&!roster[1].in_party);
 put64(0xA508,999);cache_roster_party(roster,2);assert(!roster[0].in_party);put64(0xA508,777);
 mem[0x33000+RR_OFF_PARTY_HAS_GROUP]=0;cache_roster_party(roster,2);assert(!roster[0].in_party);
 mem[0x33000+RR_OFF_PARTY_HAS_GROUP]=1;put32(0xA500,-1);cache_roster_party(roster,2);assert(!roster[0].in_party);
 puts("PARTY CHECKS OK: member public/entity ID match, cross-location ID collision, leave-party reset and deleted entries");

 int base_price,npc_price;unsigned price_flags;
 put64(0x22010,0x30000);put64(0x22110,0x30100);put64(0x22120,0x23000);put64(0x23000,0x200);
 put64(0x24000+RR_OFF_ITEM_BASE_PRICE_FUNC,0x22000);put64(0x24000+RR_OFF_ITEM_SELL_PRICE_FUNC,0x22100);
 mem[0x24000+RR_OFF_ITEM_CAN_SELL]=1;
 mem[0x30000]=0xB8;put32(0x30001,1200);mem[0x30005]=0xC3;
 mem[0x30100]=0xB8;put32(0x30101,700);mem[0x30105]=0xC3;
 loot_read_prices(0x24000,&base_price,&npc_price,&price_flags);assert(price_flags==3&&base_price==1200&&npc_price==700);
 mem[0x24000+RR_OFF_ITEM_CAN_SELL]=0;
 loot_read_prices(0x24000,&base_price,&npc_price,&price_flags);assert(price_flags==RR_LOOT_PRICE_BASE&&npc_price==0);
 mem[0x24000+RR_OFF_ITEM_CAN_SELL]=1;
 mem[0x30100]=0x8B;mem[0x30101]=0x41;mem[0x30102]=0x10;mem[0x30103]=0xC3;put32(0x23010,500);
 loot_read_prices(0x24000,&base_price,&npc_price,&price_flags);assert(price_flags==3&&npc_price==500);
 put32(0x23010,-1);loot_read_prices(0x24000,&base_price,&npc_price,&price_flags);assert(price_flags==1);
 put64(0x22010,0x30200);mem[0x30200]=0xE9;put32(0x30201,(int)(0x30000-0x30205));
 loot_read_prices(0x24000,&base_price,&npc_price,&price_flags);assert((price_flags&1)&&base_price==1200);
 mem[0x30200]=0xC3;loot_read_prices(0x24000,&base_price,&npc_price,&price_flags);assert(price_flags==0);
 put64(0x22010,0x30300);memcpy(mem+0x30300,"\xF3\x0F\x1E\xFA\xB8",5);put32(0x30305,0);mem[0x30309]=0xC3;
 loot_read_prices(0x24000,&base_price,&npc_price,&price_flags);assert((price_flags&1)&&base_price==0);
 assert(writes==before_reads);
 puts("PRICE CHECKS OK: exact base/NPC values, captured values, unsellable items, jump stubs, unknown callbacks and zero prices; no code execution/writes");

 int64_t owner_id=-9,large_owner=INT64_C(4294967297);
 put64(0x37000+RR_OFF_CAR_CHARACTER_OWNER,0x39000);
 put64(0x38000+RR_OFF_HELI_CHARACTER_OWNER,0x39000);
 put64(0x39000+RR_OFF_VEHICLE_OWNER_PUBLIC,0x3A000);
 put64(0x39000+RR_OFF_VEHICLE_OWNER_EMPTY,UINT64_MAX);
 put64(0x3A000,0x200);put64(0x3A000+RR_OFF_PRIM_VALUE,large_owner);
 put64(0x37000+0x50,0x3B000); /* current driver/control owner differs */
 assert(vehicle_owner_id(0x37000,0,&owner_id)&&owner_id==large_owner);
 assert(vehicle_owner_id(0x38000,1,&owner_id)&&owner_id==large_owner);
 assert(!vehicle_owner_id(0x37000,1,&owner_id)&&owner_id==0);
 assert(!vehicle_owner_id(0x37000,2,&owner_id));
 put64(0x3A018,UINT64_MAX);assert(vehicle_owner_id(0x37000,0,&owner_id)&&owner_id==0);
 put64(0x3A018,0);assert(!vehicle_owner_id(0x37000,0,&owner_id));
 put64(0x39000+RR_OFF_VEHICLE_OWNER_EMPTY,0);assert(vehicle_owner_id(0x37000,0,&owner_id)&&owner_id==0);
 put64(0x3A018,UINT64_MAX-1);assert(!vehicle_owner_id(0x37000,0,&owner_id));
 put64(0x3A018,large_owner);put64(0x3A000,0);assert(!vehicle_owner_id(0x37000,0,&owner_id));
 put64(0x3A000,0x200);put64(0x39000+RR_OFF_VEHICLE_OWNER_PUBLIC,0);assert(!vehicle_owner_id(0x37000,0,&owner_id));
 put64(0x39000+RR_OFF_VEHICLE_OWNER_PUBLIC,0x3A000);
 VEH_CACHE owners[4]={{0}};
 for(int i=0;i<4;++i)owners[i].owner_known=1;
 owners[0].owner_public_id=large_owner;owners[1].owner_public_id=999;
 owners[2].owner_public_id=777;owners[3].owner_public_id=888;
 g_ncache=2;g_cache[0].public_id=large_owner;snprintf(g_cache[0].name,sizeof(g_cache[0].name),"Alice");
 g_cache[1].public_id=999;g_cache[1].local=1;
 put64(g_battle+RR_OFF_BATTLE_WORLD,g_world);put64(g_battle+RR_OFF_BATTLE_SLOTS,0x1F000);put64(0x1F018,0x8600);
 put32(0xA600,0);put32(0xA608,17);put64(0xA610,0x20000);put64(0x20010,0x21000);
 put64(0x21000+RR_OFF_SLOT_ENTITY_ID,0x21100);put32(0x21118,17);
 put64(0x21000+RR_OFF_SLOT_PUBLIC_ID,0x21200);put64(0x21200,0x200);put64(0x21218,777);
 put64(0x21000+RR_OFF_SLOT_NICKNAME,0x21300);put64(0x21318,0x21400);memcpy(mem+0x21400,"DistantOwner",13);
 cache_vehicle_owner_names(owners,4);
 assert(!strcmp(owners[0].owner_name,"Alice")&&!strcmp(owners[1].owner_name,"YOU"));
 assert(!strcmp(owners[2].owner_name,"DistantOwner")&&!owners[3].owner_name[0]);
 put64(0x21218,778);cache_vehicle_owner_names(owners,4);assert(!owners[2].owner_name[0]);
 put64(0x21218,777);put32(0x21118,18);cache_vehicle_owner_names(owners,4);assert(!owners[2].owner_name[0]);
 put32(0x21118,17);put32(0xA600,-1);cache_vehicle_owner_names(owners,4);assert(!owners[2].owner_name[0]);
 put32(0xA600,0);put64(g_battle+RR_OFF_BATTLE_WORLD,0x1008);cache_vehicle_owner_names(owners,4);assert(!owners[2].owner_name[0]);
 assert(writes==before_reads);
 puts("VEHICLE OWNER CHECKS OK: car/helicopter owner, driver exclusion, 64-bit public ID, empty/invalid data, cached/slot nicknames, ID mismatch and deleted slots; zero writes");
 return 0;
}
'''
    test_c = work / 'memory_checks.c'
    test_c.write_text(harness, encoding='utf-8')
    run('cl','/nologo','/W3','/MT','/D_CRT_SECURE_NO_WARNINGS','/I'+str(root),str(test_c),'/Fe:'+str(work/'memory_checks.exe'),'/Fo:'+str(work/'memory_checks.obj'))
    run(str(work / 'memory_checks.exe'))

    aim_checks = (root.parent/'tests/aim_checks.c').read_text(encoding='utf-8')
    host_gate = '''static int g_sw=1920,g_sh=1080,g_nrender,focus_allowed;
static HWND g_hwnd;
static RR_PLAYER* g_render;
static RR_CAMERA g_rcam;
static int game_focused(void) { return focus_allowed; }
'''+function((root/'rr_esp.c').read_text(encoding='utf-8'), 'static void aa_tick(unsigned snapshot_seq, DWORD snapshot_at) {')
    aim_checks = aim_checks.replace('/* HOST_GATE_UNDER_TEST */',host_gate)
    (work/'aim_checks.c').write_text(aim_checks,encoding='utf-8')
    run('cl','/nologo','/W3','/MT','/D_CRT_SECURE_NO_WARNINGS','/I'+str(root),str(work/'aim_checks.c'),'/Fe:'+str(work/'aim_checks.exe'),'/Fo:'+str(work/'aim_checks.obj'),'user32.lib')
    run(str(work/'aim_checks.exe'))

    # Exhaustive aim coverage: every pick/point/step/trigger function plus the
    # closed-loop plant that measures what the crosshair actually does.
    aim_full = (root.parent/'tests/aim_full_checks.c').read_text(encoding='utf-8')
    (work/'aim_full_checks.c').write_text(aim_full,encoding='utf-8')
    run('cl','/nologo','/W3','/MT','/D_CRT_SECURE_NO_WARNINGS','/I'+str(root),str(work/'aim_full_checks.c'),'/Fe:'+str(work/'aim_full_checks.exe'),'/Fo:'+str(work/'aim_full_checks.obj'),'user32.lib')
    run(str(work/'aim_full_checks.exe'))

    # Exercise the real --selftest entry point without the GUI application's administrator manifest.
    # The selftest links the external overlay, which needs build_port.bat objects and the
    # WebView2 SDK. This source tree is internal-only, so skip that step when the external
    # build has not been produced instead of failing the checks that cover this change.
    objects = ['rr_esp','rr_game','rr_mem','rr_unity','rr_chain','rr_draw','rr_aim','rr_recoil','rr_antiaim','rr_dupe','rr_il','rr_exec','rr_los_geom','rr_los_capture','rr_los_runtime','rr_los','rr_spawn','rr_nodescan','rr_classscan','rr_find','gpu','menu_webview']
    missing = [o for o in objects if not (root/(o+'.obj')).exists()]
    if missing or not (root/'WebView2SDK/lib/x64/WebView2Loader.dll.lib').exists():
        print('SELFTEST SKIPPED: external overlay objects/WebView2 SDK not built '
              f'(missing {len(missing)} objects); internal aim/menu checks already passed')
    else:
        runner = work / 'selftest_runner.c'
        runner.write_text('#include <windows.h>\nint WINAPI WinMain(HINSTANCE,HINSTANCE,LPSTR,int);\nint main(void){return WinMain(GetModuleHandleW(NULL),NULL,"--selftest --nohide --no-pause",SW_HIDE);}\n')
        run('cl','/nologo','/MT','/c',str(runner),'/Fo:'+str(work/'selftest_runner.obj'))
        libraries = 'kernel32 user32 gdi32 comctl32 advapi32 ole32 oleaut32 uuid shell32 ws2_32 d3d11 dxgi d2d1 dwrite dcomp d3dcompiler dbghelp'.split()
        run('link','/nologo','/SUBSYSTEM:CONSOLE','/MACHINE:X64','/OUT:'+str(work/'selftest_runner.exe'),str(work/'selftest_runner.obj'),*[str(root/(o+'.obj')) for o in objects],*[name+'.lib' for name in libraries],'/LIBPATH:'+str(root/'WebView2SDK/lib/x64'),'WebView2Loader.dll.lib')
        run(str(work / 'selftest_runner.exe'))
        selftest = (root/'selftest.log').read_text(encoding='utf-8')
        assert 'SELFTEST OK (0 failures)' in selftest, selftest
        (root.parent/'tests/selftest.log').write_text(selftest,encoding='utf-8')
        print('SELFTEST OK (0 failures)')
print('ALL OFFLINE CHECKS PASSED')
