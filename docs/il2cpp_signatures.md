# IL2CPP signature harvest — ROB RUN (Unity 6000.3.18f1, metadata v39)

Static harvest of the installed build. Everything below comes from
`ROB RUN_Data/il2cpp_data/Metadata/global-metadata.dat` plus the field-offset and
type tables of `GameAssembly.dll`. No runtime evidence is claimed for any entry
marked *unverified*.

## Target identity

| item | value |
|---|---|
| install | `C:\Program Files (x86)\Steam\steamapps\common\Rob Run` |
| metadata | v39 (magic `0xFAB11BAF`), 34,498,620 bytes |
| types | 38,195 |
| fields | 162,880 |
| methods | 265,397 |
| GameAssembly image size | `0x9050000` |
| code section | `.text` 8,028,672 + `il2cpp` 100,592,640 bytes |

Reproduce with:

```bat
python tools\harvest_il2cpp.py find <regex> [...]
python tools\harvest_il2cpp.py dump <exact.Namespace.TypeName>
python tools\harvest_il2cpp.py methods <regex>
python tools\harvest_il2cpp.py strings <regex> [...]
python tools\harvest_il2cpp.py bulk offsets\map.json <regex> [...]
```

Generated maps: `offsets/type_index.txt` (all type names), `offsets/inventory_map.json`
(406 inventory / replication / systems types), `offsets/messages_map.json`
(616 message / network / http / replication types).

## Metadata layout this build uses

Header triples are `(offset, byte size, count)`; the type-definition record is 82
bytes with the counts packed as `u16`:

| location | meaning |
|---|---|
| `+0x1A` | `fieldStart` |
| `+0x1E` | `methodStart` |
| `+0x3A` | `method_count` |
| `+0x3E` | `field_count` |
| `+0x44` | `vtable_count` |

`fieldStart + field_count` indexes the 12-byte field table (`nameIndex`,
`typeIndex`, `token`); the per-type offset array hangs off the metadata
registration at `+88`. Verified against `InventoryItemDescription`, whose table
reproduces the already-trusted `offsets/item_pricing_fields.json` byte for byte.

## Inventory object graph (instance offsets, classes)

```
InventoryModel                +0x10 Description : InventoryCellsSizeDescription
                              +0x18 Slots       : IInventorySlotsModel
InventorySlotModel            +0x10 Description
                              +0x18 ItemModel   : IInventoryItemModel
                              +0x20 StackModel  : IInventoryStackModel
                              +0x28 AmmoModel
                              +0x30 DurabilityModel
                              +0x38 CumulativePointsModel
InventorySlotStackModel       +0x10 _data       : InventorySlotData
                              +0x18 _itemModel  : IInventoryItemModel
InventorySlotItemModel        +0x10 _data       : InventorySlotData
CharacterInventoryModel       +0x20 _backpackSlotModel : IEquipSlotModel
                              +0x28 _startLinesCount
```

`InventorySlotsModel` itself exposes only a `get_Item` indexer; the slot storage
lives in the derived models (`CharacterInventoryModel`, `VehicleInventoryModel`,
`CargoInventoryModel`, `StorageBoxInventoryModel`, `LootEntityInventoryModel`, ...).

## Where the quantity lives

`Models.Models.InventoryStackModel.InventorySlotStackModel` is the stack owner:

```
get_Value()  / set_Value()          -> IInventoryStackModel
_data        (InventorySlotData)    -> packed InventorySlot, inline at +0x10
_itemModel   (IInventoryItemModel)  -> +0x18, item identity + IsEmpty
```

`Models.Models.InventorySlot.InventorySlot` is a bit-packed 32-bit struct, not a
`{itemId, count}` pair:

```
_raw               int   (packed)
DurabilityBits / DurabilityShift / DurabilityMask
PayloadBits    / PayloadShift    / PayloadMask
ItemBits       / ItemShift       / ItemMask
ExceptDurabilityMask / ExceptPayloadMask / ExceptItemMask
get_Durability() / get_Payload() / get_Item()
```

Consequences for the overlay:

* a stack count is a **bit field inside a packed int**, so an ESP that reads
  "count" cannot just dereference a wrapper and take `+0x18` as an `int`;
* the three masks/shifts are `static readonly`/`const` members, i.e. they sit in
  `Il2CppClass.static_fields` (`+0xB8`), which the overlay can already read;
* `InventoryItemModel` / `PrimitiveModel<T>` wrappers are `IPrimitiveData<T>`
  (`_value`), and for **open generic type definitions metadata reports every
  field at offset 0**, so generic wrapper offsets must come from a live
  instantiation (`--offsets <Class>`), never from this table.

Trustworthy `InventoryItemDescription` offsets are unchanged: `StackSize` `+0x2C`,
`Rarity` `+0x30`, `IsStackable` `+0x34`, `WeaponsFunc` `+0x50`, `SellPriceFunc`
`+0x88`, `Durability` `+0x90`, `IsSellAvailable` `+0x95`.

## Wire protocol surface

410 types under `Models.Messages.*`, 125 of them `Cheat*`. Every payload is built
from `Framework.Replication.Data.Primitive.IPrimitiveData<T>`, and the message
base class starts its fields at `+0x20` (`+0x10 Type`, `+0x18` message id slot).

Inventory-relevant messages:

* `Inventory.Take.InventoryTakeMessage` — batch: `_entityIds` (`ListData<int>`)
  plus `_commands` (`ListData<InventoryTakeCommandData>`);
  `InventoryTakeCommand` = `{EntityIdIndex +0x10, Type +0x18, FromSlot +0x20,
  ToSlotIndex +0x28, Count +0x2C}`.
* `Inventory.Swap.InventorySwapMessage` — `from`/`to` each carry
  `{Source, InventorySourceSlot, InventoryDestinationSlot, EquipSlot, PocketSlot,
  EntityId}`, so an inventory↔pocket↔equip↔external swap is one message.
* 12 `FromXToYMoveMessage` variants cover every container pair, plus
  `SellItemMessage`, `InventorySellItemMessage`, `UnloadAmmoMessage`,
  `InventoryUnsetMessage<T>`.
* `Inventory.SetInventoryItemCheatMessage` — `{SlotSource, InventorySlot,
  EquipSlot, PocketSlot, Item, Count, Ammo, Durability, CumulativePoints,
  EntityId}`; the only message shape that sets a slot's contents wholesale.

Server-authority evidence in the same harvest:

* `Models.Models.UsedTransactions.UsedTransactionsModel` with
  `Contains/Add/Remove/Clear` over `UsedTransactionsData` and
  `Transaction.RemoveUsedTransactionMessage` — a server-issued transaction log
  replicated to the client, i.e. replay protection, not a client counter.
* `Models.Http` style login/token stack (`Framework.Http.Dto.Request.Receipt.*`,
  `LoginResponse`, `OpsTokenResponse`) — the session is account-bound.
* Client-side validation strings recovered from the literal blob:
  `Item amount must be greater then 0: amount={0}, item={1}` and
  `Item amount must be less than stack size: amount={0}, stackSize={1}, item={2}`
  — the client predicts the same rules the server enforces.

## Dupe verdict, updated with binary evidence

The replication layer confirms the earlier reading and removes the last doubt:

1. Slot contents travel as **messages**, not as a client-writable replicated
   field. There is no `Count` property on a client-owned model that the server
   copies back.
2. Every applied change carries a transaction identity checked against
   `UsedTransactionsModel`. Two identical takes with the same transaction id are
   the only race with a real chance, and the check is exactly what it prevents.
3. `SetInventoryItemCheatMessage` is the only wholesale slot write, and it is a
   `Cheat*` message — server-side gating decides whether it is honoured.

So: no client-side quantity field to rewrite, no local clone that survives
replication, one narrow race surface, one cheat-message surface. Anything the
overlay could do to a local `InventorySlot` int is overwritten by the next
snapshot.

## Method addresses

Method *names* and tokens resolve for every type (`dump`), but native addresses
do not resolve from the file: this build keeps its code-pointer tables keyed by
codegen module, and the module name strings in `GameAssembly.dll` are not
referenced from any pointer table, so a static token→RVA walk is not available
from the file alone.

They do resolve from the live class object. `Il2CppClass` points at an array of
`MethodInfo*`, and `MethodInfo` carries the entry point:

```
Il2CppClass.methods      -> MethodInfo* [method_count]
MethodInfo +0x00          native code pointer (inside GameAssembly.dll)
MethodInfo +0x10          method name (C string)
MethodInfo +0x18          klass back-pointer
```

`--methoda <Class> [name-filter]` finds the table instead of assuming its
offset: every pointer in the class header from `+0x80` to `+0x1A0` is followed
and its entries validated — the method pointer must land inside the loaded
`GameAssembly.dll` image and the name must be a printable C string. The winning
candidate is printed with its offset and sample score, then every method name
with `GameAssembly+0xRVA`:

```bat
RobRunPort.exe --methoda Models.Messages.Inventory.InventoryTakeMessage
RobRunPort.exe --methoda Models.Models.InventoryStackModel.InventorySlotStackModel get_
RobRunPort.exe --methoda Models.Messages.Inventory.SetInventoryItemCheatMessage
```

A `stub` marker means the entry's klass back-pointer is null (generic
instantiation stub); the address is still the real one.

`--sigscan <pattern>` closes the other direction: a byte pattern with `??`
wildcards is scanned across the loaded `GameAssembly.dll` image and each hit is
printed as absolute address plus module RVA. That is how a function identified
in a disassembler becomes an address in this process.

```bat
RobRunPort.exe --sigscan "48 89 5C 24 ?? 57 48 83 EC 20"
```

Live verification for the whole ladder requires the game running:

* `--methoda` resolves the class through the same name-range scan `--offsets`
  uses, so it calls `game_init()` first;
* `--sigscan` needs only module load, so it works from any game state.

## Managed invocation from outside

The image exports the il2cpp C API — 371 exports, 241 of them `il2cpp_*` — so
invocation needs no offset at all. `GameAssembly.dll` is PE32+ with its export
directory at RVA `0x7f9c3f0`; the transport walks that table by name at runtime,
so a Unity upgrade changes the RVAs and nothing else. The entries that matter:

| export | signature used |
|---|---|
| `il2cpp_domain_get` | `Il2CppDomain* (void)` |
| `il2cpp_thread_attach` | `Il2CppThread* (Il2CppDomain*)` |
| `il2cpp_thread_detach` | `void (Il2CppThread*)` |
| `il2cpp_object_new` | `Il2CppObject* (Il2CppClass*)` |
| `il2cpp_runtime_invoke` | `Il2CppObject* (const MethodInfo*, void*, void**, Il2CppObject**)` |

Everything else the API offers (`il2cpp_array_new`, `il2cpp_string_new`,
`il2cpp_class_from_name`, `il2cpp_domain_get_assemblies`) resolves through the
same call and is what an object-graph builder would use next.

### Stub shape

One `VirtualAllocEx` block of `0x400` bytes, one `CreateRemoteThread`, one wait
(both calls are denied on a title with an active kernel anti-cheat — see *Live
status* below):

```
+0x000  generated x64 stub
+0x200  void* params[]        params[i] = +0x320 + i*8
+0x300  Il2CppObject* exc
+0x308  uint64_t return
+0x310  Il2CppThread* attached
+0x320  argument values (qword slots)
```

The stub is a fixed sequence, not an interpreter:

```
rax = il2cpp_domain_get();          call rax
rcx = rax
rax = il2cpp_thread_attach;         call rax ; [thr] = rax
rcx/rdx/r8/r9 = target arguments
rax = il2cpp_object_new   |  il2cpp_runtime_invoke
                                    call rax ; [ret] = rax
rax = [thr]; rcx = rax
rax = il2cpp_thread_detach;         call rax
ret
```

Every call site is `sub rsp,0x28 / call rax / add rsp,0x28`: shadow space plus the
16-byte alignment Win64 requires. Immediates load through `48 B8/B9/BA` and
`49 B8/B9`; the result store is `48 A3 moffs64`.

The attach is not decorative. Object allocation goes through the BOEHM collector,
and a thread the runtime has never seen is not a registered thread; `object_new`
from one is undefined behaviour. Attach, call, detach — one thread per call, so
nothing is left registered.

Contract: bounded (`IL_WAIT_MS 5000`), fail-closed, and the block is freed on
every path including early returns — except a timeout, where the block is left
allocated because the thread is still executing inside it. `--ilnew` allocates
without invoking a constructor, so fields hold zero/`null` until `.ctor` is
invoked with the object as `this`. A call returns `1` with `ret`/`exc` filled,
or `0` with a reason.

### Commands

```bat
RobRunPort.exe --ilprobe                        :: API presence and resolved addresses
RobRunPort.exe --ilex il2cpp_array              :: export dump, substring filter
RobRunPort.exe --ilnew Game.Shared.NetworkSendMessage
RobRunPort.exe --ilinvoke Game.Shared.NetworkSendMessage get_Message 0 2A1B3C40
```

`--ilinvoke <Class> <method> [occurrence] [this] [a0..a3]`, all numbers hex. The
class resolves through the `--offsets` path, the method through the validated
`MethodInfo*` table walk, so `occurrence` addresses overloads. A managed exception
prints `THREW exc=0x… <exception class>` — the class comes from the object's
klass `+0x10` name pointer.

### Live status on this title

Measured against `ROB RUN` with EasyAntiCheat running, module base
`0x7FFA58E50000`:

| check | result |
|---|---|
| export directory | RVA `0x7F9C3F0`, size `0x346C` — matches the file |
| export walk | 371 functions / 371 names, 241 `il2cpp_*` |
| transport API | 10 / 10 resolved; `il2cpp_runtime_invoke` = `0x7FFA5954BA40` |
| `OpenProcess` | succeeds |
| `VirtualAllocEx` | `ERROR_ACCESS_DENIED` (5) |
| `CreateRemoteThread` | `ERROR_ACCESS_DENIED` (5) |
| driver allocation | succeeds, stub written and read back |
| `SuspendThread` | `-1`, error 5 |
| `GetThreadContext` / `SetThreadContext` | 5 / 5 |
| `QueueUserAPC` | 5 on 64 of 64 threads |
| `game_class_addr("InventorySlotStackModel")` | `0` |

The thread results are handle-right stripping, not a syscall blacklist:
`OpenThread` succeeds, but the granted access omits `THREAD_GET_CONTEXT`,
`THREAD_SET_CONTEXT` and `THREAD_SUSPEND_RESUME`, so every operation that needs
them fails at the same status. That closes the context-hijack and the plain
user-mode APC together, at either integrity level.

The transport is correct and the target's export table is fully readable; the
title simply refuses remote execution. Both stages fail on the same access
check, which is what a kernel anti-cheat does to `PROCESS_VM_OPERATION` and
`PROCESS_CREATE_THREAD` handles for a protected process. So on this title,
`--ilnew` and `--ilinvoke` cannot run at all — and neither can any other
`CreateRemoteThread` or `WriteProcessMemory` design. Reads, writes, pointer
walks, region queries and instance scanning through the driver are unaffected.

The export-directory read had a real bug that this run exposed: the data
directory begins at the optional header (`PE sig + 0x18`), so the export entry is
at `+0x88` on PE32+ and `+0x78` on PE32. The earlier `+0x70` / `+0x60` reads
pointed 24 bytes too low, produced zeros, and the walk reported "export directory
unreadable" while the module was fine. Fixed and re-verified against the live
image and the on-disk file.

`rr_probe.exe` (no manifest, no elevation) is the instrument for this class of
question: `module`, `probe`, `exports`, `peek`, `regions`, `where`, `find`,
`xref`, `handle`, `remote`. See `BUILD.md`.

### Driver backend

`3_src_driver/rr_exec.sys` supplies the step EAC removes. It needs no process or
thread handle: the target is resolved by id
(`PsLookupProcessByProcessId` / `PsLookupThreadByThreadId`), one `KAPC` is
initialized and queued with `KeInsertQueueApc`, and the normal routine is the
stub block that was already written through the read/write driver. Handle
filtering has nothing to filter because no handle is opened.

Requests arrive through a file channel (`C:\Users\Public\rr_exec.bin`) rather
than an IOCTL surface, which keeps the image to one `DriverEntry`, one system
thread and a 15 ms poll — no device object, no dispatch table, nothing to fuzz
and no IRP path to get wrong. A generation counter at `0x80` lets a newer image
take over from an older resident copy without a reboot. Two guards live inside
the driver rather than the caller: the target image name must be `ROB RUN.exe`
and the routine must be in the user canonical range.

The stub gained a claim slot at `+0x318` and a done slot at `+0x340`, so several
APCs queued on several threads still execute the call exactly once; the
RIP-relative and jump displacements are patched against the runtime block base,
not the image base.

Load paths, in order of what the machine allows: the mapper (`kd2` — refused
here at the vulnerable-driver step), `1_run/byovd/rr_byovd.ps1` (borrows MSI's
`RTCore64.sys` to drop `ci!g_CiOptions`, loads both images through SCM, restores
the stock value, verifies the 577 refusal and unloads the donor — the only path
measured to survive Easy Anti-Cheat, verdict 301), or `rr_probe dseload <sys>`,
which patches `ci!g_CiOptions` through the memory transport, loads the image and
restores the value in the same run. A test-signing boot is not a path for this
title: EAC answers 206 `error_kernel_dse` even with no custom driver loaded and
the CI report faked to the stock value (`docs/byovd_eac.md`).

Measured: with enforcement off the image loads and `DriverEntry` runs. The first
build then bugchecked `0x50` inside `ObfDereferenceObject` — `PsCreateSystemThread`
returns a kernel handle, not an object pointer. Fixed to `ZwClose`.

### Next stage for the dupe payload

The send surface is mapped, but the execution route it was designed around is
closed to user-mode injection on this title: `SendMessage` /
`SetInventoryItemCheatMessage` can only be invoked from a thread inside the
game, and EAC denies the allocation, the thread, the context and the user APC.
The kernel APC in `3_src_driver/rr_exec.sys` is the route that does not need any
of them.

Two further paths remain, both memory-only:

1. **In-flight message objects.** A take/swap message exists as a live managed
   object between its construction and its dispatch. Locating that instance and
   writing `Count` (`+0x2C` on `InventoryTakeCommand`) while it is queued changes
   what the sender serialises, without touching the sending code. This is a
   scanner plus a driver write: entirely within what already works.
2. **Command injection at the model.** `NetworkStatusModel` holds
   `+0x18 Command`, `+0x30 NetworkSendMessage`. If a message object can be built
   by copying an existing one and rewriting its fields, pointing the model at it
   lets the game's own loop send it.

Both need the same missing piece: mapping a message type to a live *class*, then
to live *instances*. The class-name string is present — `InventorySlotStackModel`
at `0x20A6020F818`, region `#717` (`base 0x20A60000000`, `size 0x400000`), which
is slice 15 of 2048 in the resolver's 32 MB plan, so coverage is not the problem.
The only qword pointing at that string is `0x20A809E8BE0`, and the record around
it holds an inline copy of the name plus a `Models` namespace and small counts —
metadata-shaped, not a runtime `Il2CppClass`. `game_class_addr` returns 0 for it.

So the next diagnostic is concrete, not speculative: resolve a class that is
certainly loaded (a character type the ESP already walks), compare its record
with the one above, and decide whether message types are lazily created (class
object absent until something references it) or the class validator needs the
current metadata layout. `rr_probe xref` plus `peek` on the candidate record
answers this without elevation and without executing anything in the game.

Step 1 of the old recipe stays manual: `--classscan` returns candidate instances
and the right one is identified by reading `+0x30` and checking for a
`NetworkSendMessage` object. The `ListData<T>` containers inside take/swap
messages remain the open unknown — they own a backing array, so a payload that
applies needs that array present, not just a plausible pointer.

