/* rr_cam_offsets.h - Camera-lock fields and method RVAs.
 *
 * Source: Rob Run GameAssembly.dll size 0x09050000, PE timestamp 0x6B042A5B
 * (the 2026-11-23 client update, IL2CPP metadata 39). The managed field
 * layouts are unchanged from dump-20261112; only the native method RVAs and
 * the image guard moved with the patch.
 *
 * install_camera_push() verifies the GameAssembly image size and PE timestamp
 * before patching, so a different game build falls back to mouse aim instead
 * of writing into the wrong layout.
 */
#ifndef RR_CAM_OFFSETS_H
#define RR_CAM_OFFSETS_H

/* Managed fields (supplemental SDK). */
#define RR_OFF_AIM_ROT_INPUT       0x38u /* BattleAimTargetUpdater._playerRotationInput */
#define RR_OFF_ROTIN_YAW           0x10u /* PlayerRotationInputModel._yaw */
#define RR_OFF_ROTIN_PITCH         0x14u /* PlayerRotationInputModel._pitch */
#define RR_OFF_BATTLE_ROTATION     0x40u /* BattleModel.PlayerRotationInput */
#define RR_OFF_BATTLE_INPUT        0x50u /* BattleModel.BattleInputModel */

/* Managed method RVAs. */
#define RR_RVA_SET_PLAYER_ROTATION      0xC517E0u  /* PlayerRotationInputModel.SetRotation(float,float,float) */
#define RR_RVA_BATTLE_INPUT_SET_LOOK    0x455DF50u /* BattleInputModel.set_LookDirection(Vector2) */
#define RR_RVA_BRAIN_PUSH_CALL          0xDD7508u  /* ProcessActiveCamera -> PushStateToUnityCamera */
#define RR_RVA_BRAIN_PUSH_STATE         0xDD7590u  /* CinemachineBrain.PushStateToUnityCamera */

/* Native Cinemachine CameraState layout used by the push hook. */
#define RR_CAM_STATE_SIZE               0xE8u
#define RR_CAM_STATE_DUTCH              0x10u
#define RR_CAM_STATE_POS                0x4Cu
#define RR_CAM_STATE_ROT                0x58u
#define RR_CAM_STATE_CORRECTION         0x78u
#define RR_CAM_STATE_ROT_CORRECTION     0x84u
#define RR_CAM_STATE_BLEND_HINT         0x94u

/* Verified GameAssembly image for the build above. */
#define RR_CAM_BUILD_SIZE               0x09050000u
#define RR_CAM_BUILD_TIMESTAMP          0x6B042A5Bu

#endif /* RR_CAM_OFFSETS_H */
