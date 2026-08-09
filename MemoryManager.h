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

	enum class NativeMeleeDamageResult : uint8_t {
		Rejected,
		Attempted,
		Accepted,
		UnverifiedVehicleDispatch
	};

	struct NativeMeleeContact {
		uintptr_t entity = 0;
		std::array<float, 3> point{};
		uint8_t piece = 0;
		uint8_t entityType = 0;
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
	// Definitive Edition LOS entry at module+0x13F89A0. Targeted caller tracing
	// proves RCX/RDX are start/end CVector pointers, R8 is an opaque hit-result
	// output, and R9 is CEntity**. Used only by the engine-thread melee sweep.
	bool QueryNativeLineOfSightEntity(const std::array<float, 3>& start,
		const std::array<float, 3>& end, NativeMeleeContact& contact);
	// Applies one already-contact-qualified hit through GTA's own ped damage-event
	// or vehicle damage function. Returns true only for a proven native acceptance;
	// the void-return vehicle path records UnverifiedVehicleDispatch. No health
	// values are written by the plugin.
	bool ApplyNativeMeleeContactDamage(NativeMeleeContact& contact,
		int weaponType, int& appliedDamage);
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
