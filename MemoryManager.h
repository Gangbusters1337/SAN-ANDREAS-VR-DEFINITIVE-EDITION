#pragma once

#include <unordered_map>
#include <cstdint>
#include <iostream>
#include <windows.h>
#include <array>
#include <functional>
#include <string>
#include <vector>

#include "SettingsManager.h"

// Define the OriginalByte struct
struct OriginalByte {
    uintptr_t address; // The memory address (offset)
    uint8_t value;     // The original byte value
};

struct MemoryBlock {
    uintptr_t address;
    size_t size;
    std::vector<uint8_t> bytes; // Stores the block of bytes

	// Constructor to initialize from address, size, and a contiguous hexadecimal string
	MemoryBlock(uintptr_t addr, size_t sz, uint64_t hexValue)
		: address(addr), size(sz) {
		// Convert the hexValue into a vector of bytes
		for (size_t i = 0; i < size; ++i) {
			uint8_t byte = static_cast<uint8_t>((hexValue >> (8 * (size - 1 - i))) & 0xFF);
			bytes.push_back(byte);
		}
	}
};

// MemoryManager class
class MemoryManager {
private:
	SettingsManager* const settingsManager;

	struct RuntimePatch {
		std::string name;
		uintptr_t address = 0;
		size_t overwriteSize = 0;
		std::vector<uint8_t> originalBytes;
		void* codeCave = nullptr;
		size_t codeCaveSize = 0;
		bool applied = false;
	};

	uintptr_t GetModuleBaseAddress(LPCTSTR moduleName);
	uintptr_t GetModuleSize() const;
	void AdjustAddresses();
	uintptr_t FindPattern(const std::vector<int>& pattern) const;
	std::vector<uintptr_t> FindPatterns(const std::vector<int>& pattern) const;
	void* AllocateNear(uintptr_t target, size_t size) const;
	bool WriteProcessBytes(uintptr_t address, const std::vector<uint8_t>& bytes) const;
	bool InstallBytePatch(const char* name, uintptr_t target, const std::vector<uint8_t>& bytes);
	bool InstallHookPatch(const char* name, uintptr_t target, size_t overwriteSize,
		const std::function<std::vector<uint8_t>(uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes)>& buildCode);
	bool ResolveCombatAssistStats();
	bool ResolveCombatAssistPlayerGlobals();
	void UpdateCombatAssistPlayerPointer();
	bool ResolveCombatAssistWeaponInfo();
	bool IsCombatAssistWeaponInfoReady();
	void ApplyCombatAssistWeaponInfoValues();
	void VerifyCombatAssistWeaponInfoValues();
	void RestoreCombatAssistWeaponInfoValues();

	struct WeaponInfoOriginalValues {
		bool captured = false;
		bool instantHit = false;
		float targetRange = 0.0f;
		float weaponRange = 0.0f;
		uint16_t ammoClip = 0;
		uint16_t damage = 0;
		float accuracy = 0.0f;
		float spread = 0.0f;
		float animLoopFire = 0.0f;
		float animLoop2Fire = 0.0f;
	};

	uintptr_t baseAddressGameEXE = NULL;
	void* exceptionHandlerHandle = nullptr;  // Store the handler so we can remove it later
	std::vector<RuntimePatch> combatAssistPatches;
	bool combatAssistApplyAttempted = false;
	bool combatAssistStatsResolved = false;
	bool combatAssistStatsResolveAttempted = false;
	uintptr_t combatAssistFloatStatsOffset = 0;
	bool combatAssistPlayerGlobalsResolved = false;
	bool combatAssistPlayerGlobalsResolveAttempted = false;
	uintptr_t combatAssistCurrentPlayerIndexAddress = 0;
	uintptr_t combatAssistPlayerInfoArrayAddress = 0;
	bool combatAssistWeaponInfoResolved = false;
	bool combatAssistWeaponInfoResolveAttempted = false;
	bool combatAssistWeaponInfoReady = false;
	bool combatAssistWeaponInfoPatched = false;
	bool combatAssistWeaponInfoWaitLogged = false;
	uintptr_t combatAssistWeaponInfoOffset = 0;
	std::array<WeaponInfoOriginalValues, 80> combatAssistOriginalWeaponInfoValues{};
	volatile LONG nativeShotSpreadBypassCountA = 0;
	volatile LONG nativeShotSpreadBypassCountB = 0;
	std::array<volatile LONG, 3> nativeAimReferenceBits{};
	volatile LONG nativeAimReferenceWeapon = 0;
	std::array<volatile LONG, 3> nativeAimCapturedReferenceBits{};
	volatile LONG nativeAimCapturedWeapon = 0;
	std::array<volatile LONG, 3> nativeAimShotStartBits{};
	std::array<volatile LONG, 3> nativeAimShotTargetBits{};
	std::array<volatile LONG, 3> nativeAimHitPointBits{};
	volatile LONG64 nativeAimHitEntity = 0;
	volatile LONG nativeAimSequence = 0;
	LONG lastReadNativeAimSequence = 0;
	// The player line-trace hook captures the actual native shot arguments before
	// the trace call. The override is published as float bit patterns so the
	// small x64 code cave can consume it without calling back into C++.
	volatile LONG nativeShotTraceOriginOverrideEnabled = 0;
	// Set only by the verified local-player damaging-trace entry. Visual trail
	// hooks require this mark so an armed pre-fire snapshot cannot affect an
	// unrelated effect call.
	volatile LONG nativeShotTraceOverrideConsumed = 0;
	// The vehicle return site is enabled only for the current validated local
	// player vehicle snapshot. It is disarmed before each aim update and armed
	// again only after a fresh mock-muzzle pair is published.
	volatile LONG nativeShotTraceVehicleOverrideArmed = 0;
	volatile LONG nativeShotTraceVehicleModeActive = 0;
	volatile LONG nativeShotTraceCallSiteRva = 0;
	// Even values are stable; odd values mean C++ is publishing a new six-float
	// origin/target pair. The trace cave validates this before and after copying.
	volatile LONG nativeShotTracePublishSequence = 0;
	std::array<volatile LONG, 3> nativeShotTraceOriginOverrideBits{};
	std::array<volatile LONG, 3> nativeShotTraceTargetOverrideBits{};
	// Immutable copy captured when the authoritative damage ray consumes a shot.
	// The high-level tracer path consumes it once, independently of later aim updates.
	std::array<volatile LONG, 3> nativeShotTrailOriginBits{};
	std::array<volatile LONG, 3> nativeShotTrailTargetBits{};
	volatile LONG nativeShotTrailPending = 0;
	std::array<volatile LONG, 3> nativeShotTraceCapturedStartBits{};
	std::array<volatile LONG, 3> nativeShotTraceCapturedTargetBits{};
	std::array<volatile LONG, 3> nativeShotTraceAppliedStartBits{};
	std::array<volatile LONG, 3> nativeShotTraceAppliedTargetBits{};
	volatile LONG nativeShotTraceCapturedOverride = 0;
	volatile LONG nativeShotTraceSequence = 0;
	LONG lastReadNativeShotTraceSequence = 0;
	volatile LONG nativeLineTraceContactDisabled = 0;
	bool nativeLineTraceContactDisabledLogged = false;
	bool nativeMeleeDamageDisabled = false;
	bool nativeMeleeDamageFailureLogged = false;
	bool nativeThrowableDamageDisabled = false;
	bool nativeThrowableDamageFailureLogged = false;
	bool nativeThrowableImpactDisabled = false;
	bool nativeThrowableImpactFailureLogged = false;
	bool nativeThrowableExplosionDisabled = false;
	bool nativeThrowableExplosionFailureLogged = false;
	// The DE weapon-fire path also passes a separate start/end pair to the
	// native weapon-effect/tracer routine. Keep a distinct probe so this path
	// cannot be confused with the collision trace above.
	std::array<volatile LONG, 3> nativeShotEffectCapturedStartBits{};
	std::array<volatile LONG, 3> nativeShotEffectCapturedTargetBits{};
	std::array<volatile LONG, 3> nativeShotEffectAppliedStartBits{};
	std::array<volatile LONG, 3> nativeShotEffectAppliedTargetBits{};
	volatile LONG nativeShotEffectCapturedOverride = 0;
	volatile LONG nativeShotEffectCallSiteRva = 0;
	volatile LONG nativeShotEffectOwnerLocal = 0;
	volatile LONG nativeShotEffectSequence = 0;
	LONG lastReadNativeShotEffectSequence = 0;
	volatile LONG playerBulletDamageReductionCount = 0;
	bool playerBulletDamageReductionLogged = false;
	// One-shot local-player projectile launch override. C++ publishes a complete
	// origin/velocity pair before GTA processes the physical trigger release; the
	// verified AddProjectile convergence hook consumes it exactly once for weapon
	// types 16..18. Native ammo, fuse, ownership, projectile and explosion logic
	// remain untouched.
	volatile LONG nativeThrowableMotionOverrideActive = 0;
	volatile LONG nativeThrowableMotionWeapon = 0;
	volatile LONG nativeThrowableMotionPublishSequence = 0;
	volatile LONG nativeThrowableMotionConsumedSequence = 0;
	std::array<volatile LONG, 3> nativeThrowableMotionOriginBits{};
	std::array<volatile LONG, 3> nativeThrowableMotionVelocityBits{};
	// Captured at the already-proven local-player weapon-fire prologue. The
	// native impact path requires the real live CWeapon entry; a type-only stub
	// can reach generic damage code but cannot reproduce Molotov ownership.
	volatile LONG64 nativeThrowableLiveWeaponEntry = 0;
	volatile LONG nativeThrowableLiveWeaponSequence = 0;
	bool nativeThrowableMotionPatchApplyAttempted = false;
	// The custom Molotov flight does not require a native launch interception.
	// Keep this false until a launch ABI is proved for the exact game build.
	bool nativeThrowableMotionPatchInstalled = false;
	LONG lastReadNativeThrowableMotionConsumedSequence = 0;
	// Passive capture at the native throwable launch convergence point. This is
	// deliberately separate from the custom origin/velocity override state so
	// native and custom release paths can be compared without changing either.
	volatile LONG nativeThrowableLaunchProbeSequence = 0;
	volatile LONG nativeThrowableLaunchProbeWeapon = 0;
	volatile LONG nativeThrowableLaunchProbeOverridden = 0;
	std::array<volatile LONG, 3> nativeThrowableLaunchProbeOriginBits{};
	std::array<volatile LONG, 3> nativeThrowableLaunchProbeVelocityBits{};
	std::array<volatile LONG, 3> nativeThrowableLaunchProbeDirectionBits{};
	volatile LONG nativeThrowableLaunchProbeForceBits = 0;
	volatile LONG64 nativeThrowableLaunchProbeDirectionPointer = 0;
	volatile LONG64 nativeThrowableLaunchProbeTarget = 0;
	bool nativeThrowableLaunchProbePatchInstalled = false;
	LONG lastReadNativeThrowableLaunchProbeSequence = 0;
	ULONGLONG lastCombatAssistWeaponInfoReadyCheckTime = 0;
	ULONGLONG lastCombatAssistWeaponInfoVerifyTime = 0;
	ULONGLONG lastCombatAssistWeaponInfoOverwriteLogTime = 0;
	uintptr_t cachedPlayerPointer = 0;
	struct ManualReloadWeaponState {
		uintptr_t weaponEntry = 0;
		uint32_t weaponType = 0;
		uint32_t hiddenReserve = 0;
		uint32_t lastVisibleTotal = 0;
		uint32_t magazineCapacity = 0;
		bool emptyLatched = false;
		bool initialized = false;
	};
	std::array<ManualReloadWeaponState, 47> manualReloadWeapons{};
	bool manualReloadCaptureApplyAttempted = false;
	bool manualReloadEmptyStateKnown = false;
	bool manualReloadEmptyStateReported = false;
	volatile LONG64 manualReloadCapturedWeaponEntry = 0;
	volatile LONG manualReloadCapturedWeaponType = 0;
	volatile LONG manualReloadCapturedPreShotClip = 0;
	volatile LONG manualReloadCaptureSequence = 0;
	LONG lastReportedManualReloadCaptureSequence = 0;
	bool playerSemiAutoFireGateApplyAttempted = false;
	volatile LONG playerSemiAutoPullHeld = 0;
	volatile LONG playerSemiAutoPullWeaponType = 0;
	volatile LONG playerSemiAutoShotPermit = 0;
	volatile LONG playerSemiAutoBlockedCount = 0;
	bool customAkimboFirePatchApplyAttempted = false;
	bool customAkimboFirePatchInstalled = false;
	volatile LONG customAkimboEnabled = 0;
	volatile LONG customAkimboWeaponType = 0;
	// Native CTaskSimpleUseGun flags are bit 0=right and bit 1=left.
	volatile LONG customAkimboHeldMask = 0;
	volatile LONG customAkimboPendingMask = 0;
	volatile LONG customAkimboTaskFireMask = 0;
	volatile LONG customAkimboActiveHand = -1;
	volatile LONG customAkimboTraceValidMask = 0;
	volatile LONG customAkimboTaskInjectionSequence = 0;
	volatile LONG customAkimboAcceptedShotSequence = 0;
	// Controller-hand bits: bit 0=left, bit 1=right. Consumed by the visual
	// layer only; native damage/ammo ownership remains independent of effects.
	volatile LONG customAkimboAcceptedHandMask = 0;
	volatile LONG64 customAkimboTaskPointer = 0;
	std::array<std::array<volatile LONG, 3>, 2> customAkimboTraceOriginBits{};
	std::array<std::array<volatile LONG, 3>, 2> customAkimboTraceTargetBits{};
	bool manualReloadStageActive = false;
	uintptr_t manualReloadStageEntry = 0;
	uint32_t manualReloadStageWeaponType = 0;
	uint32_t manualReloadStageOriginalWeaponState = 0;
	uint32_t manualReloadStageOriginalClip = 0;
	uint32_t manualReloadStageTargetClip = 0;
	uint32_t manualReloadStageConsumedReserve = 0;
	ULONGLONG manualReloadStageStartedAt = 0;
	void ReportManualReloadEmptyState(bool empty);
	bool SelectPlayerWeaponType(uint32_t weaponType);
	bool vehicleLayoutCacheValid = false;
	size_t cachedVehiclePointerOffset = 0;
	size_t cachedVehicleModelOffset = 0;
	uintptr_t cachedResolvedVehiclePointer = 0;
	ULONGLONG lastVehicleLayoutScanTime = 0;
	uintptr_t healthRecoveryPlayerPointer = 0;
	ULONGLONG lastHealthRecoverySampleTime = 0;
	ULONGLONG lastHealthDamageTime = 0;
	ULONGLONG healthRecoveryControlPauseStartTime = 0;
	float lastObservedPlayerHealth = -1.0f;
	uintptr_t healthRecoveryHealthAddress = 0;
	uintptr_t healthRecoveryMaxHealthAddress = 0;
	bool healthRecoveryLayoutValid = false;
	uintptr_t scriptSpaceAddress = 0;
	bool scriptSpaceValidated = false;
	bool scriptSpaceValidationAttempted = false;
	bool triggerTimingArmed = false;
	uint32_t triggerTimingSequence = 0;
	uint64_t triggerTimingEdgeTimestampUs = 0;
	bool triggerTimingAimProxyLogged = false;
	static volatile LONG64 nativeShotTimingTimestampUs;
	static volatile LONG nativeShotTimingSequence;
	static volatile LONG nativeShotTimingPath;
	static volatile LONG nativeShotTimingPending;
	static volatile LONG triggerTimingActiveSequence;
	static volatile LONG triggerTimingActive;

public:
	MemoryManager(SettingsManager* sm) : settingsManager(sm) {};
	static std::array<uintptr_t, 16> cameraMatrixAddresses;

	struct WeaponInfoDebugSnapshot {
		bool found = false;
		int modelId = -1;
		int matchingEntries = 0;
		uint32_t fireType = 0;
		float minSpread = 0.0f;
		float maxSpread = 0.0f;
		float minAccuracy = 0.0f;
		float maxAccuracy = 0.0f;
		float targetRange = 0.0f;
		float weaponRange = 0.0f;
		uint16_t damage = 0;
	};

	struct AimCalibrationSample {
		std::array<float, 3> intendedDirection{};
		std::array<float, 3> shotStart{};
		std::array<float, 3> shotTarget{};
		std::array<float, 3> hitPoint{};
		uintptr_t hitEntity = 0;
		int weaponType = 0;
		uint32_t sequence = 0;
	};

	struct NativeShotTraceProbe {
		std::array<float, 3> rawStart{};
		std::array<float, 3> rawTarget{};
		std::array<float, 3> appliedStart{};
		std::array<float, 3> appliedTarget{};
		bool overridden = false;
		uint32_t callSiteRva = 0;
		uint32_t sequence = 0;
	};

	struct NativeShotEffectProbe {
		std::array<float, 3> rawStart{};
		std::array<float, 3> rawTarget{};
		std::array<float, 3> appliedStart{};
		std::array<float, 3> appliedTarget{};
		bool overridden = false;
		uint32_t callSiteRva = 0;
		bool ownerLocal = false;
		uint32_t sequence = 0;
	};

	struct NativeThrowableLaunchProbe {
		std::array<float, 3> rawOrigin{};
		std::array<float, 3> rawVelocity{};
		std::array<float, 3> rawDirection{};
		float force = 0.0f;
		uintptr_t target = 0;
		bool directionSupplied = false;
		int weaponType = 0;
		bool overridden = false;
		uint32_t sequence = 0;
	};

	enum class NativeMeleeDamageResult : uint8_t {
		Rejected,
		Attempted,
		Accepted,
		UnverifiedVehicleDispatch
	};

	struct NativeMeleeContact {
		uintptr_t entity = 0;
		std::array<float, 3> point{};
		std::array<float, 3> normal{ 0.0f, 0.0f, 1.0f };
		uint8_t piece = 0;
		uint8_t entityType = 0;
		// Preserve the native CColPoint bytes returned by the synchronous LOS
		// query. The impact dispatcher consumes more than position/piece fields.
		alignas(16) std::array<uint8_t, 0x48> nativeCollisionPoint{};
		bool nativeCollisionPointValid = false;
		float targetHealthBefore = -1.0f;
		float targetHealthAfter = -1.0f;
		NativeMeleeDamageResult damageResult = NativeMeleeDamageResult::Rejected;
	};

	std::array<uintptr_t, 3> aimForwardVectorAddresses 	{ 0x53E2668, 0x53E266C, 0x53E2670 }; // x, y, z
	uintptr_t xAxisSpraysAimAddress = 0x53E2558;
	std::array<uintptr_t, 3> cameraPositionAddresses { 0x53E2674, 0x53E2678, 0x53E267C }; // x, y, z
	std::array<uintptr_t, 3> playerHeadPositionAddresses { 0x58013D8, 0x58013DC, 0x58013E0 }; // x, y, z
	std::array<uintptr_t, 3> playerPositionAddresses { 0x5067948, 0x506794C, 0x5067950 }; // x, y, z

	static uintptr_t playerShootInstructionAddress;
	static uintptr_t playerShootCam45InstructionAddress;

	uintptr_t cameraModeAddress = 0x53E2580;
	uintptr_t vehicleCameraModeAddress = 0x53E24A0;
	uintptr_t onFootCameraModeAddress = 0x53E2490;
	uintptr_t playerIsInControlAddress = 0x53E8840;
	uintptr_t playerIsInVehicleAddress = 0x51B39D4;
	uintptr_t vehicleTypeAddress = 0x5031278;
	uintptr_t playerShootFromCarInputAddress = 0x50251A8;
	uintptr_t weaponWheelDisplayedAddress = 0x507C580;
	//uintptr_t cutscenePlayingAddress = 0x53E254C;

	void InitMemoryManager();
	void ToggleAllMemoryInstructions(bool enableOriginalInstructions);
	void ToggleHeliCanonCameraModMemoryInstructions(bool enableOriginalInstructions);
	void NopVehicleRelatedMemoryInstructions();
	void RestoreVehicleRelatedMemoryInstructions();
	void RestoreCarAimingVectorInstructions();
	void ApplyCombatAssistPatches();
	void ApplyManualReloadCapturePatch();
	void ApplyPlayerSemiAutoFireGatePatch();
	void ApplyCustomAkimboFirePatch();
	void ApplyNativeThrowableMotionPatch();
	void MaintainManualReloadMode();
	void RestoreManualReloadState();
	void MaintainCombatAssistValues();
	void UpdateHealthRecovery(bool playerInControl);
	void RestoreCombatAssistPatches();
	void RefreshCombatAssistWeaponInfoValues();
	bool ReadWeaponInfoDebugSnapshot(int modelId, WeaponInfoDebugSnapshot& snapshot);
	void GetNativeShotSpreadBypassCounts(uint32_t& pathA, uint32_t& pathB) const;
	void SetAimCalibrationReference(float x, float y, float z, int weaponType);
	bool ReadAimCalibrationSample(AimCalibrationSample& sample);
	void DiscardPendingAimCalibrationSample();
	void RecordTriggerTimingEdge(uint32_t sequence, int weaponType, bool aimRequested,
		bool inVehicle, bool inputEnabled, bool eligible, bool gateEnabled,
		const char* side, double luaClockSeconds);
	void RecordTriggerTimingRelease(uint32_t sequence, const char* side, double luaClockSeconds);
	void RecordTriggerTimingAimVectorProxy(int weaponType, bool firstWeapon, bool inVehicle, int cameraMode);
	void RecordTriggerTimingMuzzleParticle(int weaponType, bool firstWeapon);
	void FlushTriggerTimingNativeShot(int weaponType, bool inVehicle);
	void ResetTriggerTimingProbe();
	static void CaptureNativeShotObservation(uintptr_t instructionAddress);
	bool SetNativeShotTraceOverride(const std::array<float, 3>& origin, const std::array<float, 3>& target);
	void SetCustomAkimboState(bool enabled, int weaponType, uint8_t heldMask, uint8_t edgeMask);
	bool SetCustomAkimboHandTrace(int hand, const std::array<float, 3>& origin,
		const std::array<float, 3>& target);
	void ClearCustomAkimboState();
	uint8_t ConsumeCustomAkimboAcceptedHandMask();
	bool SetNativeThrowableMotionOverride(const std::array<float, 3>& origin,
		const std::array<float, 3>& velocity, int weaponType, uint32_t sequence);
	void ClearNativeThrowableMotionOverride();
	bool ConsumeNativeThrowableMotionApplied(uint32_t& sequence);
	bool ReadLatestNativeThrowableLaunchProbe(NativeThrowableLaunchProbe& probe);
	// Definitive Edition LOS entry at module+0x13F89A0. Targeted caller tracing
	// proves RCX/RDX are start/end CVector pointers, R8 is an opaque hit-result
	// output, and R9 is CEntity**. Used only by the engine-thread melee sweep.
	bool QueryNativeLineOfSightEntity(const std::array<float, 3>& start,
		const std::array<float, 3>& end, NativeMeleeContact& contact,
		bool requireEntity = true);
	// Applies one already-contact-qualified hit through GTA's own ped damage-event
	// or vehicle damage function. Returns true only for a proven native acceptance;
	// the void-return vehicle path records UnverifiedVehicleDispatch. No health
	// values are written by the plugin.
	bool ApplyNativeMeleeContactDamage(NativeMeleeContact& contact,
		int weaponType, int& appliedDamage);
	// One bounded native impact event for the custom Molotov flight. This is
	// deliberately separate from melee damage so projectile/fire behavior cannot
	// inherit melee combo lookup or hand-swing state.
	bool ApplyNativeThrowableImpactDamage(NativeMeleeContact& contact,
		int weaponType, int& appliedDamage, int requestedDamage = 0);
	// Impact-only native handoff for the custom Molotov flight. This does not
	// create, move, or launch a projectile.
	bool ApplyNativeThrowableImpactEvent(NativeMeleeContact& contact, int weaponType);
	// Invokes DE's own grenade/Molotov explosion lifecycle at an already-resolved
	// native point. Custom hand flight and collision remain owned by WeaponManager.
	bool ApplyNativeThrowableExplosion(const std::array<float, 3>& impactPoint,
		int weaponType);
	void SetVehicleShotTraceOverrideArmed(bool armed);
	void SetVehicleShotTraceModeActive(bool active);
	uint32_t GetNativeShotTraceSequenceSnapshot() const;
	void ClearNativeShotTraceOverride();
	bool InstallRuntimeArrayCallbackHook(const char* name, uintptr_t target, size_t overwriteSize,
		const void* expectedObjectPointerStorage, int32_t vectorStride, const char* path,
		void* callback);
	bool ReadLatestNativeShotTraceProbe(NativeShotTraceProbe& probe);
	bool ReadLatestNativeShotEffectProbe(NativeShotEffectProbe& probe);
	bool CyclePlayerWeaponSlot(int direction);
	bool ReloadCurrentWeaponOneMagazine(int expectedWeaponType = -1, bool dualWield = false);
	bool ReadPhoneRingingState(bool& ringing);
	int ResolveCurrentVehicleModelId(int vehicleType);
	bool vehicleRelatedMemoryInstructionsNoped = true;

	static bool FirstWeaponIsShooting;

	void InstallBreakpoints();
	bool SetHardwareBreakpoint(HANDLE hThread, int index, void* address, bool* flag);
	void RemoveBreakpoints();
	void RemoveExceptionHandler();
	static bool breakpointsInstalled;

	//void GetAllBytes();
	//void WriteBytesToIniFile(const char* header, const std::vector<std::pair<uintptr_t, size_t>>& addresses);
};
