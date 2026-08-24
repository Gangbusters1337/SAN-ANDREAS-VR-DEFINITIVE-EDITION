#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#define GLM_FORCE_QUAT_DATA_XYZW
#include "glm/glm.hpp"
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "uevr/API.hpp"
#include "PlayerManager.h"
#include "CameraController.h"
#include "MemoryManager.h"
#include "Utilities.h"
#include "SettingsManager.h"


class WeaponManager {
private:
	PlayerManager* const playerManager;
	CameraController* const cameraController;
	MemoryManager* const memoryManager;
	SettingsManager* const settingsManager;

	//Shoot detection
	bool firstWeaponIsShooting = false;
	bool secondWeaponIsShooting = false;
	uevr::API::UObject* firstWeaponLastParticleShot = nullptr;
	uevr::API::UObject* secondWeaponLastParticleShot = nullptr;
	std::vector<uevr::API::UObject*> firstWeaponPreviousParticles;
	std::vector<uevr::API::UObject*> secondWeaponPreviousParticles;
	bool shotHierarchyProbeLogged = false;
	std::atomic<uint8_t> customAkimboHeldMaskSnapshot{ 0 };
	std::atomic<uint8_t> customAkimboEdgeMaskPending{ 0 };
	std::atomic<int> customAkimboInputWeapon{ -1 };
	bool customAkimboActive = false;
	int customAkimboLastRejection = -1;
	// Preserve the hand that owned GTA's live mesh when the second controller
	// joins. Magnetic suspension clears its normal primary-hand latch.
	int customAkimboPrimaryHand = -1;
	std::array<glm::fvec3, 2> customAkimboMuzzleWorld{};
	uint8_t customAkimboMuzzleValidMask = 0;
	uint8_t customAkimboFlashAlternateHand = 0;
	glm::fvec3 customAkimboLastEffectMuzzleWorld{};
	bool customAkimboLastEffectMuzzleValid = false;
	// GTA emits a muzzle particle only for its one authoritative weapon mesh.
	// Cache that native effect template and use it for the presentation-only
	// left lane after the corresponding native shot has been accepted.
	uevr::API::UObject* customAkimboMuzzleEffectTemplate = nullptr;
	std::vector<uevr::API::UObject*> customAkimboMuzzleEffectTemplates;
	std::vector<glm::fvec3> customAkimboMuzzleEffectScales;
	int customAkimboMuzzleEffectTemplateWeapon = -1;
	uint8_t customAkimboPendingEffectHandMask = 0;
	uint64_t customAkimboPendingEffectSince = 0;
	bool customAkimboMissingEffectTemplateLogged = false;

	//Weapon infos
	uevr::API::UObject* firstWeaponMesh = nullptr;
	uevr::API::UObject* secondWeaponMesh = nullptr;
	// Presentation-only duplicate for custom akimbo. GTA's single native weapon
	// entry remains authoritative for ammo, cooldown, damage, audio and effects.
	uevr::API::UObject* customAkimboVisualMesh = nullptr;
	uevr::API::UObject* customAkimboVisualStaticMesh = nullptr;
	uevr::API::UObject* customAkimboVisualCharacter = nullptr;
	int customAkimboVisualWeapon = -1;
	// The weapon actor/component that owns the moved mesh. Native smoke/trail
	// components may be siblings of the mesh rather than its children.
	uevr::API::UObject* firstWeaponContainer = nullptr;
	uevr::API::UObject* secondWeaponContainer = nullptr;
	uevr::API::UObject* torso = nullptr;
	uevr::API::UObject* firstWeaponStaticMesh = nullptr;
	uevr::API::UObject* secondWeaponStaticMesh = nullptr;
	const std::unordered_map<std::wstring, int> weaponNameToIndex = {
		{L"SM_unarmed", 0},           // Unarmed
		{L"SM_brassknuckle", 1},    // BrassKnuckles
		{L"SM_golfclub", 2},         // GolfClub
		{L"SM_nitestick", 3},       // NightStick
		{L"SM_knifecur", 4},            // Knife
		{L"SM_bat", 5},      // BaseballBat
		{L"SM_shovel", 6},           // Shovel
		{L"SM_poolcue", 7},          // PoolCue
		{L"SM_katana", 8},           // Katana
		{L"SM_chnsaw", 9},         // Chainsaw
		{L"SM_gun_dildo1", 10},          // Dildo1
		{L"SM_gun_dildo2", 11},          // Dildo2
		{L"SM_gun_vibe1", 12},           // Vibe1
		{L"SM_gun_vibe2", 13},           // Vibe2
		{L"SM_flowera", 14},         // Flowers
		{L"SM_gun_cane", 15},            // Cane
		{L"SM_grenade", 16},         // Grenade
		{L"SM_teargas", 17},         // Teargas
		{L"SM_molotov", 18},         // Molotov
		{L"SM_colt45", 22},          // Pistol Colt 45
		{L"SM_silenced", 23},        // Silenced Pistol
		{L"SM_desert_eagle", 24},     // Desert Eagle
		{L"SM_chromegun", 25},         // Shotgun
		{L"SM_sawnoff", 26},         // Sawnoff Shotgun
		{L"SM_shotgspa", 27},          // Spas12
		{L"SM_micro_uzi", 28},             // MicroUzi
		{L"SM_mp5lng", 29},             // MP5
		{L"SM_ak47", 30},            // AK47
		{L"SM_m4", 31},              // M4
		{L"SM_tec9", 32},            // Tec9
		{L"SM_cuntgun", 33},         // Rifle (Cuntgun)
		{L"SM_sniper", 34},          // Sniper Rifle
		{L"SM_rocketla", 35},  // RocketLauncher
		{L"SM_heatseek", 36},// RocketLauncherHeatSeek
		{L"SM_flame", 37},    // Flamethrower
		{L"SM_minigun2", 38},         // Minigun
		{L"SM_satchel", 39},         // Satchel
		{L"SM_detonator", 40},       // Detonator
		{L"SM_spraycan", 41},        // SprayCan
		{L"SM_fire_ex", 42},    // Extinguisher
		{L"SM_camera", 43},          // Camera
		{L"SM_nvgoggles", 44},     // NightVision
		{L"SM_irgoggles", 45},        // Infrared
		{L"SM_gun_para", 46}        // Parachute
	};

	//aiming
	glm::fvec3 crosshairOffset = { 0.0f, -1.0f, 2.0f };
	glm::fvec3 calculatedAimForward = { 0.0f, 0.0f, 0.0f };
	glm::fvec3 calculatedAimPosition = { 0.0f, 0.0f, 0.0f };
	glm::fvec3 lastRawBarrelAimForward = { 1.0f, 0.0f, 0.0f };
	glm::fvec3 lastPreLatchAimForward = { 1.0f, 0.0f, 0.0f };
	glm::fvec3 lastAimMuzzlePosition = { 0.0f, 0.0f, 0.0f };
	glm::fvec3 lastAimMuzzleWorldPosition = { 0.0f, 0.0f, 0.0f };
	bool lastLegacyCrosshairCompensationApplied = false;
	std::unordered_set<int> camModsRequiringAimHandling = {5, 7, 8, 9, 15, 34, 39, 40, 41, 42, 45, 51, 52, 53, 55, 65};
	glm::fvec3 lastStableGameAimForward = { 1.0f, 0.0f, 0.0f };
	glm::fvec3 lastStableGameAimPosition = { 0.0f, 0.0f, 0.0f };
	bool hasStableGameAim = false;
	int stableGameAimLatchFrames = 0;
	bool aimLatchFallbackLoggedThisShot = false;
	bool aimLatchJumpLoggedThisShot = false;
	const int stableGameAimLatchFrameCount = 18;

	// Experimental two-hand stabilization. Grip state is copied from the raw
	// XInput callback; frame work is limited to cached controller poses and math.
	std::atomic<uint8_t> gripStateMask{ 0 };
	std::array<std::atomic<uint32_t>, 2> gripPressGeneration{};
	std::atomic<float> twoHandViewPitch{ 0.0f };
	std::atomic<float> twoHandViewYaw{ 0.0f };
	std::atomic<float> twoHandViewRoll{ 0.0f };
	std::atomic<bool> twoHandViewRotationValid{ false };
	std::atomic<int8_t> twoHandFirstGripHandSnapshot{ -1 };
	std::atomic<int8_t> twoHandPrimaryHand{ -1 };
	// Published by the game-thread attachment update so the raw XInput/DUALGRIP
	// callbacks never read the non-atomic motionConfiguredFirstHand or other
	// transient UObject state.
	std::atomic<int8_t> twoHandConfiguredHandSnapshot{ -1 };
	std::atomic<bool> twoHandLatchEligibleSnapshot{ false };
	bool twoHandSupportActive = false;
	bool twoHandOffsetApplied = false;
	glm::fquat twoHandRotationOffset = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	uevr::API::UObject* twoHandAppliedWeaponMesh = nullptr;
	// Calibrate the weapon's neutral orientation relative to the primary grip
	// before applying two-hand correction. This lets the basis follow the
	// primary controller without reading back a same-frame UObject transform.
	bool twoHandPrimaryBasisValid = false;
	glm::fquat twoHandWeaponRelativeToPrimary = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	uevr::API::UObject* twoHandPrimaryBasisWeaponMesh = nullptr;
	glm::fvec3 twoHandNeutralActualForward = glm::fvec3(0.0f);
	float twoHandPostSettleElapsed = 0.0f;
	bool twoHandPostSettleLogged = false;
	glm::fvec3 twoHandLastDesiredForward = glm::fvec3(0.0f);
	glm::fvec3 twoHandFilteredSupportDirection = glm::fvec3(0.0f);
	bool twoHandFilteredSupportDirectionValid = false;
	// Preserve the last stabilized world orientation so support-direction
	// updates cannot introduce an arbitrary roll around the weapon's forward axis.
	bool twoHandStableTargetValid = false;
	glm::fquat twoHandStableTargetRotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	// The primary fake hand normally follows its controller-relative bone pose.
	// While two-hand stabilization rotates the weapon, apply the same component-
	// space delta to that hand's wrist orientation without changing its position.
	bool twoHandWristOverrideActive = false;
	int8_t twoHandWristPrimaryHand = -1;
	glm::fquat twoHandWristRotationDelta = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	glm::fquat twoHandWristPrimaryPoseRotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	bool IsTwoHandLongGun() const;
	void RestoreTwoHandRotationOffset();
	glm::fquat ComposeTwoHandRotationOffset(const glm::fquat& baseOffset, uevr::API::UObject* weaponMesh) const;

	//recoil
	glm::fvec3 defaultWeaponRotationEuler = { 0.4f, 0.0f, 0.0f };
	glm::fvec3 defaultWeaponPosition = { 0.0f, 0.0f, 0.0f };
	// Uncalibrated hands share one neutral origin. Per-weapon calibration owns
	// intentional weapon placement; left/right are mirrored from that record.
	const glm::fvec3 leftControllerWeaponGripOffset = { 0.0f, 0.0f, 0.0f };
	glm::fvec3 currentFirstWeaponRecoilPosition = { 0.0f, 0.0f, 0.0f };
	glm::fvec3 currentFirstWeaponRecoilRotationEuler = { 0.0f, 0.0f, 0.0f };
	glm::fvec3 currentSecondWeaponRecoilPosition = { 0.0f, 0.0f, 0.0f };
	glm::fvec3 currentSecondWeaponRecoilRotationEuler = { 0.0f, 0.0f, 0.0f };
	struct WeaponRecoilState {
	glm::fvec3* position;
	glm::fvec3* rotation;
	};
	float recoilPositionRecoverySpeed = 10.0f;
	float recoilRotationRecoverySpeed = 8.0f;
	WeaponRecoilState GetRecoilState(uevr::API::UObject* weaponMesh);
	glm::fvec3 GetWeaponGripPositionOffset(int hand) const;
	glm::fquat GetWeaponGripRotationOffset(int hand) const;
	glm::fvec3 GetWeaponGripPositionOffset(uevr::API::UObject* weaponMesh) const;
	void ApplyRecoil(uevr::API::UObject* weaponMesh, bool isShooting, const glm::fvec3& positionRecoilForce, const glm::fvec3& rotationRecoilForceEuler, float delta);
	void HandleCameraWeaponAiming();
	bool IsWeaponShooting(bool firstWeapon) const;
	bool IsAimStabilizedWeapon() const;
	bool IsUtilityAimBypassWeapon() const;
	int CurrentWeaponModelIdForStats() const;
	void LogSpreadProbeIfShot(bool firstWeapon, bool isShooting);
	bool weaponScaledVisible = true;
	bool motionWeaponTrackingEnabled = true;
	bool visualWeaponTrackingEnabled = true;
	// One movable body-relative idle slot. This is presentation state only; the
	// normal controller-driven weapon path remains authoritative while held.
	bool magneticIdleWeaponActive = false;
	bool magneticReleaseRequested = false;
	bool magneticGripAttached = false;
	// Identifies the weapon that owns the current magnetic grip. This prevents a
	// stale melee-grip guard from surviving a weapon-change transition.
	int magneticGripWeaponId = -1;
	std::array<glm::fvec3, 2> motionMeleePreviousControllerPositions{};
	std::array<bool, 2> motionMeleeControllerPositionValid{ false, false };
	// Final held-pose contact samples. These are native-world base/tip points
	// derived from the held mesh, or the bounded hand geometry for short fists
	// and missing/degenerate melee bounds.
	std::array<glm::fvec3, 2> motionMeleePreviousBasePoints{};
	std::array<glm::fvec3, 2> motionMeleePreviousTipPoints{};
	std::array<bool, 2> motionMeleeContactPoseValid{ false, false };
	std::array<std::unordered_set<uintptr_t>, 2> motionMeleeContactEntities{};
	std::array<uint32_t, 2> motionMeleeContactSwingGeneration{};
	uint32_t motionMeleeTransitionGeneration = 0;
	int motionMeleeLastWeapon = -1;
	uint8_t motionMeleeLastGripMask = 0;
	bool motionMeleeLastEligible = false;
	std::array<bool, 2> motionMeleeSwingArmed{ true, true };
	std::array<float, 2> motionMeleeLowSpeedDwellRemaining{};
	std::array<float, 2> motionMeleeInterSwingCooldownRemaining{};
	std::array<float, 2> motionMeleeContactWindowRemaining{};
	std::array<bool, 2> motionMeleeHandActiveState{ false, false };
	// 0 = short hand/fist geometry, 1 = held weapon mesh, -1 = inactive.
	// Reset when ownership changes so a weapon-to-fist role transfer can never
	// sweep between two unrelated geometry spaces.
	std::array<int8_t, 2> motionMeleeHandGeometryMode{ -1, -1 };
	// Raw physical trigger state is presentation-only for physical melee. The
	// authoritative engine-thread snapshot below prevents those triggers from
	// leaking into GTA's native fight/fire input while retaining both clenches.
	std::atomic<uint8_t> motionMeleeClenchMask{ 0 };
	std::atomic<bool> motionMeleeNativeTriggerBlockSnapshot{ false };
	uint8_t motionMeleeLastLoggedClenchMask = 0xFF;
	uint32_t interactionEngineTickGeneration = 0;
	std::atomic<uint32_t> motionMeleeContactSequence{ 0 };
	std::unordered_set<uintptr_t> motionMeleeRejectedContactDiagnostics{};
	uevr::API::UObject* motionMeleeDebugAxisComponent = nullptr;
	uevr::API::UObject* motionMeleeDebugAxisCharacter = nullptr;
	bool motionMeleeDebugAxisCreationFailed = false;
	uevr::API::UObject* motionMeleePedImpactSound = nullptr;
	uevr::API::UObject* motionMeleeFistImpactSound = nullptr;
	uevr::API::UObject* motionMeleeBluntImpactSound = nullptr;
	uevr::API::UObject* motionMeleeSharpImpactSound = nullptr;
	uevr::API::UObject* motionMeleeVehicleImpactSound = nullptr;
	bool motionMeleeImpactSoundLoadAttempted = false;
	bool motionMeleeImpactAudioFailureLogged = false;
	uevr::API::UObject* motionThrowableMolotovImpactEffect = nullptr;
	uevr::API::UObject* motionThrowableMolotovFlameEffect = nullptr;
	uevr::API::UObject* motionThrowableSmokeEffect = nullptr;
	uevr::API::UObject* motionThrowableVehicleImpactEffect = nullptr;
	uevr::API::UObject* motionThrowableVehicleExplosionEffect = nullptr;
	uevr::API::UObject* motionThrowableImpactSound = nullptr;
	// The game's own Molotov-impact Blueprint. Custom flight owns the bottle
	// until a measured collision, then this actor owns the native fire behavior.
	uevr::API::UClass* motionThrowableNativeExplosionClass = nullptr;
	// BP_Fire is the persistent native fire owner. The custom bottle remains
	// responsible for flight; this class is spawned only after collision.
	uevr::API::UClass* motionThrowableNativeFireClass = nullptr;
	uevr::API::UClass* motionThrowableNativeSpawnLibraryClass = nullptr;
	uevr::API::UObject* motionThrowableNativeMolotovDebrisTemplate = nullptr;
	uint8_t motionThrowableNativeMolotovTypeByte = 0;
	bool motionThrowableNativeMolotovSelectionResolved = false;
	bool motionThrowableImpactAssetsLoadAttempted = false;
	bool motionThrowableNativeExplosionLoadAttempted = false;
	bool motionThrowableImpactFailureLogged = false;
	struct MotionThrowableImpactVisual
	{
		uevr::API::UObject* fire = nullptr;
		uevr::API::UObject* smoke = nullptr;
		ULONGLONG expiresAt = 0;
	};
	std::vector<MotionThrowableImpactVisual> motionThrowableImpactVisuals;
	struct MotionThrowableNativeImpactActors
	{
		uevr::API::UObject* explosion = nullptr;
		uevr::API::UObject* fire = nullptr;
		ULONGLONG explosionExpiresAt = 0;
		ULONGLONG fireExpiresAt = 0;
		bool extinguishCalled = false;
	};
	std::vector<MotionThrowableNativeImpactActors> motionThrowableNativeImpactActors;
	struct MotionThrowableVehicleBurn
	{
		uintptr_t entity = 0;
		std::array<float, 3> nativePoint{};
		ULONGLONG expiresAt = 0;
		ULONGLONG nextDamageAt = 0;
		uint32_t ticks = 0;
	};
	std::vector<MotionThrowableVehicleBurn> motionThrowableVehicleBurns;
	uevr::API::UObject* motionMeleeGeometryMesh = nullptr;
	int motionMeleeGeometryWeapon = -1;
	int motionMeleeGeometryPrincipalAxis = -1;
	glm::fvec3 motionMeleeGeometryLocalBase{};
	glm::fvec3 motionMeleeGeometryLocalTip{};
	float motionMeleeGeometryLocalRadius = 0.0f;
	bool motionMeleeGeometryValid = false;
	// Per-hand physical throwable sampling. Lua publishes raw input transitions;
	// pose sampling and authoritative weapon/state validation remain engine-thread
	// owned. A release can arm one verified native launch override.
	std::atomic<bool> throwableProbeEdgePending{ false };
	std::atomic<bool> throwableProbeReleasePending{ false };
	std::atomic<bool> throwableProbeCancelPending{ false };
	std::atomic<uint32_t> throwableProbeEdgeSequence{ 0 };
	std::atomic<uint8_t> throwableProbeEdgeHandMask{ 0 };
	std::atomic<uint8_t> throwableProbeEdgeGripMask{ 0 };
	std::atomic<int> throwableProbeEdgeLuaWeapon{ -1 };
	std::atomic<uint32_t> throwableProbeReleaseSequence{ 0 };
	std::atomic<uint8_t> throwableProbeReleaseHandMask{ 0 };
	std::atomic<uint8_t> throwableProbeReleaseGripMask{ 0 };
	std::atomic<uint32_t> throwableProbeReleaseHoldMilliseconds{ 0 };
	std::atomic<int> throwableProbeReleaseLuaWeapon{ -1 };
	bool throwableProbeActive = false;
	uint32_t throwableProbeSequence = 0;
	uint8_t throwableProbeHandMask = 0;
	uint8_t throwableProbeGripMask = 0;
	std::array<bool, 2> throwableProbePoseValid{ false, false };
	std::array<bool, 2> throwableProbeLastPoseFromVisibleBone{ false, false };
	std::array<glm::fvec3, 2> throwableProbePreviousWorldPositions{};
	std::array<glm::fvec3, 2> throwableProbeLastWorldPositions{};
	std::array<glm::fvec3, 2> throwableProbeLastDirections{};
	std::array<glm::fquat, 2> throwableProbePreviousWorldRotations{
		glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f),
		glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f) };
	std::array<glm::fquat, 2> throwableProbeLastWorldRotations{
		glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f),
		glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f) };
	std::array<glm::fvec3, 2> throwableProbeSmoothedVelocityMps{};
	std::array<glm::fvec3, 2> throwableProbeSmoothedAngularVelocity{};
	std::array<uint32_t, 2> throwableProbeSampleCounts{};
	static constexpr size_t ThrowableVelocityHistorySize = 12;
	std::array<std::array<glm::fvec3, ThrowableVelocityHistorySize>, 2>
		throwableProbeVelocityHistoryMps{};
	std::array<std::array<ULONGLONG, ThrowableVelocityHistorySize>, 2>
		throwableProbeVelocityHistoryTimes{};
	std::array<std::array<glm::fvec3, ThrowableVelocityHistorySize>, 2>
		throwableProbePositionHistoryM{};
	std::array<std::array<ULONGLONG, ThrowableVelocityHistorySize>, 2>
		throwableProbePositionHistoryTimes{};
	std::array<size_t, 2> throwableProbeVelocityHistoryCursor{};
	uint32_t throwableMotionOverrideSequence = 0;
	ULONGLONG throwableMotionOverrideExpiresAt = 0;
	int throwableMotionOverrideWeapon = -1;
	uevr::API::UObject* throwableMotionHiddenMesh = nullptr;
	ULONGLONG throwableMotionVisualRestoreAt = 0;
	// A custom proxy owns the released bottle visual until the next accepted
	// grip.  The native source mesh is body-attached, so restoring it at proxy
	// impact makes an unused Molotov jog with the hidden character body.
	bool motionThrowableSourceHidden = false;
	struct MotionThrowableFlight
	{
		bool active = false;
		uint32_t sequence = 0;
		uint32_t generation = 0;
		int weaponType = -1;
		int hand = -1;
		uevr::API::UObject* ownerCharacter = nullptr;
		uevr::API::UObject* sourceMesh = nullptr;
		uevr::API::UObject* visual = nullptr;
		uevr::API::UObject* collisionProxy = nullptr;
		glm::fvec3 position{};
		glm::fvec3 previousPosition{};
		glm::fvec3 linearVelocityUE{};
		glm::fvec3 angularVelocity{};
		glm::fquat rotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
		float ageSeconds = 0.0f;
		uint32_t bounceCount = 0;
		bool settled = false;
		bool earlyDiagnosticLogged = false;
		bool midDiagnosticLogged = false;
	};
	MotionThrowableFlight motionThrowableFlight{};
	// A new grip may begin before the prior proxy reaches a surface. Keep a
	// small bounded set of detached flights alive so rapid throws do not destroy
	// the older bottle or lose its collision/effect opportunity.
	std::vector<MotionThrowableFlight> motionThrowableDetachedFlights;
	bool processingDetachedThrowableFlights = false;
	bool motionThrowableWorldSweepReadyLogged = false;
	bool motionThrowableWorldSweepFailureLogged = false;
	int motionThrowableWorldTraceChannel = -1;
	uint8_t motionThrowableWorldTraceProbeCursor = 0;
	void ResetPhysicalThrowableProbe(const char* reason, bool logCancellation);
	void ResetMotionThrowableFlight(const char* reason, bool logResult);
	void DestroyMotionThrowableFlightVisual(MotionThrowableFlight& flight);
	void DestroyMotionThrowableImpactVisual(MotionThrowableImpactVisual& visual);
	void DestroyMotionThrowableNativeImpactActors(
		MotionThrowableNativeImpactActors& actors, bool destroyExplosion,
		bool destroyFire);
	void ProcessMotionThrowableImpactVisuals();
	void StartMotionThrowableVehicleBurn(
		const MemoryManager::NativeMeleeContact& contact);
	void ProcessMotionThrowableVehicleBurns();
	void EnsureMotionThrowableVisualRenderable(uevr::API::UObject* component);
	bool StartMotionThrowableFlight(uint32_t sequence, int weaponType, int hand,
		const glm::fvec3& position, const glm::fquat& rotation,
		const glm::fvec3& linearVelocityMps, const glm::fvec3& angularVelocity);
	void ProcessMotionThrowableFlight(float delta);
	bool SetMotionThrowableVisualTransform(uevr::API::UObject* component,
		const glm::fvec3& position, const glm::fquat& rotation,
		bool sweep = false) const;
	bool ReadMotionMeleeHandWorldPose(int controllerHand,
		const glm::fvec3& controllerPosition, const glm::fquat& controllerRotation,
		glm::fvec3& worldPosition, glm::fquat& worldRotation,
		bool& usedVisibleBone) const;
	bool ReadMotionMeleeControllerWorldPose(int controllerHand,
		const glm::fvec3& controllerPosition, const glm::fquat& controllerRotation,
		glm::fvec3& worldPosition, glm::fquat& worldRotation) const;
	bool ReadMotionMeleeHandGeometry(int controllerHand,
		const glm::fvec3& controllerPosition, const glm::fquat& controllerRotation,
		glm::fvec3& baseUE, glm::fvec3& tipUE, float& radiusUE) const;
	bool ReadMotionMeleeContactGeometry(const glm::fvec3& meshPosition,
		const glm::fquat& meshRotation, glm::fvec3& baseUE, glm::fvec3& tipUE,
		float& radiusUE);
	bool EnsureMotionMeleeDebugAxis();
	void UpdateMotionMeleeDebugAxis(const glm::fvec3& startUE,
		const glm::fvec3& endUE, float radiusUE);
	void HideMotionMeleeDebugAxis();
	bool PlayMotionMeleeImpactAudio(const MemoryManager::NativeMeleeContact& contact,
		int damageWeaponType);
	bool PlayMotionThrowableImpactEffect(int weaponType,
		const glm::fvec3& impactPoint,
		const MemoryManager::NativeMeleeContact* contact = nullptr);
	bool vehicleFreeAimPresentationActive = false;
	// Engine-thread diagnostic state only; never retain a vehicle UObject here.
	int vehicleFreeAimLastRejection = -1;
	std::atomic<bool> vehicleFaceButtonHeld{ false };
	std::atomic<bool> vehicleFaceButtonFirePending{ false };
	// One-shot gate for the vehicle damage-ray override. A frame-level aim
	// snapshot must never authorize a vehicle trace by itself.
	std::atomic<bool> vehicleShotTraceArmPending{ false };
	uint32_t vehicleFaceButtonSequence = 0;
	uint32_t vehicleFaceButtonLastLoggedSequence = 0;
	uint32_t vehicleFaceButtonTraceSequenceAtPress = 0;
	uint64_t vehicleFaceButtonNoShotCheckTime = 0;
	bool vehicleFaceButtonNoShotCheckPending = false;
	int magneticGripHand = -1;
	int magneticIdleAnchorHand = -1;
	int magneticIdleAnchorBucket = -1;
	int magneticIdleWeaponId = -1;
	uint8_t magneticProcessedGripMask = 0;
	std::array<uint32_t, 2> magneticConsumedGripPressGeneration{};
	std::array<uint32_t, 2> magneticLoggedPendingGripPressGeneration{};
	glm::fvec3 magneticIdleLocalPosition = { -12.0f, 28.0f, -48.0f };
	glm::fvec3 magneticIdleLocalForward = { 1.0f, 0.0f, 0.0f };
	glm::fvec3 magneticIdleLocalRight = { 0.0f, 1.0f, 0.0f };
	glm::fvec3 magneticIdleLocalUp = { 0.0f, 0.0f, 1.0f };
	glm::fquat magneticStableBodyRotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	bool magneticStableBodyRotationValid = false;
	uint64_t magneticStableBodyRotationLastUpdate = 0;
	uint64_t magneticBodyFrameRebaseAt = 0;
	uevr::API::UObject* magneticAnchoredWeaponMesh = nullptr;
	uevr::API::UObject* magneticDetachedWeaponMesh = nullptr;
	uevr::API::UObject* magneticIdleNativeParent = nullptr;
	uevr::API::UObject* magneticBodyAnchorParent = nullptr;
	uevr::API::UObject* magneticLegFrameComponent = nullptr;
	uevr::API::FName magneticLeftThighBone{};
	uevr::API::FName magneticRightThighBone{};
	bool magneticLegFrameBonesResolved = false;
	bool magneticLegFrameResolutionAttempted = false;
	bool magneticLegFrameFallbackLogged = false;
	uevr::API::UObject* magneticDetachedPlayerCharacter = nullptr;
	uevr::API::FName magneticIdleNativeSocket{};
	bool magneticIdleWeaponDetached = false;
	bool magneticIdleWeaponBodyAttached = false;
	bool magneticIdleDetachFailureLogged = false;
	bool magneticCustomAnchorValid = false;
	int magneticCustomAnchorWeaponId = -1;
	struct MagneticWaistAnchor {
		glm::fvec3 position{};
		glm::fvec3 forward{};
		glm::fvec3 right{};
		glm::fvec3 up{};
		int hand = -1;
		uint32_t gripGeneration = 0;
	};
	std::unordered_map<int, MagneticWaistAnchor> magneticWaistAnchors;
	bool magneticWaistAnchorsLoaded = false;
	int magneticLastHeldPoseHand = -1;
	uint32_t magneticLastHeldPoseGripGeneration = 0;
	bool explicitWeaponCyclePending = false;
	int explicitWeaponCycleSourceWeaponId = -1;
	int explicitWeaponCycleRestoreAnchorHand = -1;
	int explicitWeaponCycleRestoreAnchorBucket = -1;
	uint64_t explicitWeaponCycleDeadline = 0;
	std::atomic<bool> magneticTriggerBlockedSnapshot{ false };
	bool IsMagneticIdleSlotEligible() const;
	bool IsControllerHeldUtility() const;
	int ResolveMagneticHolsterVerticalAxis() const;
	bool ReadMagneticBodyFrame(glm::fvec3& origin, glm::fquat& rotation);
	void SetMagneticIdleAnchor(int hand);
	bool CaptureMagneticReleaseAnchor(int hand, bool logCapture = true);
	bool ReadMagneticWaistAnchorsFile(const std::string& path,
		std::unordered_map<int, MagneticWaistAnchor>& result, int& loaded) const;
	bool WriteMagneticWaistAnchorsFile(const std::string& path,
		const std::unordered_map<int, MagneticWaistAnchor>& values) const;
	void LoadMagneticWaistAnchors();
	bool SaveMagneticWaistAnchors();
	void ConsumeCurrentGripPressGenerations();
	bool BeginMagneticGrip(int hand, const char* reason);
	void EnterMagneticIdleSlot(int anchorHand = -1, bool allowSavedPose = true);
	void SuspendMagneticIdleSlot();
	void ApplyMagneticIdlePose();
	bool DetachMagneticIdleWeapon();
	bool AttachMagneticIdleWeaponToBody();
	void DetachMagneticIdleWeaponFromBody();
	void RestoreMagneticIdleWeaponAttachment(const char* reason);
	uevr::API::UObject* visibilityAppliedFirstWeaponMesh = nullptr;
	uevr::API::UObject* visibilityAppliedSecondWeaponMesh = nullptr;
	uevr::API::UObject* motionConfiguredFirstWeaponMesh = nullptr;
	uevr::API::UObject* motionConfiguredSecondWeaponMesh = nullptr;
	int motionConfiguredFirstHand = -1;
	int motionConfiguredSecondHand = -1;
	int motionConfiguredFirstCalibrationRole = -1;
	enum class RuntimeHandRole : uint8_t {
		Inactive = 0,
		FreeTracked,
		PrimaryWeapon,
		SupportWeapon,
		VehicleNative,
		VehiclePrimary,
		CalibrationPrimary,
		CalibrationSupport
	};
	struct RuntimeHandState {
		RuntimeHandRole role = RuntimeHandRole::Inactive;
		uint32_t generation = 0;
		int weaponId = -1;
		bool gripHeld = false;
	};
	std::array<RuntimeHandState, 2> runtimeHandStates{};
	uint32_t runtimeHandGeneration = 0;
	bool runtimeHandStateReset = true;
	const char* RuntimeHandRoleName(RuntimeHandRole role) const;
	void RefreshRuntimeHandRoles(const char* reason);
	void ResetRuntimeHandState(const char* reason, bool restoreTransient, bool cancelCalibration);
	bool freeAimWeaponHandsPresentationActive = false;
	bool freeAimWeaponHandsVisibilityInitialized = false;
	bool freeAimWeaponHandsVisible = true;
	uevr::API::UObject* freeAimFakeHandsCharacter = nullptr;
	uevr::API::UObject* freeAimFakeLeftHand = nullptr;
	uevr::API::UObject* freeAimFakeRightHand = nullptr;
	// Independent, baked single-hand meshes render only the closed hand. Each is
	// parented to its proven split skeletal clone, so existing controller/weapon
	// tracking stays authoritative without duplicating the opposite arm/hand.
	uevr::API::UObject* freeAimClenchedLeftHand = nullptr;
	uevr::API::UObject* freeAimClenchedRightHand = nullptr;
	uevr::API::UObject* freeAimClenchedLeftMesh = nullptr;
	uevr::API::UObject* freeAimClenchedRightMesh = nullptr;
	uevr::API::UObject* freeAimFakeWatch = nullptr;
	bool freeAimFakeHandsReady = false;
	bool freeAimFakeHandsInitialized = false;
	bool freeAimFakeHandsInitializing = false;
	int freeAimFakeHandsWarmupFrames = 0;
	int freeAimFakeHandsAlignmentFramesRemaining = 0;
	uevr::API::FName freeAimFakeLeftHandBoneName{};
	uevr::API::FName freeAimFakeRightHandBoneName{};
	uevr::API::FName freeAimFakeWatchHandBoneName{};
	bool freeAimClenchedHandsReady = false;
	uint8_t freeAimClenchedVisibleMask = 0;
	bool freeAimClenchedAssetFailureLogged = false;
	bool freeAimFakeHandsActive = false;
	bool freeAimSupportHandAttached = false;
	bool freeAimSupportWatchAttached = false;
	int8_t freeAimSupportAttachedHand = -1;
	uevr::API::UObject* freeAimSupportAttachedWeapon = nullptr;
	// During an active stabilized two-hand grip, the rear/primary fake hand can
	// follow the final weapon transform through the calibrated primary relation.
	// It is transient presentation state only; release restores controller motion.
	bool freeAimPrimaryHandAttached = false;
	int8_t freeAimPrimaryAttachedHand = -1;
	uevr::API::UObject* freeAimPrimaryAttachedWeapon = nullptr;
	// A primary/rear hand must spend one complete engine frame following its
	// controller (including the anatomical/wrist correction) before its final
	// visible bone pose is frozen into weapon-local space.
	uint8_t freeAimPrimaryAttachPrimeFramesRemaining = 0;
	int8_t freeAimPrimaryAttachPrimeHand = -1;
	uevr::API::UObject* freeAimPrimaryAttachPrimeWeapon = nullptr;
	uint32_t freeAimPrimaryAttachPrimeGeneration = 0;
	uint32_t freeAimPrimaryAttachPrimeEngineTick = 0;
	bool freeAimAppliedSupportContactActive = false;
	bool freeAimAppliedPrimaryGripAttachmentActive = false;
	bool freeAimSupportContactDiagnosticLogged = false;
	bool freeAimPrimaryAttachFailureDiagnosticLogged = false;
	bool freeAimBareHandCapabilityProbeLogged = false;
	bool nativeVehicleHandPoseTraceWasInVehicle = false;
	bool nativeVehicleHandPoseTraceCaptured = false;
	uint64_t nativeVehicleHandPoseTraceAt = 0;
	bool freeAimLeftPalmOffsetApplied = false;
	bool freeAimAppliedTwoHandWristOverrideActive = false;
	int freeAimAppliedCalibrationWeaponId = -1;
	bool freeAimAppliedVehicleRightOnly = false;
	bool vehicleNativeRightArmHidden = false;
	uevr::API::UObject* vehicleNativeRightArmComponent = nullptr;
	uevr::API::FName vehicleNativeRightArmBoneName{};
	bool freeAimHandsCreationBlocked = false;
	uevr::API::UObject* freeAimHandsBlockedCharacter = nullptr;
	bool freeAimHandsOffsetsLogged = false;
	bool freeAimHandsFailureLogged = false;
	bool freeAimWatchActive = false;
	bool freeAimWatchFailureLogged = false;
	int freeAimSupportFailureStageLogged = -1;
	bool IsFreeAimHandsEligibleWeapon() const;
	bool CreateFreeAimFakeHands();
	bool ApplyFreeAimFakeHandPresentation();
	void RemoveFreeAimFakeHands(bool destroyComponents = false);
	void ProcessNativeVehicleHandPoseTrace();
	bool firstSpreadProbeShotActive = false;
	bool secondSpreadProbeShotActive = false;
	struct AimCalibrationResidual {
		float yawDegrees = 0.0f;
		float pitchDegrees = 0.0f;
		float totalDegrees = 0.0f;
		float distance = 0.0f;
		int weaponType = 0;
		bool scoped = false;
	};
	std::vector<AimCalibrationResidual> aimCalibrationResiduals;
	uint32_t aimCalibrationGeneralSamples = 0;
	uint32_t aimCalibrationScopedSamples = 0;
	bool aimCalibrationWasEnabled = false;
	void SetComponentVisibility(uevr::API::UObject* object, bool visible,
		bool propagateToChildren = true);

	// Primary and support calibration are stored independently for each hand.
	// Reflection is used only to seed a missing side; it never couples later edits.
	enum class GripCalibrationRecord : uint8_t {
		PrimaryGrip = 0,
		SupportContact = 1
	};

	struct GripCalibrationTransform {
		glm::fvec3 position = glm::fvec3(0.0f);
		glm::fquat rotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
		bool valid = false;
		// v4 records used a full root-to-hand palm translation. They remain
		// recognizable so that the unsafe primary records can stay inert.
		bool palmFramed = false;
		// v6 primary records store an orientation-only controller-to-palm
		// relation. Position remains the existing compact weapon/controller
		// offset, so no skeleton root-to-hand translation enters calibration.
		bool anatomicalFramed = false;
	};

	struct ControllerPalmAdapter {
		// UEVR's controller-relative offset that presents the selected clone's
		// hand bone as the visible anatomical palm.
		glm::fvec3 position = glm::fvec3(0.0f);
		glm::fquat rotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
		bool valid = false;
	};
	mutable std::array<ControllerPalmAdapter, 2> cachedControllerPalmAdapters{};
	mutable std::array<uevr::API::UObject*, 2> cachedControllerPalmAdapterComponents{};
	mutable std::array<uint32_t, 2> cachedControllerPalmAdapterBones{};

	struct GripCalibrationSession {
		bool active = false;
		uint32_t runtimeGeneration = 0;
		int controllerHand = -1;
		int weaponId = -1;
		GripCalibrationRecord record = GripCalibrationRecord::PrimaryGrip;
		glm::fvec3 baselinePosition = glm::fvec3(0.0f);
		glm::fquat baselineRotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
		glm::fvec3 beginGripPosition = glm::fvec3(0.0f);
		glm::fquat beginAimRotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
		glm::fvec3 frozenWorldPosition = glm::fvec3(0.0f);
		Utilities::FRotator frozenWorldRotation{};
		uevr::API::UObject* frozenWeaponMesh = nullptr;
	};

	static constexpr size_t GripCalibrationRecordCount = 2;
	struct WeaponGripCalibration {
		std::array<GripCalibrationTransform, 2> primaryGrip{};
		std::array<GripCalibrationTransform, 2> supportContact{};
	};
	using GripCalibrationMap = std::unordered_map<int, WeaponGripCalibration>;
	GripCalibrationMap gripCalibrationTransforms;
	std::array<GripCalibrationSession, 2> gripCalibrationSessions{};
	std::atomic<uint8_t> gripCalibrationButtonMask{ 0 };
	uint8_t gripCalibrationProcessedButtonMask = 0;
	bool gripCalibrationLoaded = false;
	bool gripCalibrationFailureLogged = false;
	bool gripCalibrationModeObserved = false;
	const char* GripCalibrationRecordName(GripCalibrationRecord record) const;
	int GripCalibrationRecordIndex(GripCalibrationRecord record) const;
	bool IsGripCalibrationEligible(int controllerHand) const;
	bool ReadGripCalibrationTransform(int weaponId, GripCalibrationRecord record,
		int requestedHand, GripCalibrationTransform& result, int* sourceHand = nullptr) const;
	GripCalibrationTransform MirrorCanonicalTransform(const GripCalibrationTransform& value) const;
	glm::fquat MirrorLateralRotation(const glm::fquat& value) const;
	bool GetCanonicalPrimaryGripForHand(int hand, GripCalibrationTransform& result) const;
	bool GetSupportContactForHand(int hand, GripCalibrationTransform& result) const;
	bool ReadHandAnatomicalBasisRotation(int hand, glm::fquat& result) const;
	bool ConvertMirroredHandContact(const GripCalibrationTransform& source,
		int sourceHand, int destinationHand, GripCalibrationTransform& result) const;
	bool ReadGripCalibrationFile(const std::string& path, GripCalibrationMap& result,
		int& loaded, int& legacyIgnored) const;
	bool WriteGripCalibrationFile(const std::string& path, const GripCalibrationMap& values) const;
	bool ValidateGripCalibrationRoundTrip(const GripCalibrationMap& expected,
		const GripCalibrationMap& actual) const;
	void LoadGripCalibration();
	bool SaveGripCalibration();
	bool ReadControllerCalibrationPose(int controllerHand, glm::fvec3& gripPosition, glm::fquat& aimRotation) const;
	bool ReadCurrentWeaponWorldPose(glm::fvec3& position, Utilities::FRotator& rotation) const;
	bool ReadCurrentWeaponWorldTransform(glm::fvec3& position, glm::fquat& rotation) const;
	bool SetCurrentWeaponWorldPose(const glm::fvec3& position, const Utilities::FRotator& rotation) const;
	ControllerPalmAdapter BuildControllerPalmAdapter(int hand,
		const glm::fvec3& boneTranslation, const glm::fquat& boneRotation,
		bool applyPalmOffset = true) const;
	bool ReadControllerPalmAdapter(int hand, ControllerPalmAdapter& result) const;
	bool GetFakeHandControllerOffset(int controllerHand, glm::fvec3& position, glm::fquat& rotation) const;
	bool AttachSkeletalComponentToWeapon(uevr::API::UObject* component,
		const uevr::API::FName& boneName, const GripCalibrationTransform& contact);
	bool RealignSkeletalComponentToWeapon(uevr::API::UObject* component,
		const uevr::API::FName& boneName, const GripCalibrationTransform& contact);
	bool AttachSupportFakeHandToWeapon(int controllerHand, const GripCalibrationTransform& contact);
	bool RealignSupportFakeHandToWeapon(int controllerHand, const GripCalibrationTransform& contact);
	void RestoreSupportFakeHandAttachment();
	bool BuildPrimaryWeaponLocalContact(int controllerHand, GripCalibrationTransform& contact) const;
	bool AttachPrimaryFakeHandToWeapon(int controllerHand);
	void RestorePrimaryFakeHandAttachment();
	bool BeginGripCalibration(int controllerHand);
	void EndGripCalibration(int controllerHand, bool save);
	void EnforceGripCalibrationFreeze();
	bool IsVehicleFreeAimSupportedWeapon() const;
	bool ApplyVehicleNativeRightArmPresentation(bool hidden);

public:
	WeaponManager(PlayerManager* pm, CameraController* cc, MemoryManager* mm, SettingsManager* sm) : playerManager(pm), cameraController(cc), memoryManager(mm), settingsManager(sm)
	{
		twoHandConfiguredHandSnapshot.store(sm != nullptr && sm->leftHandedMode != SettingsManager::Disabled ? 0 : 1, std::memory_order_relaxed);
	};
	enum WeaponType {
		Unarmed = 0,
		BrassKnuckles = 1,
		GolfClub = 2,
		NightStick = 3,
		Knife = 4,
		BaseballBat = 5,
		Shovel = 6,
		PoolCue = 7,
		Katana = 8,
		Chainsaw = 9,
		Dildo1 = 10,
		Dildo2 = 11,
		Vibe1 = 12,
		Vibe2 = 13,
		Flowers = 14,
		Cane = 15,
		Grenade = 16,
		Teargas = 17,
		Molotov = 18,
		Pistol = 22,
		PistolSilenced = 23,
		DesertEagle = 24,
		Shotgun = 25,
		Sawnoff = 26,
		Spas12 = 27,
		MicroUzi = 28,
		Mp5 = 29,
		Ak47 = 30,
		M4 = 31,
		Tec9 = 32,
		Rifle = 33,
		Sniper = 34,
		RocketLauncher = 35,
		RocketLauncherHs = 36,
		Flamethrower = 37,
		Minigun = 38,
		Satchel = 39,
		Detonator = 40,
		SprayCan = 41,
		Extinguisher = 42,
		Camera = 43,
		NightVision = 44,
		Infrared = 45,
		Parachute = 46
	};
	WeaponType currentWeaponEquipped = Unarmed;
	WeaponType previousWeaponEquipped = Unarmed;
	bool firstWeaponShotDone = false;
	
	void UpdateActualWeaponMesh();
	bool EnsureCustomAkimboVisual();
	void RemoveCustomAkimboVisual(const char* reason);
	void CacheCustomAkimboEffectTemplate(uevr::API::UObject* component);
	bool HideBulletTrace();
	void UpdateShootingState(bool firstWeapon);
	void RedirectWorldShotEffects(bool firstWeapon);
	void ResetShootingState();
	void ProcessAiming(bool firstWeapon, bool applyGameAim = true);
	void ProcessAimCalibrationSample();
	void ResetAimCalibration();
	bool HasUsableWeapon(bool firstWeapon) const;
	void ProcessWeaponVisibility();
	void SetWeaponScaled(bool visible, bool force = false);
	void SetMotionWeaponTrackingEnabled(bool enabled, bool force = false);
	bool IsGameplayWeaponTrackingActive() const { return motionWeaponTrackingEnabled; }
	bool ShouldBlockWeaponTriggerInput() const { return magneticTriggerBlockedSnapshot.load(std::memory_order_acquire); }
	bool ShouldBlockNativeMeleeTriggerInput() const {
		return motionMeleeNativeTriggerBlockSnapshot.load(std::memory_order_acquire);
	}
	void ProcessMagneticIdleWeapon();
	void BeginInteractionEngineTick();
	void ProcessMotionMelee(float delta);
	void QueuePhysicalThrowableProbeEvent(int eventType, uint32_t sequence,
		uint8_t handMask, uint8_t gripMask, uint32_t holdMilliseconds,
		int luaWeaponId);
	void ProcessPhysicalThrowableProbe(float delta);
	void SetMeleeClenchState(bool leftTriggerHeld, bool rightTriggerHeld);
	void SetCustomAkimboInputState(uint8_t heldMask, uint8_t edgeMask, int luaWeaponId);
	void ProcessCustomAkimboState();
	bool IsCustomAkimboActive() const { return customAkimboActive; }
	void SetVehicleFaceButtonState(bool held);
	bool IsVehicleFreeAimActive() const;
	void SetFreeAimWeaponHandsPresentationActive(bool active);
	void ProcessFreeAimWeaponHands(bool force = false);
	void RestoreFreeAimWeaponHands();
	void DiscardPlayerOwnedRuntimeState(const char* reason);
	void SetGripState(bool leftGripHeld, bool rightGripHeld);
	bool PrepareForExplicitWeaponCycle();
	void InitializeGripCalibration();
	void InitializeMagneticHolster();
	uint8_t GetDualThumbCalibrationHandMask() const;
	void SetCalibrationButtonState(bool leftButtonHeld, bool rightButtonHeld);
	void ProcessGripCalibration();
	void CancelGripCalibration();
	void ResetGripCalibration();
	bool IsGripCalibrationActive() const { return gripCalibrationSessions[0].active || gripCalibrationSessions[1].active; }
	void SetTwoHandViewRotation(const UEVR_Rotatorf& rotation);
	bool ShouldPreservePrimaryHandForTwoHand() const;
	void ProcessTwoHandStabilization(float delta);
	void ProcessWeaponHandling(float delta);
	void ProcessBulletTracePostTick();
	void UnhookAndRepositionWeapon(bool restoreHands = true, bool force = false);

};
