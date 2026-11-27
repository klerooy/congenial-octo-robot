/* robrun_offsets.h - Loot, categories and item metadata */
#ifndef ROBRUN_OFFSETS_H
#define ROBRUN_OFFSETS_H

/* === EXISTING OFFSETS (VERIFIED FROM DUMP) === */
#define RR_OFF_WORLD_ENTITIES       0x38u   /* WorldModel.WorldEntitiesModel */
#define RR_OFF_WEM_BATTLE_CHARS     0x18u   /* WorldEntitiesModel._battleCharacters */
#define RR_OFF_WEM_VEHICLES         0x58u   /* WorldEntitiesModel._vehicles */
#define RR_OFF_CHAR_ID              0x38u   /* BattleCharacterModel.Id */
#define RR_OFF_CHAR_DATA            0x40u   /* BattleCharacterModel.Data */
#define RR_OFF_CHAR_CONFIG          0x48u   /* BattleCharacterModel.Config */
#define RR_OFF_CHAR_TRANSFORM       0x58u   /* BattleCharacterModel.Transform */
#define RR_OFF_CHAR_REPL_TRANSFORM  0x60u   /* BattleCharacterModel.ReplicatedTransform */
#define RR_OFF_CHAR_ISDEAD          0xF0u   /* BattleCharacterModel.IsDead */
#define RR_OFF_CHAR_PRIVATE         0x10u   /* BattleCharacterModel.PrivateModel */
#define RR_OFF_CTM_X                0x10u   /* CharacterTransformModel.X */
#define RR_OFF_CTM_Y                0x14u   /* CharacterTransformModel.Y */
#define RR_OFF_CTM_Z                0x18u   /* CharacterTransformModel.Z */
#define RR_OFF_CTM_PITCH            0x1Cu   /* CharacterTransformModel.Pitch */
#define RR_OFF_CTM_YAW              0x20u   /* CharacterTransformModel.Yaw */
#define RR_OFF_CTM_ROLL             0x24u   /* CharacterTransformModel.Roll */
#define RR_OFF_TRANSFORM_X          0x10u   /* TransformModel._x */
#define RR_OFF_TRANSFORM_Y          0x18u   /* TransformModel._y */
#define RR_OFF_TRANSFORM_Z          0x20u   /* TransformModel._z */
#define RR_OFF_TRANSFORM_RX         0x28u   /* TransformModel._rx */
#define RR_OFF_TRANSFORM_RY         0x30u   /* TransformModel._ry */
#define RR_OFF_TRANSFORM_RZ         0x38u   /* TransformModel._rz */
#define RR_OFF_RTRANSFORM_POS       0x10u   /* CharacterReplicatedTransformModel._position */
#define RR_OFF_RTRANSFORM_RX        0x18u   /* CharacterReplicatedTransformModel._rx */
#define RR_OFF_RTRANSFORM_RY        0x20u   /* CharacterReplicatedTransformModel._ry */
#define RR_OFF_RTRANSFORM_RZ        0x28u   /* CharacterReplicatedTransformModel._rz */
#define RR_OFF_DATA_COMMON          0x20u   /* BattleCharacterData.CommonData */
#define RR_OFF_DATA_PRIVATE         0x28u   /* BattleCharacterData.PrivateData */
#define RR_OFF_DATA_PUBLIC          0x30u   /* BattleCharacterData.PublicData */
#define RR_OFF_COMMON_ROLE          0x20u   /* BattleCharacterCommonData.Role */
#define RR_OFF_PRIVATE_HEALTH       0x38u   /* BattleCharacterPrivateData.Health */
#define RR_OFF_PUBLIC_ARMOR         0x38u   /* BattleCharacterPublicData.HasArmor */
#define RR_OFF_CHAR_WEAPON          0x150u  /* BattleCharacterModel.Weapon */
#define RR_OFF_CHAR_CROUCH          0x160u  /* BattleCharacterModel.CrouchModel */
#define RR_OFF_CHAR_STANCE          0x1F0u  /* BattleCharacterModel.BattleStanceModel */
#define RR_OFF_CHAR_PBM             0x258u  /* BattleCharacterModel.PlayerBattleModel */
#define RR_OFF_WEAPON_ITEM          0x10u   /* WeaponModel._itemInHands */
#define RR_OFF_CROUCH_BOOL          0x10u   /* CrouchModel._isCrouching */
#define RR_OFF_STANCE_REF           0x18u   /* BattleStanceModel._battleStance */
#define RR_OFF_PBM_DATA             0x10u   /* PlayerBattleModel.Data */
#define RR_OFF_CAM_POS              0x10u   /* BattleCameraModel.Position */
#define RR_OFF_CAM_ROT              0x1Cu   /* BattleCameraModel.Orientation */
#define RR_OFF_CAM_FWD              0x3Cu   /* BattleCameraModel.Forward */
#define RR_OFF_CAM_RIGHT            0x48u   /* BattleCameraModel.Right */
#define RR_OFF_CAM_UP               0x54u   /* BattleCameraModel.Up */
#define RR_OFF_CAM_DIST             0x60u   /* BattleCameraModel.DistanceToFollowTarget */
#define RR_OFF_HEALTH_VALUE         0x10u   /* HealthModel._health */
#define RR_OFF_HEALTH_DEAD          0x18u   /* HealthModel._isDead */
#define RR_OFF_HEALTH_MAX           0x20u   /* HealthModel._maxAmount */

/* === NEW: LOOT ESP (VERIFIED FROM 04_EntityManager.cs) === */
/* WorldEntitiesModel loot dictionaries - private field offsets */
#define RR_OFF_WEM_LOOTS            0x38u   /* Dictionary<int, ILootModel> _loots */
#define RR_OFF_WEM_STATIC_LOOT      0x98u   /* Dictionary<int, StaticLootObjectModel> _staticLootObjects */
#define RR_OFF_WEM_RANDOM_LOOT      0xA0u   /* Dictionary<int, RandomLootObjectModel> _randomLootObjects */
#define RR_OFF_WEM_CASH_LOOT        0x268u  /* Dictionary<int, CashLootNodeModel> _cashLootNodes */
#define RR_OFF_WEM_CONTAINERS       0x20u   /* Dictionary<int, ContainerModel> _containers */
#define RR_OFF_WEM_VEHICLE_LOOT     0x40u   /* Dictionary<int, IVehicleLootModel> _vehicleLoots */
#define RR_OFF_WEM_DOORS            0x48u   /* Dictionary<int, DoorModel> _doors */

/* Loot entity position offsets - from dump analysis */
/* StaticLootObjectModel and RandomLootObjectModel store position at +0x24 via IPositionModel */
#define RR_OFF_STATIC_LOOT_POS      0x24u
#define RR_OFF_RANDOM_LOOT_POS      0x24u
#define RR_OFF_CASH_LOOT_POS        0x10u   /* CashLootNodeModel position */
#define RR_OFF_CONTAINER_POS        0x38u   /* ContainerModel -> IPositionModel */
#define RR_OFF_DOOR_POS             0x38u   /* DoorModel -> IPositionModel */
#define RR_OFF_LOOT_POS             0x38u   /* ILootModel generic position */
#define RR_OFF_VEHICLE_LOOT_POS     0x38u   /* IVehicleLootModel position */

/* === NEW: CATEGORIES / ROLE FLAGS === */
/* PlayerRoleDescription is read as string name via RR_OFF_DESC_NAME (0x10) */
/* Role detection is done by strcmp in rr_game.c fill_entry() */
/* Existing RR_OFF_COMMON_ROLE (0x20) -> ReferenceData<PlayerRoleDescription> -> +0x18 value */

/* === SKELETON OFFSETS (VERIFIED FROM 03_Skeleton.cs) === */
#define RR_OFF_CHAR_SKELETON        0x138u  /* SkeletonModel */
#define RR_OFF_CHAR_INVEH           0x1C8u  /* InVehicleModel */
#define RR_OFF_CHAR_KNOCKOUT        0x298u  /* KnockoutModel */
#define RR_OFF_INVEH_ID             0x10u   /* _entityId */
#define RR_OFF_INVEH_TYPE           0x20u   /* _vehicleType */

/* Knockout state, verified against metadata v39. The replicated common data
 * carries both the end timestamp and the phase reference; the phase is a
 * pointer to one of two static enum items. DelayAfterRevive is a short
 * post-revive phase, so only the KnockedOut item counts as down. */
#define RR_OFF_COMMON_KNOCKOUT_END_TS 0x158u /* BattleCharacterCommonData.KnockoutEndTs */
#define RR_OFF_COMMON_KNOCKOUT_PHASE  0x160u /* BattleCharacterCommonData.KnockoutPhase */
#define RR_OFF_KNOCKOUT_END_TS        0x10u  /* KnockoutModel._endTs */
#define RR_OFF_KNOCKOUT_PHASE         0x18u  /* KnockoutModel._knockoutPhase */
#define RR_OFF_KNOCKED_OUT_STATIC     0x08u  /* KnockoutPhaseDescription.<KnockedOut> */
#define RR_OFF_IDENTIFIED_ID          0x10u  /* Framework.Core.Identified.Identified.<Id> */

/* === STATIC FIELD GLOBALS (UNCHANGED) === */
#define RR_SF_WORLDMODEL            0x81D36C8u
#define RR_SF_BATTLECAMERAMODEL     0x81CB420u
#define RR_SF_WORLDENTITIESMODEL    0x81D0710u
#define RR_SF_BATTLECHARACTERMODEL  0x8196748u

/* === LOCAL SPREAD (01_Player_Entity.cs / 04_EntityManager.cs) === */
#define RR_OFF_PRIV_WEAPON_SPREAD   0x228u /* BattleCharacterPrivateModel.WeaponSpread */
#define RR_OFF_BATTLE_PLAYER        0xD0u /* BattleEntitiesModel.Player */
#define RR_OFF_PLAYER_CLIENT        0x18u /* PlayerEntityModel.ClientModel */
#define RR_OFF_PLAYER_SERVER        0x20u /* PlayerEntityModel.ServerModel */
#define RR_OFF_CLIENT_FAKE_SPREAD    0x2C0u /* PlayerClientModel.FakeSpreadModel */
#define RR_OFF_FAKE_SPREAD_VALUES    0x10u /* PlayerFakeSpreadModel.Spread, Air, Move */

#define RR_OFF_WEM_NPC_DESCRIPTIONS 0x3C0u /* WorldEntitiesModel._npcDescriptions, 04_EntityManager.cs */

/* Item pricing fields verified against the installed GameAssembly.dll and metadata v39.
 * Exact source hashes and delegate signatures are in offsets/item_pricing_fields.json. */
#define RR_OFF_ITEM_BASE_PRICE_FUNC      0x78u
#define RR_OFF_ITEM_SELL_PRICE_FUNC      0x88u
#define RR_OFF_ITEM_CAN_SELL             0x95u
#define RR_PRICING_IMAGE_SIZE             0x904F000u

/* Party fields verified from the installed binary; see offsets/party_fields.json. */
#define RR_OFF_BATTLE_PARTY_ACCESS       0x448u
#define RR_OFF_PARTY_ACCESS_WORLD        0x10u
#define RR_OFF_PARTY_ACCESS_MODEL        0x18u
#define RR_OFF_PARTY_HAS_GROUP           0x60u
#define RR_OFF_PARTY_ACTUAL_MEMBERS      0x38u
#define RR_OFF_PARTY_MEMBER_DATA         0x38u
#define RR_OFF_PARTY_MEMBER_ENTITY       0x40u

#define RR_OFF_SLOT_PUBLIC_ID              0x28u /* BattleCharacterSlotData.PublicId, metadata v39 */

/* Character ownership, verified metadata v39; offsets/vehicle_owner_fields.json. */
#define RR_OFF_CAR_CHARACTER_OWNER       0x30u
#define RR_OFF_HELI_CHARACTER_OWNER      0xD8u
#define RR_OFF_VEHICLE_OWNER_PUBLIC      0x10u /* IPrimitiveData<long>, direct data */
#define RR_OFF_VEHICLE_OWNER_EMPTY       0x18u /* empty public ID sentinel */

/* Inventory chain verified from metadata v39; docs/il2cpp_signatures.md.
 * Class layouts resolve statically. Generic wrappers (IPrimitiveData<T>) report
 * every field at offset 0 in metadata, so resolve those on a live instance with
 * --offsets before trusting an offset. Stack counts are bit fields inside the
 * packed InventorySlot int, not a plain int field. */
#define RR_OFF_INV_SLOTS                 0x18u /* InventoryModel.Slots */
#define RR_OFF_INV_SLOT_DESC             0x10u /* InventorySlotModel.Description */
#define RR_OFF_INV_SLOT_ITEM             0x18u /* InventorySlotModel.ItemModel */
#define RR_OFF_INV_SLOT_STACK            0x20u /* InventorySlotModel.StackModel */
#define RR_OFF_INV_SLOT_AMMO             0x28u /* InventorySlotModel.AmmoModel */
#define RR_OFF_INV_SLOT_DURABILITY       0x30u /* InventorySlotModel.DurabilityModel */
#define RR_OFF_INV_SLOT_POINTS           0x38u /* InventorySlotModel.CumulativePointsModel */
#define RR_OFF_STACK_DATA                0x10u /* InventorySlotStackModel._data (packed slot) */
#define RR_OFF_STACK_ITEM                0x18u /* InventorySlotStackModel._itemModel */
#define RR_OFF_ITEM_DATA                 0x10u /* InventorySlotItemModel._data */
#define RR_OFF_CHARINV_BACKPACK          0x20u /* CharacterInventoryModel._backpackSlotModel */
#define RR_OFF_CHARINV_STARTLINES        0x28u /* CharacterInventoryModel._startLinesCount */

#endif /* ROBRUN_OFFSETS_H */
