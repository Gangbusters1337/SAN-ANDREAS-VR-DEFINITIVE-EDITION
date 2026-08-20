#include <windows.h>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "uevr/API.hpp"
#include "MemoryManager.h"
#include "WeaponManager.h"

DWORD PID;

namespace {
	constexpr float AlmostMaxWeaponSkill = 998.0f;
	constexpr uint32_t LongBulletRangeBits = 0x461C4000; // 10000.0f
	constexpr float EnhancedWeaponRange = 15000.0f;
	constexpr float NoSpreadWeaponAccuracy = 1.25f;
	constexpr ULONGLONG WeaponInfoVerifyIntervalMs = 3000;
	constexpr ULONGLONG WeaponInfoReadyCheckIntervalMs = 500;
	constexpr ULONGLONG WeaponInfoOverwriteLogCooldownMs = 1000;
	constexpr ULONGLONG HealthRecoverySampleIntervalMs = 1000;
	constexpr ULONGLONG HealthRecoveryDelayMs = 10000;
	constexpr float WeaponInfoFloatEpsilon = 0.001f;
	constexpr float HealthChangeEpsilon = 0.05f;
	constexpr float HealthRecoveryFraction = 0.5f;
	constexpr uint32_t HalfDamageFloatBits = 0x3F000000; // 0.5f
	constexpr uint32_t FirstBulletWeaponType = 0x16; // pistol
	constexpr uint32_t LastStandardBulletWeaponType = 0x22; // sniper rifle
	constexpr uint32_t MinigunWeaponType = 0x26;
	volatile LONG* gPlayerSemiAutoPullHeld = nullptr;
	volatile LONG* gPlayerSemiAutoShotPermit = nullptr;
	volatile LONG* gCustomAkimboEnabled = nullptr;
	volatile LONG* gCustomAkimboWeaponType = nullptr;
	volatile LONG* gCustomAkimboPendingMask = nullptr;
	volatile LONG* gCustomAkimboTaskFireMask = nullptr;
	volatile LONG* gCustomAkimboActiveHand = nullptr;
	volatile LONG* gCustomAkimboAcceptedShotSequence = nullptr;
	volatile LONG* gCustomAkimboAcceptedHandMask = nullptr;
	bool IsPlayerSemiAutoGateWeapon(uint32_t weaponType) {
		return (weaponType >= 22 && weaponType <= 27) || weaponType == 33 || weaponType == 34;
	}
	constexpr uintptr_t ScriptSpaceOffset = 0x51BEAE0;
	constexpr uintptr_t ScriptSpaceSignatureOffset = 0x20000;
	constexpr size_t PhoneRingingGlobalIndex = 14;
	constexpr std::array<uint8_t, 32> ScriptSpaceSignature{
		0x03, 0x20, 0x00, 0x04, 0x00, 0x4D, 0x00, 0x01,
		0x29, 0x00, 0x02, 0x00, 0x8D, 0x03, 0x03, 0x12,
		0x00, 0x02, 0x20, 0x01, 0x02, 0x24, 0x01, 0x02,
		0xAC, 0x10, 0x02, 0xA8, 0x10, 0x05, 0xFF, 0x00
	};
	// Verified against the live Definitive Edition player ped layout.
	constexpr size_t PlayerHealthOffset = 0x76C;
	constexpr size_t PlayerMaxHealthOffset = 0x770;
	constexpr uint32_t CombatAssistReserveAmmo = 400;
	constexpr size_t WeaponInfoCount = 80;
	constexpr size_t WeaponInfoSize = 0x70;
	constexpr size_t WeaponInfoFireTypeOffset = 0x00;
	constexpr size_t WeaponInfoTargetRangeOffset = 0x04;
	constexpr size_t WeaponInfoWeaponRangeOffset = 0x08;
	constexpr size_t WeaponInfoModelIdOffset = 0x0C;
	constexpr size_t WeaponInfoModelId2Offset = 0x10;
	constexpr size_t WeaponInfoAmmoClipOffset = 0x20;
	constexpr size_t WeaponInfoDamageOffset = 0x22;
	constexpr size_t WeaponInfoAccuracyOffset = 0x38;
	constexpr size_t WeaponInfoAnimLoopStartOffset = 0x40;
	constexpr size_t WeaponInfoAnimLoopEndOffset = 0x44;
	constexpr size_t WeaponInfoAnimLoopFireOffset = 0x48;
	constexpr size_t WeaponInfoAnimLoop2StartOffset = 0x4C;
	constexpr size_t WeaponInfoAnimLoop2EndOffset = 0x50;
	constexpr size_t WeaponInfoAnimLoop2FireOffset = 0x54;
	constexpr size_t WeaponInfoSpreadOffset = 0x68;
	constexpr size_t WeaponInfoBaseMeleeComboOffset = 0x6E;
	constexpr uintptr_t MeleeComboTableOffset = 0x531B2A0;
	constexpr size_t MeleeComboInfoSize = 0x88;
	constexpr size_t MeleeComboFirstStrikeDamageOffset = 0x55;
	constexpr uint8_t FirstMeleeCombo = 4;
	constexpr uint8_t MeleeComboCount = 12;
	constexpr size_t NativeEntityTypeOffset = 0x6A;
	constexpr uint8_t NativeEntityTypeMask = 0x07;
	constexpr uint8_t NativeEntityTypeWorld = 1;
	constexpr uint8_t NativeEntityTypeVehicle = 2;
	constexpr uint8_t NativeEntityTypePed = 3;
	constexpr size_t NativePedHealthOffset = 0x76C;
	constexpr size_t NativeVehicleHealthOffset = 0x79C;
	constexpr size_t NativeMeleeContactPieceOffset = 0x24;
	constexpr uintptr_t NativeLineOfSightRva = 0x13F89A0;
	// Native weapon callers write their shooter here before LOS. The LOS core at
	// 0x118C85A/0x118D2C4 compares each candidate against this pointer and skips it.
	constexpr uintptr_t NativeLineOfSightIgnoreEntityRva = 0x523A8D8;
	constexpr uintptr_t NativePedDamageRva = 0x13F1150;
	constexpr uintptr_t NativeVehicleDamageRva = 0x13C96B0;
	// Proven native weapon-impact dispatcher. Its callers pass the live weapon
	// entry, attacker, victim, start/end vectors, a CColPoint, and a positive
	// impact factor. It owns the downstream native reaction/effect path.
	constexpr uintptr_t NativeThrowableImpactRva = 0x13F16A0;
	// CWeapon::Fire routes grenade, tear-gas, Molotov, and satchel through this
	// common native projectile constructor. The first seven bytes are a stable
	// stack/register prologue in the currently supported executable hash.
	constexpr uintptr_t NativeThrowableAddProjectileRva = 0x13EADF0;
	constexpr std::array<uint8_t, 7> NativeThrowableAddProjectilePrologue{
		0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x08
	};
	constexpr int NativeMolotovWeaponType = 18;
	// Native weapon-18 projectile impact calls this exact CExplosion::AddExplosion
	// entry with explosion type 1. Unlike the cooked BP shells, this registers the
	// timed explosion plus native ground fire, ped/world/car ignition and damage.
	constexpr uintptr_t NativeAddExplosionRva = 0x13E96A0;
	constexpr std::array<uint8_t, 16> NativeAddExplosionPrologue{
		0x40, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D,
		0x6C, 0x24, 0xC0, 0x48, 0x81, 0xEC, 0x40, 0x01
	};
	constexpr int NativeMolotovExplosionType = 1;
	constexpr int NativeGrenadeWeaponType = 16;
	constexpr int NativeGrenadeExplosionType = 0;

	const char* NativeMeleeEntityTypeLabel(uint8_t entityType) {
		switch (entityType) {
		case NativeEntityTypeWorld: return "world";
		case NativeEntityTypeVehicle: return "vehicle";
		case NativeEntityTypePed: return "ped";
		default: return "unknown";
		}
	}

	const char* NativeMeleeDamageResultLabel(MemoryManager::NativeMeleeDamageResult result) {
		switch (result) {
		case MemoryManager::NativeMeleeDamageResult::Attempted: return "attempted";
		case MemoryManager::NativeMeleeDamageResult::Accepted: return "accepted";
		case MemoryManager::NativeMeleeDamageResult::UnverifiedVehicleDispatch:
			return "unverified_vehicle";
		case MemoryManager::NativeMeleeDamageResult::Rejected:
		default: return "rejected";
		}
	}
	constexpr uint32_t WeaponFireInstantHit = 1;
	constexpr float FirstShotFireDelay = 0.05f;
	constexpr float MinShotWindowMargin = 0.02f;
	constexpr auto ManualReloadMagazineSizes = [] {
		std::array<uint32_t, 47> sizes{};
		sizes[22] = 17; // Colt 45
		sizes[23] = 17; // Silenced pistol
		sizes[24] = 7;  // Desert Eagle
		sizes[25] = 1;  // Shotgun
		sizes[26] = 2;  // Sawn-off shotgun
		sizes[27] = 7;  // Combat shotgun
		sizes[28] = 50; // Micro SMG
		sizes[29] = 30; // MP5
		sizes[30] = 30; // AK-47
		sizes[31] = 50; // M4
		sizes[32] = 50; // Tec-9
		sizes[33] = 1;  // Country rifle
		return sizes;
	}();

	float EarlierFireTime(float start, float end, float fire) {
		if (!std::isfinite(start) || !std::isfinite(end) || !std::isfinite(fire) || end <= start || fire <= start)
			return fire;

		const float desired = (std::min)(fire, start + FirstShotFireDelay);
		const float latestSafe = end - MinShotWindowMargin;
		if (latestSafe > start)
			return (std::min)(desired, latestSafe);

		return desired;
	}

	uint64_t TriggerTimingTimestampUs() {
		static const LARGE_INTEGER frequency = [] {
			LARGE_INTEGER value{};
			QueryPerformanceFrequency(&value);
			return value;
		}();
		LARGE_INTEGER counter{};
		QueryPerformanceCounter(&counter);
		if (frequency.QuadPart <= 0)
			return 0;
		const uint64_t wholeSeconds = static_cast<uint64_t>(counter.QuadPart / frequency.QuadPart);
		const uint64_t remainder = static_cast<uint64_t>(counter.QuadPart % frequency.QuadPart);
		return (wholeSeconds * 1000000ULL) + ((remainder * 1000000ULL) / static_cast<uint64_t>(frequency.QuadPart));
	}

	void AppendU32(std::vector<uint8_t>& bytes, uint32_t value) {
		for (int i = 0; i < 4; ++i)
			bytes.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
	}

	void AppendU64(std::vector<uint8_t>& bytes, uint64_t value) {
		for (int i = 0; i < 8; ++i)
			bytes.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
	}

	bool AppendRelJmp(std::vector<uint8_t>& bytes, uintptr_t instructionAddress, uintptr_t targetAddress) {
		const int64_t rel = static_cast<int64_t>(targetAddress) - static_cast<int64_t>(instructionAddress + 5);
		if (rel < std::numeric_limits<int32_t>::min() || rel > std::numeric_limits<int32_t>::max())
			return false;

		bytes.push_back(0xE9);
		AppendU32(bytes, static_cast<uint32_t>(static_cast<int32_t>(rel)));
		return true;
	}

	size_t AppendShortJcc(std::vector<uint8_t>& bytes, uint8_t opcode) {
		const size_t offset = bytes.size();
		bytes.push_back(opcode);
		bytes.push_back(0x00);
		return offset;
	}

	bool PatchShortJcc(std::vector<uint8_t>& bytes, size_t instructionOffset, size_t targetOffset) {
		const int displacement = static_cast<int>(targetOffset) - static_cast<int>(instructionOffset + 2);
		if (displacement < -128 || displacement > 127)
			return false;

		bytes[instructionOffset + 1] = static_cast<uint8_t>(static_cast<int8_t>(displacement));
		return true;
	}

	size_t AppendNearJcc(std::vector<uint8_t>& bytes, uint8_t opcode) {
		const size_t offset = bytes.size();
		bytes.push_back(0x0F);
		bytes.push_back(opcode);
		AppendU32(bytes, 0);
		return offset;
	}

	bool PatchNearJcc(std::vector<uint8_t>& bytes, size_t instructionOffset, size_t targetOffset) {
		const int64_t displacement = static_cast<int64_t>(targetOffset) - static_cast<int64_t>(instructionOffset + 6);
		if (displacement < std::numeric_limits<int32_t>::min() || displacement > std::numeric_limits<int32_t>::max())
			return false;

		const int32_t relative = static_cast<int32_t>(displacement);
		std::memcpy(bytes.data() + instructionOffset + 2, &relative, sizeof(relative));
		return true;
	}

	void AppendCachedPlayerGuardWithCompare(std::vector<uint8_t>& bytes, uintptr_t cachedPlayerPointerAddress, const std::vector<uint8_t>& compareCachedPlayerBytes, size_t& jeOffset, size_t& jneOffset) {
		bytes.push_back(0x9C); // pushfq
		bytes.push_back(0x41); bytes.push_back(0x52); // push r10
		bytes.push_back(0x49); bytes.push_back(0xBA); AppendU64(bytes, cachedPlayerPointerAddress); // mov r10, imm64
		bytes.push_back(0x4D); bytes.push_back(0x8B); bytes.push_back(0x12); // mov r10,[r10]
		bytes.push_back(0x4D); bytes.push_back(0x85); bytes.push_back(0xD2); // test r10,r10
		jeOffset = AppendShortJcc(bytes, 0x74);
		bytes.insert(bytes.end(), compareCachedPlayerBytes.begin(), compareCachedPlayerBytes.end());
		jneOffset = AppendShortJcc(bytes, 0x75);
	}

	void AppendCachedPlayerGuard(std::vector<uint8_t>& bytes, uintptr_t cachedPlayerPointerAddress, size_t& jeOffset, size_t& jneOffset) {
		AppendCachedPlayerGuardWithCompare(bytes, cachedPlayerPointerAddress, { 0x4C, 0x39, 0xD7 }, jeOffset, jneOffset); // cmp rdi,r10
	}

	void AppendCachedPlayerGuardRBX(std::vector<uint8_t>& bytes, uintptr_t cachedPlayerPointerAddress, size_t& jeOffset, size_t& jneOffset) {
		AppendCachedPlayerGuardWithCompare(bytes, cachedPlayerPointerAddress, { 0x4C, 0x39, 0xD3 }, jeOffset, jneOffset); // cmp rbx,r10
	}

	void AppendCachedPlayerGuardR11(std::vector<uint8_t>& bytes, uintptr_t cachedPlayerPointerAddress, size_t& jeOffset, size_t& jneOffset) {
		AppendCachedPlayerGuardWithCompare(bytes, cachedPlayerPointerAddress, { 0x4D, 0x39, 0xD3 }, jeOffset, jneOffset); // cmp r11,r10
	}

	void AppendCachedPlayerGuardRDX(std::vector<uint8_t>& bytes, uintptr_t cachedPlayerPointerAddress, size_t& jeOffset, size_t& jneOffset) {
		AppendCachedPlayerGuardWithCompare(bytes, cachedPlayerPointerAddress, { 0x4C, 0x39, 0xD2 }, jeOffset, jneOffset); // cmp rdx,r10
	}

	void AppendPopGuard(std::vector<uint8_t>& bytes) {
		bytes.push_back(0x41); bytes.push_back(0x5A); // pop r10
		bytes.push_back(0x9D); // popfq
	}

	void AppendRegisterFloatLoad(std::vector<uint8_t>& bytes, uint32_t floatBits, const std::vector<uint8_t>& movdFromR10d) {
		bytes.push_back(0x41); bytes.push_back(0x52); // push r10
		bytes.push_back(0x41); bytes.push_back(0xBA); AppendU32(bytes, floatBits); // mov r10d,imm32
		bytes.insert(bytes.end(), movdFromR10d.begin(), movdFromR10d.end());
		bytes.push_back(0x41); bytes.push_back(0x5A); // pop r10
	}

	uint32_t ReadU32(uintptr_t address) {
		uint32_t value = 0;
		std::memcpy(&value, reinterpret_cast<void*>(address), sizeof(value));
		return value;
	}

	int32_t ReadI32(uintptr_t address) {
		int32_t value = 0;
		std::memcpy(&value, reinterpret_cast<void*>(address), sizeof(value));
		return value;
	}

	bool IsReadableProtection(DWORD protect) {
		if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0)
			return false;

		const DWORD baseProtect = protect & 0xff;
		return baseProtect == PAGE_READONLY ||
			baseProtect == PAGE_READWRITE ||
			baseProtect == PAGE_WRITECOPY ||
			baseProtect == PAGE_EXECUTE_READ ||
			baseProtect == PAGE_EXECUTE_READWRITE ||
			baseProtect == PAGE_EXECUTE_WRITECOPY;
	}

	bool IsReadableMemory(uintptr_t address, size_t size) {
		if (address < 0x10000 || size == 0)
			return false;

		MEMORY_BASIC_INFORMATION memoryInfo{};
		if (VirtualQuery(reinterpret_cast<void*>(address), &memoryInfo, sizeof(memoryInfo)) == 0)
			return false;

		const uintptr_t regionBase = reinterpret_cast<uintptr_t>(memoryInfo.BaseAddress);
		const uintptr_t regionEnd = regionBase + memoryInfo.RegionSize;
		if (memoryInfo.State != MEM_COMMIT || !IsReadableProtection(memoryInfo.Protect))
			return false;

		return address >= regionBase && address + size <= regionEnd && address + size >= address;
	}

	bool IsWritableMemory(uintptr_t address, size_t size) {
		if (!IsReadableMemory(address, size))
			return false;

		MEMORY_BASIC_INFORMATION memoryInfo{};
		if (VirtualQuery(reinterpret_cast<void*>(address), &memoryInfo, sizeof(memoryInfo)) == 0)
			return false;

		const DWORD baseProtect = memoryInfo.Protect & 0xff;
		return baseProtect == PAGE_READWRITE ||
			baseProtect == PAGE_WRITECOPY ||
			baseProtect == PAGE_EXECUTE_READWRITE ||
			baseProtect == PAGE_EXECUTE_WRITECOPY;
	}

	template<typename T>
	bool TryRead(uintptr_t address, T& value) {
		if (!IsReadableMemory(address, sizeof(T)))
			return false;

		std::memcpy(&value, reinterpret_cast<void*>(address), sizeof(T));
		return true;
	}

	bool MemoryBlockContainsPointer(uintptr_t baseAddress, size_t scanSize, uintptr_t pointerValue) {
		for (size_t offset = 0; offset + sizeof(uintptr_t) <= scanSize; offset += sizeof(uintptr_t)) {
			uintptr_t value = 0;
			if (!TryRead(baseAddress + offset, value))
				continue;
			if (value == pointerValue)
				return true;
		}
		return false;
	}

	bool FloatChanged(float current, float expected) {
		return std::isfinite(current) && std::isfinite(expected) && std::fabs(current - expected) > WeaponInfoFloatEpsilon;
	}

	float ExpectedRangeValue(float original, float enhanced) {
		return (std::max)(original, enhanced);
	}

	uint16_t ExpectedDamageValue(uint16_t originalDamage, bool damageBoostEnabled) {
		if (!damageBoostEnabled || originalDamage == 0)
			return originalDamage;

		const uint32_t doubledDamage = static_cast<uint32_t>(originalDamage) * 2;
		return static_cast<uint16_t>((std::min)(0xFFFFu, doubledDamage));
	}

	int WeaponSlotForWeaponType(int weaponType) {
		switch (weaponType) {
		case 0:
		case 1:
			return 0;
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 7:
		case 8:
		case 9:
			return 1;
		case 22:
		case 23:
		case 24:
			return 2;
		case 25:
		case 26:
		case 27:
			return 3;
		case 28:
		case 29:
		case 32:
			return 4;
		case 30:
		case 31:
			return 5;
		case 33:
		case 34:
			return 6;
		case 35:
		case 36:
		case 37:
		case 38:
			return 7;
		case 16:
		case 17:
		case 18:
		case 39:
			return 8;
		case 41:
		case 42:
			return 9;
		case 10:
		case 11:
		case 12:
		case 13:
		case 14:
		case 15:
			return 10;
		case 43:
			return 11;
		case 44:
		case 45:
		case 46:
			return 12;
		default:
			return -1;
		}
	}

	bool IsWeaponTypePlausible(uint32_t weaponType) {
		return weaponType <= 46;
	}

	uint32_t ManualReloadClipSize(uint32_t weaponType) {
		return weaponType < ManualReloadMagazineSizes.size()
			? ManualReloadMagazineSizes[weaponType]
			: 0;
	}

	bool IsSingleRoundManualReloadWeapon(uint32_t weaponType) {
		return weaponType == 25 || weaponType == 33;
	}

	bool UsesDirectManualReloadWrite(uint32_t weaponType) {
		// GTA does not consistently enter its native reload transition after the
		// staged empty-magazine handoff for the AK-47. Refill it directly while
		// retaining the same one-magazine accounting used by the staged path.
		return IsSingleRoundManualReloadWeapon(weaponType) || weaponType == 30;
	}

	bool UsesTubeFedManualReloadLoad(uint32_t weaponType) {
		return weaponType == 25;
	}

	bool SupportsDualWieldManualReload(uint32_t weaponType) {
		return weaponType == 22 || weaponType == 26 || weaponType == 28 || weaponType == 32;
	}

	uint32_t ManualReloadMagazineCapacity(uint32_t weaponType, bool dualWield) {
		const uint32_t baseCapacity = ManualReloadClipSize(weaponType);
		return dualWield && SupportsDualWieldManualReload(weaponType)
			? baseCapacity * 2
			: baseCapacity;
	}

	uint32_t ManualReloadPlayableLoadSize(uint32_t weaponType, uint32_t magazineCapacity) {
		// GTA exposes the pump shotgun as a one-round chamber even though its
		// normal playable load is five shots. Keep the chamber representation at
		// one and retain the other four rounds for automatic pump/chamber cycling.
		return UsesTubeFedManualReloadLoad(weaponType) ? 5u : magazineCapacity;
	}

	bool IsWeaponSlotAvailable(int slot, uint32_t weaponType, uint32_t ammoInClip, uint32_t ammoTotal) {
		if (slot == 0)
			return weaponType == 0 || weaponType == 1;

		if (weaponType == 0 || !IsWeaponTypePlausible(weaponType))
			return false;

		if (WeaponSlotForWeaponType(static_cast<int>(weaponType)) != slot)
			return false;

		if (slot == 1 || slot == 10 || slot == 11 || slot == 12)
			return true;

		return ammoInClip > 0 || ammoTotal > 0;
	}

	bool IsKnownBikeModel(int modelId) {
		const int bikeModels[] = { 448, 461, 462, 463, 468, 471, 481, 509, 510, 521, 522, 523, 581, 586 };
		return std::find(std::begin(bikeModels), std::end(bikeModels), modelId) != std::end(bikeModels);
	}

	bool IsKnownPlaneModel(int modelId) {
		const int planeModels[] = { 460, 476, 511, 512, 513, 519, 520, 553, 577, 592, 593 };
		return std::find(std::begin(planeModels), std::end(planeModels), modelId) != std::end(planeModels);
	}

	bool IsKnownHeliModel(int modelId) {
		const int heliModels[] = { 417, 425, 447, 469, 487, 488, 497, 548, 563 };
		return std::find(std::begin(heliModels), std::end(heliModels), modelId) != std::end(heliModels);
	}

	bool IsKnownVehicleModelForType(int modelId, int vehicleType) {
		if (modelId < 400 || modelId > 611)
			return false;

		constexpr int CarOrBoat = 10;
		constexpr int Bike = 13;
		constexpr int Helicopter = 16;
		constexpr int Plane = 19;

		switch (vehicleType) {
		case Bike:
			return IsKnownBikeModel(modelId);
		case Helicopter:
			return IsKnownHeliModel(modelId);
		case Plane:
			return IsKnownPlaneModel(modelId);
		case CarOrBoat:
			return !IsKnownBikeModel(modelId) && !IsKnownPlaneModel(modelId) && !IsKnownHeliModel(modelId);
		default:
			return true;
		}
	}

	int FindVehicleModelIdInObject(uintptr_t vehiclePointer, int vehicleType, size_t& modelOffset) {
		for (size_t offset = 0; offset < 0x180; offset += sizeof(uint16_t)) {
			uint16_t value = 0;
			if (!TryRead(vehiclePointer + offset, value))
				continue;

			const int modelId = static_cast<int>(value);
			if (IsKnownVehicleModelForType(modelId, vehicleType)) {
				modelOffset = offset;
				return modelId;
			}
		}

		return -1;
	}
}


std::vector<MemoryBlock> matrixInstructionsRotationAddresses = {
	{0x111DE7E, 7, 0xf30f118b300800},
	{0x111DE85, 1, 0x00},
	{0x111DECC, 7, 0xf30f118b340800},
	{0x111DED3, 1, 0x00},
	{0x111DED9, 7, 0xf30f11b3380800},
	{0x111DEE0, 1, 0x00},
	{0x111DE5C, 7, 0xf3440f11834008},
	{0x111DE63, 1, 0x00},
	{0x111DE64, 1, 0x00},
	{0x111DE3B, 7, 0xf30f11b3440800},
	{0x111DE42, 1, 0x00},
	{0x111DE68, 7, 0xf30f11bb480800},
	{0x111DE6F, 1, 0x00},
	{0x111DE75, 7, 0xf3440f11a35008},
	{0x111DE7C, 1, 0x00},
	{0x111DE7D, 1, 0x00},
	{0x111DE8F, 7, 0xf3440f118b5408},
	{0x111DE96, 1, 0x00},
	{0x111DE97, 1, 0x00},
	{0x111DE98, 7, 0xf3440f119b5808},
	{0x111DE9F, 1, 0x00},
	{0x111DEA0, 1, 0x00}
};
std::vector<MemoryBlock> matrixInstructionsPositionAddresses = {
	{0x111DEA5, 7, 0xf3440f11b36008},
	{0x111DEAC, 1, 0x00},
	{0x111DEAD, 1, 0x00},
	{0x111DF57, 7, 0xf30f1183600800},
	{0x111DF5E, 1, 0x00},
	{0x111DEB3, 7, 0xf30f1183640800},
	{0x111DEBA, 1, 0x00},
	{0x111DF72, 7, 0xf30f1183640800},
	{0x111DF79, 1, 0x00},
	{0x111DEBE, 7, 0xf3440f11bb6808},
	{0x111DEC5, 1, 0x00},
	{0x111DEC6, 1, 0x00},
	{0x111DF8D, 7, 0xf30f1183680800},
	{0x111DF94, 1, 0x00},
};
std::vector<MemoryBlock> ingameCameraPositionInstructionsAddresses = {
	{0x1109F20, 3, 0xf20f11},
	{0x1109F23, 1, 0x06},
	{0x1109F96, 3, 0xf30f11},
	{0x1109F99, 1, 0x06},
	{0x110A28E, 3, 0xf30f11},
	{0x110A291, 1, 0x06},
	{0x11255AB, 3, 0xf30f11},
	{0x11255AE, 1, 0x03},
	{0x11070E2, 3, 0xf30f11},
	{0x11070E5, 1, 0x03},
	{0x110A3BD, 3, 0xf30f11},
	{0x110A3C0, 1, 0x06},
	{0x11080C6, 7, 0xf20f1106894608},
	{0x1109F24, 3, 0x894608},
	{0x1109FBC, 5, 0xf30f114608},
	{0x110A252, 5, 0xf3440f1146},
	{0x110A257, 1, 0x08},
	{0x110A2C0, 5, 0xf30f114608},
	{0x11255B4, 5, 0xf30f114b08},
	{0x11070FF, 5, 0xf30f114308},
	{0x110A3DD, 5, 0xf30f114608},
	{0x1108165, 5, 0xf3440f115e},
	{0x110816A, 1, 0x08},
	{0x1109FA4, 5, 0xf30f114604},
	{0x110A29C, 5, 0xf30f114604},
	{0x11255B3, 5, 0x04f30f114b},
	{0x11070F0, 5, 0xf30f114304},
	{0x110A3CB, 5, 0xf30f114604},
	{0x110D0BC, 3, 0xf20f11},
	{0x110D0BF, 1, 0x03},
};
std::vector<MemoryBlock> ingameCameraPositionSniperAndCamWpnInstructionsAddresses = {
	{0x110E06D, 3, 0xf30f11},
	{0x110E070, 1, 0x03},
	{0x110E018, 7, 0xf30f11871c0100},
	{0x110E01F, 1, 0x00},
	{0x110E3F2, 3, 0xf30f11},
	{0x110E3F5, 1, 0x03},
	{0x110E1F2, 3, 0xf30f11},
	{0x110E1F5, 1, 0x33},
	{0x110E0B2, 7, 0xf30f11871c0100},
	{0x110E0B9, 1, 0x00},
	{0x110E0D2, 7, 0xf30f1187200100},
	{0x110E0D9, 1, 0x00},
	{0x110e20c, 5, 0xf30f117b04},
	{0x110e3ff, 5, 0xf30f114304},
	{0x110dfc4, 3, 0x894b08},
	{0x110dfdb, 7, 0xf30f1187240100},
	{0x110dfe2, 1, 0x00},
	{0x110e21d, 5, 0xf3440f1143},
	{0x110e222, 1, 0x08},
	{0x110e40d, 5, 0xf30f114308},
	{0x110e045, 7, 0xf30f1187200100},
	{0x110e04c, 1, 0x00},
	{0x110dfc0, 3, 0xf20f11},
	{0x110dfc3, 1, 0x03},
};
std::vector<MemoryBlock> pitchAxisAimingInstructionsAddresses = {
	{0x11077c9, 3, 0xf30f11},
	{0x11077cc, 1, 0x07},
	{0x1107c9b, 5, 0xf3440f110f},
	{0x1109dce, 3, 0xf30f11},
	{0x1109dd1, 1, 0x33},
	{0x1109e1c, 3, 0xf30f11},
	{0x1109e1f, 1, 0x0b},
	{0x110d8ad, 5, 0xf3440f1117},
	{0x1108dbd, 3, 0x448933},
	{0x1108e23, 3, 0xf30f11},
	{0x1108e26, 1, 0x03},
	{0x110903f, 3, 0xf30f11},
	{0x1109042, 1, 0x03},
};
std::vector<MemoryBlock> aimingForwardVectorInstructionsAddresses = {
	{0x11090E8, 5, 0xf2410f1107},
	//{0xAE0410, 5, 0xf3440f1101}, //cause aggressive spawning velocity of cars
	{0x1109EA5, 5, 0xf2410f1107},
	{0x1105AAC, 7, 0xf20f1189100100},
	{0x1105AB3, 1, 0x00},
	{0x1107E3B, 7, 0xf20f1187100100},
	{0x1107E42, 1, 0x00},
	{0x1108E75, 5, 0xf2410f1107},
	//{0xAE0406, 5, 0xf30f117104}, //cause aggressive spawning velocity of cars
	{0x11090ED, 3, 0x418947},
	{0x11090F0, 1, 0x08},
	//{0xAE040B, 5, 0xf30f117908}, //cause aggressive spawning velocity of cars
	{0x1109EAA, 3, 0x418947},
	{0x1109EAD, 1, 0x08},
	{0x1105AC9, 5, 0x8981180100},
	{0x1105ACE, 1, 0x00},
	{0x1107E43, 5, 0x8987180100},
	{0x1107E48, 1, 0x00},
	{0x1108E7A, 3, 0x418947},
	{0x1108E7D, 1, 0x08},
	//Cause extinguisher, spraycan, flamethrower up and down aiming issues
	//{0x1105A60, 7, 0xc741180000F041},
	//{0x1108D75, 7, 0xc7431800007a44},
	//{0x1105A4F, 3, 0x668941},
	//{0x1105A52, 1, 0x28},
	//{0x11202A4, 7, 0x6689ac38b00100},
	//{0x11202AB, 1, 0x00},
	//{0x11205DB, 7, 0x6689ac38b00100},
	//{0x11205E2, 1, 0x00},
	{0x11077C9, 3, 0xf30f11},
	{0x11077CC, 1, 0x07},
	{0x1107C9B, 5, 0xf3440f110f},
	{0x110D0F2, 5, 0xf2410f1101}
};
std::vector<MemoryBlock> aimingUpVectorInstructionsAddresses = {
	{0x1105840, 5, 0xf20f118134}, 
	{0x1105845, 3, 0x010000},
	{0x1105A00, 5, 0xf20f118234}, 
	{0x1105A05, 3, 0x010000},
	{0x1105854, 5, 0x89813c0100}, 
	{0x1105859, 1, 0x00},
	{0x1105A08, 5, 0x89823c0100}, 
	{0x1105A0D, 1, 0x00},
};
std::vector<MemoryBlock> rocketLauncherAimingVectorInstructionsAddresses = {
	{0x110E71D, 5, 0x8987180100},
	{0x110E722, 1, 0x00},
	{0x110E70B, 7, 0xf20f1187100100},
	{0x110E712, 1, 0x00}
};
std::vector<MemoryBlock> sniperAimingVectorInstructionsAddresses = {
	{0x110E19E, 5, 0x8987180100},	
	{0x110E1A3, 1, 0x00},
	{0x110E196, 7, 0xf20f1187100100},	
	{0x110E19D, 1, 0x00}
};
std::vector<MemoryBlock> carAimingVectorInstructionsAddresses = {
	{0x110BB78, 3, 0x418945},	
	{0x110BB7B, 1, 0x08},
	{0x110C5A4, 3, 0x418945},	
	{0x110C5A7, 1, 0x08},
	{0x110C59E, 5, 0xf2410f1145},	
	{0x110C5A3, 1, 0x00},
	{0x110BB68, 5, 0xf2410f114d},	
	{0x110BB6D, 1, 0x00},
	{0x110CE81, 3, 0x894208},
	{0x110CE7A, 3, 0xf20f11}, 
	{0x110CE7D, 1, 0x02}
};

uintptr_t MemoryManager::playerShootInstructionAddress = 0x11C6A7E;
uintptr_t MemoryManager::playerShootCam45InstructionAddress = 0x112D6F0; //
//uintptr_t MemoryManager::cameraShootInstructionAddress = 0x13F4000; // Take photo function address;

bool MemoryManager::FirstWeaponIsShooting = false;
volatile LONG64 MemoryManager::nativeShotTimingTimestampUs = 0;
volatile LONG MemoryManager::nativeShotTimingSequence = 0;
volatile LONG MemoryManager::nativeShotTimingPath = 0;
volatile LONG MemoryManager::nativeShotTimingPending = 0;
volatile LONG MemoryManager::triggerTimingActiveSequence = 0;
volatile LONG MemoryManager::triggerTimingActive = 0;

std::array<uintptr_t, 16> MemoryManager::cameraMatrixAddresses{}; // x, y, z

// Struct for each breakpoint
struct BreakpointInfo {
    void* address;
    bool* flag;  // Pointer to the boolean variable to update
};

// Global breakpoints
BreakpointInfo breakpoints[4];  // DR0, DR1, DR2, DR3, only DR0 is used in this plugin

bool MemoryManager::SetHardwareBreakpoint(HANDLE hThread, int index, void* address, bool* flag) {
    if (index < 0 || index > 3) return false;  // DR0-DR3 are valid

    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;

    if (!GetThreadContext(hThread, &ctx)) return false;

    // Assign the correct debug register (DR0 - DR3)
    switch (index) {
        case 0: ctx.Dr0 = (DWORD64)address; break;
        case 1: ctx.Dr1 = (DWORD64)address; break;
        case 2: ctx.Dr2 = (DWORD64)address; break;
        case 3: ctx.Dr3 = (DWORD64)address; break;
        default: return false;
    }

    // Enable the corresponding debug control bits
    ctx.Dr7 |= (1ULL << (index * 2)); // Enable breakpoint (L0, L1, L2, L3)

    if (!SetThreadContext(hThread, &ctx)) return false;

    // Store the breakpoint information
    breakpoints[index] = { address, flag };

    return true;
}

LONG WINAPI ExceptionHandler(EXCEPTION_POINTERS* pException) {
    if (pException->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {  
        uintptr_t instructionAddress = (uintptr_t)pException->ExceptionRecord->ExceptionAddress;

		if (instructionAddress == MemoryManager::playerShootInstructionAddress || instructionAddress == MemoryManager::playerShootCam45InstructionAddress) {
			MemoryManager::FirstWeaponIsShooting = true;
			MemoryManager::CaptureNativeShotObservation(instructionAddress);
		}
		
		// Set Resume Flag (RF) to prevent infinite breakpoint triggering
        pException->ContextRecord->EFlags |= (1 << 16);  // Set RF bit in EFLAGS

        // Move execution to the next instruction to avoid freezing
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    return EXCEPTION_CONTINUE_SEARCH; // Let other handlers process it if it's not our breakpoint
}

bool MemoryManager::breakpointsInstalled = false;

void MemoryManager::InstallBreakpoints() {
	if (settingsManager->debugMod) uevr::API::get()->log_info("InstallBreakpoints()");
	if (breakpointsInstalled)
		return;
    HANDLE hThread = GetCurrentThread();

    // Set the breakpoints
    SetHardwareBreakpoint(hThread, 0, (void*)MemoryManager::playerShootInstructionAddress, &MemoryManager::FirstWeaponIsShooting);
	SetHardwareBreakpoint(hThread, 1, (void*)MemoryManager::playerShootCam45InstructionAddress, &MemoryManager::FirstWeaponIsShooting);
	//SetHardwareBreakpoint(hThread, 1, (void*)MemoryManager::cameraShootInstructionAddress, &MemoryManager::isShooting);

    // Install exception handler
    exceptionHandlerHandle = AddVectoredExceptionHandler(1, ExceptionHandler);
	breakpointsInstalled = true;
}

void MemoryManager::RemoveBreakpoints() {
	if (settingsManager->debugMod) uevr::API::get()->log_info("RemoveBreakpoints()");
	// Clear hardware breakpoints
    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;

    HANDLE hThread = GetCurrentThread();  // Only applies to current thread (set for all if needed)
    GetThreadContext(hThread, &ctx);

    // Remove breakpoints by clearing DR0, DR1, and their control bits
    ctx.Dr0 = 0;
    ctx.Dr1 = 0;
    ctx.Dr7 &= ~(1 << 0); // Clear enable bit for DR0
    ctx.Dr7 &= ~(1 << 2); // Clear enable bit for DR1

    SetThreadContext(hThread, &ctx);

    // Remove the exception handler
    if (exceptionHandlerHandle) {
        RemoveVectoredExceptionHandler(exceptionHandlerHandle);
        exceptionHandlerHandle = nullptr;  // Prevent accidental double removal
    }
	breakpointsInstalled = false;
}

void MemoryManager::RemoveExceptionHandler() {
	if (settingsManager->debugMod) uevr::API::get()->log_info("RemoveExceptionHandler()");
    static PVOID handler = nullptr;  // Store handler pointer globally
    if (handler) {
        RemoveVectoredExceptionHandler(handler);
        handler = nullptr;
    }
}


// Function to NOP a batch of addresses
void NopMemory(const std::vector<MemoryBlock>& memoryBlocks) {
	for (const auto& [address, size, bytes] : memoryBlocks) {
		DWORD oldProtect;
		VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE, &oldProtect);

		for (size_t i = 0; i < size; ++i) {
			uintptr_t currentAddr = address + i;
			*reinterpret_cast<uint8_t*>(currentAddr) = 0x90; // Write NOP
		}

		VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);
	}
}

// Function to restore original bytes for a batch of addresses
void RestoreMemory(const std::vector<MemoryBlock>& memoryBlocks) {
	for (const auto& block : memoryBlocks) {
		DWORD oldProtect;
		VirtualProtect((LPVOID)block.address, block.size, PAGE_EXECUTE_READWRITE, &oldProtect);

		for (size_t i = 0; i < block.size; ++i) {
            *reinterpret_cast<uint8_t*>(block.address + i) = block.bytes[i];
        }

		VirtualProtect((LPVOID)block.address, block.size, oldProtect, &oldProtect);
	}
}

void MemoryManager::InitMemoryManager()
{
	baseAddressGameEXE = GetModuleBaseAddress(nullptr);
	scriptSpaceAddress = baseAddressGameEXE != 0 ? baseAddressGameEXE + ScriptSpaceOffset : 0;
	AdjustAddresses();
}

bool MemoryManager::ReadPhoneRingingState(bool& ringing)
{
	ringing = false;
	if (scriptSpaceAddress == 0)
		return false;

	if (!scriptSpaceValidated)
	{
		const uintptr_t signatureAddress = scriptSpaceAddress + ScriptSpaceSignatureOffset;
		std::array<uint8_t, ScriptSpaceSignature.size()> observed{};
		if (IsReadableMemory(signatureAddress, observed.size()))
			std::memcpy(observed.data(), reinterpret_cast<const void*>(signatureAddress), observed.size());

		scriptSpaceValidated = observed == ScriptSpaceSignature;
		if (scriptSpaceValidated)
		{
			scriptSpaceValidationAttempted = true;
			uevr::API::get()->log_info("[Phone] validated mainV1.scm script globals at module+0x%llX",
				static_cast<unsigned long long>(ScriptSpaceOffset));
		}
		else if (!scriptSpaceValidationAttempted)
		{
			scriptSpaceValidationAttempted = true;
			uevr::API::get()->log_info("[Phone] waiting for mainV1.scm script globals");
		}
	}

	if (!scriptSpaceValidated)
		return false;

	int32_t ringingValue = 0;
	if (!TryRead(scriptSpaceAddress + PhoneRingingGlobalIndex * sizeof(int32_t), ringingValue))
		return false;

	// The main script uses 0 while idle and a non-zero flag while the phone rings.
	ringing = ringingValue != 0;
	return true;
}

uintptr_t MemoryManager::GetModuleBaseAddress(LPCTSTR moduleName) {
	HMODULE hModule = GetModuleHandle(moduleName);
	if (hModule == nullptr) {
		//uevr::API::get()->log_info("Failed to get the base address of the module.");
		return 0;
	}
	return reinterpret_cast<uintptr_t>(hModule);
}

uintptr_t MemoryManager::GetModuleSize() const {
	if (baseAddressGameEXE == NULL)
		return 0;

	const auto dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(baseAddressGameEXE);
	if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
		return 0;

	const auto ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(baseAddressGameEXE + dosHeader->e_lfanew);
	if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
		return 0;

	return ntHeaders->OptionalHeader.SizeOfImage;
}

uintptr_t MemoryManager::FindPattern(const std::vector<int>& pattern) const {
	const uintptr_t moduleSize = GetModuleSize();
	if (baseAddressGameEXE == NULL || moduleSize == 0 || pattern.empty() || pattern.size() > moduleSize)
		return 0;

	const auto* const moduleBytes = reinterpret_cast<const uint8_t*>(baseAddressGameEXE);
	const size_t scanEnd = moduleSize - pattern.size();
	for (size_t i = 0; i <= scanEnd; ++i) {
		bool matched = true;
		for (size_t j = 0; j < pattern.size(); ++j) {
			if (pattern[j] >= 0 && moduleBytes[i + j] != static_cast<uint8_t>(pattern[j])) {
				matched = false;
				break;
			}
		}
		if (matched)
			return baseAddressGameEXE + i;
	}

	return 0;
}

std::vector<uintptr_t> MemoryManager::FindPatterns(const std::vector<int>& pattern) const {
	std::vector<uintptr_t> matches;
	const uintptr_t moduleSize = GetModuleSize();
	if (baseAddressGameEXE == NULL || moduleSize == 0 || pattern.empty() || pattern.size() > moduleSize)
		return matches;

	const auto* const moduleBytes = reinterpret_cast<const uint8_t*>(baseAddressGameEXE);
	const size_t scanEnd = moduleSize - pattern.size();
	for (size_t i = 0; i <= scanEnd; ++i) {
		bool matched = true;
		for (size_t j = 0; j < pattern.size(); ++j) {
			if (pattern[j] >= 0 && moduleBytes[i + j] != static_cast<uint8_t>(pattern[j])) {
				matched = false;
				break;
			}
		}
		if (matched)
			matches.push_back(baseAddressGameEXE + i);
	}

	return matches;
}

void* MemoryManager::AllocateNear(uintptr_t target, size_t size) const {
	SYSTEM_INFO systemInfo{};
	GetSystemInfo(&systemInfo);

	const uintptr_t granularity = systemInfo.dwAllocationGranularity;
	const uintptr_t maxDistance = 0x7FFF0000;
	const uintptr_t base = target & ~(granularity - 1);

	for (uintptr_t distance = 0; distance < maxDistance; distance += granularity) {
		const uintptr_t lower = base > distance ? base - distance : 0;
		if (lower != 0) {
			if (void* memory = VirtualAlloc(reinterpret_cast<void*>(lower), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
				return memory;
		}

		const uintptr_t upper = base + distance;
		if (upper > base) {
			if (void* memory = VirtualAlloc(reinterpret_cast<void*>(upper), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
				return memory;
		}
	}

	return nullptr;
}

bool MemoryManager::WriteProcessBytes(uintptr_t address, const std::vector<uint8_t>& bytes) const {
	if (address == 0 || bytes.empty())
		return false;

	DWORD oldProtect = 0;
	if (!VirtualProtect(reinterpret_cast<void*>(address), bytes.size(), PAGE_EXECUTE_READWRITE, &oldProtect))
		return false;

	std::memcpy(reinterpret_cast<void*>(address), bytes.data(), bytes.size());
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), bytes.size());

	DWORD ignoredProtect = 0;
	VirtualProtect(reinterpret_cast<void*>(address), bytes.size(), oldProtect, &ignoredProtect);
	return true;
}

bool MemoryManager::InstallBytePatch(const char* name, uintptr_t target, const std::vector<uint8_t>& bytes) {
	if (target == 0 || bytes.empty())
		return false;

	RuntimePatch patch{};
	patch.name = name;
	patch.address = target;
	patch.overwriteSize = bytes.size();
	patch.originalBytes.resize(bytes.size());
	std::memcpy(patch.originalBytes.data(), reinterpret_cast<void*>(target), bytes.size());

	if (!WriteProcessBytes(target, bytes))
		return false;

	patch.applied = true;
	combatAssistPatches.push_back(patch);
	return true;
}

bool MemoryManager::InstallHookPatch(const char* name, uintptr_t target, size_t overwriteSize,
	const std::function<std::vector<uint8_t>(uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes)>& buildCode) {
	if (target == 0 || overwriteSize < 5)
		return false;

	RuntimePatch patch{};
	patch.name = name;
	patch.address = target;
	patch.overwriteSize = overwriteSize;
	patch.originalBytes.resize(overwriteSize);
	std::memcpy(patch.originalBytes.data(), reinterpret_cast<void*>(target), overwriteSize);

	// The player damaging-trace cave preserves the native argument pair, checks
	// the verified player-fire return sites, performs the one-shot snapshot
	// validation, and restores the original path.  That generated sequence is
	// larger than the old 512-byte scratch cave, so the hook silently failed
	// before it could ever produce a NativeTraceProbe.
	const size_t caveSize = 4096;
	void* cave = AllocateNear(target, caveSize);
	if (cave == nullptr)
		return false;

	std::vector<uint8_t> caveCode = buildCode(reinterpret_cast<uintptr_t>(cave), target + overwriteSize, patch.originalBytes);
	if (caveCode.empty() || caveCode.size() > caveSize) {
		uevr::API::get()->log_error("[CombatAssist] %s cave generation failed size=%zu capacity=%zu",
			name != nullptr ? name : "<unnamed>", caveCode.size(), caveSize);
		VirtualFree(cave, 0, MEM_RELEASE);
		return false;
	}

	std::memcpy(cave, caveCode.data(), caveCode.size());
	FlushInstructionCache(GetCurrentProcess(), cave, caveCode.size());

	std::vector<uint8_t> jumpBytes;
	if (!AppendRelJmp(jumpBytes, target, reinterpret_cast<uintptr_t>(cave))) {
		VirtualFree(cave, 0, MEM_RELEASE);
		return false;
	}
	while (jumpBytes.size() < overwriteSize)
		jumpBytes.push_back(0x90);

	if (!WriteProcessBytes(target, jumpBytes)) {
		VirtualFree(cave, 0, MEM_RELEASE);
		return false;
	}

	patch.codeCave = cave;
	patch.codeCaveSize = caveSize;
	patch.applied = true;
	combatAssistPatches.push_back(patch);
	return true;
}

bool MemoryManager::InstallRuntimeArrayCallbackHook(const char* name, uintptr_t target,
	size_t overwriteSize, const void* expectedObjectPointerStorage, int32_t vectorStride,
	const char* path, void* callback) {
	if (name == nullptr || target == 0 || overwriteSize < 5
		|| expectedObjectPointerStorage == nullptr || (vectorStride != 12 && vectorStride != 24)
		|| path == nullptr || callback == nullptr)
		return false;

	for (const auto& existing : combatAssistPatches) {
		if (existing.applied && existing.address == target)
			return false;
	}

	return InstallHookPatch(name, target, overwriteSize,
		[expectedObjectPointerStorage, vectorStride, path, callback](uintptr_t caveAddress,
			uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes) {
			std::vector<uint8_t> code;
			code.push_back(0x9C); // pushfq
			code.push_back(0x50); // push rax
			code.push_back(0x51); // push rcx
			code.push_back(0x52); // push rdx
			code.insert(code.end(), { 0x41, 0x50 }); // push r8
			code.insert(code.end(), { 0x41, 0x51 }); // push r9
			code.insert(code.end(), { 0x41, 0x52 }); // push r10
			code.insert(code.end(), { 0x41, 0x53 }); // push r11

			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(expectedObjectPointerStorage));
			code.insert(code.end(), { 0x4D, 0x8B, 0x12 }); // mov r10,[r10]
			code.insert(code.end(), { 0x4C, 0x39, 0xD1 }); // cmp rcx,r10
			const size_t unrelatedObjectOffset = AppendNearJcc(code, 0x85); // jne restore

			// Reserve x64 shadow space, align RSP, and preserve all volatile XMM
			// argument registers before entering normal C++ code.
			code.insert(code.end(), { 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00 }); // sub rsp,88h
			for (uint8_t xmm = 0; xmm < 6; ++xmm) {
				code.insert(code.end(), { 0xF3, 0x0F, 0x7F,
					static_cast<uint8_t>(0x44 | (xmm << 3)), 0x24,
					static_cast<uint8_t>(0x20 + xmm * 0x10) }); // movdqu [rsp+disp],xmmN
			}

			code.insert(code.end(), { 0x4C, 0x89, 0xC1 }); // mov rcx,r8 (RawArray*)
			code.push_back(0xBA); AppendU32(code, static_cast<uint32_t>(vectorStride)); // mov edx,stride
			code.push_back(0x49); code.push_back(0xB8); AppendU64(code, reinterpret_cast<uintptr_t>(path));
			code.push_back(0x48); code.push_back(0xB8); AppendU64(code, reinterpret_cast<uintptr_t>(callback));
			code.insert(code.end(), { 0xFF, 0xD0 }); // call rax

			for (uint8_t xmm = 0; xmm < 6; ++xmm) {
				code.insert(code.end(), { 0xF3, 0x0F, 0x6F,
					static_cast<uint8_t>(0x44 | (xmm << 3)), 0x24,
					static_cast<uint8_t>(0x20 + xmm * 0x10) }); // movdqu xmmN,[rsp+disp]
			}
			code.insert(code.end(), { 0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00 }); // add rsp,88h

			const size_t restoreOffset = code.size();
			code.insert(code.end(), { 0x41, 0x5B }); // pop r11
			code.insert(code.end(), { 0x41, 0x5A }); // pop r10
			code.insert(code.end(), { 0x41, 0x59 }); // pop r9
			code.insert(code.end(), { 0x41, 0x58 }); // pop r8
			code.push_back(0x5A); // pop rdx
			code.push_back(0x59); // pop rcx
			code.push_back(0x58); // pop rax
			code.push_back(0x9D); // popfq
			code.insert(code.end(), originalBytes.begin(), originalBytes.end());
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress)
				|| !PatchNearJcc(code, unrelatedObjectOffset, restoreOffset))
				return std::vector<uint8_t>{};
			return code;
		});
}

void MemoryManager::AdjustAddresses() {
	if (settingsManager->debugMod) uevr::API::get()->log_info("AdjustAddresses()");
	MemoryManager::cameraMatrixAddresses = {
	0x53E2C00, 0x53E2C04, 0x53E2C08, 0x53E2C0C,
	0x53E2C10, 0x53E2C14, 0x53E2C18, 0x53E2C1C,
	0x53E2C20, 0x53E2C24, 0x53E2C28, 0x53E2C2C,
	0x53E2C30, 0x53E2C34, 0x53E2C38, 0x53E2C3C };

	for (auto& [address, size, bytes] : matrixInstructionsRotationAddresses) { address += baseAddressGameEXE; }
	for (auto& [address, size, bytes] : matrixInstructionsPositionAddresses) { address += baseAddressGameEXE; }
	for (auto& [address, size, bytes] : ingameCameraPositionInstructionsAddresses) { address += baseAddressGameEXE; }
	for (auto& [address, size, bytes] : ingameCameraPositionSniperAndCamWpnInstructionsAddresses) { address += baseAddressGameEXE; }
	for (auto& [address, size, bytes] : aimingForwardVectorInstructionsAddresses) { address += baseAddressGameEXE; }
	for (auto& [address, size, bytes] : aimingUpVectorInstructionsAddresses) { address += baseAddressGameEXE; }
	for (auto& [address, size, bytes] : pitchAxisAimingInstructionsAddresses) { address += baseAddressGameEXE; }
	for (auto& [address, size, bytes] : rocketLauncherAimingVectorInstructionsAddresses) { address += baseAddressGameEXE; }
	for (auto& [address, size, bytes] : sniperAimingVectorInstructionsAddresses) { address += baseAddressGameEXE; }
	for (auto& [address, size, bytes] : carAimingVectorInstructionsAddresses) { address += baseAddressGameEXE; }

	for (auto& address : MemoryManager::cameraMatrixAddresses) address += baseAddressGameEXE;
	for (auto& address : aimForwardVectorAddresses) address += baseAddressGameEXE;
	for (auto& address : cameraPositionAddresses) address += baseAddressGameEXE;
	for (auto& address : playerHeadPositionAddresses) address += baseAddressGameEXE;
	for (auto& address : playerPositionAddresses) address += baseAddressGameEXE;
	

	playerIsInControlAddress += baseAddressGameEXE;
	playerIsInVehicleAddress += baseAddressGameEXE;
	vehicleTypeAddress += baseAddressGameEXE;
	weaponWheelDisplayedAddress += baseAddressGameEXE;
	cameraModeAddress += baseAddressGameEXE;
	vehicleCameraModeAddress += baseAddressGameEXE;
	onFootCameraModeAddress += baseAddressGameEXE;
	xAxisSpraysAimAddress += baseAddressGameEXE;
	playerShootInstructionAddress += baseAddressGameEXE;
	playerShootCam45InstructionAddress += baseAddressGameEXE;
	//cameraShootInstructionAddress += baseAddressGameEXE;
	playerShootFromCarInputAddress += baseAddressGameEXE;
	/*cutscenePlayingAddress += baseAddressGameEXE;*/
}

void MemoryManager::NopVehicleRelatedMemoryInstructions()
{
	if (settingsManager->debugMod) uevr::API::get()->log_info("NopVehicleRelatedMemoryInstructions()");
	NopMemory(matrixInstructionsPositionAddresses);
	NopMemory(ingameCameraPositionInstructionsAddresses);
	NopMemory(ingameCameraPositionSniperAndCamWpnInstructionsAddresses);
	NopMemory(pitchAxisAimingInstructionsAddresses);
	NopMemory(aimingForwardVectorInstructionsAddresses);
	NopMemory(aimingUpVectorInstructionsAddresses);
	NopMemory(rocketLauncherAimingVectorInstructionsAddresses);
	NopMemory(sniperAimingVectorInstructionsAddresses);
	vehicleRelatedMemoryInstructionsNoped = true;
};

void MemoryManager::RestoreVehicleRelatedMemoryInstructions()
{
	if (settingsManager->debugMod) uevr::API::get()->log_info("RestoreVehicleRelatedMemoryInstructions()");
	RestoreMemory(matrixInstructionsPositionAddresses);
	RestoreMemory(ingameCameraPositionInstructionsAddresses);
	RestoreMemory(ingameCameraPositionSniperAndCamWpnInstructionsAddresses);
	RestoreMemory(pitchAxisAimingInstructionsAddresses);
	RestoreMemory(aimingForwardVectorInstructionsAddresses);
	RestoreMemory(aimingUpVectorInstructionsAddresses);
	RestoreMemory(rocketLauncherAimingVectorInstructionsAddresses);
	RestoreMemory(sniperAimingVectorInstructionsAddresses);
	vehicleRelatedMemoryInstructionsNoped = false;
}

void MemoryManager::RestoreCarAimingVectorInstructions()
{
	if (settingsManager->debugMod) uevr::API::get()->log_info("RestoreCarAimingVectorInstructions()");
	RestoreMemory(carAimingVectorInstructionsAddresses);
}

void MemoryManager::ToggleHeliCanonCameraModMemoryInstructions(bool restoreInstructions)
{
	if (!restoreInstructions)
	{
		NopMemory(matrixInstructionsRotationAddresses);
		NopMemory(pitchAxisAimingInstructionsAddresses);
		NopMemory(aimingForwardVectorInstructionsAddresses);
	}
	if (restoreInstructions)
	{
		RestoreMemory(matrixInstructionsRotationAddresses);
		RestoreMemory(pitchAxisAimingInstructionsAddresses);
		RestoreMemory(aimingForwardVectorInstructionsAddresses);
	}
}

void MemoryManager::ToggleAllMemoryInstructions(bool restoreInstructions)
{
	if (settingsManager->debugMod) uevr::API::get()->log_info("ToggleAllMemoryInstructions(enabled : %i )", restoreInstructions);
	if (!restoreInstructions)
	{
		NopMemory(matrixInstructionsRotationAddresses);
		NopMemory(matrixInstructionsPositionAddresses);
		NopMemory(ingameCameraPositionInstructionsAddresses);
		NopMemory(ingameCameraPositionSniperAndCamWpnInstructionsAddresses);
		NopMemory(pitchAxisAimingInstructionsAddresses);
		NopMemory(aimingForwardVectorInstructionsAddresses);
		NopMemory(aimingUpVectorInstructionsAddresses);
		NopMemory(rocketLauncherAimingVectorInstructionsAddresses);
		NopMemory(sniperAimingVectorInstructionsAddresses);
		NopMemory(carAimingVectorInstructionsAddresses);
	}
	if (restoreInstructions)
	{
		RestoreMemory(matrixInstructionsRotationAddresses);
		RestoreMemory(matrixInstructionsPositionAddresses);
		RestoreMemory(ingameCameraPositionInstructionsAddresses);
		RestoreMemory(ingameCameraPositionSniperAndCamWpnInstructionsAddresses);
		RestoreMemory(pitchAxisAimingInstructionsAddresses);
		RestoreMemory(aimingForwardVectorInstructionsAddresses);
		RestoreMemory(aimingUpVectorInstructionsAddresses);
		RestoreMemory(rocketLauncherAimingVectorInstructionsAddresses);
		RestoreMemory(sniperAimingVectorInstructionsAddresses);
		RestoreMemory(carAimingVectorInstructionsAddresses);
	}
	vehicleRelatedMemoryInstructionsNoped = !restoreInstructions;
}

bool MemoryManager::ResolveCombatAssistStats() {
	if (combatAssistStatsResolved)
		return true;
	if (combatAssistStatsResolveAttempted)
		return false;

	combatAssistStatsResolveAttempted = true;

	const uintptr_t statsPattern = FindPattern({
		0x00, 0x83, 0xF8, 0x52, 0x73, 0x0B, 0xF3, 0x0F, 0x10,
		-1, -1, -1, -1, -1, -1, 0xEB
	});

	if (statsPattern == 0) {
		uevr::API::get()->log_error("%s", "[CombatAssist] CJStats signature not found; weapon skills were not raised");
		return false;
	}

	combatAssistFloatStatsOffset = ReadU32(statsPattern + 0x0B);
	const uintptr_t moduleSize = GetModuleSize();
	if (combatAssistFloatStatsOffset == 0 || combatAssistFloatStatsOffset >= moduleSize) {
		uevr::API::get()->log_error("[CombatAssist] CJStats offset looked invalid: 0x%llX", static_cast<unsigned long long>(combatAssistFloatStatsOffset));
		combatAssistFloatStatsOffset = 0;
		return false;
	}

	combatAssistStatsResolved = true;
	uevr::API::get()->log_info("[CombatAssist] CJStats float table offset: 0x%llX", static_cast<unsigned long long>(combatAssistFloatStatsOffset));
	return true;
}

bool MemoryManager::ResolveCombatAssistPlayerGlobals() {
	if (combatAssistPlayerGlobalsResolved)
		return true;
	if (combatAssistPlayerGlobalsResolveAttempted)
		return false;

	combatAssistPlayerGlobalsResolveAttempted = true;

	const uintptr_t playerGlobalsPattern = FindPattern({
		0x0F, 0xB6, 0x05, -1, -1, -1, -1,
		0x48, 0x8D, 0x15, -1, -1, -1, -1,
		0x48, 0x69, 0xC0, 0xC0, 0x01, 0x00, 0x00,
		0x48, 0x8B, 0x14, 0x10
	});

	if (playerGlobalsPattern == 0) {
		uevr::API::get()->log_error("%s", "[CombatAssist] current-player globals signature not found; player-only hooks stay guarded off");
		return false;
	}

	combatAssistCurrentPlayerIndexAddress = playerGlobalsPattern + 0x07 + ReadI32(playerGlobalsPattern + 0x03);
	combatAssistPlayerInfoArrayAddress = playerGlobalsPattern + 0x0E + ReadI32(playerGlobalsPattern + 0x0A);

	const uintptr_t moduleSize = GetModuleSize();
	const uintptr_t moduleEnd = baseAddressGameEXE + moduleSize;
	if (combatAssistCurrentPlayerIndexAddress <= baseAddressGameEXE ||
		combatAssistCurrentPlayerIndexAddress >= moduleEnd ||
		combatAssistPlayerInfoArrayAddress <= baseAddressGameEXE ||
		combatAssistPlayerInfoArrayAddress >= moduleEnd) {
		uevr::API::get()->log_error("[CombatAssist] current-player globals looked invalid: index=0x%llX array=0x%llX",
			static_cast<unsigned long long>(combatAssistCurrentPlayerIndexAddress),
			static_cast<unsigned long long>(combatAssistPlayerInfoArrayAddress));
		combatAssistCurrentPlayerIndexAddress = 0;
		combatAssistPlayerInfoArrayAddress = 0;
		return false;
	}

	combatAssistPlayerGlobalsResolved = true;
	uevr::API::get()->log_info("[CombatAssist] current-player globals: index=0x%llX array=0x%llX",
		static_cast<unsigned long long>(combatAssistCurrentPlayerIndexAddress - baseAddressGameEXE),
		static_cast<unsigned long long>(combatAssistPlayerInfoArrayAddress - baseAddressGameEXE));
	return true;
}

void MemoryManager::UpdateCombatAssistPlayerPointer() {
	if (!ResolveCombatAssistPlayerGlobals())
		return;

	const uint8_t playerIndex = *reinterpret_cast<uint8_t*>(combatAssistCurrentPlayerIndexAddress);
	if (playerIndex >= 8) {
		cachedPlayerPointer = 0;
		return;
	}

	cachedPlayerPointer = *reinterpret_cast<uintptr_t*>(combatAssistPlayerInfoArrayAddress + (static_cast<uintptr_t>(playerIndex) * 0x1C0));
}

bool MemoryManager::CyclePlayerWeaponSlot(int direction) {
	UpdateCombatAssistPlayerPointer();

	const uintptr_t playerPed = cachedPlayerPointer;
	if (playerPed == 0 || !IsReadableMemory(playerPed, 0x1000)) {
		uevr::API::get()->log_warn("[DirectWeaponCycle] no readable current player ped");
		return false;
	}

	constexpr int SlotCount = 13;
	constexpr size_t WeaponTypeOffset = 0x00;
	constexpr size_t AmmoInClipOffset = 0x08;
	constexpr size_t AmmoTotalOffset = 0x0C;
	constexpr size_t SelectedSlotPostArrayOffset = 0x0C;
	struct WeaponSlotLayout {
		size_t arrayOffset;
		size_t stride;
		size_t selectedSlotOffset;
		bool exact;
	};

	const WeaponSlotLayout layouts[] = {
		// San Andreas CPed layout: m_aWeapons at 0x5A0, selected slot at 0x718.
		{ 0x5A0, 0x1C, 0x718, true },
	};

	auto tryCycleLayout = [&](const WeaponSlotLayout& layout) -> int {
		if (!IsReadableMemory(playerPed + layout.arrayOffset, (layout.stride * SlotCount) + 0x20) ||
			!IsReadableMemory(playerPed + layout.selectedSlotOffset, sizeof(uint8_t))) {
			return 0;
		}

		uint32_t slotTypes[SlotCount]{};
		uint32_t ammoInClip[SlotCount]{};
		uint32_t ammoTotal[SlotCount]{};
		int validSlotTypes = 0;
		for (int slot = 0; slot < SlotCount; ++slot) {
			const uintptr_t entry = playerPed + layout.arrayOffset + (static_cast<size_t>(slot) * layout.stride);
			if (!TryRead(entry + WeaponTypeOffset, slotTypes[slot]) ||
				!TryRead(entry + AmmoInClipOffset, ammoInClip[slot]) ||
				!TryRead(entry + AmmoTotalOffset, ammoTotal[slot])) {
				return 0;
			}

			const int expectedSlot = WeaponSlotForWeaponType(static_cast<int>(slotTypes[slot]));
			if (expectedSlot == slot || (slot == 0 && slotTypes[slot] == 0)) {
				++validSlotTypes;
			}
			else if (slotTypes[slot] != 0) {
				return 0;
			}
		}

		if (validSlotTypes < 8)
			return 0;

		const uintptr_t selectedSlotAddress = playerPed + layout.selectedSlotOffset;
		uint8_t selectedSlot = 0;
		if (!TryRead(selectedSlotAddress, selectedSlot) || selectedSlot >= SlotCount)
			return 0;

		const int selectedTypeSlot = WeaponSlotForWeaponType(static_cast<int>(slotTypes[selectedSlot]));
		if (selectedTypeSlot < 0 || selectedTypeSlot != selectedSlot) {
			uevr::API::get()->log_warn("[DirectWeaponCycle] rejected layout selected=%u type=%u array=ped+0x%llX selected=ped+0x%llX stride=0x%llX",
				selectedSlot,
				slotTypes[selectedSlot],
				static_cast<unsigned long long>(layout.arrayOffset),
				static_cast<unsigned long long>(layout.selectedSlotOffset),
				static_cast<unsigned long long>(layout.stride));
			return 0;
		}

		int targetSlot = -1;
		const int step = direction >= 0 ? 1 : -1;
		for (int i = 1; i <= SlotCount; ++i) {
			const int candidate = (selectedSlot + (step * i) + (SlotCount * 2)) % SlotCount;
			if (candidate == selectedSlot)
				continue;
			if (IsWeaponSlotAvailable(candidate, slotTypes[candidate], ammoInClip[candidate], ammoTotal[candidate])) {
				targetSlot = candidate;
				break;
			}
		}

		if (targetSlot < 0) {
			uevr::API::get()->log_warn("[DirectWeaponCycle] no next available slot; selected=%u type=%u array=ped+0x%llX selected=ped+0x%llX stride=0x%llX",
				selectedSlot,
				slotTypes[selectedSlot],
				static_cast<unsigned long long>(layout.arrayOffset),
				static_cast<unsigned long long>(layout.selectedSlotOffset),
				static_cast<unsigned long long>(layout.stride));
			return -1;
		}

		DWORD oldProtect = 0;
		if (!VirtualProtect(reinterpret_cast<void*>(selectedSlotAddress), sizeof(uint8_t), PAGE_EXECUTE_READWRITE, &oldProtect))
			return -1;
		*reinterpret_cast<uint8_t*>(selectedSlotAddress) = static_cast<uint8_t>(targetSlot);
		VirtualProtect(reinterpret_cast<void*>(selectedSlotAddress), sizeof(uint8_t), oldProtect, &oldProtect);

		uevr::API::get()->log_info("[DirectWeaponCycle] selected slot %u(type=%u) -> %d(type=%u); array=ped+0x%llX selected=ped+0x%llX stride=0x%llX",
			selectedSlot,
			slotTypes[selectedSlot],
			targetSlot,
			slotTypes[targetSlot],
			static_cast<unsigned long long>(layout.arrayOffset),
			static_cast<unsigned long long>(layout.selectedSlotOffset),
			static_cast<unsigned long long>(layout.stride));
		return 1;
	};

	for (const WeaponSlotLayout& layout : layouts) {
		const int result = tryCycleLayout(layout);
		if (result > 0)
			return true;
		if (result < 0)
			return false;
	}

	uevr::API::get()->log_warn("[DirectWeaponCycle] could not resolve player weapon slots");
	return false;
}

bool MemoryManager::SelectPlayerWeaponType(uint32_t weaponType) {
	UpdateCombatAssistPlayerPointer();

	const uintptr_t playerPed = cachedPlayerPointer;
	const int targetSlot = WeaponSlotForWeaponType(static_cast<int>(weaponType));
	if (playerPed == 0 || !IsReadableMemory(playerPed, 0x1000) || targetSlot < 0 || targetSlot >= 13)
		return false;

	constexpr size_t WeaponArrayOffset = 0x5A0;
	constexpr size_t WeaponStride = 0x1C;
	constexpr size_t SelectedSlotOffset = 0x718;
	const uintptr_t weaponEntry = playerPed + WeaponArrayOffset + (static_cast<size_t>(targetSlot) * WeaponStride);
	const uintptr_t selectedSlotAddress = playerPed + SelectedSlotOffset;
	uint32_t slotWeaponType = 0;
	uint8_t selectedSlot = 0;
	if (!TryRead(weaponEntry, slotWeaponType) || slotWeaponType != weaponType ||
		!TryRead(selectedSlotAddress, selectedSlot) || selectedSlot >= 13) {
		return false;
	}
	if (selectedSlot == static_cast<uint8_t>(targetSlot))
		return true;

	DWORD oldProtect = 0;
	if (!VirtualProtect(reinterpret_cast<void*>(selectedSlotAddress), sizeof(uint8_t), PAGE_EXECUTE_READWRITE, &oldProtect))
		return false;
	*reinterpret_cast<uint8_t*>(selectedSlotAddress) = static_cast<uint8_t>(targetSlot);
	VirtualProtect(reinterpret_cast<void*>(selectedSlotAddress), sizeof(uint8_t), oldProtect, &oldProtect);
	uevr::API::get()->log_info("[ManualReload] reselected slot %u -> %d for weapon type=%u",
		selectedSlot, targetSlot, weaponType);
	return true;
}

void MemoryManager::ReportManualReloadEmptyState(bool empty) {
	if (manualReloadEmptyStateKnown && manualReloadEmptyStateReported == empty)
		return;

	manualReloadEmptyStateKnown = true;
	manualReloadEmptyStateReported = empty;
	uevr::API::get()->dispatch_lua_event("manualReloadEmptyState", empty ? "true" : "false");
	uevr::API::get()->log_info("[ManualReload] empty visual state=%s", empty ? "true" : "false");
}

bool MemoryManager::ReloadCurrentWeaponOneMagazine(int expectedWeaponType, bool dualWield) {
	if (!settingsManager->activeManualReloadMode) {
		uevr::API::get()->log_warn("[ManualReload] rejected: manual reload mode is not active; enable it and restart/reinject");
		return false;
	}

	uint8_t controlState = 1;
	if (!TryRead(playerIsInControlAddress, controlState) || controlState != 0) {
		uevr::API::get()->log_info("[ManualReload] ignored while player controls are unavailable");
		return false;
	}

	uint8_t inVehicle = 0;
	if (!TryRead(playerIsInVehicleAddress, inVehicle) || inVehicle != 0) {
		uevr::API::get()->log_info("[ManualReload] ignored unless on-foot state is confirmed");
		return false;
	}

	constexpr size_t WeaponTypeOffset = 0x00;
	constexpr size_t WeaponStateOffset = 0x04;
	constexpr size_t AmmoInClipOffset = 0x08;
	constexpr size_t AmmoTotalOffset = 0x0C;
	constexpr uint32_t WeaponStateReady = 0;

	const uintptr_t weaponEntry = static_cast<uintptr_t>(InterlockedCompareExchange64(
		&manualReloadCapturedWeaponEntry, 0, 0));
	const uint32_t capturedWeaponType = static_cast<uint32_t>(InterlockedCompareExchange(
		&manualReloadCapturedWeaponType, 0, 0));
	if (weaponEntry == 0 || !IsReadableMemory(weaponEntry, 0x10)) {
		uevr::API::get()->log_info("[ManualReload] no captured weapon yet; fire one round with this weapon first");
		return false;
	}
	uint32_t actualWeaponType = 0;
	uint32_t weaponStateValue = 0;
	uint32_t ammoInClip = 0;
	uint32_t ammoTotal = 0;
	if (!TryRead(weaponEntry + WeaponTypeOffset, actualWeaponType) ||
		!TryRead(weaponEntry + WeaponStateOffset, weaponStateValue) ||
		!TryRead(weaponEntry + AmmoInClipOffset, ammoInClip) ||
		!TryRead(weaponEntry + AmmoTotalOffset, ammoTotal)) {
		uevr::API::get()->log_warn("[ManualReload] rejected: current weapon entry is unreadable");
		return false;
	}

	uint32_t weaponType = actualWeaponType;
	bool restoringClearedWeaponEntry = false;
	if (actualWeaponType != capturedWeaponType) {
		if (actualWeaponType == 0 && capturedWeaponType < manualReloadWeapons.size() &&
			ManualReloadClipSize(capturedWeaponType) != 0) {
			const ManualReloadWeaponState& capturedState = manualReloadWeapons[capturedWeaponType];
			if (capturedState.initialized && capturedState.weaponEntry == weaponEntry) {
				weaponType = capturedWeaponType;
				restoringClearedWeaponEntry = true;
			}
		}
	}

	const uint32_t baseMagazineSize = ManualReloadClipSize(weaponType);
	if (baseMagazineSize == 0) {
		uevr::API::get()->log_info("[ManualReload] ignored unsupported weapon type=%u", weaponType);
		return false;
	}
	if (weaponType != capturedWeaponType ||
		(expectedWeaponType >= 0 && weaponType != static_cast<uint32_t>(expectedWeaponType))) {
		uevr::API::get()->log_info("[ManualReload] current weapon is not captured yet expected=%d captured=%u actual=%u; fire one round first",
			expectedWeaponType, capturedWeaponType, weaponType);
		return false;
	}

	if (manualReloadStageActive) {
		uevr::API::get()->log_info("[ManualReload] ignored: a reload is already pending");
		return false;
	}

	ManualReloadWeaponState& weaponState = manualReloadWeapons[weaponType];
	if (!weaponState.initialized || weaponState.weaponEntry != weaponEntry) {
		weaponState.weaponEntry = weaponEntry;
		weaponState.weaponType = weaponType;
		weaponState.hiddenReserve = ammoTotal > ammoInClip ? ammoTotal - ammoInClip : 0;
		weaponState.lastVisibleTotal = ammoTotal;
		weaponState.magazineCapacity = baseMagazineSize;
		weaponState.emptyLatched = ammoInClip == 0;
		weaponState.initialized = true;
	}
	else if (ammoTotal > weaponState.lastVisibleTotal) {
		const uint64_t combinedReserve = static_cast<uint64_t>(weaponState.hiddenReserve) +
			(ammoTotal - weaponState.lastVisibleTotal);
		weaponState.hiddenReserve = static_cast<uint32_t>((std::min)(
			combinedReserve, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));
	}

	const uint32_t dualMagazineSize = ManualReloadMagazineCapacity(weaponType, true);
	const bool observedDualCapacity = SupportsDualWieldManualReload(weaponType) &&
		ammoInClip > baseMagazineSize && ammoInClip <= dualMagazineSize;
	const bool useDualCapacity = SupportsDualWieldManualReload(weaponType) &&
		(dualWield || observedDualCapacity || weaponState.magazineCapacity > baseMagazineSize);
	const uint32_t magazineSize = ManualReloadMagazineCapacity(weaponType, useDualCapacity);
	weaponState.magazineCapacity = magazineSize;
	if (ammoInClip > magazineSize) {
		uevr::API::get()->log_info("[ManualReload] ignored unsupported oversized clip type=%u clip=%u expected=%u dual=%s",
			weaponType, ammoInClip, magazineSize, useDualCapacity ? "true" : "false");
		return false;
	}

	const uint32_t playableLoadSize = ManualReloadPlayableLoadSize(weaponType, magazineSize);
	const uint64_t currentPlayableLoad = UsesTubeFedManualReloadLoad(weaponType)
		? static_cast<uint64_t>(ammoInClip) + weaponState.hiddenReserve
		: ammoInClip;
	if (currentPlayableLoad >= playableLoadSize) {
		uevr::API::get()->log_info("[ManualReload] ignored: magazine is already full type=%u clip=%u playable=%llu/%u",
			weaponType, ammoInClip, static_cast<unsigned long long>(currentPlayableLoad), playableLoadSize);
		return false;
	}

	uint32_t targetClip = magazineSize;
	uint32_t consumedReserve = 0;
	if (!settingsManager->activeCombatAssistAmmo) {
		const uint32_t needed = magazineSize - ammoInClip;
		consumedReserve = (std::min)(needed, weaponState.hiddenReserve);
		targetClip = ammoInClip + consumedReserve;
		if (targetClip <= ammoInClip) {
			uevr::API::get()->log_info("[ManualReload] ignored: no reserve ammunition type=%u clip=%u",
				weaponType, ammoInClip);
			return false;
		}
	}

	if (UsesDirectManualReloadWrite(weaponType)) {
		const uint32_t remainingHiddenReserve =
			UsesTubeFedManualReloadLoad(weaponType) && settingsManager->activeCombatAssistAmmo
			? playableLoadSize - targetClip
			: weaponState.hiddenReserve - consumedReserve;
		const uint64_t visibleTotal64 = static_cast<uint64_t>(targetClip) + remainingHiddenReserve;
		const uint32_t visibleTotal = static_cast<uint32_t>((std::min)(
			visibleTotal64, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));
		std::vector<uint8_t> restoredWeapon;
		uintptr_t writeAddress = weaponEntry + WeaponStateOffset;
		if (restoringClearedWeaponEntry) {
			writeAddress = weaponEntry;
			AppendU32(restoredWeapon, weaponType);
		}
		AppendU32(restoredWeapon, WeaponStateReady);
		AppendU32(restoredWeapon, targetClip);
		AppendU32(restoredWeapon, visibleTotal);
		if (!WriteProcessBytes(writeAddress, restoredWeapon)) {
			uevr::API::get()->log_warn("[ManualReload] failed direct single-round reload type=%u", weaponType);
			return false;
		}

		weaponState.hiddenReserve = remainingHiddenReserve;
		weaponState.lastVisibleTotal = visibleTotal;
		weaponState.emptyLatched = false;
		ReportManualReloadEmptyState(false);
		const bool selected = SelectPlayerWeaponType(weaponType);
		uevr::API::get()->log_info("[ManualReload] direct reload completed type=%u oldClip=%u clip=%u playableLoad=%u hidden=%u reselected=%s",
			weaponType, ammoInClip, targetClip, visibleTotal, weaponState.hiddenReserve,
			selected ? "true" : "false");
		return true;
	}

	std::vector<uint8_t> stagedWeapon;
	uintptr_t stagedWriteAddress = weaponEntry + WeaponStateOffset;
	if (restoringClearedWeaponEntry) {
		stagedWriteAddress = weaponEntry;
		AppendU32(stagedWeapon, weaponType);
	}
	AppendU32(stagedWeapon, WeaponStateReady);
	AppendU32(stagedWeapon, 0);
	AppendU32(stagedWeapon, targetClip);
	if (!WriteProcessBytes(stagedWriteAddress, stagedWeapon)) {
		uevr::API::get()->log_warn("[ManualReload] failed to stage native reload type=%u", weaponType);
		return false;
	}

	weaponState.hiddenReserve -= consumedReserve;
	weaponState.lastVisibleTotal = targetClip;
	manualReloadStageActive = true;
	manualReloadStageEntry = weaponEntry;
	manualReloadStageWeaponType = weaponType;
	manualReloadStageOriginalWeaponState = weaponStateValue;
	manualReloadStageOriginalClip = ammoInClip;
	manualReloadStageTargetClip = targetClip;
	manualReloadStageConsumedReserve = consumedReserve;
	manualReloadStageStartedAt = GetTickCount64();

	uevr::API::get()->log_info("[ManualReload] right-grip %s reload requested type=%u oldClip=%u targetClip=%u dual=%s infiniteReserve=%s restoredEntry=%s",
		ammoInClip == 0 ? "empty" : "tactical", weaponType, ammoInClip, targetClip,
		useDualCapacity ? "true" : "false",
		settingsManager->activeCombatAssistAmmo ? "true" : "false",
		restoringClearedWeaponEntry ? "true" : "false");
	return true;
}

void MemoryManager::MaintainManualReloadMode() {
	if (!settingsManager->activeManualReloadMode) {
		RestoreManualReloadState();
		return;
	}

	// The capture hook guards on the current player ped even when the broader
	// combat-assist feature is disabled, so keep that pointer fresh here.
	if (!manualReloadCaptureApplyAttempted)
		ApplyManualReloadCapturePatch();
	UpdateCombatAssistPlayerPointer();

	uint8_t controlState = 1;
	uint8_t inVehicle = 0;
	const bool playerInControl = TryRead(playerIsInControlAddress, controlState) && controlState == 0;
	const bool playerOnFoot = TryRead(playerIsInVehicleAddress, inVehicle) && inVehicle == 0;
	if (!playerInControl || !playerOnFoot) {
		RestoreManualReloadState();
		return;
	}

	constexpr size_t WeaponTypeOffset = 0x00;
	constexpr size_t WeaponStateOffset = 0x04;
	constexpr size_t AmmoInClipOffset = 0x08;
	constexpr size_t AmmoTotalOffset = 0x0C;
	constexpr uint32_t WeaponStateReady = 0;
	constexpr uint32_t WeaponStateOutOfAmmo = 3;
	constexpr uint32_t RetentionAmmo = 1;
	constexpr ULONGLONG ReloadTimeoutMs = 3500;

	if (manualReloadStageActive) {
		uint32_t stagedWeaponType = 0;
		uint32_t stagedClip = 0;
		uint32_t stagedTotal = 0;
		const bool stageReadable =
			TryRead(manualReloadStageEntry + WeaponTypeOffset, stagedWeaponType) &&
			TryRead(manualReloadStageEntry + AmmoInClipOffset, stagedClip) &&
			TryRead(manualReloadStageEntry + AmmoTotalOffset, stagedTotal) &&
			stagedWeaponType == manualReloadStageWeaponType;

		// The immediate-aim reload pulse can consume one round as the native
		// reload completes (the tested pistol path reports 16 for a target of 17).
		// Do not accept an arbitrary non-zero clip, because dual-wield reloads may
		// fill one hand before reaching their doubled target capacity.
		const uint32_t minimumCompletedClip = manualReloadStageTargetClip > 1
			? manualReloadStageTargetClip - 1
			: manualReloadStageTargetClip;
		if (stageReadable && stagedClip > 0 && stagedClip >= minimumCompletedClip) {
			const uint32_t retainedTotal = stagedClip == std::numeric_limits<uint32_t>::max()
				? stagedClip : stagedClip + RetentionAmmo;
			if (stagedTotal != retainedTotal) {
				std::vector<uint8_t> visibleTotal;
				AppendU32(visibleTotal, retainedTotal);
				WriteProcessBytes(manualReloadStageEntry + AmmoTotalOffset, visibleTotal);
			}
			ManualReloadWeaponState& weaponState = manualReloadWeapons[stagedWeaponType];
			weaponState.lastVisibleTotal = retainedTotal;
			weaponState.emptyLatched = false;
			ReportManualReloadEmptyState(false);
			uevr::API::get()->log_info("[ManualReload] native reload completed type=%u clip=%u target=%u",
				stagedWeaponType, stagedClip, manualReloadStageTargetClip);
			manualReloadStageActive = false;
		}
		else if (!stageReadable || GetTickCount64() - manualReloadStageStartedAt > ReloadTimeoutMs) {
			if (manualReloadStageWeaponType < manualReloadWeapons.size()) {
				ManualReloadWeaponState& weaponState = manualReloadWeapons[manualReloadStageWeaponType];
				const uint64_t restoredReserve =
					static_cast<uint64_t>(weaponState.hiddenReserve) + manualReloadStageConsumedReserve;
				weaponState.hiddenReserve = static_cast<uint32_t>((std::min)(
					restoredReserve, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));
			}

			if (stageReadable) {
				const bool restoreEmpty = manualReloadStageOriginalClip == 0;
				const uint32_t restoredVisibleTotal = restoreEmpty
					? RetentionAmmo
					: (manualReloadStageOriginalClip == std::numeric_limits<uint32_t>::max()
						? manualReloadStageOriginalClip : manualReloadStageOriginalClip + RetentionAmmo);
				std::vector<uint8_t> restoredWeapon;
				AppendU32(restoredWeapon, restoreEmpty ? WeaponStateOutOfAmmo : manualReloadStageOriginalWeaponState);
				AppendU32(restoredWeapon, restoreEmpty ? 0 : manualReloadStageOriginalClip);
				AppendU32(restoredWeapon, restoredVisibleTotal);
				WriteProcessBytes(manualReloadStageEntry + WeaponStateOffset, restoredWeapon);
				ManualReloadWeaponState& weaponState = manualReloadWeapons[manualReloadStageWeaponType];
				weaponState.lastVisibleTotal = restoredVisibleTotal;
				weaponState.emptyLatched = restoreEmpty;
				ReportManualReloadEmptyState(restoreEmpty);
			}
			uevr::API::get()->log_warn("[ManualReload] native reload did not start; restored clip type=%u clip=%u",
				manualReloadStageWeaponType, manualReloadStageOriginalClip);
			manualReloadStageActive = false;
		}

		if (!manualReloadStageActive) {
			manualReloadStageEntry = 0;
			manualReloadStageWeaponType = 0;
			manualReloadStageOriginalWeaponState = 0;
			manualReloadStageOriginalClip = 0;
			manualReloadStageTargetClip = 0;
			manualReloadStageConsumedReserve = 0;
			manualReloadStageStartedAt = 0;
		}
		else {
			return;
		}
	}

	const uintptr_t weaponEntry = static_cast<uintptr_t>(InterlockedCompareExchange64(
		&manualReloadCapturedWeaponEntry, 0, 0));
	const uint32_t capturedWeaponType = static_cast<uint32_t>(InterlockedCompareExchange(
		&manualReloadCapturedWeaponType, 0, 0));
	if (weaponEntry == 0 || capturedWeaponType >= manualReloadWeapons.size() ||
		!IsReadableMemory(weaponEntry, 0x10)) {
		return;
	}
	uint32_t weaponType = 0;
	uint32_t weaponStateValue = 0;
	uint32_t ammoInClip = 0;
	uint32_t ammoTotal = 0;
	if (!TryRead(weaponEntry + WeaponTypeOffset, weaponType) ||
		!TryRead(weaponEntry + WeaponStateOffset, weaponStateValue) ||
		!TryRead(weaponEntry + AmmoInClipOffset, ammoInClip) ||
		!TryRead(weaponEntry + AmmoTotalOffset, ammoTotal) ||
		weaponType != capturedWeaponType ||
		ManualReloadClipSize(weaponType) == 0) {
		return;
	}

	ManualReloadWeaponState& weaponState = manualReloadWeapons[weaponType];
	if (!weaponState.initialized || weaponState.weaponEntry != weaponEntry) {
		weaponState.weaponEntry = weaponEntry;
		weaponState.weaponType = weaponType;
		weaponState.hiddenReserve = ammoTotal > ammoInClip ? ammoTotal - ammoInClip : 0;
		weaponState.lastVisibleTotal = ammoTotal;
		weaponState.magazineCapacity = ManualReloadClipSize(weaponType);
		weaponState.emptyLatched = false;
		weaponState.initialized = true;
		ReportManualReloadEmptyState(false);
	}
	else if (ammoTotal > weaponState.lastVisibleTotal) {
		const uint64_t combinedReserve = static_cast<uint64_t>(weaponState.hiddenReserve) +
			(ammoTotal - weaponState.lastVisibleTotal);
		weaponState.hiddenReserve = static_cast<uint32_t>((std::min)(
			combinedReserve, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));
	}
	if (SupportsDualWieldManualReload(weaponType) &&
		ammoInClip > ManualReloadClipSize(weaponType) &&
		ammoInClip <= ManualReloadMagazineCapacity(weaponType, true)) {
		weaponState.magazineCapacity = ManualReloadMagazineCapacity(weaponType, true);
	}

	const LONG captureSequence = InterlockedCompareExchange(&manualReloadCaptureSequence, 0, 0);
	const uint32_t preShotClip = static_cast<uint32_t>(InterlockedCompareExchange(
		&manualReloadCapturedPreShotClip, 0, 0));
	const bool newShotCapture = captureSequence != lastReportedManualReloadCaptureSequence;
	if (newShotCapture && (UsesTubeFedManualReloadLoad(weaponType) || weaponType == 33) &&
		preShotClip <= 1 && ammoInClip == 0 && weaponState.hiddenReserve > 0) {
		// GTA models the pump shotgun and country rifle as a one-round chamber
		// plus total ammo. Move the next available round into the chamber.
		const uint32_t visibleTotal = (std::max)(ammoTotal, weaponState.hiddenReserve);
		weaponState.hiddenReserve = visibleTotal > 0 ? visibleTotal - 1 : 0;
		std::vector<uint8_t> chamberedWeapon;
		AppendU32(chamberedWeapon, WeaponStateReady);
		AppendU32(chamberedWeapon, 1);
		AppendU32(chamberedWeapon, visibleTotal);
		if (WriteProcessBytes(weaponEntry + WeaponStateOffset, chamberedWeapon)) {
			weaponState.lastVisibleTotal = visibleTotal;
			weaponState.emptyLatched = false;
			ReportManualReloadEmptyState(false);
			SelectPlayerWeaponType(weaponType);
			uevr::API::get()->log_info("[ManualReload] auto-chambered next round type=%u total=%u hidden=%u",
				weaponType, visibleTotal, weaponState.hiddenReserve);
			lastReportedManualReloadCaptureSequence = captureSequence;
			return;
		}
		uevr::API::get()->log_warn("[ManualReload] failed to auto-chamber next round type=%u", weaponType);
	}
	if ((newShotCapture && preShotClip <= 1) || ammoInClip == 0)
		weaponState.emptyLatched = true;

	if (weaponState.emptyLatched) {
		const uint32_t heldEmptyTotal = IsSingleRoundManualReloadWeapon(weaponType)
			? (std::max)(weaponState.hiddenReserve, RetentionAmmo)
			: RetentionAmmo;
		if (weaponStateValue != WeaponStateOutOfAmmo || ammoInClip != 0 || ammoTotal != heldEmptyTotal) {
			std::vector<uint8_t> heldEmptyWeapon;
			AppendU32(heldEmptyWeapon, WeaponStateOutOfAmmo);
			AppendU32(heldEmptyWeapon, 0);
			AppendU32(heldEmptyWeapon, heldEmptyTotal);
			WriteProcessBytes(weaponEntry + WeaponStateOffset, heldEmptyWeapon);
		}
		ammoInClip = 0;
		weaponState.lastVisibleTotal = heldEmptyTotal;
		ReportManualReloadEmptyState(true);
	}
	else {
		const uint64_t retainedTotal64 = IsSingleRoundManualReloadWeapon(weaponType)
			? static_cast<uint64_t>(ammoInClip) + weaponState.hiddenReserve
			: (ammoInClip == std::numeric_limits<uint32_t>::max()
				? ammoInClip : static_cast<uint64_t>(ammoInClip) + RetentionAmmo);
		const uint32_t retainedTotal = static_cast<uint32_t>((std::min)(
			retainedTotal64, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));
		if (ammoTotal != retainedTotal) {
			std::vector<uint8_t> visibleTotal;
			AppendU32(visibleTotal, retainedTotal);
			WriteProcessBytes(weaponEntry + AmmoTotalOffset, visibleTotal);
		}
		weaponState.lastVisibleTotal = retainedTotal;
	}

	if (settingsManager->debugMod && weaponState.emptyLatched) {
		uevr::API::get()->log_info("[ManualReload] holding empty weapon type=%u hidden=%u", weaponType, weaponState.hiddenReserve);
	}

	if (newShotCapture) {
		uevr::API::get()->log_info("[ManualReload] captured live weapon entry type=%u preShotClip=%u clip=%u hidden=%u empty=%s",
			weaponType, preShotClip, ammoInClip, weaponState.hiddenReserve,
			weaponState.emptyLatched ? "true" : "false");
		lastReportedManualReloadCaptureSequence = captureSequence;
	}
}

void MemoryManager::RestoreManualReloadState() {
	if (manualReloadStageActive && manualReloadStageWeaponType < manualReloadWeapons.size()) {
		ManualReloadWeaponState& weaponState = manualReloadWeapons[manualReloadStageWeaponType];
		const uint64_t restoredReserve =
			static_cast<uint64_t>(weaponState.hiddenReserve) + manualReloadStageConsumedReserve;
		weaponState.hiddenReserve = static_cast<uint32_t>((std::min)(
			restoredReserve, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));

		uint32_t stagedWeaponType = 0;
		if (TryRead(manualReloadStageEntry, stagedWeaponType) && stagedWeaponType == manualReloadStageWeaponType) {
			const uint64_t restoredTotal =
				static_cast<uint64_t>(manualReloadStageOriginalClip) + weaponState.hiddenReserve;
			std::vector<uint8_t> restoredMagazine;
			AppendU32(restoredMagazine, manualReloadStageOriginalClip);
			AppendU32(restoredMagazine, static_cast<uint32_t>((std::min)(
				restoredTotal, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))));
			WriteProcessBytes(manualReloadStageEntry + 0x08, restoredMagazine);
		}
	}

	for (size_t weaponIndex = 0; weaponIndex < manualReloadWeapons.size(); ++weaponIndex) {
		ManualReloadWeaponState& weaponState = manualReloadWeapons[weaponIndex];
		if (!weaponState.initialized || weaponState.weaponEntry == 0 ||
			(manualReloadStageActive && weaponIndex == manualReloadStageWeaponType)) {
			continue;
		}

		uint32_t weaponType = 0;
		uint32_t weaponStateValue = 0;
		uint32_t ammoInClip = 0;
		if (!TryRead(weaponState.weaponEntry, weaponType) ||
			!TryRead(weaponState.weaponEntry + 0x04, weaponStateValue) ||
			!TryRead(weaponState.weaponEntry + 0x08, ammoInClip) ||
			weaponType != weaponState.weaponType) {
			continue;
		}

		const uint64_t restoredTotal = static_cast<uint64_t>(ammoInClip) + weaponState.hiddenReserve;
		const uint32_t restoredTotalValue = static_cast<uint32_t>((std::min)(
			restoredTotal, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));
		if (weaponState.emptyLatched) {
			std::vector<uint8_t> restoredWeapon;
			AppendU32(restoredWeapon, restoredTotalValue > 0 ? 0 : weaponStateValue);
			AppendU32(restoredWeapon, ammoInClip);
			AppendU32(restoredWeapon, restoredTotalValue);
			WriteProcessBytes(weaponState.weaponEntry + 0x04, restoredWeapon);
		}
		else {
			std::vector<uint8_t> visibleTotal;
			AppendU32(visibleTotal, restoredTotalValue);
			WriteProcessBytes(weaponState.weaponEntry + 0x0C, visibleTotal);
		}
	}

	manualReloadWeapons = {};
	InterlockedExchange64(&manualReloadCapturedWeaponEntry, 0);
	InterlockedExchange(&manualReloadCapturedWeaponType, 0);
	InterlockedExchange(&manualReloadCapturedPreShotClip, 0);
	InterlockedExchange(&manualReloadCaptureSequence, 0);
	lastReportedManualReloadCaptureSequence = 0;
	manualReloadStageActive = false;
	manualReloadStageEntry = 0;
	manualReloadStageWeaponType = 0;
	manualReloadStageOriginalWeaponState = 0;
	manualReloadStageOriginalClip = 0;
	manualReloadStageTargetClip = 0;
	manualReloadStageConsumedReserve = 0;
	manualReloadStageStartedAt = 0;
	if (manualReloadEmptyStateKnown && manualReloadEmptyStateReported)
		ReportManualReloadEmptyState(false);
	manualReloadEmptyStateKnown = false;
	manualReloadEmptyStateReported = false;
}

int MemoryManager::ResolveCurrentVehicleModelId(int vehicleType) {
	UpdateCombatAssistPlayerPointer();

	const uintptr_t playerPed = cachedPlayerPointer;
	if (playerPed == 0 || !IsReadableMemory(playerPed, 0x400))
		return -1;

	auto logResolvedVehicle = [&](uintptr_t vehiclePointer, size_t vehiclePointerOffset, int modelId, size_t modelOffset) {
		static uintptr_t lastLoggedVehiclePointer = 0;
		static int lastLoggedModelId = -1;
		if (vehiclePointer == lastLoggedVehiclePointer && modelId == lastLoggedModelId)
			return;

		lastLoggedVehiclePointer = vehiclePointer;
		lastLoggedModelId = modelId;
		uevr::API::get()->log_info("[CameraProfiles] vehicle model=%d ped+0x%llX -> 0x%llX model+0x%llX",
			modelId,
			static_cast<unsigned long long>(vehiclePointerOffset),
			static_cast<unsigned long long>(vehiclePointer),
			static_cast<unsigned long long>(modelOffset));
	};

	// Once the layout has been proven, resolving a new car is two guarded reads.
	// The vehicle backlink is checked only when the vehicle pointer changes.
	if (vehicleLayoutCacheValid) {
		uintptr_t vehiclePointer = 0;
		if (TryRead(playerPed + cachedVehiclePointerOffset, vehiclePointer) &&
			vehiclePointer != 0 && vehiclePointer != playerPed && IsReadableMemory(vehiclePointer, 0x200)) {
			const bool pointerChanged = vehiclePointer != cachedResolvedVehiclePointer;
			if (!pointerChanged || MemoryBlockContainsPointer(vehiclePointer, 0x1400, playerPed)) {
				uint16_t modelValue = 0;
				if (TryRead(vehiclePointer + cachedVehicleModelOffset, modelValue)) {
					const int modelId = static_cast<int>(modelValue);
					if (IsKnownVehicleModelForType(modelId, vehicleType)) {
						cachedResolvedVehiclePointer = vehiclePointer;
						logResolvedVehicle(vehiclePointer, cachedVehiclePointerOffset, modelId, cachedVehicleModelOffset);
						return modelId;
					}
				}
			}
		}
	}

	// The fallback discovery scan is intentionally rare. It learns the DE layout
	// once, after which normal vehicle changes stay on the fast path above.
	const ULONGLONG now = GetTickCount64();
	if (now - lastVehicleLayoutScanTime < 1000)
		return -1;
	lastVehicleLayoutScanTime = now;

	for (size_t vehiclePointerOffset = 0x400; vehiclePointerOffset < 0x1400; vehiclePointerOffset += sizeof(uint32_t)) {
		uintptr_t vehiclePointer = 0;
		if (!TryRead(playerPed + vehiclePointerOffset, vehiclePointer))
			continue;
		if (vehiclePointer == 0 || vehiclePointer == playerPed || !IsReadableMemory(vehiclePointer, 0x200))
			continue;

		// The real current vehicle should point back to the current player ped as driver/passenger.
		if (!MemoryBlockContainsPointer(vehiclePointer, 0x1400, playerPed))
			continue;

		size_t modelOffset = 0;
		const int modelId = FindVehicleModelIdInObject(vehiclePointer, vehicleType, modelOffset);
		if (modelId < 0)
			continue;

		vehicleLayoutCacheValid = true;
		cachedVehiclePointerOffset = vehiclePointerOffset;
		cachedVehicleModelOffset = modelOffset;
		cachedResolvedVehiclePointer = vehiclePointer;
		logResolvedVehicle(vehiclePointer, vehiclePointerOffset, modelId, modelOffset);

		return modelId;
	}

	return -1;
}

bool MemoryManager::ResolveCombatAssistWeaponInfo() {
	if (combatAssistWeaponInfoResolved)
		return true;
	if (combatAssistWeaponInfoResolveAttempted)
		return false;

	combatAssistWeaponInfoResolveAttempted = true;

	const uintptr_t getWeaponInfoPattern = FindPattern({
		0xB8, 0x2F, 0x00, 0x00, 0x00, 0x84, 0xD2, 0x75, -1,
		0x8D, 0x41, 0x19, 0x48, 0x0F, 0xBF, 0xC0,
		0x48, 0x8D, 0x0D, -1, -1, -1, -1,
		0x48, 0x6B, 0xC0, 0x70, 0x48, 0x03, 0xC1, 0xC3
	});

	if (getWeaponInfoPattern == 0) {
		uevr::API::get()->log_error("%s", "[CombatAssist] CWeaponInfo signature not found; weapon table values were not raised");
		return false;
	}

	const uintptr_t leaAddress = getWeaponInfoPattern + 0x10;
	const uintptr_t weaponInfoTable = leaAddress + 0x07 + ReadI32(leaAddress + 0x03);
	const uintptr_t moduleSize = GetModuleSize();
	if (weaponInfoTable <= baseAddressGameEXE || weaponInfoTable + (WeaponInfoCount * WeaponInfoSize) > baseAddressGameEXE + moduleSize) {
		uevr::API::get()->log_error("[CombatAssist] CWeaponInfo table looked invalid: 0x%llX", static_cast<unsigned long long>(weaponInfoTable));
		return false;
	}

	combatAssistWeaponInfoOffset = weaponInfoTable - baseAddressGameEXE;
	combatAssistWeaponInfoResolved = true;
	uevr::API::get()->log_info("[CombatAssist] CWeaponInfo table offset: 0x%llX", static_cast<unsigned long long>(combatAssistWeaponInfoOffset));
	return true;
}

bool MemoryManager::IsCombatAssistWeaponInfoReady() {
	if (!ResolveCombatAssistWeaponInfo() || combatAssistWeaponInfoOffset == 0)
		return false;
	if (combatAssistWeaponInfoReady)
		return true;

	const uintptr_t weaponInfoTable = baseAddressGameEXE + combatAssistWeaponInfoOffset;
	int plausibleInstantHitEntries = 0;
	for (size_t i = 0; i < WeaponInfoCount; ++i) {
		const uintptr_t entry = weaponInfoTable + (i * WeaponInfoSize);
		const uint32_t fireType = *reinterpret_cast<uint32_t*>(entry + WeaponInfoFireTypeOffset);
		const float targetRange = *reinterpret_cast<float*>(entry + WeaponInfoTargetRangeOffset);
		const float weaponRange = *reinterpret_cast<float*>(entry + WeaponInfoWeaponRangeOffset);
		const int32_t modelId = *reinterpret_cast<int32_t*>(entry + WeaponInfoModelIdOffset);
		const int32_t modelId2 = *reinterpret_cast<int32_t*>(entry + WeaponInfoModelId2Offset);
		const uint16_t ammoClip = *reinterpret_cast<uint16_t*>(entry + WeaponInfoAmmoClipOffset);
		const uint16_t damage = *reinterpret_cast<uint16_t*>(entry + WeaponInfoDamageOffset);
		const float accuracy = *reinterpret_cast<float*>(entry + WeaponInfoAccuracyOffset);
		const float spread = *reinterpret_cast<float*>(entry + WeaponInfoSpreadOffset);
		const bool hasWeaponModel = modelId > 0 || modelId2 > 0;
		const bool plausibleValues = std::isfinite(targetRange) && targetRange > 0.0f
			&& std::isfinite(weaponRange) && weaponRange > 0.0f
			&& ammoClip > 0 && damage > 0
			&& std::isfinite(accuracy) && accuracy >= 0.0f
			&& std::isfinite(spread) && spread >= 0.0f;

		if (fireType == WeaponFireInstantHit && hasWeaponModel && plausibleValues)
			++plausibleInstantHitEntries;
	}

	// The table contains several skill-level variants of ordinary firearms. A small
	// quorum avoids mistaking the zero-filled pre-load storage for initialized data.
	if (plausibleInstantHitEntries < 6)
		return false;

	combatAssistWeaponInfoReady = true;
	uevr::API::get()->log_info("[CombatAssist] CWeaponInfo table ready with %d plausible instant-hit entries", plausibleInstantHitEntries);
	return true;
}

void MemoryManager::ApplyCombatAssistWeaponInfoValues() {
	if (!IsCombatAssistWeaponInfoReady()) {
		if (!combatAssistWeaponInfoWaitLogged) {
			combatAssistWeaponInfoWaitLogged = true;
			uevr::API::get()->log_info("%s", "[CombatAssist] CWeaponInfo table is not populated yet; deferred retry enabled");
		}
		return;
	}

	uintptr_t weaponInfoTable = baseAddressGameEXE + combatAssistWeaponInfoOffset;
	int patchedEntries = 0;
	for (size_t i = 0; i < WeaponInfoCount; ++i) {
		const uintptr_t entry = weaponInfoTable + (i * WeaponInfoSize);
		const uint32_t fireType = *reinterpret_cast<uint32_t*>(entry + WeaponInfoFireTypeOffset);
		const int32_t modelId = *reinterpret_cast<int32_t*>(entry + WeaponInfoModelIdOffset);
		const int32_t modelId2 = *reinterpret_cast<int32_t*>(entry + WeaponInfoModelId2Offset);
		const bool instantHit = fireType == WeaponFireInstantHit;
		const bool hasWeaponModel = modelId > 0 || modelId2 > 0;
		const bool noSpreadTarget = settingsManager->enableWeaponNoSpread && instantHit && hasWeaponModel;

		if (!instantHit && !noSpreadTarget)
			continue;

		auto& original = combatAssistOriginalWeaponInfoValues[i];
		if (!original.captured) {
			original.captured = true;
			original.instantHit = instantHit;
			original.targetRange = *reinterpret_cast<float*>(entry + WeaponInfoTargetRangeOffset);
			original.weaponRange = *reinterpret_cast<float*>(entry + WeaponInfoWeaponRangeOffset);
			original.ammoClip = *reinterpret_cast<uint16_t*>(entry + WeaponInfoAmmoClipOffset);
			original.damage = *reinterpret_cast<uint16_t*>(entry + WeaponInfoDamageOffset);
			original.accuracy = *reinterpret_cast<float*>(entry + WeaponInfoAccuracyOffset);
			original.spread = *reinterpret_cast<float*>(entry + WeaponInfoSpreadOffset);
			original.animLoopFire = *reinterpret_cast<float*>(entry + WeaponInfoAnimLoopFireOffset);
			original.animLoop2Fire = *reinterpret_cast<float*>(entry + WeaponInfoAnimLoop2FireOffset);
		}

		auto* targetRange = reinterpret_cast<float*>(entry + WeaponInfoTargetRangeOffset);
		auto* weaponRange = reinterpret_cast<float*>(entry + WeaponInfoWeaponRangeOffset);
		auto* damage = reinterpret_cast<uint16_t*>(entry + WeaponInfoDamageOffset);
		auto* animLoopStart = reinterpret_cast<float*>(entry + WeaponInfoAnimLoopStartOffset);
		auto* animLoopEnd = reinterpret_cast<float*>(entry + WeaponInfoAnimLoopEndOffset);
		auto* animLoopFire = reinterpret_cast<float*>(entry + WeaponInfoAnimLoopFireOffset);
		auto* animLoop2Start = reinterpret_cast<float*>(entry + WeaponInfoAnimLoop2StartOffset);
		auto* animLoop2End = reinterpret_cast<float*>(entry + WeaponInfoAnimLoop2EndOffset);
		auto* animLoop2Fire = reinterpret_cast<float*>(entry + WeaponInfoAnimLoop2FireOffset);
		auto* accuracy = reinterpret_cast<float*>(entry + WeaponInfoAccuracyOffset);
		auto* spread = reinterpret_cast<float*>(entry + WeaponInfoSpreadOffset);

		if (instantHit) {
			*targetRange = original.targetRange;
			*weaponRange = ExpectedRangeValue(original.weaponRange, EnhancedWeaponRange);
			*damage = ExpectedDamageValue(original.damage, settingsManager->enableCombatAssistDamage);
			if (settingsManager->enableCombatAssistAnimationTiming) {
				*animLoopFire = EarlierFireTime(*animLoopStart, *animLoopEnd, *animLoopFire);
				*animLoop2Fire = EarlierFireTime(*animLoop2Start, *animLoop2End, *animLoop2Fire);
			} else {
				*animLoopFire = original.animLoopFire;
				*animLoop2Fire = original.animLoop2Fire;
			}
		}

		if (noSpreadTarget) {
			*accuracy = NoSpreadWeaponAccuracy;
			*spread = 0.0f;
		}
		++patchedEntries;
	}

	if (patchedEntries <= 0) {
		uevr::API::get()->log_warn("%s", "[CombatAssist] CWeaponInfo was ready but no instant-hit entries matched; no values changed");
		return;
	}

	if (!combatAssistWeaponInfoPatched) {
		combatAssistWeaponInfoPatched = true;
		uevr::API::get()->log_info("[CombatAssist] Updated CWeaponInfo values for %d weapon entries; damage=%s animationTiming=%s weaponNoSpread=%s targetRange=vanilla weaponRange=%.0f",
			patchedEntries,
			settingsManager->enableCombatAssistDamage ? "true" : "false",
			settingsManager->enableCombatAssistAnimationTiming ? "true" : "false",
			settingsManager->enableWeaponNoSpread ? "true" : "false",
			EnhancedWeaponRange);
	}
}

void MemoryManager::VerifyCombatAssistWeaponInfoValues() {
	if (!combatAssistWeaponInfoPatched || combatAssistWeaponInfoOffset == 0)
		return;

	const ULONGLONG now = GetTickCount64();
	if (now - lastCombatAssistWeaponInfoVerifyTime < WeaponInfoVerifyIntervalMs)
		return;
	lastCombatAssistWeaponInfoVerifyTime = now;

	uintptr_t weaponInfoTable = baseAddressGameEXE + combatAssistWeaponInfoOffset;
	int overwrittenFields = 0;
	for (size_t i = 0; i < WeaponInfoCount; ++i) {
		const auto& original = combatAssistOriginalWeaponInfoValues[i];
		if (!original.captured)
			continue;

		const uintptr_t entry = weaponInfoTable + (i * WeaponInfoSize);
		const int32_t modelId = *reinterpret_cast<int32_t*>(entry + WeaponInfoModelIdOffset);
		const int32_t modelId2 = *reinterpret_cast<int32_t*>(entry + WeaponInfoModelId2Offset);
		const bool hasWeaponModel = modelId > 0 || modelId2 > 0;
		const bool noSpreadTarget = settingsManager->enableWeaponNoSpread && original.instantHit && hasWeaponModel;

		auto logOverwrite = [&](const char* field, double current, double expected, double originalValue) {
			++overwrittenFields;
			if (now - lastCombatAssistWeaponInfoOverwriteLogTime >= WeaponInfoOverwriteLogCooldownMs) {
				lastCombatAssistWeaponInfoOverwriteLogTime = now;
				uevr::API::get()->log_warn(
					"[CombatAssist] Game overwrote runtime weapon stat: index=%llu model=%d/%d field=%s current=%.4f expected=%.4f original=%.4f; reapplying",
					static_cast<unsigned long long>(i),
					modelId,
					modelId2,
					field,
					current,
					expected,
					originalValue);
			}
		};

		if (original.instantHit) {
			const float expectedTargetRange = original.targetRange;
			const float expectedWeaponRange = ExpectedRangeValue(original.weaponRange, EnhancedWeaponRange);
			const uint16_t expectedDamage = ExpectedDamageValue(original.damage, settingsManager->enableCombatAssistDamage);
			const float expectedAnimLoopFire = settingsManager->enableCombatAssistAnimationTiming
				? EarlierFireTime(*reinterpret_cast<float*>(entry + WeaponInfoAnimLoopStartOffset),
					*reinterpret_cast<float*>(entry + WeaponInfoAnimLoopEndOffset),
					original.animLoopFire)
				: original.animLoopFire;
			const float expectedAnimLoop2Fire = settingsManager->enableCombatAssistAnimationTiming
				? EarlierFireTime(*reinterpret_cast<float*>(entry + WeaponInfoAnimLoop2StartOffset),
					*reinterpret_cast<float*>(entry + WeaponInfoAnimLoop2EndOffset),
					original.animLoop2Fire)
				: original.animLoop2Fire;

			auto* targetRange = reinterpret_cast<float*>(entry + WeaponInfoTargetRangeOffset);
			auto* weaponRange = reinterpret_cast<float*>(entry + WeaponInfoWeaponRangeOffset);
			auto* damage = reinterpret_cast<uint16_t*>(entry + WeaponInfoDamageOffset);
			auto* animLoopFire = reinterpret_cast<float*>(entry + WeaponInfoAnimLoopFireOffset);
			auto* animLoop2Fire = reinterpret_cast<float*>(entry + WeaponInfoAnimLoop2FireOffset);

			if (FloatChanged(*targetRange, expectedTargetRange)) {
				logOverwrite("targetRange", *targetRange, expectedTargetRange, original.targetRange);
				*targetRange = expectedTargetRange;
			}
			if (FloatChanged(*weaponRange, expectedWeaponRange)) {
				logOverwrite("weaponRange", *weaponRange, expectedWeaponRange, original.weaponRange);
				*weaponRange = expectedWeaponRange;
			}
			if (*damage != expectedDamage) {
				logOverwrite("damage", *damage, expectedDamage, original.damage);
				*damage = expectedDamage;
			}
			if (FloatChanged(*animLoopFire, expectedAnimLoopFire)) {
				logOverwrite("animLoopFire", *animLoopFire, expectedAnimLoopFire, original.animLoopFire);
				*animLoopFire = expectedAnimLoopFire;
			}
			if (FloatChanged(*animLoop2Fire, expectedAnimLoop2Fire)) {
				logOverwrite("animLoop2Fire", *animLoop2Fire, expectedAnimLoop2Fire, original.animLoop2Fire);
				*animLoop2Fire = expectedAnimLoop2Fire;
			}
		}

		if (noSpreadTarget) {
			auto* accuracy = reinterpret_cast<float*>(entry + WeaponInfoAccuracyOffset);
			auto* spread = reinterpret_cast<float*>(entry + WeaponInfoSpreadOffset);
			if (FloatChanged(*accuracy, NoSpreadWeaponAccuracy)) {
				logOverwrite("accuracy", *accuracy, NoSpreadWeaponAccuracy, original.accuracy);
				*accuracy = NoSpreadWeaponAccuracy;
			}
			if (FloatChanged(*spread, 0.0f)) {
				logOverwrite("spread", *spread, 0.0f, original.spread);
				*spread = 0.0f;
			}
		}
	}

	if (overwrittenFields > 0) {
		uevr::API::get()->log_warn("[CombatAssist] Reapplied %d overwritten runtime weapon stat field(s)", overwrittenFields);
	}
}

void MemoryManager::RestoreCombatAssistWeaponInfoValues() {
	if (!combatAssistWeaponInfoPatched || combatAssistWeaponInfoOffset == 0)
		return;

	uintptr_t weaponInfoTable = baseAddressGameEXE + combatAssistWeaponInfoOffset;
	for (size_t i = 0; i < WeaponInfoCount; ++i) {
		const auto& original = combatAssistOriginalWeaponInfoValues[i];
		if (!original.captured)
			continue;

		const uintptr_t entry = weaponInfoTable + (i * WeaponInfoSize);
		*reinterpret_cast<float*>(entry + WeaponInfoTargetRangeOffset) = original.targetRange;
		*reinterpret_cast<float*>(entry + WeaponInfoWeaponRangeOffset) = original.weaponRange;
		*reinterpret_cast<uint16_t*>(entry + WeaponInfoAmmoClipOffset) = original.ammoClip;
		*reinterpret_cast<uint16_t*>(entry + WeaponInfoDamageOffset) = original.damage;
		*reinterpret_cast<float*>(entry + WeaponInfoAccuracyOffset) = original.accuracy;
		*reinterpret_cast<float*>(entry + WeaponInfoSpreadOffset) = original.spread;
		*reinterpret_cast<float*>(entry + WeaponInfoAnimLoopFireOffset) = original.animLoopFire;
		*reinterpret_cast<float*>(entry + WeaponInfoAnimLoop2FireOffset) = original.animLoop2Fire;
	}

	combatAssistWeaponInfoPatched = false;
	combatAssistWeaponInfoWaitLogged = false;
	lastCombatAssistWeaponInfoVerifyTime = 0;
	lastCombatAssistWeaponInfoOverwriteLogTime = 0;
}

void MemoryManager::RefreshCombatAssistWeaponInfoValues() {
	RestoreCombatAssistWeaponInfoValues();
	ApplyCombatAssistWeaponInfoValues();
}

bool MemoryManager::ReadWeaponInfoDebugSnapshot(int modelId, WeaponInfoDebugSnapshot& snapshot) {
	snapshot = WeaponInfoDebugSnapshot{};
	snapshot.modelId = modelId;

	if (modelId <= 0 || !ResolveCombatAssistWeaponInfo())
		return false;

	const uintptr_t weaponInfoTable = baseAddressGameEXE + combatAssistWeaponInfoOffset;
	if (weaponInfoTable == 0)
		return false;

	float minSpread = std::numeric_limits<float>::max();
	float maxSpread = std::numeric_limits<float>::lowest();
	float minAccuracy = std::numeric_limits<float>::max();
	float maxAccuracy = std::numeric_limits<float>::lowest();

	for (size_t i = 0; i < WeaponInfoCount; ++i) {
		const uintptr_t entry = weaponInfoTable + (i * WeaponInfoSize);
		const int32_t entryModelId = *reinterpret_cast<int32_t*>(entry + WeaponInfoModelIdOffset);
		const int32_t entryModelId2 = *reinterpret_cast<int32_t*>(entry + WeaponInfoModelId2Offset);
		if (entryModelId != modelId && entryModelId2 != modelId)
			continue;

		const float spread = *reinterpret_cast<float*>(entry + WeaponInfoSpreadOffset);
		const float accuracy = *reinterpret_cast<float*>(entry + WeaponInfoAccuracyOffset);
		if (std::isfinite(spread)) {
			minSpread = (std::min)(minSpread, spread);
			maxSpread = (std::max)(maxSpread, spread);
		}
		if (std::isfinite(accuracy)) {
			minAccuracy = (std::min)(minAccuracy, accuracy);
			maxAccuracy = (std::max)(maxAccuracy, accuracy);
		}

		if (!snapshot.found) {
			snapshot.found = true;
			snapshot.fireType = *reinterpret_cast<uint32_t*>(entry + WeaponInfoFireTypeOffset);
			snapshot.targetRange = *reinterpret_cast<float*>(entry + WeaponInfoTargetRangeOffset);
			snapshot.weaponRange = *reinterpret_cast<float*>(entry + WeaponInfoWeaponRangeOffset);
			snapshot.damage = *reinterpret_cast<uint16_t*>(entry + WeaponInfoDamageOffset);
		}
		++snapshot.matchingEntries;
	}

	if (!snapshot.found)
		return false;

	snapshot.minSpread = minSpread == std::numeric_limits<float>::max() ? 0.0f : minSpread;
	snapshot.maxSpread = maxSpread == std::numeric_limits<float>::lowest() ? 0.0f : maxSpread;
	snapshot.minAccuracy = minAccuracy == std::numeric_limits<float>::max() ? 0.0f : minAccuracy;
	snapshot.maxAccuracy = maxAccuracy == std::numeric_limits<float>::lowest() ? 0.0f : maxAccuracy;
	return true;
}

void MemoryManager::GetNativeShotSpreadBypassCounts(uint32_t& pathA, uint32_t& pathB) const {
	pathA = static_cast<uint32_t>(nativeShotSpreadBypassCountA);
	pathB = static_cast<uint32_t>(nativeShotSpreadBypassCountB);
}

bool MemoryManager::QueryNativeLineOfSightEntity(const std::array<float, 3>& start,
	const std::array<float, 3>& end, NativeMeleeContact& contact, bool requireEntity)
{
	contact = NativeMeleeContact{};
	if (InterlockedCompareExchange(&nativeLineTraceContactDisabled, 0, 0) != 0)
		return false;

	auto disableNativeContact = [this](const char* reason) {
		InterlockedExchange(&nativeLineTraceContactDisabled, 1);
		if (!nativeLineTraceContactDisabledLogged)
		{
			nativeLineTraceContactDisabledLogged = true;
			uevr::API::get()->log_error(
				"[MotionMelee] native LOS contact disabled for session reason=%s",
				reason == nullptr ? "unknown" : reason);
		}
	};

	for (const float value : start)
		if (!std::isfinite(value))
			return false;
	for (const float value : end)
		if (!std::isfinite(value))
			return false;

	if (baseAddressGameEXE == 0)
		return false;

	struct NativeVector { float x, y, z; };
	// RVA 0x13F89A0 normalizes arguments 5-13 before tail-jumping to the core
	// LOS routine, but it copies argument 14 from the caller. Declaring all 14
	// arguments gives Win64 the required outgoing stack area and lets us provide
	// that final value explicitly instead of inheriting arbitrary caller data.
	using NativeLineOfSight = bool(__fastcall*)(const NativeVector*, const NativeVector*,
		void*, uintptr_t*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
		uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
	alignas(16) std::array<uint8_t, 0x100> hitResult{};
	const NativeVector nativeStart{ start[0], start[1], start[2] };
	const NativeVector nativeEnd{ end[0], end[1], end[2] };
	const float dx = nativeEnd.x - nativeStart.x;
	const float dy = nativeEnd.y - nativeStart.y;
	const float dz = nativeEnd.z - nativeStart.z;
	const float lengthSquared = dx * dx + dy * dy + dz * dz;
	if (!std::isfinite(lengthSquared) || lengthSquared < 0.00000001f || lengthSquared > 9.0f)
		return false;

	if (baseAddressGameEXE > (std::numeric_limits<uintptr_t>::max)() - NativeLineOfSightRva)
	{
		disableNativeContact("LOS address overflow");
		return false;
	}
	const uintptr_t lineOfSightAddress = baseAddressGameEXE + NativeLineOfSightRva;
	const std::array<uint8_t, 8> lineOfSightSignature{
		0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48 };
	bool validLineOfSightEntry = IsReadableMemory(
		lineOfSightAddress, lineOfSightSignature.size())
		&& std::memcmp(reinterpret_cast<const void*>(lineOfSightAddress),
			lineOfSightSignature.data(), lineOfSightSignature.size()) == 0;
	if (!validLineOfSightEntry)
	{
		// Combat-assist installs its own verified E9 detour at this same native
		// entry. Accept only the exact jump to the registered live code cave;
		// an arbitrary or stale detour must still fail closed.
		for (const auto& patch : combatAssistPatches)
		{
			if (!patch.applied || patch.address != lineOfSightAddress
				|| patch.codeCave == nullptr || patch.overwriteSize != 7)
				continue;
			int32_t displacement = 0;
			__try
			{
				if (*reinterpret_cast<const uint8_t*>(lineOfSightAddress) != 0xE9
					|| *reinterpret_cast<const uint8_t*>(lineOfSightAddress + 5) != 0x90
					|| *reinterpret_cast<const uint8_t*>(lineOfSightAddress + 6) != 0x90)
					continue;
				std::memcpy(&displacement,
					reinterpret_cast<const void*>(lineOfSightAddress + 1), sizeof(displacement));
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				continue;
			}
			const uintptr_t detourTarget = static_cast<uintptr_t>(
				static_cast<int64_t>(lineOfSightAddress + 5) + displacement);
			if (detourTarget == reinterpret_cast<uintptr_t>(patch.codeCave))
			{
				validLineOfSightEntry = true;
				break;
			}
		}
	}
	if (!validLineOfSightEntry)
	{
		disableNativeContact("LOS entry signature mismatch");
		return false;
	}
	auto lineOfSight = reinterpret_cast<NativeLineOfSight>(lineOfSightAddress);
	UpdateCombatAssistPlayerPointer();
	const uintptr_t localPlayerPed = cachedPlayerPointer;
	NativeVector hitPoint{};
	uintptr_t hitEntity = 0;
	bool hit = false;
	if (localPlayerPed == 0
		|| baseAddressGameEXE > (std::numeric_limits<uintptr_t>::max)()
			- NativeLineOfSightIgnoreEntityRva)
		return false;
	const uintptr_t ignoreEntityAddress = baseAddressGameEXE
		+ NativeLineOfSightIgnoreEntityRva;
	if (!IsWritableMemory(ignoreEntityAddress, sizeof(uintptr_t)))
	{
		disableNativeContact("LOS ignore-entity global unavailable");
		return false;
	}

	// Match GTA's own weapon path: exclude the local player only for this
	// synchronous engine-thread query, then restore the prior world setting.
	uintptr_t previousIgnoreEntity = 0;
	bool ignoreEntityOverridden = false;
	hitResult.fill(0);
	__try
	{
		previousIgnoreEntity = *reinterpret_cast<uintptr_t*>(ignoreEntityAddress);
		*reinterpret_cast<uintptr_t*>(ignoreEntityAddress) = localPlayerPed;
		ignoreEntityOverridden = true;
		hit = lineOfSight(&nativeStart, &nativeEnd, hitResult.data(), &hitEntity,
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
		*reinterpret_cast<uintptr_t*>(ignoreEntityAddress) = previousIgnoreEntity;
		ignoreEntityOverridden = false;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		if (ignoreEntityOverridden)
		{
			__try
			{
				*reinterpret_cast<uintptr_t*>(ignoreEntityAddress) = previousIgnoreEntity;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
		disableNativeContact("native call or ignore-entity restore fault");
		return false;
	}
	if (!hit || hitEntity == localPlayerPed || (requireEntity && hitEntity == 0))
		return false;

	std::memcpy(&hitPoint, hitResult.data(), sizeof(hitPoint));
	if (!std::isfinite(hitPoint.x) || !std::isfinite(hitPoint.y)
		|| !std::isfinite(hitPoint.z))
		return false;
	if (hitEntity != 0 && hitEntity > (std::numeric_limits<uintptr_t>::max)()
		- NativeEntityTypeOffset - sizeof(uint8_t))
		return false;

	// DE's native CColPoint consumer reads the contact position at +0. The
	// vector at +0x10 is the collision normal and must not be used as a point.

	uint8_t entityType = 0;
	if (hitEntity != 0)
	{
		__try
		{
			if (!TryRead(hitEntity + NativeEntityTypeOffset, entityType))
				return false;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	contact.entity = hitEntity;
	contact.point = { hitPoint.x, hitPoint.y, hitPoint.z };
	std::memcpy(contact.nativeCollisionPoint.data(), hitResult.data(),
		contact.nativeCollisionPoint.size());
	contact.nativeCollisionPointValid = true;
	std::memcpy(contact.normal.data(), hitResult.data() + 0x10,
		sizeof(float) * contact.normal.size());
	const float normalLength = std::sqrt(
		contact.normal[0] * contact.normal[0]
		+ contact.normal[1] * contact.normal[1]
		+ contact.normal[2] * contact.normal[2]);
	if (!std::isfinite(normalLength) || normalLength <= 0.001f)
		contact.normal = { 0.0f, 0.0f, 1.0f };
	contact.piece = hitResult[NativeMeleeContactPieceOffset];
	contact.entityType = entityType & NativeEntityTypeMask;
	return true;
}

bool MemoryManager::ApplyNativeMeleeContactDamage(NativeMeleeContact& contact,
	int weaponType, int& appliedDamage)
{
	appliedDamage = 0;
	contact.damageResult = NativeMeleeDamageResult::Rejected;
	// The physical prototype is deliberately limited to unarmed/melee IDs.
	if (nativeMeleeDamageDisabled || baseAddressGameEXE == 0
		|| weaponType < 0 || weaponType > 15
		|| contact.entity == 0
		|| (contact.entityType != NativeEntityTypeVehicle
			&& contact.entityType != NativeEntityTypePed)
		|| !IsReadableMemory(contact.entity, 0x800))
		return false;
	if (contact.entity > (std::numeric_limits<uintptr_t>::max)()
		- NativeEntityTypeOffset - sizeof(uint8_t))
		return false;
	for (const float value : contact.point)
		if (!std::isfinite(value))
			return false;

	uint8_t currentEntityType = 0;
	__try
	{
		if (!TryRead(contact.entity + NativeEntityTypeOffset, currentEntityType))
			return false;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
	currentEntityType &= NativeEntityTypeMask;
	if (currentEntityType != contact.entityType)
		return false;

	UpdateCombatAssistPlayerPointer();
	const uintptr_t playerPed = cachedPlayerPointer;
	if (playerPed == 0 || playerPed == contact.entity
		|| !IsReadableMemory(playerPed, 0x1000))
		return false;
	if (!ResolveCombatAssistWeaponInfo() || combatAssistWeaponInfoOffset == 0)
		return false;

	const uintptr_t weaponInfoDelta = combatAssistWeaponInfoOffset
		+ (static_cast<uintptr_t>(weaponType) * WeaponInfoSize);
	if (weaponInfoDelta < combatAssistWeaponInfoOffset
		|| baseAddressGameEXE > (std::numeric_limits<uintptr_t>::max)() - weaponInfoDelta)
		return false;
	const uintptr_t weaponInfo = baseAddressGameEXE + weaponInfoDelta;
	if (!IsReadableMemory(weaponInfo, WeaponInfoSize))
		return false;
	// Melee entries intentionally leave CWeaponInfo::m_nDamage at zero. DE's
	// native GetStrikeDamage (RVA 0x12E0B50) instead indexes the selected
	// CMeleeInfo record and reads its per-move damage byte at +0x55. Physical
	// contact has no native fight animation move, so use the first strike value.
	uint8_t baseCombo = 0;
	__try
	{
		if (!TryRead(weaponInfo + WeaponInfoBaseMeleeComboOffset, baseCombo))
			return false;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
	if (baseCombo < FirstMeleeCombo
		|| baseCombo >= FirstMeleeCombo + MeleeComboCount)
		return false;
	const uintptr_t meleeInfoDelta = MeleeComboTableOffset
		+ (static_cast<uintptr_t>(baseCombo - FirstMeleeCombo) * MeleeComboInfoSize);
	if (meleeInfoDelta < MeleeComboTableOffset
		|| baseAddressGameEXE > (std::numeric_limits<uintptr_t>::max)() - meleeInfoDelta)
		return false;
	const uintptr_t meleeInfo = baseAddressGameEXE + meleeInfoDelta;
	if (!IsReadableMemory(meleeInfo, MeleeComboInfoSize))
		return false;
	uint8_t nativeDamage = 0;
	__try
	{
		if (!TryRead(meleeInfo + MeleeComboFirstStrikeDamageOffset, nativeDamage))
			return false;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
	if (nativeDamage == 0)
		return false;

	const std::array<uint8_t, 11> pedSignature{
		0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10 };
	const std::array<uint8_t, 11> vehicleSignature{
		0xF3, 0x0F, 0x11, 0x5C, 0x24, 0x20, 0x55, 0x53, 0x57, 0x41, 0x57 };
	const bool targetIsPed = contact.entityType == NativeEntityTypePed;
	const uintptr_t targetRva = targetIsPed ? NativePedDamageRva : NativeVehicleDamageRva;
	if (baseAddressGameEXE > (std::numeric_limits<uintptr_t>::max)() - targetRva)
		return false;
	const uintptr_t target = baseAddressGameEXE + targetRva;
	const std::array<uint8_t, 11>& expected = targetIsPed ? pedSignature : vehicleSignature;
	bool signatureMatches = false;
	__try
	{
		signatureMatches = IsReadableMemory(target, expected.size())
			&& std::memcmp(reinterpret_cast<const void*>(target), expected.data(), expected.size()) == 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		signatureMatches = false;
	}
	if (!signatureMatches)
	{
		nativeMeleeDamageDisabled = true;
		if (!nativeMeleeDamageFailureLogged)
		{
			nativeMeleeDamageFailureLogged = true;
			uevr::API::get()->log_error(
				"[MotionMelee] native damage signature mismatch type=%s(%u); direct contact damage disabled",
				NativeMeleeEntityTypeLabel(contact.entityType),
				static_cast<unsigned int>(contact.entityType));
		}
		return false;
	}

	if (targetIsPed)
	{
		// DE CWeapon::GenerateDamageEvent returns bool; only true is accepted/proven.
		using GenerateDamageEvent = bool(__fastcall*)(uintptr_t, uintptr_t,
			int32_t, int32_t, int32_t, uint8_t);
		const int32_t piece = contact.piece <= 7 ? contact.piece : 0;
		contact.damageResult = NativeMeleeDamageResult::Attempted;
		bool accepted = false;
		__try
		{
			auto generateDamageEvent = reinterpret_cast<GenerateDamageEvent>(target);
			accepted = generateDamageEvent(contact.entity, playerPed, weaponType,
				static_cast<int32_t>(nativeDamage), piece, 0);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			nativeMeleeDamageDisabled = true;
			if (!nativeMeleeDamageFailureLogged)
			{
				nativeMeleeDamageFailureLogged = true;
				uevr::API::get()->log_error(
					"[MotionMelee] native damage result=attempted type=%s(%u) call faulted; direct contact damage disabled",
					NativeMeleeEntityTypeLabel(contact.entityType),
					static_cast<unsigned int>(contact.entityType));
			}
			return false;
		}

		contact.damageResult = accepted
			? NativeMeleeDamageResult::Accepted
			: NativeMeleeDamageResult::Rejected;
		if (accepted)
			appliedDamage = static_cast<int>(nativeDamage);
		uevr::API::get()->log_info(
			"[MotionMelee] native damage result=%s type=%s(%u) damage=%d",
			NativeMeleeDamageResultLabel(contact.damageResult),
			NativeMeleeEntityTypeLabel(contact.entityType),
			static_cast<unsigned int>(contact.entityType), appliedDamage);
		return accepted;
	}

	// DE CVehicle::InflictDamage returns void. Verify acceptance by comparing its
	// proven health field before/after the synchronous native call. This still
	// does not claim the separate full CColPoint impact-FX path was executed.
	struct NativeVector { float x, y, z; };
	using InflictVehicleDamage = void(__fastcall*)(uintptr_t, uintptr_t,
		int32_t, float, const NativeVector*);
	const NativeVector point{ contact.point[0], contact.point[1], contact.point[2] };
	float healthBefore = 0.0f;
	float healthAfter = 0.0f;
	if (contact.entity > (std::numeric_limits<uintptr_t>::max)()
		- NativeVehicleHealthOffset - sizeof(float)
		|| !TryRead(contact.entity + NativeVehicleHealthOffset, healthBefore)
		|| !std::isfinite(healthBefore))
		return false;
	contact.targetHealthBefore = healthBefore;
	contact.damageResult = NativeMeleeDamageResult::Attempted;
	__try
	{
		auto inflictVehicleDamage = reinterpret_cast<InflictVehicleDamage>(target);
		inflictVehicleDamage(contact.entity, playerPed, weaponType,
			static_cast<float>(nativeDamage), &point);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		nativeMeleeDamageDisabled = true;
		if (!nativeMeleeDamageFailureLogged)
		{
			nativeMeleeDamageFailureLogged = true;
			uevr::API::get()->log_error(
				"[MotionMelee] native damage result=attempted type=%s(%u) call faulted; direct contact damage disabled",
				NativeMeleeEntityTypeLabel(contact.entityType),
				static_cast<unsigned int>(contact.entityType));
		}
		return false;
	}

	const bool readHealthAfter = TryRead(contact.entity + NativeVehicleHealthOffset, healthAfter)
		&& std::isfinite(healthAfter);
	if (readHealthAfter)
		contact.targetHealthAfter = healthAfter;
	if (readHealthAfter && healthAfter < healthBefore - 0.001f)
	{
		contact.damageResult = NativeMeleeDamageResult::Accepted;
		appliedDamage = static_cast<int>((std::max)(1.0f, healthBefore - healthAfter));
		uevr::API::get()->log_info(
			"[MotionMelee] native damage result=accepted type=vehicle(%u) damage=%d health=%.2f->%.2f impact_fx=unverified",
			static_cast<unsigned int>(contact.entityType), appliedDamage,
			healthBefore, healthAfter);
		return true;
	}

	contact.damageResult = NativeMeleeDamageResult::UnverifiedVehicleDispatch;
	uevr::API::get()->log_info(
		"[MotionMelee] native damage result=%s type=%s(%u) damage=0 health=%.2f->%.2f impact_fx=unverified",
		NativeMeleeDamageResultLabel(contact.damageResult),
		NativeMeleeEntityTypeLabel(contact.entityType),
		static_cast<unsigned int>(contact.entityType), healthBefore, healthAfter);
	return false;
}

bool MemoryManager::ApplyNativeThrowableImpactEvent(NativeMeleeContact& contact,
	int weaponType)
{
	contact.damageResult = NativeMeleeDamageResult::Rejected;
	if (nativeThrowableImpactDisabled || baseAddressGameEXE == 0
		|| weaponType != 18 || contact.entity == 0
		|| (contact.entityType != NativeEntityTypeWorld
			&& contact.entityType != NativeEntityTypeVehicle
			&& contact.entityType != NativeEntityTypePed)
		|| !contact.nativeCollisionPointValid
		|| !IsReadableMemory(contact.entity, 0x800))
		return false;

	UpdateCombatAssistPlayerPointer();
	const uintptr_t playerPed = cachedPlayerPointer;
	if (playerPed == 0 || playerPed == contact.entity
		|| !IsReadableMemory(playerPed, 0x1000))
		return false;

	// The dispatcher reads more than a weapon type from its CWeapon entry. The
	// player-fire prologue gives us that exact entry for a real native Molotov
	// trigger; prefer it over the older array guess. A type-only stub can reach
	// generic impact code but cannot reproduce native Molotov ownership/effects.
	constexpr size_t WeaponArrayOffset = 0x5A0;
	constexpr size_t WeaponStride = 0x1C;
	constexpr size_t SelectedSlotOffset = 0x718;
	constexpr uint8_t WeaponSlotCount = 13;
	uint8_t selectedSlot = 0;
	if (!TryRead(playerPed + SelectedSlotOffset, selectedSlot)
		|| selectedSlot >= WeaponSlotCount)
		return false;
	uintptr_t weaponEntry = 0;
	uint8_t resolvedSlot = selectedSlot;
	const char* entrySource = "none";
	const LONG captureSequenceBefore = InterlockedCompareExchange(
		&nativeThrowableLiveWeaponSequence, 0, 0);
	const uintptr_t capturedWeaponEntry = static_cast<uintptr_t>(
		InterlockedCompareExchange64(&nativeThrowableLiveWeaponEntry, 0, 0));
	uint32_t capturedWeaponType = 0;
	const bool capturedWeaponValid = captureSequenceBefore != 0
		&& capturedWeaponEntry != 0
		&& IsReadableMemory(capturedWeaponEntry, 0x10)
		&& TryRead(capturedWeaponEntry, capturedWeaponType)
		&& capturedWeaponType == static_cast<uint32_t>(weaponType)
		&& captureSequenceBefore == InterlockedCompareExchange(
			&nativeThrowableLiveWeaponSequence, 0, 0);
	if (capturedWeaponValid)
	{
		weaponEntry = capturedWeaponEntry;
		resolvedSlot = 0xFE;
		entrySource = "native-trigger";
	}
	else
	{
		for (uint8_t slot = 0; slot < WeaponSlotCount; ++slot)
		{
			const uintptr_t candidate = playerPed + WeaponArrayOffset
				+ (static_cast<size_t>(slot) * WeaponStride);
			if (!IsReadableMemory(candidate, 0x10))
				continue;
			uint32_t candidateType = 0;
			if (!TryRead(candidate, candidateType)
				|| candidateType != static_cast<uint32_t>(weaponType))
				continue;
			if (weaponEntry == 0 || slot == selectedSlot)
			{
				weaponEntry = candidate;
				resolvedSlot = slot;
				entrySource = "inventory-scan";
			}
			if (slot == selectedSlot)
				break;
		}
	}
	if (weaponEntry == 0)
	{
		uevr::API::get()->log_info(
			"[MotionThrowableImpact] native dispatcher skipped type=%d selectedSlot=%u reason=no-live-molotov-entry",
			weaponType, static_cast<unsigned int>(selectedSlot));
		return false;
	}

	if (baseAddressGameEXE > (std::numeric_limits<uintptr_t>::max)()
		- NativeThrowableImpactRva)
		return false;
	const uintptr_t target = baseAddressGameEXE + NativeThrowableImpactRva;
	const std::array<uint8_t, 16> signature{
		0x4C, 0x89, 0x4C, 0x24, 0x20, 0x55, 0x53, 0x57,
		0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
	bool signatureMatches = false;
	__try
	{
		signatureMatches = IsReadableMemory(target, signature.size())
			&& std::memcmp(reinterpret_cast<const void*>(target), signature.data(),
				signature.size()) == 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		signatureMatches = false;
	}
	if (!signatureMatches)
	{
		nativeThrowableImpactDisabled = true;
		if (!nativeThrowableImpactFailureLogged)
		{
			nativeThrowableImpactFailureLogged = true;
			uevr::API::get()->log_error(
				"[MotionThrowableImpact] native dispatcher signature mismatch at RVA 0x%llX; impact handoff disabled",
				static_cast<unsigned long long>(NativeThrowableImpactRva));
		}
		return false;
	}

	struct NativeVector { float x, y, z; };
	NativeVector impact{
		contact.point[0], contact.point[1], contact.point[2] };
	NativeVector normal{
		contact.normal[0], contact.normal[1], contact.normal[2] };
	const float normalLength = std::sqrt(
		normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
	if (!std::isfinite(normalLength) || normalLength <= 0.001f)
	{
		normal = { 0.0f, 0.0f, 1.0f };
	}
	else
	{
		normal.x /= normalLength;
		normal.y /= normalLength;
		normal.z /= normalLength;
	}
	const NativeVector origin{
		impact.x - normal.x * 0.05f,
		impact.y - normal.y * 0.05f,
		impact.z - normal.z * 0.05f };

	const bool hasHealth = contact.entityType == NativeEntityTypePed
		|| contact.entityType == NativeEntityTypeVehicle;
	const size_t healthOffset = contact.entityType == NativeEntityTypePed
		? NativePedHealthOffset : NativeVehicleHealthOffset;
	float healthBefore = -1.0f;
	float healthAfter = -1.0f;
	const bool healthAddressValid = hasHealth
		&& contact.entity <= (std::numeric_limits<uintptr_t>::max)()
		- healthOffset - sizeof(float);
	const bool healthBeforeValid = healthAddressValid
		&& TryRead(contact.entity + healthOffset, healthBefore)
		&& std::isfinite(healthBefore);
	if (healthBeforeValid)
		contact.targetHealthBefore = healthBefore;

	using NativeThrowableImpact = void(__fastcall*)(uintptr_t, uintptr_t, uintptr_t,
		const NativeVector*, const NativeVector*, void*, int32_t);
	contact.damageResult = NativeMeleeDamageResult::Attempted;
	bool callFaulted = false;
	__try
	{
		auto nativeImpact = reinterpret_cast<NativeThrowableImpact>(target);
		nativeImpact(weaponEntry, playerPed, contact.entity, &origin, &impact,
			contact.nativeCollisionPoint.data(), 1);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		callFaulted = true;
	}
	if (callFaulted)
	{
		nativeThrowableImpactDisabled = true;
		if (!nativeThrowableImpactFailureLogged)
		{
			nativeThrowableImpactFailureLogged = true;
			uevr::API::get()->log_error(
				"[MotionThrowableImpact] native dispatcher faulted; impact handoff disabled");
		}
		contact.damageResult = NativeMeleeDamageResult::Rejected;
		return false;
	}

	const bool healthAfterValid = healthAddressValid
		&& TryRead(contact.entity + healthOffset, healthAfter)
		&& std::isfinite(healthAfter);
	if (healthAfterValid)
		contact.targetHealthAfter = healthAfter;
	const bool damageAccepted = healthBeforeValid && healthAfterValid
		&& healthAfter < healthBefore - 0.001f;
	if (damageAccepted)
		contact.damageResult = NativeMeleeDamageResult::Accepted;
	uevr::API::get()->log_info(
		"[MotionThrowableImpact] native dispatcher called type=%s(%u) weapon=%d entrySource=%s selectedSlot=%u resolvedSlot=%u weaponEntry=0x%llX target=0x%llX accepted=%s health=%.2f->%.2f colpoint=preserved",
		NativeMeleeEntityTypeLabel(contact.entityType),
		static_cast<unsigned int>(contact.entityType), weaponType,
		entrySource,
		static_cast<unsigned int>(selectedSlot),
		static_cast<unsigned int>(resolvedSlot),
		static_cast<unsigned long long>(weaponEntry),
		static_cast<unsigned long long>(contact.entity),
		damageAccepted ? "true" : "false", contact.targetHealthBefore,
		contact.targetHealthAfter);
	return true;
}

bool MemoryManager::ApplyNativeThrowableExplosion(
	const std::array<float, 3>& impactPoint, int weaponType)
{
	const int explosionType = weaponType == NativeGrenadeWeaponType
		? NativeGrenadeExplosionType
		: (weaponType == NativeMolotovWeaponType ? NativeMolotovExplosionType : -1);
	if (explosionType < 0 || nativeThrowableExplosionDisabled || baseAddressGameEXE == 0
		|| !std::isfinite(impactPoint[0]) || !std::isfinite(impactPoint[1])
		|| !std::isfinite(impactPoint[2])
		|| baseAddressGameEXE > (std::numeric_limits<uintptr_t>::max)()
			- NativeAddExplosionRva)
		return false;

	UpdateCombatAssistPlayerPointer();
	const uintptr_t playerPed = cachedPlayerPointer;
	if (playerPed == 0 || !IsReadableMemory(playerPed, 0x1000))
		return false;

	const uintptr_t target = baseAddressGameEXE + NativeAddExplosionRva;
	bool signatureMatches = false;
	__try
	{
		signatureMatches = IsReadableMemory(target, NativeAddExplosionPrologue.size())
			&& std::memcmp(reinterpret_cast<const void*>(target),
				NativeAddExplosionPrologue.data(), NativeAddExplosionPrologue.size()) == 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		signatureMatches = false;
	}
	if (!signatureMatches)
	{
		nativeThrowableExplosionDisabled = true;
		if (!nativeThrowableExplosionFailureLogged)
		{
			nativeThrowableExplosionFailureLogged = true;
			uevr::API::get()->log_error(
				"[MotionThrowableImpact] native AddExplosion signature mismatch at RVA 0x%llX; throwable handoff disabled",
				static_cast<unsigned long long>(NativeAddExplosionRva));
		}
		return false;
	}

	struct NativeVector { float x, y, z; };
	const NativeVector nativeImpact{
		impactPoint[0], impactPoint[1], impactPoint[2] };
	using NativeAddExplosion = void(__fastcall*)(
		uintptr_t victim,
		uintptr_t creator,
		int32_t explosionType,
		const NativeVector* position,
		uint32_t lifetime,
		bool useSound,
		float cameraShake,
		bool invisible);

	bool callFaulted = false;
	__try
	{
		auto addExplosion = reinterpret_cast<NativeAddExplosion>(target);
		addExplosion(0, playerPed, explosionType, &nativeImpact,
			0, true, -1.0f, false);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		callFaulted = true;
	}
	if (callFaulted)
	{
		nativeThrowableExplosionDisabled = true;
		if (!nativeThrowableExplosionFailureLogged)
		{
			nativeThrowableExplosionFailureLogged = true;
			uevr::API::get()->log_error(
				"[MotionThrowableImpact] native AddExplosion faulted; throwable handoff disabled");
		}
		return false;
	}

	uevr::API::get()->log_info(
		"[MotionThrowableImpact] native AddExplosion accepted weapon=%d explosionType=%d creator=0x%llX point=(%.3f %.3f %.3f) lifecycle=de-native",
		weaponType, explosionType, static_cast<unsigned long long>(playerPed), nativeImpact.x,
		nativeImpact.y, nativeImpact.z);
	return true;
}

bool MemoryManager::ApplyNativeThrowableImpactDamage(NativeMeleeContact& contact,
	int weaponType, int& appliedDamage, int requestedDamage)
{
	appliedDamage = 0;
	contact.damageResult = NativeMeleeDamageResult::Rejected;
	if (nativeThrowableDamageDisabled || baseAddressGameEXE == 0
		|| weaponType != 18 || contact.entity == 0
		|| (contact.entityType != NativeEntityTypeVehicle
			&& contact.entityType != NativeEntityTypePed)
		|| !IsReadableMemory(contact.entity, 0x800))
		return false;
	if (contact.entity > (std::numeric_limits<uintptr_t>::max)()
		- NativeEntityTypeOffset - sizeof(uint8_t))
		return false;
	for (const float value : contact.point)
		if (!std::isfinite(value))
			return false;

	uint8_t currentEntityType = 0;
	__try
	{
		if (!TryRead(contact.entity + NativeEntityTypeOffset, currentEntityType))
			return false;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
	currentEntityType &= NativeEntityTypeMask;
	if (currentEntityType != contact.entityType)
		return false;

	UpdateCombatAssistPlayerPointer();
	const uintptr_t playerPed = cachedPlayerPointer;
	if (playerPed == 0 || playerPed == contact.entity
		|| !IsReadableMemory(playerPed, 0x1000))
		return false;
	if (!ResolveCombatAssistWeaponInfo() || combatAssistWeaponInfoOffset == 0)
		return false;
	const uintptr_t weaponInfoDelta = combatAssistWeaponInfoOffset
		+ (static_cast<uintptr_t>(weaponType) * WeaponInfoSize);
	if (weaponInfoDelta < combatAssistWeaponInfoOffset
		|| baseAddressGameEXE > (std::numeric_limits<uintptr_t>::max)()
			- weaponInfoDelta)
		return false;
	const uintptr_t weaponInfo = baseAddressGameEXE + weaponInfoDelta;
	if (!IsReadableMemory(weaponInfo, WeaponInfoSize))
		return false;
	uint16_t weaponInfoDamage = 0;
	__try
	{
		if (!TryRead(weaponInfo + WeaponInfoDamageOffset, weaponInfoDamage))
			return false;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
	// Molotov CWeaponInfo damage is not guaranteed to be populated on every
	// DE build because the native projectile path normally supplies the burn
	// event. Use its value when present and a bounded impact-event fallback when
	// the field is zero; no plugin health field is written directly.
	constexpr int ThrowableImpactFallbackDamage = 40;
	const int nativeDamage = (weaponInfoDamage > 0 && weaponInfoDamage <= 1000)
		? static_cast<int>(weaponInfoDamage) : ThrowableImpactFallbackDamage;
	const int damage = requestedDamage > 0
		? (std::clamp)(requestedDamage, 1, 1000) : nativeDamage;

	const std::array<uint8_t, 11> pedSignature{
		0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10 };
	const std::array<uint8_t, 11> vehicleSignature{
		0xF3, 0x0F, 0x11, 0x5C, 0x24, 0x20, 0x55, 0x53, 0x57, 0x41, 0x57 };
	const bool targetIsPed = contact.entityType == NativeEntityTypePed;
	const uintptr_t targetRva = targetIsPed ? NativePedDamageRva : NativeVehicleDamageRva;
	if (baseAddressGameEXE > (std::numeric_limits<uintptr_t>::max)() - targetRva)
		return false;
	const uintptr_t target = baseAddressGameEXE + targetRva;
	const std::array<uint8_t, 11>& expected = targetIsPed ? pedSignature : vehicleSignature;
	bool signatureMatches = false;
	__try
	{
		signatureMatches = IsReadableMemory(target, expected.size())
			&& std::memcmp(reinterpret_cast<const void*>(target), expected.data(), expected.size()) == 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		signatureMatches = false;
	}
	if (!signatureMatches)
	{
		nativeThrowableDamageDisabled = true;
		if (!nativeThrowableDamageFailureLogged)
		{
			nativeThrowableDamageFailureLogged = true;
			uevr::API::get()->log_error(
				"[MotionThrowableDamage] native signature mismatch type=%s(%u); impact damage disabled",
				NativeMeleeEntityTypeLabel(contact.entityType),
				static_cast<unsigned int>(contact.entityType));
		}
		return false;
	}

	if (targetIsPed)
	{
		using GenerateDamageEvent = bool(__fastcall*)(uintptr_t, uintptr_t,
			int32_t, int32_t, int32_t, uint8_t);
		const int32_t piece = (contact.piece >= 3 && contact.piece <= 9)
			? static_cast<int32_t>(contact.piece) : 3;
		contact.damageResult = NativeMeleeDamageResult::Attempted;
		bool accepted = false;
		__try
		{
			auto generateDamageEvent = reinterpret_cast<GenerateDamageEvent>(target);
			accepted = generateDamageEvent(contact.entity, playerPed, weaponType,
				damage, piece, 0);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			nativeThrowableDamageDisabled = true;
			if (!nativeThrowableDamageFailureLogged)
			{
				nativeThrowableDamageFailureLogged = true;
				uevr::API::get()->log_error(
					"[MotionThrowableDamage] ped call faulted; impact damage disabled");
			}
			return false;
		}
		contact.damageResult = accepted
			? NativeMeleeDamageResult::Accepted
			: NativeMeleeDamageResult::Rejected;
		if (accepted)
			appliedDamage = damage;
		uevr::API::get()->log_info(
			"[MotionThrowableDamage] result=%s type=ped(%u) weapon=%d damage=%d piece=%d infoDamage=%u",
			NativeMeleeDamageResultLabel(contact.damageResult),
			static_cast<unsigned int>(contact.entityType), weaponType, appliedDamage,
			piece, static_cast<unsigned int>(weaponInfoDamage));
		return accepted;
	}

	struct NativeVector { float x, y, z; };
	using InflictVehicleDamage = void(__fastcall*)(uintptr_t, uintptr_t,
		int32_t, float, const NativeVector*);
	const NativeVector point{ contact.point[0], contact.point[1], contact.point[2] };
	float healthBefore = 0.0f;
	float healthAfter = 0.0f;
	if (contact.entity > (std::numeric_limits<uintptr_t>::max)()
		- NativeVehicleHealthOffset - sizeof(float)
		|| !TryRead(contact.entity + NativeVehicleHealthOffset, healthBefore)
		|| !std::isfinite(healthBefore))
		return false;
	contact.targetHealthBefore = healthBefore;
	contact.damageResult = NativeMeleeDamageResult::Attempted;
	__try
	{
		auto inflictVehicleDamage = reinterpret_cast<InflictVehicleDamage>(target);
		inflictVehicleDamage(contact.entity, playerPed, weaponType,
			static_cast<float>(damage), &point);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		nativeThrowableDamageDisabled = true;
		if (!nativeThrowableDamageFailureLogged)
		{
			nativeThrowableDamageFailureLogged = true;
			uevr::API::get()->log_error(
				"[MotionThrowableDamage] vehicle call faulted; impact damage disabled");
		}
		return false;
	}
	const bool readHealthAfter = TryRead(contact.entity + NativeVehicleHealthOffset, healthAfter)
		&& std::isfinite(healthAfter);
	if (readHealthAfter)
		contact.targetHealthAfter = healthAfter;
	if (readHealthAfter && healthAfter < healthBefore - 0.001f)
	{
		contact.damageResult = NativeMeleeDamageResult::Accepted;
		appliedDamage = static_cast<int>((std::max)(1.0f, healthBefore - healthAfter));
		uevr::API::get()->log_info(
			"[MotionThrowableDamage] result=accepted type=vehicle(%u) weapon=%d damage=%d health=%.2f->%.2f infoDamage=%u",
			static_cast<unsigned int>(contact.entityType), weaponType, appliedDamage,
			healthBefore, healthAfter, static_cast<unsigned int>(weaponInfoDamage));
		return true;
	}
	contact.damageResult = NativeMeleeDamageResult::UnverifiedVehicleDispatch;
	uevr::API::get()->log_info(
		"[MotionThrowableDamage] result=unverified_vehicle type=vehicle(%u) weapon=%d health=%.2f->%.2f infoDamage=%u",
		static_cast<unsigned int>(contact.entityType), weaponType,
		healthBefore, healthAfter, static_cast<unsigned int>(weaponInfoDamage));
	return false;
}

void MemoryManager::RecordTriggerTimingEdge(uint32_t sequence, int weaponType, bool aimRequested,
	bool inVehicle, bool inputEnabled, bool eligible, bool gateEnabled,
	const char* side, double luaClockSeconds) {
	const bool armPlayerGate = gateEnabled && eligible && inputEnabled && !inVehicle
		&& IsPlayerSemiAutoGateWeapon(static_cast<uint32_t>(weaponType));
	InterlockedExchange(&playerSemiAutoPullWeaponType,
		armPlayerGate ? static_cast<LONG>(weaponType) : 0);
	InterlockedExchange(&playerSemiAutoShotPermit, armPlayerGate ? 1 : 0);
	InterlockedExchange(&playerSemiAutoBlockedCount, 0);
	InterlockedExchange(&playerSemiAutoPullHeld, armPlayerGate ? 1 : 0);

	if (!settingsManager->debugInputLayerProbe) {
		ResetTriggerTimingProbe();
		return;
	}

	triggerTimingSequence = sequence;
	triggerTimingEdgeTimestampUs = TriggerTimingTimestampUs();
	triggerTimingAimProxyLogged = false;
	triggerTimingArmed = eligible && inputEnabled;
	InterlockedExchange(&triggerTimingActiveSequence, triggerTimingArmed ? static_cast<LONG>(sequence) : 0);
	InterlockedExchange(&triggerTimingActive, triggerTimingArmed ? 1 : 0);
	const char* safeSide = side == nullptr ? "unknown" : side;
	uevr::API::get()->log_info(
		"[TriggerTiming] seq=%u event=trigger_edge qpc_us=%llu lua_clock=%.6f weapon=%d side=%s aim_requested=%s eligible=%s input_enabled=%s vehicle=%s",
		sequence,
		static_cast<unsigned long long>(triggerTimingEdgeTimestampUs),
		luaClockSeconds,
		weaponType,
		safeSide,
		aimRequested ? "true" : "false",
		eligible ? "true" : "false",
		inputEnabled ? "true" : "false",
		inVehicle ? "true" : "false");
}

void MemoryManager::RecordTriggerTimingRelease(uint32_t sequence, const char* side, double luaClockSeconds) {
	InterlockedExchange(&playerSemiAutoPullHeld, 0);
	InterlockedExchange(&playerSemiAutoPullWeaponType, 0);
	InterlockedExchange(&playerSemiAutoShotPermit, 0);
	const LONG blockedCount = InterlockedExchange(&playerSemiAutoBlockedCount, 0);
	if (blockedCount > 0) {
		uevr::API::get()->log_info(
			"[TriggerGate] physical release sequence=%u blocked_repeats=%ld",
			sequence, blockedCount);
	}

	if (!settingsManager->debugInputLayerProbe)
		return;

	const uint64_t timestampUs = TriggerTimingTimestampUs();
	const uint64_t deltaUs = triggerTimingEdgeTimestampUs != 0 && timestampUs >= triggerTimingEdgeTimestampUs
		? timestampUs - triggerTimingEdgeTimestampUs : 0;
	uevr::API::get()->log_info(
		"[TriggerTiming] seq=%u event=trigger_release qpc_us=%llu delta_us=%llu lua_clock=%.6f side=%s",
		sequence,
		static_cast<unsigned long long>(timestampUs),
		static_cast<unsigned long long>(deltaUs),
		luaClockSeconds,
		side == nullptr ? "unknown" : side);

	if (sequence == triggerTimingSequence) {
		triggerTimingArmed = false;
		InterlockedExchange(&triggerTimingActiveSequence, 0);
		InterlockedExchange(&triggerTimingActive, 0);
	}
}

void MemoryManager::RecordTriggerTimingAimVectorProxy(int weaponType, bool firstWeapon, bool inVehicle, int cameraMode) {
	if (!settingsManager->debugInputLayerProbe || !triggerTimingArmed || triggerTimingAimProxyLogged)
		return;

	triggerTimingAimProxyLogged = true;
	const uint64_t timestampUs = TriggerTimingTimestampUs();
	const uint64_t deltaUs = timestampUs >= triggerTimingEdgeTimestampUs
		? timestampUs - triggerTimingEdgeTimestampUs : 0;
	uevr::API::get()->log_info(
		"[TriggerTiming] seq=%u event=aim_vector_ready_proxy qpc_us=%llu delta_us=%llu weapon=%d hand=%s camera_mode=%d vehicle=%s",
		triggerTimingSequence,
		static_cast<unsigned long long>(timestampUs),
		static_cast<unsigned long long>(deltaUs),
		weaponType,
		firstWeapon ? "first" : "second",
		cameraMode,
		inVehicle ? "true" : "false");
}

void MemoryManager::RecordTriggerTimingMuzzleParticle(int weaponType, bool firstWeapon) {
	if (!settingsManager->debugInputLayerProbe)
		return;

	const uint64_t timestampUs = TriggerTimingTimestampUs();
	const bool sequenceArmed = triggerTimingArmed && triggerTimingSequence != 0;
	const uint64_t deltaUs = sequenceArmed && timestampUs >= triggerTimingEdgeTimestampUs
		? timestampUs - triggerTimingEdgeTimestampUs : 0;
	uevr::API::get()->log_info(
		"[TriggerTiming] seq=%u event=muzzle_particle_observed qpc_us=%llu delta_us=%llu weapon=%d hand=%s correlated=%s",
		sequenceArmed ? triggerTimingSequence : 0,
		static_cast<unsigned long long>(timestampUs),
		static_cast<unsigned long long>(deltaUs),
		weaponType,
		firstWeapon ? "first" : "second",
		sequenceArmed ? "true" : "false");
}

void MemoryManager::FlushTriggerTimingNativeShot(int weaponType, bool inVehicle) {
	if (InterlockedExchange(&nativeShotTimingPending, 0) == 0)
		return;

	const uint64_t timestampUs = static_cast<uint64_t>(InterlockedCompareExchange64(&nativeShotTimingTimestampUs, 0, 0));
	const uint32_t sequence = static_cast<uint32_t>(InterlockedCompareExchange(&nativeShotTimingSequence, 0, 0));
	const LONG path = InterlockedCompareExchange(&nativeShotTimingPath, 0, 0);
	if (!settingsManager->debugInputLayerProbe)
		return;

	const uint64_t deltaUs = sequence != 0 && sequence == triggerTimingSequence
		&& timestampUs >= triggerTimingEdgeTimestampUs
		? timestampUs - triggerTimingEdgeTimestampUs : 0;
	uevr::API::get()->log_info(
		"[TriggerTiming] seq=%u event=native_shot_observed qpc_us=%llu delta_us=%llu weapon=%d path=%s correlated=%s vehicle=%s",
		sequence,
		static_cast<unsigned long long>(timestampUs),
		static_cast<unsigned long long>(deltaUs),
		weaponType,
		path == 2 ? "player_shoot_cam45" : "player_shoot",
		sequence != 0 ? "true" : "false",
		inVehicle ? "true" : "false");
}

void MemoryManager::ResetTriggerTimingProbe() {
	triggerTimingArmed = false;
	triggerTimingSequence = 0;
	triggerTimingEdgeTimestampUs = 0;
	triggerTimingAimProxyLogged = false;
	InterlockedExchange(&triggerTimingActiveSequence, 0);
	InterlockedExchange(&triggerTimingActive, 0);
	InterlockedExchange64(&nativeShotTimingTimestampUs, 0);
	InterlockedExchange(&nativeShotTimingSequence, 0);
	InterlockedExchange(&nativeShotTimingPath, 0);
	InterlockedExchange(&nativeShotTimingPending, 0);
}

void MemoryManager::CaptureNativeShotObservation(uintptr_t instructionAddress) {
	// The player-fire prologue is only an attempt boundary; GTA may still reject
	// it for cooldown. Consume the one-shot permit here, at the observed native
	// shot boundary, so a quick physical tap remains pending until accepted.
	if (gPlayerSemiAutoPullHeld != nullptr && gPlayerSemiAutoShotPermit != nullptr
		&& InterlockedCompareExchange(gPlayerSemiAutoPullHeld, 0, 0) != 0)
		InterlockedExchange(gPlayerSemiAutoShotPermit, 0);

	// The hand-specific FireGun entry publishes the active controller hand before
	// the accepted native shot reaches this hardware breakpoint. Clear only that
	// hand's retained semi-auto edge; the opposite hand remains independently
	// pending and retries through the same authoritative native weapon state.
	if (gCustomAkimboEnabled != nullptr && gCustomAkimboActiveHand != nullptr
		&& gCustomAkimboPendingMask != nullptr && gCustomAkimboTaskFireMask != nullptr
		&& gCustomAkimboWeaponType != nullptr && gCustomAkimboAcceptedShotSequence != nullptr
		&& InterlockedCompareExchange(gCustomAkimboEnabled, 0, 0) != 0) {
		const LONG hand = InterlockedCompareExchange(gCustomAkimboActiveHand, 0, 0);
		const LONG weapon = InterlockedCompareExchange(gCustomAkimboWeaponType, 0, 0);
		const LONG nativeBit = hand == 0 ? 0x02 : (hand == 1 ? 0x01 : 0x00);
		if (nativeBit != 0) {
			InterlockedAnd(gCustomAkimboPendingMask, ~nativeBit);
			if (weapon == 22 || weapon == 26)
				InterlockedAnd(gCustomAkimboTaskFireMask, ~nativeBit);
			InterlockedIncrement(gCustomAkimboAcceptedShotSequence);
			if (gCustomAkimboAcceptedHandMask != nullptr)
				InterlockedOr(gCustomAkimboAcceptedHandMask, 1L << hand);
		}
	}

	if (InterlockedCompareExchange(&triggerTimingActive, 0, 0) == 0)
		return;

	const uint64_t timestampUs = TriggerTimingTimestampUs();
	const LONG sequence = InterlockedCompareExchange(&triggerTimingActiveSequence, 0, 0);
	const LONG path = instructionAddress == playerShootCam45InstructionAddress ? 2 : 1;
	InterlockedExchange64(&nativeShotTimingTimestampUs, static_cast<LONG64>(timestampUs));
	InterlockedExchange(&nativeShotTimingSequence, sequence);
	InterlockedExchange(&nativeShotTimingPath, path);
	InterlockedExchange(&nativeShotTimingPending, 1);
}

void MemoryManager::SetAimCalibrationReference(float x, float y, float z, int weaponType) {
	if (!settingsManager->enableAimCalibrationProbe)
		return;

	const float values[] = { x, y, z };
	for (size_t i = 0; i < nativeAimReferenceBits.size(); ++i) {
		LONG bits = 0;
		std::memcpy(&bits, &values[i], sizeof(bits));
		InterlockedExchange(&nativeAimReferenceBits[i], bits);
	}
	InterlockedExchange(&nativeAimReferenceWeapon, static_cast<LONG>(weaponType));
}

bool MemoryManager::ReadAimCalibrationSample(AimCalibrationSample& sample) {
	const LONG sequenceBefore = InterlockedCompareExchange(&nativeAimSequence, 0, 0);
	if (sequenceBefore == 0 || sequenceBefore == lastReadNativeAimSequence)
		return false;

	auto readFloatArray = [](const std::array<volatile LONG, 3>& source, std::array<float, 3>& destination) {
		for (size_t i = 0; i < source.size(); ++i) {
			const LONG bits = InterlockedCompareExchange(const_cast<volatile LONG*>(&source[i]), 0, 0);
			std::memcpy(&destination[i], &bits, sizeof(bits));
		}
	};

	AimCalibrationSample captured{};
	readFloatArray(nativeAimCapturedReferenceBits, captured.intendedDirection);
	readFloatArray(nativeAimShotStartBits, captured.shotStart);
	readFloatArray(nativeAimShotTargetBits, captured.shotTarget);
	readFloatArray(nativeAimHitPointBits, captured.hitPoint);
	captured.hitEntity = static_cast<uintptr_t>(InterlockedCompareExchange64(&nativeAimHitEntity, 0, 0));
	captured.weaponType = static_cast<int>(InterlockedCompareExchange(&nativeAimCapturedWeapon, 0, 0));

	const LONG sequenceAfter = InterlockedCompareExchange(&nativeAimSequence, 0, 0);
	if (sequenceBefore != sequenceAfter)
		return false;

	captured.sequence = static_cast<uint32_t>(sequenceAfter);
	lastReadNativeAimSequence = sequenceAfter;
	sample = captured;
	return true;
}

void MemoryManager::DiscardPendingAimCalibrationSample() {
	lastReadNativeAimSequence = InterlockedCompareExchange(&nativeAimSequence, 0, 0);
}

bool MemoryManager::SetNativeShotTraceOverride(
	const std::array<float, 3>& origin, const std::array<float, 3>& target) {
	// Expire an unconsumed tracer when the next frame-level pose is published.
	InterlockedExchange(&nativeShotTrailPending, 0);
	double distanceSquared = 0.0;
	for (size_t i = 0; i < origin.size(); ++i) {
		if (!std::isfinite(origin[i]) || !std::isfinite(target[i])) {
			ClearNativeShotTraceOverride();
			return false;
		}
		const double delta = static_cast<double>(target[i]) - static_cast<double>(origin[i]);
		distanceSquared += delta * delta;
	}
	if (!std::isfinite(distanceSquared) || distanceSquared < 1.0) {
		ClearNativeShotTraceOverride();
		return false;
	}

	// Publish under a sequence lock. The cave either observes one coherent pair
	// or restores the native vectors; it can never consume a partially updated
	// origin such as the zero-origin failure seen during rapid aim updates.
	InterlockedIncrement(&nativeShotTracePublishSequence); // odd: update in progress
	InterlockedExchange(&nativeShotTraceOverrideConsumed, 0);

	const auto publishFloatArray = [](const std::array<float, 3>& values,
		std::array<volatile LONG, 3>& destination) {
		for (size_t i = 0; i < values.size(); ++i) {
			LONG bits = 0;
			std::memcpy(&bits, &values[i], sizeof(bits));
			InterlockedExchange(&destination[i], bits);
		}
	};

	publishFloatArray(origin, nativeShotTraceOriginOverrideBits);
	publishFloatArray(target, nativeShotTraceTargetOverrideBits);
	InterlockedIncrement(&nativeShotTracePublishSequence); // even: stable pair
	InterlockedExchange(&nativeShotTraceOriginOverrideEnabled, 1);
	return true;
}

void MemoryManager::SetCustomAkimboState(bool enabled, int weaponType,
	uint8_t heldMask, uint8_t edgeMask) {
	const bool supported = weaponType == 22 || weaponType == 26
		|| weaponType == 28 || weaponType == 32;
	if (!customAkimboFirePatchInstalled || !enabled || !supported) {
		ClearCustomAkimboState();
		return;
	}

	const LONG sanitizedHeld = static_cast<LONG>(heldMask & 0x03U);
	const LONG sanitizedEdges = static_cast<LONG>(edgeMask & 0x03U);
	InterlockedExchange(&customAkimboWeaponType, static_cast<LONG>(weaponType));
	InterlockedExchange(&customAkimboHeldMask, sanitizedHeld);
	if (sanitizedEdges != 0)
		InterlockedOr(&customAkimboPendingMask, sanitizedEdges);

	// SMGs repeat while held. Pistols and sawn-offs retain each hand's edge until
	// the native shot observation confirms that exact hand was accepted.
	const bool automatic = weaponType == 28 || weaponType == 32;
	const LONG fireMask = automatic
		? sanitizedHeld
		: InterlockedCompareExchange(&customAkimboPendingMask, 0, 0);
	InterlockedExchange(&customAkimboTaskFireMask, fireMask & 0x03);
	InterlockedExchange(&customAkimboEnabled, 1);
}

bool MemoryManager::SetCustomAkimboHandTrace(int hand,
	const std::array<float, 3>& origin, const std::array<float, 3>& target) {
	if (!customAkimboFirePatchInstalled || hand < 0 || hand > 1)
		return false;
	double distanceSquared = 0.0;
	for (size_t axis = 0; axis < 3; ++axis) {
		if (!std::isfinite(origin[axis]) || !std::isfinite(target[axis]))
			return false;
		const double delta = static_cast<double>(target[axis])
			- static_cast<double>(origin[axis]);
		distanceSquared += delta * delta;
	}
	if (!std::isfinite(distanceSquared) || distanceSquared < 1.0)
		return false;

	for (size_t axis = 0; axis < 3; ++axis) {
		LONG originBits = 0;
		LONG targetBits = 0;
		std::memcpy(&originBits, &origin[axis], sizeof(originBits));
		std::memcpy(&targetBits, &target[axis], sizeof(targetBits));
		InterlockedExchange(&customAkimboTraceOriginBits[hand][axis], originBits);
		InterlockedExchange(&customAkimboTraceTargetBits[hand][axis], targetBits);
	}
	InterlockedOr(&customAkimboTraceValidMask, 1L << hand);
	return true;
}

void MemoryManager::ClearCustomAkimboState() {
	InterlockedExchange(&customAkimboEnabled, 0);
	InterlockedExchange(&customAkimboWeaponType, 0);
	InterlockedExchange(&customAkimboHeldMask, 0);
	InterlockedExchange(&customAkimboPendingMask, 0);
	InterlockedExchange(&customAkimboTaskFireMask, 0);
	InterlockedExchange(&customAkimboActiveHand, -1);
	InterlockedExchange(&customAkimboTraceValidMask, 0);
	InterlockedExchange(&customAkimboAcceptedHandMask, 0);
	InterlockedExchange64(&customAkimboTaskPointer, 0);
}

uint8_t MemoryManager::ConsumeCustomAkimboAcceptedHandMask() {
	return static_cast<uint8_t>(InterlockedExchange(
		&customAkimboAcceptedHandMask, 0) & 0x03);
}

void MemoryManager::ClearNativeShotTraceOverride() {
	InterlockedExchange(&nativeShotTraceOriginOverrideEnabled, 0);
	InterlockedExchange(&nativeShotTraceOverrideConsumed, 0);
	InterlockedExchange(&nativeShotTraceVehicleOverrideArmed, 0);
	InterlockedExchange(&nativeShotTraceVehicleModeActive, 0);
	InterlockedExchange(&nativeShotTraceCallSiteRva, 0);
	InterlockedExchange(&nativeShotTrailPending, 0);
	InterlockedExchange(&nativeShotEffectCapturedOverride, 0);
	InterlockedExchange(&nativeShotEffectCallSiteRva, 0);
	InterlockedExchange(&nativeShotEffectOwnerLocal, 0);
}

void MemoryManager::SetVehicleShotTraceOverrideArmed(bool armed) {
	InterlockedExchange(&nativeShotTraceVehicleOverrideArmed, armed ? 1 : 0);
}

void MemoryManager::SetVehicleShotTraceModeActive(bool active) {
	InterlockedExchange(&nativeShotTraceVehicleModeActive, active ? 1 : 0);
	if (!active)
		InterlockedExchange(&nativeShotTraceVehicleOverrideArmed, 0);
}

uint32_t MemoryManager::GetNativeShotTraceSequenceSnapshot() const {
	return static_cast<uint32_t>(InterlockedCompareExchange(
		const_cast<volatile LONG*>(&nativeShotTraceSequence), 0, 0));
}

bool MemoryManager::ReadLatestNativeShotTraceProbe(NativeShotTraceProbe& probe) {
	const LONG sequenceBefore = InterlockedCompareExchange(&nativeShotTraceSequence, 0, 0);
	if (sequenceBefore == 0 || sequenceBefore == lastReadNativeShotTraceSequence)
		return false;

	const auto readFloatArray = [](const std::array<volatile LONG, 3>& source,
		std::array<float, 3>& destination) {
		for (size_t i = 0; i < source.size(); ++i) {
			const LONG bits = InterlockedCompareExchange(
				const_cast<volatile LONG*>(&source[i]), 0, 0);
			std::memcpy(&destination[i], &bits, sizeof(bits));
		}
	};

	NativeShotTraceProbe captured{};
	readFloatArray(nativeShotTraceCapturedStartBits, captured.rawStart);
	readFloatArray(nativeShotTraceCapturedTargetBits, captured.rawTarget);
	readFloatArray(nativeShotTraceAppliedStartBits, captured.appliedStart);
	readFloatArray(nativeShotTraceAppliedTargetBits, captured.appliedTarget);
	captured.overridden = InterlockedCompareExchange(&nativeShotTraceCapturedOverride, 0, 0) != 0;
	captured.callSiteRva = static_cast<uint32_t>(InterlockedCompareExchange(&nativeShotTraceCallSiteRva, 0, 0));

	const LONG sequenceAfter = InterlockedCompareExchange(&nativeShotTraceSequence, 0, 0);
	if (sequenceBefore != sequenceAfter)
		return false;

	captured.sequence = static_cast<uint32_t>(sequenceAfter);
	lastReadNativeShotTraceSequence = sequenceAfter;
	probe = captured;
	return true;
}

bool MemoryManager::ReadLatestNativeShotEffectProbe(NativeShotEffectProbe& probe) {
	const LONG sequenceBefore = InterlockedCompareExchange(&nativeShotEffectSequence, 0, 0);
	if (sequenceBefore == 0 || sequenceBefore == lastReadNativeShotEffectSequence)
		return false;

	const auto readFloatArray = [](const std::array<volatile LONG, 3>& source,
		std::array<float, 3>& destination) {
		for (size_t i = 0; i < source.size(); ++i) {
			const LONG bits = InterlockedCompareExchange(
				const_cast<volatile LONG*>(&source[i]), 0, 0);
			std::memcpy(&destination[i], &bits, sizeof(bits));
		}
	};

	NativeShotEffectProbe captured{};
	readFloatArray(nativeShotEffectCapturedStartBits, captured.rawStart);
	readFloatArray(nativeShotEffectCapturedTargetBits, captured.rawTarget);
	readFloatArray(nativeShotEffectAppliedStartBits, captured.appliedStart);
	readFloatArray(nativeShotEffectAppliedTargetBits, captured.appliedTarget);
	captured.overridden = InterlockedCompareExchange(&nativeShotEffectCapturedOverride, 0, 0) != 0;
	captured.callSiteRva = static_cast<uint32_t>(InterlockedCompareExchange(&nativeShotEffectCallSiteRva, 0, 0));
	captured.ownerLocal = InterlockedCompareExchange(&nativeShotEffectOwnerLocal, 0, 0) != 0;

	const LONG sequenceAfter = InterlockedCompareExchange(&nativeShotEffectSequence, 0, 0);
	if (sequenceBefore != sequenceAfter)
		return false;

	captured.sequence = static_cast<uint32_t>(sequenceAfter);
	lastReadNativeShotEffectSequence = sequenceAfter;
	probe = captured;
	return true;
}
void MemoryManager::ApplyCombatAssistPatches() {
	if (combatAssistApplyAttempted)
		return;

	combatAssistApplyAttempted = true;
	ResolveCombatAssistStats();
	UpdateCombatAssistPlayerPointer();
	ApplyCombatAssistWeaponInfoValues();

	auto installHook = [this](const char* name, const std::vector<int>& pattern, size_t overwriteSize,
		const std::function<std::vector<uint8_t>(uintptr_t, uintptr_t, const std::vector<uint8_t>&)>& buildCode) {
		const uintptr_t target = FindPattern(pattern);
		if (target == 0) {
			uevr::API::get()->log_error("[CombatAssist] %s signature not found", name);
			return false;
		}

		const bool installed = InstallHookPatch(name, target, overwriteSize, buildCode);
		if (!installed)
			uevr::API::get()->log_error("[CombatAssist] %s hook failed", name);
		else
			uevr::API::get()->log_info("[CombatAssist] %s hook installed at 0x%llX", name, static_cast<unsigned long long>(target - baseAddressGameEXE));
			return installed;
	};

	auto installHookAt = [this](const char* name, uintptr_t target, size_t overwriteSize,
		const std::function<std::vector<uint8_t>(uintptr_t, uintptr_t, const std::vector<uint8_t>&)>& buildCode) {
		if (target == 0) {
			uevr::API::get()->log_error("[CombatAssist] %s target not found", name);
			return false;
		}

		const bool installed = InstallHookPatch(name, target, overwriteSize, buildCode);
		if (!installed)
			uevr::API::get()->log_error("[CombatAssist] %s hook failed", name);
		else
			uevr::API::get()->log_info("[CombatAssist] %s hook installed at 0x%llX", name, static_cast<unsigned long long>(target - baseAddressGameEXE));
		return installed;
	};

	auto installBytes = [this](const char* name, const std::vector<int>& pattern, const std::vector<uint8_t>& bytes) {
		const uintptr_t target = FindPattern(pattern);
		if (target == 0) {
			uevr::API::get()->log_error("[CombatAssist] %s signature not found", name);
			return false;
		}

		const bool installed = InstallBytePatch(name, target, bytes);
		if (!installed)
			uevr::API::get()->log_error("[CombatAssist] %s patch failed", name);
		else
			uevr::API::get()->log_info("[CombatAssist] %s patch installed at 0x%llX", name, static_cast<unsigned long long>(target - baseAddressGameEXE));
		return installed;
	};

	if (!combatAssistPlayerGlobalsResolved) {
		installHook("Player pointer cache",
			{ 0x48, 0x8B, 0x82, 0x88, 0x05, 0x00, 0x00, 0xF6, 0x40, 0x48, 0x08, 0x74 },
			7,
			[this](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes) {
				std::vector<uint8_t> code;
				code.push_back(0x48); code.push_back(0xB8); AppendU64(code, reinterpret_cast<uintptr_t>(&cachedPlayerPointer));
				code.push_back(0x48); code.push_back(0x89); code.push_back(0x10); // mov [rax],rdx
				code.insert(code.end(), originalBytes.begin(), originalBytes.end());
				if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
					return std::vector<uint8_t>{};
				return code;
			});
	}

	if (settingsManager->enableCompactWeaponReticle) {
		installHook("Compact aiming reticle (visual only)",
			{ 0xF3, 0x0F, 0x5E, 0x58, 0x38 },
			5,
			[this](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes) {
				std::vector<uint8_t> code;
				size_t jeOffset = 0;
				size_t jneOffset = 0;
				AppendCachedPlayerGuardR11(code, reinterpret_cast<uintptr_t>(&cachedPlayerPointer), jeOffset, jneOffset);
				AppendPopGuard(code);
				code.insert(code.end(), { 0x0F, 0x57, 0xDB }); // xorps xmm3,xmm3
				if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
					return std::vector<uint8_t>{};

				const size_t originalOffset = code.size();
				if (!PatchShortJcc(code, jeOffset, originalOffset) || !PatchShortJcc(code, jneOffset, originalOffset))
					return std::vector<uint8_t>{};
				AppendPopGuard(code);
				code.insert(code.end(), originalBytes.begin(), originalBytes.end());
				if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
					return std::vector<uint8_t>{};
				return code;
			});
	}

	installHook("Player bullet damage resistance",
		{
			0x48, 0x89, 0x5C, 0x24, 0x08,
			0x48, 0x89, 0x6C, 0x24, 0x10,
			0x48, 0x89, 0x74, 0x24, 0x18,
			0x57, 0x48, 0x83, 0xEC, 0x50,
			0x83, 0xBA, 0xE0, 0x07, 0x00, 0x00, 0x01
		},
		5,
		[this](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes) {
			std::vector<uint8_t> code;
			size_t noCachedPlayerOffset = 0;
			size_t notPlayerOffset = 0;
			AppendCachedPlayerGuardRDX(code, reinterpret_cast<uintptr_t>(&cachedPlayerPointer),
				noCachedPlayerOffset, notPlayerOffset);

			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&settingsManager->enableBulletDamageResistance)); // mov r10,flag
			code.insert(code.end(), { 0x41, 0x80, 0x3A, 0x00 }); // cmp byte ptr [r10],0
			const size_t flagDisabledOffset = AppendShortJcc(code, 0x74); // je original

			code.insert(code.end(), { 0x44, 0x8B, 0x51, 0x10 }); // mov r10d,[rcx+10h] (weapon type)
			code.insert(code.end(), { 0x41, 0x83, 0xFA, static_cast<uint8_t>(FirstBulletWeaponType) });
			const size_t belowBulletRangeOffset = AppendShortJcc(code, 0x72); // jb original
			code.insert(code.end(), { 0x41, 0x83, 0xFA, static_cast<uint8_t>(LastStandardBulletWeaponType) });
			const size_t standardBulletOffset = AppendShortJcc(code, 0x76); // jbe reduce
			code.insert(code.end(), { 0x41, 0x83, 0xFA, static_cast<uint8_t>(MinigunWeaponType) });
			const size_t notMinigunOffset = AppendShortJcc(code, 0x75); // jne original

			const size_t reduceDamageOffset = code.size();
			code.push_back(0x41); code.push_back(0xBA); AppendU32(code, HalfDamageFloatBits); // mov r10d,0.5f
			code.insert(code.end(), { 0x66, 0x41, 0x0F, 0x6E, 0xCA }); // movd xmm1,r10d
			code.insert(code.end(), { 0xF3, 0x0F, 0x10, 0x41, 0x08 }); // movss xmm0,[rcx+8]
			code.insert(code.end(), { 0xF3, 0x0F, 0x59, 0xC1 }); // mulss xmm0,xmm1
			code.insert(code.end(), { 0xF3, 0x0F, 0x11, 0x41, 0x08 }); // movss [rcx+8],xmm0
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&playerBulletDamageReductionCount));
			code.insert(code.end(), { 0xF0, 0x41, 0xFF, 0x02 }); // lock inc dword ptr [r10]
			AppendPopGuard(code);
			code.insert(code.end(), originalBytes.begin(), originalBytes.end());
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
				return std::vector<uint8_t>{};

			const size_t originalPathOffset = code.size();
			AppendPopGuard(code);
			code.insert(code.end(), originalBytes.begin(), originalBytes.end());
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
				return std::vector<uint8_t>{};

			if (!PatchShortJcc(code, noCachedPlayerOffset, originalPathOffset)
				|| !PatchShortJcc(code, notPlayerOffset, originalPathOffset)
				|| !PatchShortJcc(code, flagDisabledOffset, originalPathOffset)
				|| !PatchShortJcc(code, belowBulletRangeOffset, originalPathOffset)
				|| !PatchShortJcc(code, standardBulletOffset, reduceDamageOffset)
				|| !PatchShortJcc(code, notMinigunOffset, originalPathOffset))
				return std::vector<uint8_t>{};

			return code;
		});

	auto installPlayerSpreadBypass = [this, &installHook](const char* name, const std::vector<int>& pattern, uint8_t originalShortJcc, volatile LONG* bypassCounter) {
		return installHook(name, pattern, 10,
			[this, originalShortJcc, bypassCounter](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes) {
				if (originalBytes.size() != 10)
					return std::vector<uint8_t>{};

				int32_t originalBranchDisplacement = 0;
				std::memcpy(&originalBranchDisplacement, originalBytes.data() + 6, sizeof(originalBranchDisplacement));
				const uintptr_t skipRandomSpreadAddress = returnAddress + originalBranchDisplacement;

				std::vector<uint8_t> code;
				size_t noCachedPlayerOffset = 0;
				size_t notPlayerOffset = 0;
				AppendCachedPlayerGuardRBX(code, reinterpret_cast<uintptr_t>(&cachedPlayerPointer), noCachedPlayerOffset, notPlayerOffset);

				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(&settingsManager->enableWeaponNoSpread)); // mov r10,flag
				code.insert(code.end(), { 0x41, 0x80, 0x3A, 0x00 }); // cmp byte ptr [r10],0
				const size_t flagDisabledOffset = AppendShortJcc(code, 0x74); // je original

				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(bypassCounter)); // mov r10,counter
				code.insert(code.end(), { 0xF0, 0x41, 0xFF, 0x02 }); // lock inc dword ptr [r10]
				AppendPopGuard(code);
				if (!AppendRelJmp(code, caveAddress + code.size(), skipRandomSpreadAddress))
					return std::vector<uint8_t>{};

				const size_t originalPathOffset = code.size();
				if (!PatchShortJcc(code, noCachedPlayerOffset, originalPathOffset)
					|| !PatchShortJcc(code, notPlayerOffset, originalPathOffset)
					|| !PatchShortJcc(code, flagDisabledOffset, originalPathOffset))
					return std::vector<uint8_t>{};

				AppendPopGuard(code);
				code.insert(code.end(), originalBytes.begin(), originalBytes.begin() + 4); // original comiss/ucomiss
				const size_t originalConditionOffset = AppendShortJcc(code, originalShortJcc);
				if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
					return std::vector<uint8_t>{};

				const size_t originalBranchOffset = code.size();
				if (!PatchShortJcc(code, originalConditionOffset, originalBranchOffset))
					return std::vector<uint8_t>{};
				if (!AppendRelJmp(code, caveAddress + code.size(), skipRandomSpreadAddress))
					return std::vector<uint8_t>{};
				return code;
			});
	};

	installPlayerSpreadBypass("Player native shot spread bypass A",
		{ 0x45, 0x0F, 0x2F, 0xD7, 0x0F, 0x86, -1, -1, -1, -1, 0x4D, 0x85, 0xFF, 0x74, 0x6D },
		0x76,
		&nativeShotSpreadBypassCountA); // jbe
	installPlayerSpreadBypass("Player native shot spread bypass B",
		{ 0x45, 0x0F, 0x2E, 0xD7, 0x0F, 0x84, -1, -1, -1, -1, 0xF3, 0x44, 0x0F, 0x10, 0x35 },
		0x74,
		&nativeShotSpreadBypassCountB); // je

	installHook("Player native aim calibration capture",
		{
			0x48, 0x8B, 0x7D, 0x80,
			0x48, 0x85, 0xFF,
			0x74, 0x24,
			0x48, 0x8B, 0x0D, -1, -1, -1, -1,
			0x48, 0x8B, 0x01,
			0xFF, 0x90, 0x88, 0x00, 0x00, 0x00,
			0x84, 0xC0, 0x74, 0x10,
			0x48, 0x8B, 0x0D, -1, -1, -1, -1,
			0x48, 0x8B, 0x01,
			0xFF, 0x90, 0x58, 0x01, 0x00, 0x00,
			0x0F, 0xB6, 0x05
		},
		7,
		[this](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes) {
			std::vector<uint8_t> code;
			code.push_back(0x9C); // pushfq
			code.push_back(0x50); // push rax
			code.push_back(0x41); code.push_back(0x52); // push r10

			code.push_back(0x49); code.push_back(0xBA); AppendU64(code, reinterpret_cast<uintptr_t>(&cachedPlayerPointer));
			code.push_back(0x4D); code.push_back(0x8B); code.push_back(0x12); // mov r10,[r10]
			code.push_back(0x4D); code.push_back(0x85); code.push_back(0xD2); // test r10,r10
			const size_t noPlayerOffset = AppendNearJcc(code, 0x84); // je original
			code.push_back(0x4C); code.push_back(0x39); code.push_back(0xD3); // cmp rbx,r10
			const size_t notPlayerOffset = AppendNearJcc(code, 0x85); // jne original

			code.push_back(0x49); code.push_back(0xBA); AppendU64(code, reinterpret_cast<uintptr_t>(&settingsManager->enableAimCalibrationProbe));
			code.insert(code.end(), { 0x41, 0x80, 0x3A, 0x00 }); // cmp byte ptr [r10],0
			const size_t flagDisabledOffset = AppendNearJcc(code, 0x84); // je original

			auto appendStackFloat = [&code](volatile LONG* destination, uint8_t stackDisplacement) {
				code.push_back(0x49); code.push_back(0xBA); AppendU64(code, reinterpret_cast<uintptr_t>(destination));
				code.push_back(0x8B); code.push_back(0x45); code.push_back(stackDisplacement); // mov eax,[rbp+disp8]
				code.push_back(0x41); code.push_back(0x89); code.push_back(0x02); // mov [r10],eax
			};
			auto appendDwordCopy = [&code](volatile LONG* source, volatile LONG* destination) {
				code.push_back(0x49); code.push_back(0xBA); AppendU64(code, reinterpret_cast<uintptr_t>(source));
				code.push_back(0x41); code.push_back(0x8B); code.push_back(0x02); // mov eax,[r10]
				code.push_back(0x49); code.push_back(0xBA); AppendU64(code, reinterpret_cast<uintptr_t>(destination));
				code.push_back(0x41); code.push_back(0x89); code.push_back(0x02); // mov [r10],eax
			};

			for (size_t i = 0; i < nativeAimReferenceBits.size(); ++i)
				appendDwordCopy(&nativeAimReferenceBits[i], &nativeAimCapturedReferenceBits[i]);
			appendDwordCopy(&nativeAimReferenceWeapon, &nativeAimCapturedWeapon);

			for (size_t i = 0; i < nativeAimShotStartBits.size(); ++i)
				appendStackFloat(&nativeAimShotStartBits[i], static_cast<uint8_t>(0x10 + (i * sizeof(float))));
			for (size_t i = 0; i < nativeAimShotTargetBits.size(); ++i)
				appendStackFloat(&nativeAimShotTargetBits[i], static_cast<uint8_t>(i * sizeof(float)));
			for (size_t i = 0; i < nativeAimHitPointBits.size(); ++i)
				appendStackFloat(&nativeAimHitPointBits[i], static_cast<uint8_t>(0x40 + (i * sizeof(float))));

			code.push_back(0x48); code.push_back(0x8B); code.push_back(0x45); code.push_back(0x80); // mov rax,[rbp-80h]
			code.push_back(0x49); code.push_back(0xBA); AppendU64(code, reinterpret_cast<uintptr_t>(&nativeAimHitEntity));
			code.push_back(0x49); code.push_back(0x89); code.push_back(0x02); // mov [r10],rax
			code.push_back(0x49); code.push_back(0xBA); AppendU64(code, reinterpret_cast<uintptr_t>(&nativeAimSequence));
			code.insert(code.end(), { 0xF0, 0x41, 0xFF, 0x02 }); // lock inc dword ptr [r10]

			code.push_back(0x41); code.push_back(0x5A); // pop r10
			code.push_back(0x58); // pop rax
			code.push_back(0x9D); // popfq
			code.insert(code.end(), originalBytes.begin(), originalBytes.end());
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
				return std::vector<uint8_t>{};

			const size_t originalPathOffset = code.size();
			code.push_back(0x41); code.push_back(0x5A); // pop r10
			code.push_back(0x58); // pop rax
			code.push_back(0x9D); // popfq
			code.insert(code.end(), originalBytes.begin(), originalBytes.end());
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
				return std::vector<uint8_t>{};

			if (!PatchNearJcc(code, noPlayerOffset, originalPathOffset)
				|| !PatchNearJcc(code, notPlayerOffset, originalPathOffset)
				|| !PatchNearJcc(code, flagDisabledOffset, originalPathOffset))
				return std::vector<uint8_t>{};

			return code;
		});

	// Disabled: this caller-side pattern runs continuously while aiming and is
	// not a reliable shot boundary. The verified trace entry below is narrower.
#if 0
	installHook("Player native shot trace origin redirect",
		{
			0x4C, 0x8D, 0x4D, 0x80,
			0x83, 0xBB, 0xE0, 0x07, 0x00, 0x00, 0x01,
			0x4C, 0x8D, 0x45, 0x40
		},
		11,
		[this](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes) {
			if (originalBytes.size() != 11)
				return std::vector<uint8_t>{};

			std::vector<uint8_t> code;
			size_t noCachedPlayerOffset = 0;
			size_t notPlayerOffset = 0;
			AppendCachedPlayerGuardRBX(code, reinterpret_cast<uintptr_t>(&cachedPlayerPointer),
				noCachedPlayerOffset, notPlayerOffset);

			// Keep the original path close to the guard so the guard's short
			// branches remain in range. The valid-player path jumps over it.
			const size_t skipOriginalPathOffset = code.size();
			code.push_back(0xE9);
			AppendU32(code, 0);
			const size_t originalPathOffset = code.size();
			AppendPopGuard(code);
			code.insert(code.end(), originalBytes.begin(), originalBytes.end());
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
				return std::vector<uint8_t>{};
			const size_t injectionOffset = code.size();

			auto patchNearJmp = [&code, caveAddress](size_t instructionOffset, size_t targetOffset) {
			const int64_t displacement = static_cast<int64_t>(caveAddress + targetOffset)
				- static_cast<int64_t>(caveAddress + instructionOffset + 5);
			if (displacement < std::numeric_limits<int32_t>::min()
				|| displacement > std::numeric_limits<int32_t>::max())
				return false;
			const int32_t relative = static_cast<int32_t>(displacement);
			std::memcpy(code.data() + instructionOffset + 1, &relative, sizeof(relative));
			return true;
		};

		if (!patchNearJmp(skipOriginalPathOffset, injectionOffset)
			|| !PatchShortJcc(code, noCachedPlayerOffset, originalPathOffset)
			|| !PatchShortJcc(code, notPlayerOffset, originalPathOffset))
			return std::vector<uint8_t>{};

		auto appendStackFloatCopy = [&code](volatile LONG* destination, uint8_t stackDisplacement) {
			code.push_back(0x49); code.push_back(0xBA); AppendU64(code, reinterpret_cast<uintptr_t>(destination));
			code.insert(code.end(), { 0x8B, 0x45, stackDisplacement }); // mov eax,[rbp+disp8]
			code.insert(code.end(), { 0x41, 0x89, 0x02 }); // mov [r10],eax
		};
		auto appendOverrideFloatCopy = [&code](volatile LONG* source, uint8_t stackDisplacement) {
			code.push_back(0x49); code.push_back(0xBA); AppendU64(code, reinterpret_cast<uintptr_t>(source));
			code.insert(code.end(), { 0x41, 0x8B, 0x02 }); // mov eax,[r10]
			code.insert(code.end(), { 0x89, 0x45, stackDisplacement }); // mov [rbp+disp8],eax
		};
		auto appendStoreImmediate = [&code](volatile LONG* destination, uint32_t value) {
			code.push_back(0x49); code.push_back(0xBA); AppendU64(code, reinterpret_cast<uintptr_t>(destination));
			code.push_back(0x41); code.push_back(0xC7); code.push_back(0x02); AppendU32(code, value);
		};
		auto appendIncrement = [&code](volatile LONG* destination) {
			code.push_back(0x49); code.push_back(0xBA); AppendU64(code, reinterpret_cast<uintptr_t>(destination));
			code.insert(code.end(), { 0xF0, 0x41, 0xFF, 0x02 }); // lock inc dword [r10]
		};

		// Capture the exact arguments that are about to be passed to the native
		// line trace: RCX=&[rbp+10h], RDX=&[rbp], R8=&[rbp+40h], R9=&[rbp-80h].
		for (size_t i = 0; i < nativeShotTraceCapturedStartBits.size(); ++i)
			appendStackFloatCopy(&nativeShotTraceCapturedStartBits[i], static_cast<uint8_t>(0x10 + i * sizeof(float)));
		for (size_t i = 0; i < nativeShotTraceCapturedTargetBits.size(); ++i)
			appendStackFloatCopy(&nativeShotTraceCapturedTargetBits[i], static_cast<uint8_t>(i * sizeof(float)));

		code.push_back(0x49); code.push_back(0xBA);
		AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceOriginOverrideEnabled));
		code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x00 }); // cmp dword [r10],0
		const size_t noOverrideOffset = AppendNearJcc(code, 0x84); // je capture unchanged args

		for (size_t i = 0; i < nativeShotTraceOriginOverrideBits.size(); ++i)
			appendOverrideFloatCopy(&nativeShotTraceOriginOverrideBits[i], static_cast<uint8_t>(0x10 + i * sizeof(float)));
		for (size_t i = 0; i < nativeShotTraceTargetOverrideBits.size(); ++i)
			appendOverrideFloatCopy(&nativeShotTraceTargetOverrideBits[i], static_cast<uint8_t>(i * sizeof(float)));
		appendStoreImmediate(&nativeShotTraceCapturedOverride, 1);
		const size_t jumpToCaptureAppliedOffset = code.size();
		code.push_back(0xE9);
		AppendU32(code, 0);

		const size_t captureUnchangedOffset = code.size();
		appendStoreImmediate(&nativeShotTraceCapturedOverride, 0);
		const size_t captureAppliedOffset = code.size();
		for (size_t i = 0; i < nativeShotTraceAppliedStartBits.size(); ++i)
			appendStackFloatCopy(&nativeShotTraceAppliedStartBits[i], static_cast<uint8_t>(0x10 + i * sizeof(float)));
		for (size_t i = 0; i < nativeShotTraceAppliedTargetBits.size(); ++i)
			appendStackFloatCopy(&nativeShotTraceAppliedTargetBits[i], static_cast<uint8_t>(i * sizeof(float)));
		appendIncrement(&nativeShotTraceSequence);
		AppendPopGuard(code);
		code.insert(code.end(), originalBytes.begin(), originalBytes.end());
		if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
			return std::vector<uint8_t>{};

		if (!PatchNearJcc(code, noOverrideOffset, captureUnchangedOffset)
			|| !patchNearJmp(jumpToCaptureAppliedOffset, captureAppliedOffset))
			return std::vector<uint8_t>{};
		return code;
		});
#endif

	// This trace boundary is the authoritative point where the native line trace
	// consumes the shot start/end pointers. Filter by the three verified player-
	// fire return sites instead of guessing which register contains the player.
	installHookAt("Player native trace entry redirect",
		baseAddressGameEXE + 0x13F89A0,
		7,
		[this](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes) {
			if (originalBytes.size() != 7)
				return std::vector<uint8_t>{};

			std::vector<uint8_t> code;
			code.push_back(0x9C); // pushfq; original entry starts with flag-neutral movs
			code.insert(code.end(), { 0x4C, 0x8B, 0x5C, 0x24, 0x08 }); // mov r11,[rsp+8] (caller return address)

			std::vector<size_t> playerCallJccOffsets;
			const uint32_t playerTraceReturnRvas[] = {
				0x13EF6C0,
				0x13EFE06,
				0x13F0573
			};
			for (const uint32_t playerTraceReturnRva : playerTraceReturnRvas)
			{
				code.push_back(0x49); code.push_back(0xBA); AppendU64(code, baseAddressGameEXE + playerTraceReturnRva); // mov r10,return site
				code.insert(code.end(), { 0x4D, 0x39, 0xD3 }); // cmp r11,r10
				playerCallJccOffsets.push_back(AppendNearJcc(code, 0x84)); // je player gate
			}

			// The vehicle fire path reaches this same trace entry with the verified
			// return RVA 0x13F5CD8. Only a fresh engine-thread vehicle snapshot may
			// open this exact caller; all other callers still fall through unchanged.
			const uintptr_t vehicleTraceReturnSite = baseAddressGameEXE + 0x13F5CD8;
			code.push_back(0x49); code.push_back(0xBA); AppendU64(code, vehicleTraceReturnSite);
			code.insert(code.end(), { 0x4D, 0x39, 0xD3 }); // cmp r11,r10
			const size_t vehicleMatchJccOffset = AppendShortJcc(code, 0x74); // je vehicle gate
			const size_t vehicleNonMatchJumpOffset = code.size();
			code.push_back(0xE9);
			AppendU32(code, 0);
			// While vehicle free aim owns the snapshot, no ordinary on-foot caller
			// may consume it. The one-shot arm is reserved for the exact vehicle gate.
			const size_t playerGateOffset = code.size();
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceVehicleModeActive));
			code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x00 }); // cmp dword [r10],0
			const size_t playerVehicleModeOffset = AppendNearJcc(code, 0x85); // jne skip
			// Stamp the accepted on-foot caller explicitly. R11 still contains the
			// absolute return address captured at entry; convert it to an RVA.
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, baseAddressGameEXE);
			code.insert(code.end(), { 0x4D, 0x29, 0xD3 }); // sub r11,r10
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceCallSiteRva));
			code.insert(code.end(), { 0x45, 0x89, 0x1A }); // mov [r10],r11d
			const size_t playerJumpToCaptureOffset = code.size();
			code.push_back(0xE9);
			AppendU32(code, 0);
			const size_t vehicleGateOffset = code.size();
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceVehicleOverrideArmed));
			code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x00 }); // cmp dword [r10],0
			const size_t vehicleNotArmedOffset = AppendNearJcc(code, 0x84); // je skip non-player
			// Consume the one-shot authorization at the proven vehicle call boundary.
			code.push_back(0x41); code.push_back(0xC7); code.push_back(0x02);
			AppendU32(code, 0);
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceCallSiteRva));
			code.push_back(0x41); code.push_back(0xC7); code.push_back(0x02);
			AppendU32(code, 0x13F5CD8);
			const size_t vehicleJumpToCaptureOffset = code.size();
			code.push_back(0xE9);
			AppendU32(code, 0);

			const size_t skipNonPlayerOffset = code.size();
			code.push_back(0xE9);
			AppendU32(code, 0);
			const size_t captureOffset = code.size();

			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceOriginOverrideEnabled));
			code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x00 }); // cmp dword [r10],0
			const size_t overrideDisabledOffset = AppendNearJcc(code, 0x84); // je restore
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceOverrideConsumed));
			code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x00 }); // cmp dword [r10],0
			const size_t overrideAlreadyConsumedOffset = AppendNearJcc(code, 0x85); // jne restore
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTracePublishSequence));
			code.insert(code.end(), { 0x45, 0x8B, 0x1A }); // mov r11d,[r10]
			code.insert(code.end(), { 0x41, 0xF6, 0xC3, 0x01 }); // test r11b,1
			const size_t publishInProgressOffset = AppendNearJcc(code, 0x85); // jne restore

			auto appendCapturePointerFloat = [&code](volatile LONG* destination, uint8_t sourceModRM, uint8_t displacement) {
				code.push_back(0x49); code.push_back(0xBA); AppendU64(code, reinterpret_cast<uintptr_t>(destination));
				code.insert(code.end(), { 0x8B, sourceModRM, displacement }); // mov eax,[rcx/rdx+disp]
				code.insert(code.end(), { 0x41, 0x89, 0x02 }); // mov [r10],eax
			};
			auto appendOverridePointerFloat = [&code](volatile LONG* source, uint8_t destinationModRM, uint8_t displacement) {
				code.push_back(0x49); code.push_back(0xBA); AppendU64(code, reinterpret_cast<uintptr_t>(source));
				code.insert(code.end(), { 0x41, 0x8B, 0x02 }); // mov eax,[r10]
				code.insert(code.end(), { 0x89, destinationModRM, displacement }); // mov [rcx/rdx+disp],eax
			};
			auto appendStoreImmediate = [&code](volatile LONG* destination, uint32_t value) {
				code.push_back(0x49); code.push_back(0xBA); AppendU64(code, reinterpret_cast<uintptr_t>(destination));
				code.push_back(0x41); code.push_back(0xC7); code.push_back(0x02); AppendU32(code, value);
			};
			auto appendCopyDword = [&code](volatile LONG* source, volatile LONG* destination) {
				code.push_back(0x49); code.push_back(0xBA); AppendU64(code, reinterpret_cast<uintptr_t>(source));
				code.insert(code.end(), { 0x41, 0x8B, 0x02 }); // mov eax,[r10]
				code.push_back(0x49); code.push_back(0xBA); AppendU64(code, reinterpret_cast<uintptr_t>(destination));
				code.insert(code.end(), { 0x41, 0x89, 0x02 }); // mov [r10],eax
			};

			// At the trace entry RCX=&start and RDX=&target.  Capture the exact
			// native pair before changing it, then replace both pointers atomically
			// from the published mock gunflash origin/target.
			for (size_t i = 0; i < nativeShotTraceCapturedStartBits.size(); ++i)
				appendCapturePointerFloat(&nativeShotTraceCapturedStartBits[i], 0x41, static_cast<uint8_t>(i * sizeof(float)));
			for (size_t i = 0; i < nativeShotTraceCapturedTargetBits.size(); ++i)
				appendCapturePointerFloat(&nativeShotTraceCapturedTargetBits[i], 0x42, static_cast<uint8_t>(i * sizeof(float)));

			for (size_t i = 0; i < nativeShotTraceOriginOverrideBits.size(); ++i)
				appendOverridePointerFloat(&nativeShotTraceOriginOverrideBits[i], 0x41, static_cast<uint8_t>(i * sizeof(float)));
			for (size_t i = 0; i < nativeShotTraceTargetOverrideBits.size(); ++i)
				appendOverridePointerFloat(&nativeShotTraceTargetOverrideBits[i], 0x42, static_cast<uint8_t>(i * sizeof(float)));

			// Re-read the publish sequence after copying. If C++ updated either
			// vector while this cave was running, restore the caller's native pair.
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTracePublishSequence));
			code.insert(code.end(), { 0x41, 0x8B, 0x02 }); // mov eax,[r10]
			code.insert(code.end(), { 0x44, 0x39, 0xD8 }); // cmp eax,r11d
			const size_t publishChangedOffset = AppendNearJcc(code, 0x85); // jne restore native
			code.insert(code.end(), { 0xA8, 0x01 }); // test al,1
			const size_t publishBecameOddOffset = AppendNearJcc(code, 0x85); // jne restore native

			appendStoreImmediate(&nativeShotTraceCapturedOverride, 1);
			appendStoreImmediate(&nativeShotTraceOverrideConsumed, 1);
			for (size_t i = 0; i < nativeShotTraceAppliedStartBits.size(); ++i)
				appendCapturePointerFloat(&nativeShotTraceAppliedStartBits[i], 0x41, static_cast<uint8_t>(i * sizeof(float)));
			for (size_t i = 0; i < nativeShotTraceAppliedTargetBits.size(); ++i)
				appendCapturePointerFloat(&nativeShotTraceAppliedTargetBits[i], 0x42, static_cast<uint8_t>(i * sizeof(float)));

			// Freeze the exact damaging-ray snapshot for the following visual tracer.
			// Publish pending last so the tracer hook cannot observe a partial copy.
			for (size_t i = 0; i < nativeShotTrailOriginBits.size(); ++i)
				appendCopyDword(&nativeShotTraceAppliedStartBits[i], &nativeShotTrailOriginBits[i]);
			for (size_t i = 0; i < nativeShotTrailTargetBits.size(); ++i)
				appendCopyDword(&nativeShotTraceAppliedTargetBits[i], &nativeShotTrailTargetBits[i]);
			appendStoreImmediate(&nativeShotTrailPending, 1);

			code.push_back(0x49); code.push_back(0xBA); AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceSequence));
			code.insert(code.end(), { 0xF0, 0x41, 0xFF, 0x02 }); // lock inc dword [r10]
			const size_t stableJumpOffset = code.size();
			code.push_back(0xE9);
			AppendU32(code, 0);

			const size_t restoreNativeOffset = code.size();
			for (size_t i = 0; i < nativeShotTraceCapturedStartBits.size(); ++i)
				appendOverridePointerFloat(&nativeShotTraceCapturedStartBits[i], 0x41, static_cast<uint8_t>(i * sizeof(float)));
			for (size_t i = 0; i < nativeShotTraceCapturedTargetBits.size(); ++i)
				appendOverridePointerFloat(&nativeShotTraceCapturedTargetBits[i], 0x42, static_cast<uint8_t>(i * sizeof(float)));
			appendStoreImmediate(&nativeShotTraceCapturedOverride, 0);

			const size_t restoreOffset = code.size();
			code.push_back(0x9D); // popfq
			code.insert(code.end(), originalBytes.begin(), originalBytes.end());
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
				return std::vector<uint8_t>{};

			auto patchNearJmp = [&code, caveAddress](size_t instructionOffset, size_t targetOffset) {
				const int64_t displacement = static_cast<int64_t>(caveAddress + targetOffset)
					- static_cast<int64_t>(caveAddress + instructionOffset + 5);
				if (displacement < std::numeric_limits<int32_t>::min()
					|| displacement > std::numeric_limits<int32_t>::max())
					return false;
				const int32_t relative = static_cast<int32_t>(displacement);
				std::memcpy(code.data() + instructionOffset + 1, &relative, sizeof(relative));
				return true;
			};

			if (!patchNearJmp(vehicleNonMatchJumpOffset, skipNonPlayerOffset)
				|| !patchNearJmp(playerJumpToCaptureOffset, captureOffset)
				|| !patchNearJmp(vehicleJumpToCaptureOffset, captureOffset)
				|| !patchNearJmp(skipNonPlayerOffset, restoreOffset)
				|| !patchNearJmp(stableJumpOffset, restoreOffset)
				|| !PatchNearJcc(code, overrideDisabledOffset, restoreOffset)
				|| !PatchNearJcc(code, overrideAlreadyConsumedOffset, restoreOffset)
				|| !PatchNearJcc(code, publishInProgressOffset, restoreOffset)
				|| !PatchNearJcc(code, publishChangedOffset, restoreNativeOffset)
				|| !PatchNearJcc(code, publishBecameOddOffset, restoreNativeOffset)
				|| !PatchShortJcc(code, vehicleMatchJccOffset, vehicleGateOffset)
				|| !PatchNearJcc(code, vehicleNotArmedOffset, skipNonPlayerOffset)
				|| !PatchNearJcc(code, playerVehicleModeOffset, skipNonPlayerOffset))
				return std::vector<uint8_t>{};
			for (const size_t playerCallJccOffset : playerCallJccOffsets)
			{
				if (!PatchNearJcc(code, playerCallJccOffset, playerGateOffset))
					return std::vector<uint8_t>{};
			}
			return code;
		});

	// Definitive Edition's visible firearm-tracer entry. It randomizes a short
	// segment along start->hit, stores it through 0x127F9F0, and GTAWater later
	// renders that stored segment through BulletTraceMesh.
	installHookAt("Player bullet-tracer origin redirect",
		baseAddressGameEXE + 0x127F6A0,
		9,
		[this](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes) {
			const std::vector<uint8_t> expectedBytes{
				0x40, 0x53,
				0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00
			};
			if (originalBytes != expectedBytes)
				return std::vector<uint8_t>{};

			std::vector<uint8_t> code;
			code.push_back(0x9C); // pushfq
			code.push_back(0x50); // push rax
			code.insert(code.end(), { 0x41, 0x52 }); // push r10
			code.insert(code.end(), { 0x41, 0x53 }); // push r11

			auto appendCapturePointerFloat = [&code](volatile LONG* destination,
				uint8_t sourceModRM, uint8_t displacement) {
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(destination));
				code.insert(code.end(), { 0x8B, sourceModRM, displacement }); // mov eax,[rcx/rdx+disp]
				code.insert(code.end(), { 0x41, 0x89, 0x02 }); // mov [r10],eax
			};
			auto appendStoreImmediate = [&code](volatile LONG* destination, uint32_t value) {
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(destination));
				code.push_back(0x41); code.push_back(0xC7); code.push_back(0x02);
				AppendU32(code, value);
			};

			// R9 is firedBy at this entry. Reject NPC, vehicle, mission and utility
			// traces before reading or changing either vector argument.
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&cachedPlayerPointer));
			code.insert(code.end(), { 0x4D, 0x8B, 0x1A }); // mov r11,[r10]
			code.insert(code.end(), { 0x4D, 0x39, 0xD9 }); // cmp r9,r11
			const size_t notLocalPlayerOffset = AppendNearJcc(code, 0x85); // jne resume
			appendStoreImmediate(&nativeShotEffectOwnerLocal, 1);

			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTrailPending));
			code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x00 }); // cmp dword [r10],0
			const size_t noPendingShotOffset = AppendNearJcc(code, 0x84); // je resume
			code.insert(code.end(), { 0x48, 0x85, 0xC9 }); // test rcx,rcx
			const size_t nullStartOffset = AppendNearJcc(code, 0x84); // je resume
			code.insert(code.end(), { 0x48, 0x85, 0xD2 }); // test rdx,rdx
			const size_t nullEndOffset = AppendNearJcc(code, 0x84); // je resume

			for (size_t i = 0; i < nativeShotEffectCapturedStartBits.size(); ++i)
				appendCapturePointerFloat(&nativeShotEffectCapturedStartBits[i], 0x41,
					static_cast<uint8_t>(i * sizeof(float)));
			for (size_t i = 0; i < nativeShotEffectCapturedTargetBits.size(); ++i)
				appendCapturePointerFloat(&nativeShotEffectCapturedTargetBits[i], 0x42,
					static_cast<uint8_t>(i * sizeof(float)));

			// Preserve the game's hit endpoint, but originate its complete line from
			// the same immutable mock muzzle consumed by the damaging ray.
			code.push_back(0x48); code.push_back(0xB9);
			AppendU64(code, reinterpret_cast<uintptr_t>(nativeShotTrailOriginBits.data())); // mov rcx,imm64
			appendStoreImmediate(&nativeShotEffectCapturedOverride, 1);
			appendStoreImmediate(&nativeShotEffectCallSiteRva, 0x127F6A0);

			for (size_t i = 0; i < nativeShotEffectAppliedStartBits.size(); ++i)
				appendCapturePointerFloat(&nativeShotEffectAppliedStartBits[i], 0x41,
					static_cast<uint8_t>(i * sizeof(float)));
			for (size_t i = 0; i < nativeShotEffectAppliedTargetBits.size(); ++i)
				appendCapturePointerFloat(&nativeShotEffectAppliedTargetBits[i], 0x42,
					static_cast<uint8_t>(i * sizeof(float)));
			appendStoreImmediate(&nativeShotTrailPending, 0);
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotEffectSequence));
			code.insert(code.end(), { 0xF0, 0x41, 0xFF, 0x02 }); // lock inc dword [r10]

			const size_t resumeOffset = code.size();
			code.insert(code.end(), { 0x41, 0x5B }); // pop r11
			code.insert(code.end(), { 0x41, 0x5A }); // pop r10
			code.push_back(0x58); // pop rax
			code.push_back(0x9D); // popfq
			code.insert(code.end(), originalBytes.begin(), originalBytes.end());
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
				return std::vector<uint8_t>{};

			return PatchNearJcc(code, notLocalPlayerOffset, resumeOffset)
				&& PatchNearJcc(code, noPendingShotOffset, resumeOffset)
				&& PatchNearJcc(code, nullStartOffset, resumeOffset)
				&& PatchNearJcc(code, nullEndOffset, resumeOffset)
				? code
				: std::vector<uint8_t>{};
		});

	// Disabled: this target-entry route produced no probes in the active player
	// bullet path. The verified payload callsites below own the visible trail.
#if 0
	// The DE firing path passes another start/end pair to 0x13F6BA0 after the
	// collision trace. That routine dereferences R8/R9 as positions and is a
	// separate weapon-effect/tracer path. Redirect only the two player-fire
	// return sites observed in the current executable; all other callers keep
	// their original registers and behavior.
	installHookAt("Player native weapon-effect/tracer origin redirect",
		baseAddressGameEXE + 0x13F6BA0,
		5,
		[this](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes) {
			if (originalBytes.size() != 5)
				return std::vector<uint8_t>{};

			std::vector<uint8_t> code;
			code.push_back(0x9C); // pushfq
			code.insert(code.end(), { 0x4C, 0x8B, 0x5C, 0x24, 0x08 }); // mov r11,[rsp+8]

			const uintptr_t playerEffectReturnSites[] = {
				baseAddressGameEXE + 0x13F0080,
				baseAddressGameEXE + 0x13F5A39
			};
			std::vector<size_t> playerCallJccOffsets;
			for (const uintptr_t playerEffectReturnSite : playerEffectReturnSites)
			{
				code.push_back(0x49); code.push_back(0xBA); AppendU64(code, playerEffectReturnSite);
				code.insert(code.end(), { 0x4D, 0x39, 0xD3 }); // cmp r11,r10
				playerCallJccOffsets.push_back(AppendShortJcc(code, 0x74)); // je set call-site
			}

			const size_t noCallSiteJumpOffset = code.size();
			code.push_back(0xE9);
			AppendU32(code, 0);

			std::vector<size_t> setCallSiteOffsets;
			std::vector<size_t> setCallSiteJumpOffsets;
			for (const uintptr_t playerEffectReturnSite : playerEffectReturnSites)
			{
				setCallSiteOffsets.push_back(code.size());
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotEffectCallSiteRva));
				code.push_back(0x41); code.push_back(0xC7); code.push_back(0x02);
				AppendU32(code, static_cast<uint32_t>(playerEffectReturnSite - baseAddressGameEXE));
				setCallSiteJumpOffsets.push_back(code.size());
				code.push_back(0xE9);
				AppendU32(code, 0);
			}

			const size_t captureOffset = code.size();
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceOriginOverrideEnabled));
			code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x00 }); // cmp dword [r10],0
			const size_t overrideDisabledOffset = AppendNearJcc(code, 0x84); // je restore

			auto appendCaptureR8R9Float = [&code](volatile LONG* destination, uint8_t sourceModRM, uint8_t displacement) {
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(destination));
				code.insert(code.end(), { 0x41, 0x8B, sourceModRM, displacement }); // mov eax,[r8/r9+disp]
				code.insert(code.end(), { 0x41, 0x89, 0x02 }); // mov [r10],eax
			};
			auto appendSetR8R9Pointer = [&code](uint8_t registerOpcode, const std::array<volatile LONG, 3>& source) {
				code.push_back(0x49); code.push_back(registerOpcode);
				AppendU64(code, reinterpret_cast<uintptr_t>(source.data()));
			};
			auto appendStoreImmediate = [&code](volatile LONG* destination, uint32_t value) {
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(destination));
				code.push_back(0x41); code.push_back(0xC7); code.push_back(0x02);
				AppendU32(code, value);
			};
			auto appendIncrement = [&code](volatile LONG* destination) {
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(destination));
				code.insert(code.end(), { 0xF0, 0x41, 0xFF, 0x02 }); // lock inc dword [r10]
			};

			// Capture the pair the game supplied before replacing R8/R9.
			for (size_t i = 0; i < nativeShotEffectCapturedStartBits.size(); ++i)
				appendCaptureR8R9Float(&nativeShotEffectCapturedStartBits[i], 0x40, static_cast<uint8_t>(i * sizeof(float)));
			for (size_t i = 0; i < nativeShotEffectCapturedTargetBits.size(); ++i)
				appendCaptureR8R9Float(&nativeShotEffectCapturedTargetBits[i], 0x41, static_cast<uint8_t>(i * sizeof(float)));

			// R8/R9 are pointers to the native effect start/end vectors. Point them
			// at the same coherent mock muzzle pair used by the collision trace.
			appendSetR8R9Pointer(0xB8, nativeShotTraceOriginOverrideBits); // mov r8, imm64
			appendSetR8R9Pointer(0xB9, nativeShotTraceTargetOverrideBits); // mov r9, imm64
			appendStoreImmediate(&nativeShotEffectCapturedOverride, 1);

			for (size_t i = 0; i < nativeShotEffectAppliedStartBits.size(); ++i)
				appendCaptureR8R9Float(&nativeShotEffectAppliedStartBits[i], 0x40, static_cast<uint8_t>(i * sizeof(float)));
			for (size_t i = 0; i < nativeShotEffectAppliedTargetBits.size(); ++i)
				appendCaptureR8R9Float(&nativeShotEffectAppliedTargetBits[i], 0x41, static_cast<uint8_t>(i * sizeof(float)));
			appendIncrement(&nativeShotEffectSequence);

			const size_t restoreOffset = code.size();
			code.push_back(0x9D); // popfq
			code.insert(code.end(), originalBytes.begin(), originalBytes.end());
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
				return std::vector<uint8_t>{};

			auto patchNearJmp = [&code, caveAddress](size_t instructionOffset, size_t targetOffset) {
				const int64_t displacement = static_cast<int64_t>(caveAddress + targetOffset)
					- static_cast<int64_t>(caveAddress + instructionOffset + 5);
				if (displacement < std::numeric_limits<int32_t>::min()
					|| displacement > std::numeric_limits<int32_t>::max())
					return false;
				const int32_t relative = static_cast<int32_t>(displacement);
				std::memcpy(code.data() + instructionOffset + 1, &relative, sizeof(relative));
				return true;
			};

			if (!patchNearJmp(noCallSiteJumpOffset, restoreOffset)
				|| !PatchNearJcc(code, overrideDisabledOffset, restoreOffset))
				return std::vector<uint8_t>{};
			for (size_t i = 0; i < playerCallJccOffsets.size(); ++i)
			{
				if (!PatchShortJcc(code, playerCallJccOffsets[i], setCallSiteOffsets[i])
					|| !patchNearJmp(setCallSiteJumpOffsets[i], captureOffset))
					return std::vector<uint8_t>{};
			}
			return code;
		});

	// The target-entry experiment above installed but produced no probes. The
	// executable contains two verified direct call instructions to that routine;
	// redirect those callsites instead so the native R8/R9 arguments are changed
	// before the callee is entered and every invocation is observable.
	auto installNativeWeaponEffectCallsite = [this, &installHookAt](uintptr_t callSite, uint32_t callSiteRva) {
		return installHookAt("Player native weapon-effect/tracer callsite redirect",
			callSite,
			5,
			[this, callSiteRva](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes) {
				if (originalBytes.size() != 5 || originalBytes[0] != 0xE8)
					return std::vector<uint8_t>{};

				int32_t originalCallDisplacement = 0;
				std::memcpy(&originalCallDisplacement, originalBytes.data() + 1, sizeof(originalCallDisplacement));
				const uintptr_t originalCallTarget = static_cast<uintptr_t>(
					static_cast<int64_t>(returnAddress) + originalCallDisplacement);
				if (originalCallTarget != baseAddressGameEXE + 0x13F6BA0)
					return std::vector<uint8_t>{};

				std::vector<uint8_t> code;
				code.push_back(0x9C); // pushfq

				auto appendCaptureR8R9Float = [&code](volatile LONG* destination, uint8_t sourceModRM, uint8_t displacement) {
					code.push_back(0x49); code.push_back(0xBA);
					AppendU64(code, reinterpret_cast<uintptr_t>(destination));
					code.insert(code.end(), { 0x41, 0x8B, sourceModRM, displacement }); // mov eax,[r8/r9+disp]
					code.insert(code.end(), { 0x41, 0x89, 0x02 }); // mov [r10],eax
				};
				auto appendSetR8R9Pointer = [&code](uint8_t registerOpcode, const std::array<volatile LONG, 3>& source) {
					code.push_back(0x49); code.push_back(registerOpcode);
					AppendU64(code, reinterpret_cast<uintptr_t>(source.data()));
				};
				auto appendStoreImmediate = [&code](volatile LONG* destination, uint32_t value) {
					code.push_back(0x49); code.push_back(0xBA);
					AppendU64(code, reinterpret_cast<uintptr_t>(destination));
					code.push_back(0x41); code.push_back(0xC7); code.push_back(0x02);
					AppendU32(code, value);
				};
				auto appendIncrement = [&code](volatile LONG* destination) {
					code.push_back(0x49); code.push_back(0xBA);
					AppendU64(code, reinterpret_cast<uintptr_t>(destination));
					code.insert(code.end(), { 0xF0, 0x41, 0xFF, 0x02 }); // lock inc dword [r10]
				};

				for (size_t i = 0; i < nativeShotEffectCapturedStartBits.size(); ++i)
					appendCaptureR8R9Float(&nativeShotEffectCapturedStartBits[i], 0x40, static_cast<uint8_t>(i * sizeof(float)));
				for (size_t i = 0; i < nativeShotEffectCapturedTargetBits.size(); ++i)
					appendCaptureR8R9Float(&nativeShotEffectCapturedTargetBits[i], 0x41, static_cast<uint8_t>(i * sizeof(float)));
				appendStoreImmediate(&nativeShotEffectCallSiteRva, callSiteRva);

				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceOriginOverrideEnabled));
				code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x00 }); // cmp dword [r10],0
				const size_t overrideDisabledOffset = AppendNearJcc(code, 0x84); // je unchanged

				appendSetR8R9Pointer(0xB8, nativeShotTraceOriginOverrideBits); // mov r8, imm64
				appendSetR8R9Pointer(0xB9, nativeShotTraceTargetOverrideBits); // mov r9, imm64
				appendStoreImmediate(&nativeShotEffectCapturedOverride, 1);
				const size_t jumpToAppliedOffset = code.size();
				code.push_back(0xE9);
				AppendU32(code, 0);

				const size_t unchangedOffset = code.size();
				appendStoreImmediate(&nativeShotEffectCapturedOverride, 0);
				const size_t appliedOffset = code.size();
				for (size_t i = 0; i < nativeShotEffectAppliedStartBits.size(); ++i)
					appendCaptureR8R9Float(&nativeShotEffectAppliedStartBits[i], 0x40, static_cast<uint8_t>(i * sizeof(float)));
				for (size_t i = 0; i < nativeShotEffectAppliedTargetBits.size(); ++i)
					appendCaptureR8R9Float(&nativeShotEffectAppliedTargetBits[i], 0x41, static_cast<uint8_t>(i * sizeof(float)));
				appendIncrement(&nativeShotEffectSequence);

				code.push_back(0x9D); // popfq
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, originalCallTarget);
				code.insert(code.end(), { 0x41, 0xFF, 0xD2 }); // call r10
				if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
					return std::vector<uint8_t>{};

				auto patchNearJmp = [&code, caveAddress](size_t instructionOffset, size_t targetOffset) {
					const int64_t displacement = static_cast<int64_t>(caveAddress + targetOffset)
						- static_cast<int64_t>(caveAddress + instructionOffset + 5);
					if (displacement < std::numeric_limits<int32_t>::min()
						|| displacement > std::numeric_limits<int32_t>::max())
						return false;
					const int32_t relative = static_cast<int32_t>(displacement);
					std::memcpy(code.data() + instructionOffset + 1, &relative, sizeof(relative));
					return true;
				};

				return PatchNearJcc(code, overrideDisabledOffset, unchangedOffset)
					&& patchNearJmp(jumpToAppliedOffset, appliedOffset)
					? code
					: std::vector<uint8_t>{};
			});
	};
#endif

	// Disabled: these post-trace payload calls were reached and rewritten, but
	// they did not control the visible tracer origin in play testing.
#if 0
	// The active player bullet path does not call 0x13F6BA0.  After the player
	// trace it builds the native trail/effect payload in the caller and passes
	// that payload to 0x1197F20.  Its first vector is at +20h and its second
	// vector is at +2Ch; these are the actual start/end positions consumed by
	// the native visual trail.  Redirect the payload itself so the visual and
	// collision paths use the same mock gunflash origin/target.
	auto installNativeBulletTrailCallsite = [this, &installHookAt](uintptr_t callSite, uint32_t callSiteRva) {
		return installHookAt("Player native bullet-trail payload redirect",
			callSite,
			5,
			[this, callSiteRva](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes) {
				if (originalBytes.size() != 5 || originalBytes[0] != 0xE8)
					return std::vector<uint8_t>{};

				int32_t originalCallDisplacement = 0;
				std::memcpy(&originalCallDisplacement, originalBytes.data() + 1, sizeof(originalCallDisplacement));
				const uintptr_t originalCallTarget = static_cast<uintptr_t>(
					static_cast<int64_t>(returnAddress) + originalCallDisplacement);
				if (originalCallTarget != baseAddressGameEXE + 0x1197F20)
					return std::vector<uint8_t>{};

				std::vector<uint8_t> code;
				code.push_back(0x9C); // pushfq; restore before executing the original call

				auto appendCaptureStructFloat = [&code](volatile LONG* destination, uint8_t sourceOffset) {
					code.push_back(0x49); code.push_back(0xBA);
					AppendU64(code, reinterpret_cast<uintptr_t>(destination));
					code.insert(code.end(), { 0x8B, 0x42, sourceOffset }); // mov eax,[rdx+offset]
					code.insert(code.end(), { 0x41, 0x89, 0x02 }); // mov [r10],eax
				};
				auto appendOverrideStructFloat = [&code](volatile LONG* source, uint8_t destinationOffset) {
					code.push_back(0x49); code.push_back(0xBA);
					AppendU64(code, reinterpret_cast<uintptr_t>(source));
					code.insert(code.end(), { 0x41, 0x8B, 0x02 }); // mov eax,[r10]
					code.insert(code.end(), { 0x89, 0x42, destinationOffset }); // mov [rdx+offset],eax
				};
				auto appendStoreImmediate = [&code](volatile LONG* destination, uint32_t value) {
					code.push_back(0x49); code.push_back(0xBA);
					AppendU64(code, reinterpret_cast<uintptr_t>(destination));
					code.push_back(0x41); code.push_back(0xC7); code.push_back(0x02);
					AppendU32(code, value);
				};
				auto appendIncrement = [&code](volatile LONG* destination) {
					code.push_back(0x49); code.push_back(0xBA);
					AppendU64(code, reinterpret_cast<uintptr_t>(destination));
					code.insert(code.end(), { 0xF0, 0x41, 0xFF, 0x02 }); // lock inc dword [r10]
				};

				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceOriginOverrideEnabled));
				code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x00 }); // cmp dword [r10],0
				const size_t overrideDisabledOffset = AppendNearJcc(code, 0x84); // je unchanged
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceOverrideConsumed));
				code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x00 }); // cmp dword [r10],0
				const size_t traceNotConsumedOffset = AppendNearJcc(code, 0x84); // je unchanged
				code.insert(code.end(), { 0x48, 0x85, 0xD2 }); // test rdx,rdx
				const size_t nullPayloadOffset = AppendNearJcc(code, 0x84); // je unchanged

				for (size_t i = 0; i < nativeShotEffectCapturedStartBits.size(); ++i)
					appendCaptureStructFloat(&nativeShotEffectCapturedStartBits[i], static_cast<uint8_t>(0x20 + i * sizeof(float)));
				for (size_t i = 0; i < nativeShotEffectCapturedTargetBits.size(); ++i)
					appendCaptureStructFloat(&nativeShotEffectCapturedTargetBits[i], static_cast<uint8_t>(0x2C + i * sizeof(float)));

				for (size_t i = 0; i < nativeShotTraceOriginOverrideBits.size(); ++i)
					appendOverrideStructFloat(&nativeShotTraceOriginOverrideBits[i], static_cast<uint8_t>(0x20 + i * sizeof(float)));
				for (size_t i = 0; i < nativeShotTraceTargetOverrideBits.size(); ++i)
					appendOverrideStructFloat(&nativeShotTraceTargetOverrideBits[i], static_cast<uint8_t>(0x2C + i * sizeof(float)));

				appendStoreImmediate(&nativeShotEffectCapturedOverride, 1);
				for (size_t i = 0; i < nativeShotEffectAppliedStartBits.size(); ++i)
					appendCaptureStructFloat(&nativeShotEffectAppliedStartBits[i], static_cast<uint8_t>(0x20 + i * sizeof(float)));
				for (size_t i = 0; i < nativeShotEffectAppliedTargetBits.size(); ++i)
					appendCaptureStructFloat(&nativeShotEffectAppliedTargetBits[i], static_cast<uint8_t>(0x2C + i * sizeof(float)));
				appendStoreImmediate(&nativeShotEffectCallSiteRva, callSiteRva);
				appendIncrement(&nativeShotEffectSequence);

				const size_t unchangedOffset = code.size();
				code.push_back(0x9D); // popfq
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, originalCallTarget);
				code.insert(code.end(), { 0x41, 0xFF, 0xD2 }); // call r10
				if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
					return std::vector<uint8_t>{};

				return PatchNearJcc(code, overrideDisabledOffset, unchangedOffset)
					&& PatchNearJcc(code, traceNotConsumedOffset, unchangedOffset)
					&& PatchNearJcc(code, nullPayloadOffset, unchangedOffset)
					? code
					: std::vector<uint8_t>{};
			});
	};

	installNativeBulletTrailCallsite(baseAddressGameEXE + 0x13F0666, 0x13F0666);
	installNativeBulletTrailCallsite(baseAddressGameEXE + 0x13F070D, 0x13F070D);
#endif

	// Disabled: this conditional call exists beside a player trace callsite, but
	// runtime testing produced zero probes across 83 verified player shots.
#if 0
	// This verified player-fire call runs before the collision trace and passes
	// the native start/target pair to 0x13F5E30 in RDX/R8. That callee consumes
	// the pair immediately while constructing downstream firing visuals. Give it
	// the same immutable mock-muzzle pair that the following trace will consume.
	installHookAt("Player pre-trace visual origin redirect",
		baseAddressGameEXE + 0x13F0515,
		5,
		[this](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes) {
			if (originalBytes.size() != 5 || originalBytes[0] != 0xE8)
				return std::vector<uint8_t>{};

			int32_t originalCallDisplacement = 0;
			std::memcpy(&originalCallDisplacement, originalBytes.data() + 1, sizeof(originalCallDisplacement));
			const uintptr_t originalCallTarget = static_cast<uintptr_t>(
				static_cast<int64_t>(returnAddress) + originalCallDisplacement);
			if (originalCallTarget != baseAddressGameEXE + 0x13F5E30)
				return std::vector<uint8_t>{};

			std::vector<uint8_t> code;
			code.push_back(0x9C); // pushfq; restore before executing the original call

			auto appendCapturePointerFloat = [&code](volatile LONG* destination, bool fromR8, uint8_t displacement) {
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(destination));
				if (fromR8)
					code.insert(code.end(), { 0x41, 0x8B, 0x40, displacement }); // mov eax,[r8+disp]
				else
					code.insert(code.end(), { 0x8B, 0x42, displacement }); // mov eax,[rdx+disp]
				code.insert(code.end(), { 0x41, 0x89, 0x02 }); // mov [r10],eax
			};
			auto appendStoreImmediate = [&code](volatile LONG* destination, uint32_t value) {
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(destination));
				code.push_back(0x41); code.push_back(0xC7); code.push_back(0x02);
				AppendU32(code, value);
			};
			auto appendIncrement = [&code](volatile LONG* destination) {
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(destination));
				code.insert(code.end(), { 0xF0, 0x41, 0xFF, 0x02 }); // lock inc dword [r10]
			};

			for (size_t i = 0; i < nativeShotEffectCapturedStartBits.size(); ++i)
				appendCapturePointerFloat(&nativeShotEffectCapturedStartBits[i], false, static_cast<uint8_t>(i * sizeof(float)));
			for (size_t i = 0; i < nativeShotEffectCapturedTargetBits.size(); ++i)
				appendCapturePointerFloat(&nativeShotEffectCapturedTargetBits[i], true, static_cast<uint8_t>(i * sizeof(float)));
			appendStoreImmediate(&nativeShotEffectCallSiteRva, 0x13F0515);

			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceOriginOverrideEnabled));
			code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x00 }); // cmp dword [r10],0
			const size_t overrideDisabledOffset = AppendNearJcc(code, 0x84); // je unchanged

			// RDX and R8 are volatile call arguments, so replacing them here does not
			// disturb non-argument state or the caller's stack-resident native vectors.
			code.push_back(0x48); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(nativeShotTraceOriginOverrideBits.data())); // mov rdx,imm64
			code.push_back(0x49); code.push_back(0xB8);
			AppendU64(code, reinterpret_cast<uintptr_t>(nativeShotTraceTargetOverrideBits.data())); // mov r8,imm64
			appendStoreImmediate(&nativeShotEffectCapturedOverride, 1);
			const size_t jumpToAppliedOffset = code.size();
			code.push_back(0xE9);
			AppendU32(code, 0);

			const size_t unchangedOffset = code.size();
			appendStoreImmediate(&nativeShotEffectCapturedOverride, 0);
			const size_t appliedOffset = code.size();
			for (size_t i = 0; i < nativeShotEffectAppliedStartBits.size(); ++i)
				appendCapturePointerFloat(&nativeShotEffectAppliedStartBits[i], false, static_cast<uint8_t>(i * sizeof(float)));
			for (size_t i = 0; i < nativeShotEffectAppliedTargetBits.size(); ++i)
				appendCapturePointerFloat(&nativeShotEffectAppliedTargetBits[i], true, static_cast<uint8_t>(i * sizeof(float)));
			appendIncrement(&nativeShotEffectSequence);

			code.push_back(0x9D); // popfq
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, originalCallTarget);
			code.insert(code.end(), { 0x41, 0xFF, 0xD2 }); // call r10
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
				return std::vector<uint8_t>{};

			auto patchNearJmp = [&code, caveAddress](size_t instructionOffset, size_t targetOffset) {
				const int64_t displacement = static_cast<int64_t>(caveAddress + targetOffset)
					- static_cast<int64_t>(caveAddress + instructionOffset + 5);
				if (displacement < std::numeric_limits<int32_t>::min()
					|| displacement > std::numeric_limits<int32_t>::max())
					return false;
				const int32_t relative = static_cast<int32_t>(displacement);
				std::memcpy(code.data() + instructionOffset + 1, &relative, sizeof(relative));
				return true;
			};

			return PatchNearJcc(code, overrideDisabledOffset, unchangedOffset)
				&& patchNearJmp(jumpToAppliedOffset, appliedOffset)
				? code
				: std::vector<uint8_t>{};
		});
#endif

	// Disabled corrective rollback: the 0x13F007B/0x13F056E call-site experiment
	// caused plugin initialization to stop before the remaining combat hooks.
	// Keep it excluded until its ABI and lifecycle are independently proven.
#if 0
	// The game reaches this call before the authoritative damage trace. It
	// receives the same caller-owned origin/target vectors that are later passed
	// to 0x13F89A0, so this is the native fire boundary where the latest mock
	// gunflash snapshot can be turned into a one-shot override.
	installHookAt("Player native pre-trace shot snapshot",
		baseAddressGameEXE + 0x13F007B,
		5,
		[this](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes) {
			if (originalBytes.size() != 5 || originalBytes[0] != 0xE8)
				return std::vector<uint8_t>{};

			int32_t originalCallDisplacement = 0;
			std::memcpy(&originalCallDisplacement, originalBytes.data() + 1, sizeof(originalCallDisplacement));
			const uintptr_t originalCallTarget = static_cast<uintptr_t>(
				static_cast<int64_t>(returnAddress) + originalCallDisplacement);
			if (originalCallTarget != baseAddressGameEXE + 0x13F6BA0)
				return std::vector<uint8_t>{};

			std::vector<uint8_t> code;
			code.push_back(0x9C); // pushfq
			code.push_back(0x50); // push rax
			code.insert(code.end(), { 0x41, 0x52 }); // push r10
			code.insert(code.end(), { 0x41, 0x53 }); // push r11

			auto appendStoreImmediate = [&code](volatile LONG* destination, uint32_t value) {
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(destination));
				code.push_back(0x41); code.push_back(0xC7); code.push_back(0x02);
				AppendU32(code, value);
			};
			auto appendCaptureStackFloat = [&code](volatile LONG* destination, uint8_t stackDisplacement) {
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(destination));
				code.insert(code.end(), { 0x8B, 0x45, stackDisplacement }); // mov eax,[rbp+disp]
				code.insert(code.end(), { 0x41, 0x89, 0x02 }); // mov [r10],eax
			};
			auto appendCopyAimToStackAndOverride = [&code](volatile LONG* source,
				volatile LONG* overrideDestination, uint8_t stackDisplacement) {
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(source));
				code.insert(code.end(), { 0x41, 0x8B, 0x02 }); // mov eax,[r10]
				code.insert(code.end(), { 0x89, 0x45, stackDisplacement }); // mov [rbp+disp],eax
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(overrideDestination));
				code.insert(code.end(), { 0x41, 0x89, 0x02 }); // mov [r10],eax
			};
			auto appendRestoreStackFloat = [&code](volatile LONG* source, uint8_t stackDisplacement) {
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(source));
				code.insert(code.end(), { 0x41, 0x8B, 0x02 }); // mov eax,[r10]
				code.insert(code.end(), { 0x89, 0x45, stackDisplacement }); // mov [rbp+disp],eax
			};

			// This callsite passes the local player as RDX. Reject every other
			// actor before touching its vectors.
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&cachedPlayerPointer));
			code.insert(code.end(), { 0x4D, 0x8B, 0x1A }); // mov r11,[r10]
			code.insert(code.end(), { 0x4C, 0x39, 0xDA }); // cmp rdx,r11
			const size_t notLocalPlayerOffset = AppendNearJcc(code, 0x85); // jne resume

			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotAimSnapshotValid));
			code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x00 }); // cmp dword [r10],0
			const size_t snapshotInvalidOffset = AppendNearJcc(code, 0x84); // je resume

			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotAimPublishSequence));
			code.insert(code.end(), { 0x45, 0x8B, 0x1A }); // mov r11d,[r10]
			code.insert(code.end(), { 0x41, 0xF6, 0xC3, 0x01 }); // test r11b,1
			const size_t snapshotPublishingOffset = AppendNearJcc(code, 0x85); // jne resume

			// Preserve the native pair for failure cleanup and diagnostics.
			for (size_t i = 0; i < nativeShotTraceCapturedStartBits.size(); ++i)
				appendCaptureStackFloat(&nativeShotTraceCapturedStartBits[i], static_cast<uint8_t>(0x10 + i * sizeof(float)));
			for (size_t i = 0; i < nativeShotTraceCapturedTargetBits.size(); ++i)
				appendCaptureStackFloat(&nativeShotTraceCapturedTargetBits[i], static_cast<uint8_t>(i * sizeof(float)));

			for (size_t i = 0; i < nativeShotAimOriginBits.size(); ++i)
				appendCopyAimToStackAndOverride(&nativeShotAimOriginBits[i],
					&nativeShotTraceOriginOverrideBits[i], static_cast<uint8_t>(0x10 + i * sizeof(float)));
			for (size_t i = 0; i < nativeShotAimTargetBits.size(); ++i)
				appendCopyAimToStackAndOverride(&nativeShotAimTargetBits[i],
					&nativeShotTraceTargetOverrideBits[i], static_cast<uint8_t>(i * sizeof(float)));

			// Abort if the mock pair changed or was invalidated while copying.
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotAimPublishSequence));
			code.insert(code.end(), { 0x41, 0x8B, 0x02 }); // mov eax,[r10]
			code.insert(code.end(), { 0x44, 0x39, 0xD8 }); // cmp eax,r11d
			const size_t snapshotChangedOffset = AppendNearJcc(code, 0x85); // jne restore
			code.insert(code.end(), { 0xA8, 0x01 }); // test al,1
			const size_t snapshotBecameOddOffset = AppendNearJcc(code, 0x85); // jne restore
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotAimSnapshotValid));
			code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x00 }); // cmp dword [r10],0
			const size_t snapshotClearedOffset = AppendNearJcc(code, 0x84); // je restore

			// Transfer the stable sequence into the one-shot trace state. The
			// trace-entry hook will consume this exact pair once.
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTracePublishSequence));
			code.insert(code.end(), { 0x45, 0x89, 0x1A }); // mov [r10],r11d
			appendStoreImmediate(&nativeShotTraceOriginOverrideEnabled, 1);
			appendStoreImmediate(&nativeShotTraceOverrideConsumed, 0);
			const size_t successJumpOffset = code.size();
			code.push_back(0xE9);
			AppendU32(code, 0);

			const size_t restoreSnapshotOffset = code.size();
			for (size_t i = 0; i < nativeShotTraceCapturedStartBits.size(); ++i)
				appendRestoreStackFloat(&nativeShotTraceCapturedStartBits[i], static_cast<uint8_t>(0x10 + i * sizeof(float)));
			for (size_t i = 0; i < nativeShotTraceCapturedTargetBits.size(); ++i)
				appendRestoreStackFloat(&nativeShotTraceCapturedTargetBits[i], static_cast<uint8_t>(i * sizeof(float)));
			appendStoreImmediate(&nativeShotTraceOriginOverrideEnabled, 0);
			appendStoreImmediate(&nativeShotTraceOverrideConsumed, 1);

			const size_t resumeOffset = code.size();
			code.insert(code.end(), { 0x41, 0x5B }); // pop r11
			code.insert(code.end(), { 0x41, 0x5A }); // pop r10
			code.push_back(0x58); // pop rax
			code.push_back(0x9D); // popfq
			code.insert(code.end(), originalBytes.begin(), originalBytes.end());
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
				return std::vector<uint8_t>{};

			auto patchNearJmp = [&code, caveAddress](size_t instructionOffset, size_t targetOffset) {
				const int64_t displacement = static_cast<int64_t>(caveAddress + targetOffset)
					- static_cast<int64_t>(caveAddress + instructionOffset + 5);
				if (displacement < std::numeric_limits<int32_t>::min()
					|| displacement > std::numeric_limits<int32_t>::max())
					return false;
				const int32_t relative = static_cast<int32_t>(displacement);
				std::memcpy(code.data() + instructionOffset + 1, &relative, sizeof(relative));
				return true;
			};

			return PatchNearJcc(code, notLocalPlayerOffset, resumeOffset)
				&& PatchNearJcc(code, snapshotInvalidOffset, resumeOffset)
				&& PatchNearJcc(code, snapshotPublishingOffset, resumeOffset)
				&& PatchNearJcc(code, snapshotChangedOffset, restoreSnapshotOffset)
				&& PatchNearJcc(code, snapshotBecameOddOffset, restoreSnapshotOffset)
				&& PatchNearJcc(code, snapshotClearedOffset, restoreSnapshotOffset)
				&& patchNearJmp(successJumpOffset, resumeOffset)
				? code
				: std::vector<uint8_t>{};
		});

	// Reapply the consumed shot pair at the exact damage-trace callsite. This
	// protects the final native ray if the preceding helper adjusts its local
	// vectors, while the trace-entry hook still performs the one-shot proof.
	installHookAt("Player native damage-trace argument redirect",
		baseAddressGameEXE + 0x13F056E,
		5,
		[this](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes) {
			if (originalBytes.size() != 5 || originalBytes[0] != 0xE8)
				return std::vector<uint8_t>{};

			int32_t originalCallDisplacement = 0;
			std::memcpy(&originalCallDisplacement, originalBytes.data() + 1, sizeof(originalCallDisplacement));
			const uintptr_t originalCallTarget = static_cast<uintptr_t>(
				static_cast<int64_t>(returnAddress) + originalCallDisplacement);
			if (originalCallTarget != baseAddressGameEXE + 0x13F89A0)
				return std::vector<uint8_t>{};

			std::vector<uint8_t> code;
			code.push_back(0x9C); // pushfq
			code.push_back(0x50); // push rax
			code.insert(code.end(), { 0x41, 0x52 }); // push r10
			code.insert(code.end(), { 0x41, 0x53 }); // push r11

			auto appendOverrideStackFloat = [&code](volatile LONG* source, uint8_t stackDisplacement) {
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(source));
				code.insert(code.end(), { 0x41, 0x8B, 0x02 }); // mov eax,[r10]
				code.insert(code.end(), { 0x89, 0x45, stackDisplacement }); // mov [rbp+disp],eax
			};
			auto appendStoreImmediate = [&code](volatile LONG* destination, uint32_t value) {
				code.push_back(0x49); code.push_back(0xBA);
				AppendU64(code, reinterpret_cast<uintptr_t>(destination));
				code.push_back(0x41); code.push_back(0xC7); code.push_back(0x02);
				AppendU32(code, value);
			};

			// The pre-trace callsite arms this state only for the local player.
			// Do not touch unrelated native traces or NPC calls.
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceOriginOverrideEnabled));
			code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x00 }); // cmp dword [r10],0
			const size_t overrideDisabledOffset = AppendNearJcc(code, 0x84); // je resume
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceOverrideConsumed));
			code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x00 }); // cmp dword [r10],0
			const size_t overrideConsumedOffset = AppendNearJcc(code, 0x85); // jne resume

			for (size_t i = 0; i < nativeShotTraceOriginOverrideBits.size(); ++i)
				appendOverrideStackFloat(&nativeShotTraceOriginOverrideBits[i], static_cast<uint8_t>(0x10 + i * sizeof(float)));
			for (size_t i = 0; i < nativeShotTraceTargetOverrideBits.size(); ++i)
				appendOverrideStackFloat(&nativeShotTraceTargetOverrideBits[i], static_cast<uint8_t>(i * sizeof(float)));

			const size_t resumeOffset = code.size();
			code.insert(code.end(), { 0x41, 0x5B }); // pop r11
			code.insert(code.end(), { 0x41, 0x5A }); // pop r10
			code.push_back(0x58); // pop rax
			code.push_back(0x9D); // popfq
			code.insert(code.end(), originalBytes.begin(), originalBytes.end());
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
				return std::vector<uint8_t>{};

			return PatchNearJcc(code, overrideDisabledOffset, resumeOffset)
				&& PatchNearJcc(code, overrideConsumedOffset, resumeOffset)
				? code
				: std::vector<uint8_t>{};
		});
#endif

	installHook("Infinite bullet range load",
		{ 0xF3, 0x41, 0x0F, 0x10, 0x4E, 0x08, 0x4C },
		6,
		[](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>&) {
			std::vector<uint8_t> code;
			AppendRegisterFloatLoad(code, LongBulletRangeBits, { 0x66, 0x41, 0x0F, 0x6E, 0xCA }); // movd xmm1,r10d
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
				return std::vector<uint8_t>{};
			return code;
		});

	installHook("Infinite bullet range multiply",
		{ 0xF3, 0x41, 0x0F, 0x59, 0x46, 0x08, 0x0F, 0x2F },
		6,
		[](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>&) {
			std::vector<uint8_t> code;
			AppendRegisterFloatLoad(code, LongBulletRangeBits, { 0x66, 0x41, 0x0F, 0x6E, 0xC2 }); // movd xmm0,r10d
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
				return std::vector<uint8_t>{};
			return code;
		});

	if (settingsManager->enableCombatAssistAmmo && !settingsManager->activeManualReloadMode) {
		installHook("Almost infinite ammo",
			{ 0x8B, 0x43, 0x08, 0x85, 0xC0, 0x7E, 0x05, 0x2B },
			5,
			[this](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes) {
				std::vector<uint8_t> code;
				size_t jeOffset = 0;
				size_t jneOffset = 0;
				AppendCachedPlayerGuard(code, reinterpret_cast<uintptr_t>(&cachedPlayerPointer), jeOffset, jneOffset);
				AppendPopGuard(code);
				code.insert(code.end(), { 0xC7, 0x43, 0x0C }); AppendU32(code, CombatAssistReserveAmmo); // reserve ammo
				const size_t jumpToOriginalOffset = code.size();
				code.push_back(0xEB); code.push_back(0x00);

				const size_t nonPlayerOffset = code.size();
				if (!PatchShortJcc(code, jeOffset, nonPlayerOffset) || !PatchShortJcc(code, jneOffset, nonPlayerOffset))
					return std::vector<uint8_t>{};
				AppendPopGuard(code);

				const size_t originalOffset = code.size();
				if (!PatchShortJcc(code, jumpToOriginalOffset, originalOffset))
					return std::vector<uint8_t>{};
				code.insert(code.end(), originalBytes.begin(), originalBytes.end());
				if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
					return std::vector<uint8_t>{};
				return code;
			});
	}

}

void MemoryManager::ApplyPlayerSemiAutoFireGatePatch() {
	if (playerSemiAutoFireGateApplyAttempted || !settingsManager->enableDualGripAimFire)
		return;
	playerSemiAutoFireGateApplyAttempted = true;

	UpdateCombatAssistPlayerPointer();
	const uintptr_t target = FindPattern({
		0x48, 0x8B, 0xC4, 0x53, 0x55, 0x56, 0x57,
		0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
		0x48, 0x81, 0xEC, 0xB8, 0x00, 0x00, 0x00
	});
	if (target == 0) {
		uevr::API::get()->log_error("%s", "[TriggerGate] player weapon fire signature not found");
		return;
	}

	const bool installed = InstallHookPatch("Player semi-auto fire gate", target, 5,
		[this](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes) {
			std::vector<uint8_t> code;
			const auto appendBlockedReturn = [this](std::vector<uint8_t>& bytes) {
				bytes.push_back(0x49); bytes.push_back(0xBA);
				AppendU64(bytes, reinterpret_cast<uintptr_t>(&playerSemiAutoBlockedCount));
				bytes.insert(bytes.end(), { 0xF0, 0x41, 0xFF, 0x02 }); // lock inc dword ptr [r10]
				bytes.insert(bytes.end(), { 0x41, 0x5B }); // pop r11
				bytes.insert(bytes.end(), { 0x41, 0x5A }); // pop r10
				bytes.push_back(0x58); // pop rax
				bytes.push_back(0x9D); // popfq
				bytes.insert(bytes.end(), { 0x31, 0xC0, 0xC3 }); // xor eax,eax; ret
			};
			code.push_back(0x9C); // pushfq
			code.push_back(0x50); // push rax
			code.insert(code.end(), { 0x41, 0x52 }); // push r10
			code.insert(code.end(), { 0x41, 0x53 }); // push r11

			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&cachedPlayerPointer));
			code.insert(code.end(), { 0x4D, 0x8B, 0x12 }); // mov r10,[r10]
			code.insert(code.end(), { 0x4D, 0x85, 0xD2 }); // test r10,r10
			const size_t noPlayerOffset = AppendNearJcc(code, 0x84); // je allow
			code.insert(code.end(), { 0x4C, 0x39, 0xD2 }); // cmp rdx,r10
			const size_t notPlayerOffset = AppendNearJcc(code, 0x85); // jne allow

			// A genuine native Molotov trigger is the one safe source of the live
			// CWeapon entry required by the native impact dispatcher. Capture RCX
			// only after proving this is the local player and only for type 18;
			// preserve the original native call unchanged. Custom grip-release
			// flight later consumes this cached entry at its own collision point.
			code.insert(code.end(), { 0x44, 0x8B, 0x19 }); // mov r11d,[rcx]
			code.insert(code.end(), { 0x41, 0x83, 0xFB, 0x12 }); // cmp r11d,18
			const size_t notMolotovCaptureOffset = AppendNearJcc(code, 0x85); // jne normal gate
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeThrowableLiveWeaponEntry));
			code.insert(code.end(), { 0x49, 0x89, 0x0A }); // mov [r10],rcx
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeThrowableLiveWeaponSequence));
			code.insert(code.end(), { 0xF0, 0x41, 0xFF, 0x02 }); // lock inc dword ptr [r10]
			const size_t molotovCaptureDoneOffset = code.size();
			if (!PatchNearJcc(code, notMolotovCaptureOffset, molotovCaptureDoneOffset))
				return std::vector<uint8_t>{};

			// Physical unarmed/melee combat owns weapon types 0..15. Reject that
			// local-player native attack at the actual fire-function boundary, even
			// if a stale Lua weapon event re-injected RT after the XInput clear.
			code.insert(code.end(), { 0x44, 0x8B, 0x19 }); // mov r11d,[rcx]
			code.insert(code.end(), { 0x41, 0x83, 0xFB, 0x0F }); // cmp r11d,15
			const size_t firearmWeaponOffset = AppendNearJcc(code, 0x87); // ja semi-auto gate
			appendBlockedReturn(code);
			const size_t semiAutoGateOffset = code.size();
			if (!PatchNearJcc(code, firearmWeaponOffset, semiAutoGateOffset))
				return std::vector<uint8_t>{};

			// Custom akimbo owns its own per-hand pending edges at the task boundary.
			// Do not let the older aggregate semi-auto permit reject the second hand.
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&customAkimboEnabled));
			code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x00 });
			const size_t customAkimboActiveOffset = AppendNearJcc(code, 0x85); // jne allow

			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&playerSemiAutoPullHeld));
			code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x00 }); // cmp dword ptr [r10],0
			const size_t gateInactiveOffset = AppendNearJcc(code, 0x84); // je allow

			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&playerSemiAutoPullWeaponType));
			code.insert(code.end(), { 0x45, 0x8B, 0x1A }); // mov r11d,[r10]
			code.insert(code.end(), { 0x44, 0x39, 0x19 }); // cmp [rcx],r11d
			const size_t wrongWeaponOffset = AppendNearJcc(code, 0x85); // jne allow

			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&playerSemiAutoShotPermit));
			code.insert(code.end(), { 0x45, 0x8B, 0x1A }); // mov r11d,[r10]
			code.insert(code.end(), { 0x45, 0x85, 0xDB }); // test r11d,r11d
			const size_t permitPresentOffset = AppendNearJcc(code, 0x85); // jne allow

			appendBlockedReturn(code);

			const size_t allowOffset = code.size();
			if (!PatchNearJcc(code, noPlayerOffset, allowOffset) ||
				!PatchNearJcc(code, notPlayerOffset, allowOffset) ||
				!PatchNearJcc(code, customAkimboActiveOffset, allowOffset) ||
				!PatchNearJcc(code, gateInactiveOffset, allowOffset) ||
				!PatchNearJcc(code, wrongWeaponOffset, allowOffset) ||
				!PatchNearJcc(code, permitPresentOffset, allowOffset)) {
				return std::vector<uint8_t>{};
			}

			code.insert(code.end(), { 0x41, 0x5B }); // pop r11
			code.insert(code.end(), { 0x41, 0x5A }); // pop r10
			code.push_back(0x58); // pop rax
			code.push_back(0x9D); // popfq
			code.insert(code.end(), originalBytes.begin(), originalBytes.end());
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
				return std::vector<uint8_t>{};
			return code;
		});

	if (!installed) {
		uevr::API::get()->log_error("%s", "[TriggerGate] player semi-auto fire gate installation failed");
		return;
	}
	gPlayerSemiAutoPullHeld = &playerSemiAutoPullHeld;
	gPlayerSemiAutoShotPermit = &playerSemiAutoShotPermit;

	uevr::API::get()->log_info("[TriggerGate] player semi-auto fire gate installed at 0x%llX",
		static_cast<unsigned long long>(target - baseAddressGameEXE));
}

void MemoryManager::ApplyCustomAkimboFirePatch() {
	if (customAkimboFirePatchApplyAttempted || settingsManager == nullptr
		|| !settingsManager->enableCustomAkimbo)
		return;
	customAkimboFirePatchApplyAttempted = true;
	UpdateCombatAssistPlayerPointer();

	const uintptr_t taskDispatch = baseAddressGameEXE + 0x12E5B20;
	const uintptr_t handFire = baseAddressGameEXE + 0x12E6B60;
	const bool taskInstalled = InstallHookPatch("Custom akimbo task hand-mask owner",
		taskDispatch, 5,
		[this](uintptr_t caveAddress, uintptr_t returnAddress,
			const std::vector<uint8_t>& originalBytes) {
			if (originalBytes != std::vector<uint8_t>({ 0x48, 0x89, 0x5C, 0x24, 0x08 }))
				return std::vector<uint8_t>{};
			std::vector<uint8_t> code;
			code.push_back(0x9C); // pushfq
			code.push_back(0x50); // push rax
			code.insert(code.end(), { 0x41, 0x52, 0x41, 0x53 }); // push r10/r11

			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&customAkimboEnabled));
			code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x00 });
			const size_t disabledOffset = AppendNearJcc(code, 0x84); // je original
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&cachedPlayerPointer));
			code.insert(code.end(), { 0x4D, 0x8B, 0x12, 0x4D, 0x85, 0xD2 });
			const size_t noPlayerOffset = AppendNearJcc(code, 0x84);
			code.insert(code.end(), { 0x4C, 0x39, 0xD2 }); // cmp rdx,r10
			const size_t notPlayerOffset = AppendNearJcc(code, 0x85);
			// Automatic weapons must use GTA's already accepted task pulse as their
			// cadence clock. Semi-auto edges may retry until native cooldown accepts.
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&customAkimboWeaponType));
			code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x16 }); // cmp [r10],22
			const size_t semiPistolOffset = AppendNearJcc(code, 0x84);
			code.insert(code.end(), { 0x41, 0x83, 0x3A, 0x1A }); // cmp [r10],26
			const size_t semiSawnoffOffset = AppendNearJcc(code, 0x84);
			code.insert(code.end(), { 0x0F, 0xB6, 0x41, 0x15, 0x85, 0xC0 });
			const size_t nativeCadenceMissingOffset = AppendNearJcc(code, 0x84);
			const size_t loadRequestedMaskOffset = code.size();
			if (!PatchNearJcc(code, semiPistolOffset, loadRequestedMaskOffset)
				|| !PatchNearJcc(code, semiSawnoffOffset, loadRequestedMaskOffset))
				return std::vector<uint8_t>{};

			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&customAkimboTaskFireMask));
			code.insert(code.end(), { 0x45, 0x8B, 0x1A, 0x41, 0x83, 0xE3, 0x03 });
			code.insert(code.end(), { 0x44, 0x88, 0x59, 0x15 }); // mov [rcx+15h],r11b
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&customAkimboTaskPointer));
			code.insert(code.end(), { 0x49, 0x89, 0x0A }); // mov [r10],rcx
			code.insert(code.end(), { 0x45, 0x85, 0xDB });
			const size_t noRequestOffset = AppendNearJcc(code, 0x84);
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&customAkimboTaskInjectionSequence));
			code.insert(code.end(), { 0xF0, 0x41, 0xFF, 0x02 });

			const size_t originalOffset = code.size();
			if (!PatchNearJcc(code, disabledOffset, originalOffset)
				|| !PatchNearJcc(code, noPlayerOffset, originalOffset)
				|| !PatchNearJcc(code, notPlayerOffset, originalOffset)
				|| !PatchNearJcc(code, nativeCadenceMissingOffset, originalOffset)
				|| !PatchNearJcc(code, noRequestOffset, originalOffset))
				return std::vector<uint8_t>{};
			code.insert(code.end(), { 0x41, 0x5B, 0x41, 0x5A, 0x58, 0x9D });
			code.insert(code.end(), originalBytes.begin(), originalBytes.end());
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
				return std::vector<uint8_t>{};
			return code;
		});

	const bool handInstalled = InstallHookPatch("Custom akimbo hand trace selector",
		handFire, 5,
		[this](uintptr_t caveAddress, uintptr_t returnAddress,
			const std::vector<uint8_t>& originalBytes) {
			if (originalBytes != std::vector<uint8_t>({ 0x48, 0x89, 0x5C, 0x24, 0x20 }))
				return std::vector<uint8_t>{};
			std::vector<uint8_t> code;
			code.push_back(0x9C);
			code.push_back(0x50);
			code.insert(code.end(), { 0x41, 0x51, 0x41, 0x52, 0x41, 0x53 }); // r9-r11
			code.push_back(0x49); code.push_back(0xB9);
			AppendU64(code, reinterpret_cast<uintptr_t>(&customAkimboEnabled));
			code.insert(code.end(), { 0x41, 0x83, 0x39, 0x00 });
			const size_t disabledOffset = AppendNearJcc(code, 0x84);
			code.push_back(0x49); code.push_back(0xB9);
			AppendU64(code, reinterpret_cast<uintptr_t>(&cachedPlayerPointer));
			code.insert(code.end(), { 0x4D, 0x8B, 0x09, 0x4D, 0x85, 0xC9 });
			const size_t noPlayerOffset = AppendNearJcc(code, 0x84);
			code.insert(code.end(), { 0x4C, 0x39, 0xCA }); // cmp rdx,r9
			const size_t notPlayerOffset = AppendNearJcc(code, 0x85);
			code.push_back(0x49); code.push_back(0xB9);
			AppendU64(code, reinterpret_cast<uintptr_t>(&customAkimboTraceValidMask));
			code.insert(code.end(), { 0x41, 0x83, 0x39, 0x03 });
			const size_t tracesMissingOffset = AppendNearJcc(code, 0x85);

			code.insert(code.end(), { 0x45, 0x84, 0xC0 }); // test r8b,r8b
			const size_t leftHandOffset = AppendNearJcc(code, 0x85);
			// Native false means right hand, our controller index 1.
			code.push_back(0x49); code.push_back(0xBB);
			AppendU64(code, reinterpret_cast<uintptr_t>(customAkimboTraceOriginBits[1].data()));
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(customAkimboTraceTargetBits[1].data()));
			code.push_back(0x48); code.push_back(0xB8);
			AppendU64(code, reinterpret_cast<uintptr_t>(&customAkimboActiveHand));
			code.insert(code.end(), { 0xC7, 0x00, 0x01, 0x00, 0x00, 0x00 });
			const size_t rightToCopyOffset = code.size();
			code.push_back(0xE9); AppendU32(code, 0);

			const size_t leftOffset = code.size();
			code.push_back(0x49); code.push_back(0xBB);
			AppendU64(code, reinterpret_cast<uintptr_t>(customAkimboTraceOriginBits[0].data()));
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(customAkimboTraceTargetBits[0].data()));
			code.push_back(0x48); code.push_back(0xB8);
			AppendU64(code, reinterpret_cast<uintptr_t>(&customAkimboActiveHand));
			code.insert(code.end(), { 0xC7, 0x00, 0x00, 0x00, 0x00, 0x00 });

			const size_t copyOffset = code.size();
			if (!PatchNearJcc(code, leftHandOffset, leftOffset))
				return std::vector<uint8_t>{};
			const int64_t copyDisplacement = static_cast<int64_t>(caveAddress + copyOffset)
				- static_cast<int64_t>(caveAddress + rightToCopyOffset + 5);
			if (copyDisplacement < std::numeric_limits<int32_t>::min()
				|| copyDisplacement > std::numeric_limits<int32_t>::max())
				return std::vector<uint8_t>{};
			const int32_t copyRelative = static_cast<int32_t>(copyDisplacement);
			std::memcpy(code.data() + rightToCopyOffset + 1, &copyRelative, sizeof(copyRelative));

			code.push_back(0x49); code.push_back(0xB9);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTracePublishSequence));
			code.insert(code.end(), { 0xF0, 0x41, 0xFF, 0x01 }); // odd
			for (uint8_t axis = 0; axis < 3; ++axis) {
				const uint8_t displacement = static_cast<uint8_t>(axis * sizeof(LONG));
				code.insert(code.end(), { 0x41, 0x8B, 0x43, displacement });
				code.push_back(0x49); code.push_back(0xB9);
				AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceOriginOverrideBits[axis]));
				code.insert(code.end(), { 0x41, 0x89, 0x01 });
				code.insert(code.end(), { 0x41, 0x8B, 0x42, displacement });
				code.push_back(0x49); code.push_back(0xB9);
				AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceTargetOverrideBits[axis]));
				code.insert(code.end(), { 0x41, 0x89, 0x01 });
			}
			code.push_back(0x49); code.push_back(0xB9);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTracePublishSequence));
			code.insert(code.end(), { 0xF0, 0x41, 0xFF, 0x01 }); // even
			code.push_back(0x49); code.push_back(0xB9);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceOverrideConsumed));
			code.insert(code.end(), { 0x41, 0xC7, 0x01, 0, 0, 0, 0 });
			code.push_back(0x49); code.push_back(0xB9);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeShotTraceOriginOverrideEnabled));
			code.insert(code.end(), { 0x41, 0xC7, 0x01, 1, 0, 0, 0 });

			const size_t originalOffset = code.size();
			if (!PatchNearJcc(code, disabledOffset, originalOffset)
				|| !PatchNearJcc(code, noPlayerOffset, originalOffset)
				|| !PatchNearJcc(code, notPlayerOffset, originalOffset)
				|| !PatchNearJcc(code, tracesMissingOffset, originalOffset))
				return std::vector<uint8_t>{};
			code.insert(code.end(), { 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x58, 0x9D });
			code.insert(code.end(), originalBytes.begin(), originalBytes.end());
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
				return std::vector<uint8_t>{};
			return code;
		});

	customAkimboFirePatchInstalled = taskInstalled && handInstalled;
	if (!customAkimboFirePatchInstalled) {
		uevr::API::get()->log_error("%s", "[CustomAkimbo] native hand patches failed; feature remains disabled");
		ClearCustomAkimboState();
		return;
	}
	gCustomAkimboEnabled = &customAkimboEnabled;
	gCustomAkimboWeaponType = &customAkimboWeaponType;
	gCustomAkimboPendingMask = &customAkimboPendingMask;
	gCustomAkimboTaskFireMask = &customAkimboTaskFireMask;
	gCustomAkimboActiveHand = &customAkimboActiveHand;
	gCustomAkimboAcceptedShotSequence = &customAkimboAcceptedShotSequence;
	gCustomAkimboAcceptedHandMask = &customAkimboAcceptedHandMask;
	uevr::API::get()->log_info(
		"[CustomAkimbo] native task/fire patches installed task=0x12E5B20 handFire=0x12E6B60");
}

bool MemoryManager::SetNativeThrowableMotionOverride(const std::array<float, 3>& origin,
	const std::array<float, 3>& velocity, int weaponType, uint32_t sequence) {
	if (!nativeThrowableMotionPatchInstalled
		|| weaponType < 16 || weaponType > 18 || sequence == 0)
		return false;
	for (size_t axis = 0; axis < 3; ++axis) {
		if (!std::isfinite(origin[axis]) || !std::isfinite(velocity[axis]))
			return false;
	}
	// Publication occurs on the engine thread, so refresh the native local-player
	// identity here even when the optional combat-assist maintenance loop is off.
	UpdateCombatAssistPlayerPointer();
	if (cachedPlayerPointer == 0)
		return false;

	// Publish as one inactive-to-active transaction. The native cave can only
	// consume a complete origin/velocity pair and clears active after one use.
	InterlockedExchange(&nativeThrowableMotionOverrideActive, 0);
	for (size_t axis = 0; axis < 3; ++axis) {
		LONG originBits = 0;
		LONG velocityBits = 0;
		static_assert(sizeof(originBits) == sizeof(origin[axis]));
		std::memcpy(&originBits, &origin[axis], sizeof(originBits));
		std::memcpy(&velocityBits, &velocity[axis], sizeof(velocityBits));
		InterlockedExchange(&nativeThrowableMotionOriginBits[axis], originBits);
		InterlockedExchange(&nativeThrowableMotionVelocityBits[axis], velocityBits);
	}
	InterlockedExchange(&nativeThrowableMotionWeapon, static_cast<LONG>(weaponType));
	InterlockedExchange(&nativeThrowableMotionPublishSequence, static_cast<LONG>(sequence));
	InterlockedExchange(&nativeThrowableMotionOverrideActive, 1);
	return true;
}

void MemoryManager::ClearNativeThrowableMotionOverride() {
	InterlockedExchange(&nativeThrowableMotionOverrideActive, 0);
}

bool MemoryManager::ConsumeNativeThrowableMotionApplied(uint32_t& sequence) {
	const LONG consumed = InterlockedCompareExchange(
		&nativeThrowableMotionConsumedSequence, 0, 0);
	if (consumed == 0 || consumed == lastReadNativeThrowableMotionConsumedSequence)
		return false;
	lastReadNativeThrowableMotionConsumedSequence = consumed;
	sequence = static_cast<uint32_t>(consumed);
	return true;
}

bool MemoryManager::ReadLatestNativeThrowableLaunchProbe(
	NativeThrowableLaunchProbe& probe) {
	const LONG sequenceBefore = InterlockedCompareExchange(
		&nativeThrowableLaunchProbeSequence, 0, 0);
	if (sequenceBefore == 0
		|| sequenceBefore == lastReadNativeThrowableLaunchProbeSequence)
		return false;

	const auto readFloatArray = [](const std::array<volatile LONG, 3>& source,
		std::array<float, 3>& destination) {
		for (size_t i = 0; i < source.size(); ++i) {
			const LONG bits = InterlockedCompareExchange(
				const_cast<volatile LONG*>(&source[i]), 0, 0);
			std::memcpy(&destination[i], &bits, sizeof(bits));
		}
	};

	NativeThrowableLaunchProbe captured{};
	readFloatArray(nativeThrowableLaunchProbeOriginBits, captured.rawOrigin);
	readFloatArray(nativeThrowableLaunchProbeVelocityBits, captured.rawVelocity);
	readFloatArray(nativeThrowableLaunchProbeDirectionBits, captured.rawDirection);
	const LONG forceBits = InterlockedCompareExchange(
		&nativeThrowableLaunchProbeForceBits, 0, 0);
	std::memcpy(&captured.force, &forceBits, sizeof(forceBits));
	const LONG64 directionPointer = InterlockedCompareExchange64(
		&nativeThrowableLaunchProbeDirectionPointer, 0, 0);
	captured.directionSupplied = directionPointer != 0;
	captured.target = static_cast<uintptr_t>(InterlockedCompareExchange64(
		&nativeThrowableLaunchProbeTarget, 0, 0));
	captured.weaponType = static_cast<int>(InterlockedCompareExchange(
		&nativeThrowableLaunchProbeWeapon, 0, 0));
	captured.overridden = InterlockedCompareExchange(
		&nativeThrowableLaunchProbeOverridden, 0, 0) != 0;

	const LONG sequenceAfter = InterlockedCompareExchange(
		&nativeThrowableLaunchProbeSequence, 0, 0);
	if (sequenceBefore != sequenceAfter)
		return false;

	captured.sequence = static_cast<uint32_t>(sequenceAfter);
	lastReadNativeThrowableLaunchProbeSequence = sequenceAfter;
	probe = captured;
	return true;
}

void MemoryManager::ApplyNativeThrowableMotionPatch() {
	if (nativeThrowableMotionPatchApplyAttempted || settingsManager == nullptr
		|| (!settingsManager->enableMotionThrowables
			&& !settingsManager->enableThrowableMotionProbe))
		return;
	nativeThrowableMotionPatchApplyAttempted = true;
	// Keep the old motion override deliberately disabled. The custom Molotov
	// flight owns grip-release, and an earlier cave inside the downstream
	// projectile state machine crashed on the native trigger route. This probe is
	// intentionally earlier and passive: it copies the already assembled native
	// AddProjectile arguments and immediately executes the untouched prologue.
	nativeThrowableMotionPatchInstalled = false;
	ClearNativeThrowableMotionOverride();
	if (baseAddressGameEXE == 0) {
		uevr::API::get()->log_warn("%s",
			"[NativeThrowableProbe] AddProjectile probe withheld: game module base is unavailable");
		return;
	}

	UpdateCombatAssistPlayerPointer();
	const uintptr_t target = baseAddressGameEXE + NativeThrowableAddProjectileRva;
	if (std::memcmp(reinterpret_cast<const void*>(target),
		NativeThrowableAddProjectilePrologue.data(),
		NativeThrowableAddProjectilePrologue.size()) != 0) {
		uevr::API::get()->log_warn(
			"[NativeThrowableProbe] AddProjectile probe withheld: prologue mismatch at RVA 0x%llX",
			static_cast<unsigned long long>(NativeThrowableAddProjectileRva));
		return;
	}

	const bool installed = InstallHookPatch("Native Molotov AddProjectile probe", target,
		NativeThrowableAddProjectilePrologue.size(),
		[this](uintptr_t caveAddress, uintptr_t returnAddress,
			const std::vector<uint8_t>& originalBytes) {
			std::vector<uint8_t> code;
			// Preserve every scratch register/flag used by the probe. No C++ call
			// occurs in this cave, so the original native stack alignment and all
			// argument registers remain exactly as CWeapon::Fire supplied them.
			code.push_back(0x9C); // pushfq
			code.push_back(0x50); // push rax
			code.insert(code.end(), { 0x41, 0x52 }); // push r10
			code.insert(code.end(), { 0x41, 0x53 }); // push r11

			code.insert(code.end(), { 0x83, 0xFA, NativeMolotovWeaponType }); // cmp edx,18
			const size_t wrongWeaponOffset = AppendNearJcc(code, 0x85); // jne restore
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&cachedPlayerPointer));
			code.insert(code.end(), { 0x4D, 0x8B, 0x12 }); // mov r10,[r10]
			code.insert(code.end(), { 0x4D, 0x85, 0xD2 }); // test r10,r10
			const size_t noPlayerOffset = AppendNearJcc(code, 0x84); // je restore
			code.insert(code.end(), { 0x4C, 0x39, 0xD1 }); // cmp rcx,r10
			const size_t notPlayerOffset = AppendNearJcc(code, 0x85); // jne restore

			// RCX=creator, EDX=weapon, R8=origin, XMM3=force. At function entry
			// arg5 (direction) and arg6 (target) are [rsp+28h]/[rsp+30h]; the
			// four saves above shift them to 48h/50h in this cave.
			code.insert(code.end(), { 0x49, 0x8B, 0x00 }); // mov rax,[r8]
			code.push_back(0x49); code.push_back(0xBB);
			AppendU64(code, reinterpret_cast<uintptr_t>(nativeThrowableLaunchProbeOriginBits.data()));
			code.insert(code.end(), { 0x49, 0x89, 0x03 }); // mov [r11],rax
			code.insert(code.end(), { 0x41, 0x8B, 0x40, 0x08 }); // mov eax,[r8+8]
			code.insert(code.end(), { 0x41, 0x89, 0x43, 0x08 }); // mov [r11+8],eax
			code.push_back(0x49); code.push_back(0xBB);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeThrowableLaunchProbeForceBits));
			code.insert(code.end(), { 0xF3, 0x41, 0x0F, 0x11, 0x1B }); // movss [r11],xmm3

			code.insert(code.end(), { 0x4C, 0x8B, 0x54, 0x24, 0x48 }); // mov r10,[rsp+48h]
			code.push_back(0x49); code.push_back(0xBB);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeThrowableLaunchProbeDirectionPointer));
			code.insert(code.end(), { 0x4D, 0x89, 0x13 }); // mov [r11],r10
			code.insert(code.end(), { 0x4D, 0x85, 0xD2 }); // test r10,r10
			const size_t noDirectionOffset = AppendNearJcc(code, 0x84); // je target/metadata
			code.insert(code.end(), { 0x49, 0x8B, 0x02 }); // mov rax,[r10]
			code.push_back(0x49); code.push_back(0xBB);
			AppendU64(code, reinterpret_cast<uintptr_t>(nativeThrowableLaunchProbeDirectionBits.data()));
			code.insert(code.end(), { 0x49, 0x89, 0x03 }); // mov [r11],rax
			code.insert(code.end(), { 0x41, 0x8B, 0x42, 0x08 }); // mov eax,[r10+8]
			code.insert(code.end(), { 0x41, 0x89, 0x43, 0x08 }); // mov [r11+8],eax

			const size_t targetAndMetadataOffset = code.size();
			code.insert(code.end(), { 0x4C, 0x8B, 0x54, 0x24, 0x50 }); // mov r10,[rsp+50h]
			code.push_back(0x49); code.push_back(0xBB);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeThrowableLaunchProbeTarget));
			code.insert(code.end(), { 0x4D, 0x89, 0x13 }); // mov [r11],r10
			code.push_back(0x49); code.push_back(0xBB);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeThrowableLaunchProbeWeapon));
			code.insert(code.end(), { 0x41, 0x89, 0x13 }); // mov [r11],edx
			code.push_back(0x49); code.push_back(0xBB);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeThrowableLaunchProbeOverridden));
			code.insert(code.end(), { 0x41, 0xC7, 0x03, 0x00, 0x00, 0x00, 0x00 });
			code.push_back(0x49); code.push_back(0xBB);
			AppendU64(code, reinterpret_cast<uintptr_t>(&nativeThrowableLaunchProbeSequence));
			code.insert(code.end(), { 0xF0, 0x41, 0xFF, 0x03 }); // lock inc dword ptr [r11]

			const size_t restoreOffset = code.size();
			code.insert(code.end(), { 0x41, 0x5B }); // pop r11
			code.insert(code.end(), { 0x41, 0x5A }); // pop r10
			code.push_back(0x58); // pop rax
			code.push_back(0x9D); // popfq
			code.insert(code.end(), originalBytes.begin(), originalBytes.end());
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress)
				|| !PatchNearJcc(code, wrongWeaponOffset, restoreOffset)
				|| !PatchNearJcc(code, noPlayerOffset, restoreOffset)
				|| !PatchNearJcc(code, notPlayerOffset, restoreOffset)
				|| !PatchNearJcc(code, noDirectionOffset, targetAndMetadataOffset))
				return std::vector<uint8_t>{};
			return code;
		});
	if (!installed) {
		uevr::API::get()->log_error("%s",
			"[NativeThrowableProbe] AddProjectile probe installation failed; native trigger path stays untouched");
		return;
	}

	nativeThrowableLaunchProbePatchInstalled = true;
	uevr::API::get()->log_info(
		"[NativeThrowableProbe] passive AddProjectile probe installed at RVA 0x%llX",
		static_cast<unsigned long long>(NativeThrowableAddProjectileRva));
}

void MemoryManager::ApplyManualReloadCapturePatch() {
	if (manualReloadCaptureApplyAttempted || !settingsManager->activeManualReloadMode)
		return;
	manualReloadCaptureApplyAttempted = true;

	UpdateCombatAssistPlayerPointer();
	const uintptr_t target = FindPattern({
		0x8B, 0x43, 0x08, 0x85, 0xC0, 0x7E, 0x05, 0x2B
	});
	if (target == 0) {
		uevr::API::get()->log_error("%s", "[ManualReload] ammo-decrement signature not found; live weapon capture is unavailable");
		return;
	}

	const bool installed = InstallHookPatch("Manual reload live weapon capture", target, 5,
		[this](uintptr_t caveAddress, uintptr_t returnAddress, const std::vector<uint8_t>& originalBytes) {
			std::vector<uint8_t> code;
			code.push_back(0x9C); // pushfq
			code.push_back(0x50); // push rax
			code.insert(code.end(), { 0x41, 0x52 }); // push r10

			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&cachedPlayerPointer)); // mov r10,imm64
			code.insert(code.end(), { 0x4D, 0x8B, 0x12 }); // mov r10,[r10]
			code.insert(code.end(), { 0x4D, 0x85, 0xD2 }); // test r10,r10
			const size_t noPlayerOffset = AppendNearJcc(code, 0x84); // je restore
			code.insert(code.end(), { 0x4C, 0x39, 0xD7 }); // cmp rdi,r10
			const size_t notPlayerOffset = AppendNearJcc(code, 0x85); // jne restore

			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&manualReloadCapturedWeaponEntry));
			code.insert(code.end(), { 0x49, 0x89, 0x1A }); // mov [r10],rbx

			code.insert(code.end(), { 0x8B, 0x03 }); // mov eax,[rbx]
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&manualReloadCapturedWeaponType));
			code.insert(code.end(), { 0x41, 0x89, 0x02 }); // mov [r10],eax

			code.insert(code.end(), { 0x8B, 0x43, 0x08 }); // mov eax,[rbx+8]
			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&manualReloadCapturedPreShotClip));
			code.insert(code.end(), { 0x41, 0x89, 0x02 }); // mov [r10],eax

			code.push_back(0x49); code.push_back(0xBA);
			AppendU64(code, reinterpret_cast<uintptr_t>(&manualReloadCaptureSequence));
			code.insert(code.end(), { 0xF0, 0x41, 0xFF, 0x02 }); // lock inc dword ptr [r10]

			const size_t restoreOffset = code.size();
			if (!PatchNearJcc(code, noPlayerOffset, restoreOffset) ||
				!PatchNearJcc(code, notPlayerOffset, restoreOffset)) {
				return std::vector<uint8_t>{};
			}

			code.insert(code.end(), { 0x41, 0x5A }); // pop r10
			code.push_back(0x58); // pop rax
			code.push_back(0x9D); // popfq
			code.insert(code.end(), originalBytes.begin(), originalBytes.end());
			if (!AppendRelJmp(code, caveAddress + code.size(), returnAddress))
				return std::vector<uint8_t>{};
			return code;
		});

	if (!installed) {
		uevr::API::get()->log_error("%s", "[ManualReload] live weapon capture hook failed");
		return;
	}

	uevr::API::get()->log_info("[ManualReload] live weapon capture hook installed at 0x%llX",
		static_cast<unsigned long long>(target - baseAddressGameEXE));
}

void MemoryManager::MaintainCombatAssistValues() {
	UpdateCombatAssistPlayerPointer();
	if (!combatAssistWeaponInfoPatched) {
		const ULONGLONG now = GetTickCount64();
		if (now - lastCombatAssistWeaponInfoReadyCheckTime >= WeaponInfoReadyCheckIntervalMs) {
			lastCombatAssistWeaponInfoReadyCheckTime = now;
			ApplyCombatAssistWeaponInfoValues();
		}
	}
	VerifyCombatAssistWeaponInfoValues();

	if (!combatAssistStatsResolved && !ResolveCombatAssistStats())
		return;
	if (!settingsManager->enableCombatAssistWeaponSkill)
		return;

	const uintptr_t floatStatsBase = baseAddressGameEXE + combatAssistFloatStatsOffset;
	constexpr int weaponSkillStats[] = { 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F };
	for (const int statIndex : weaponSkillStats) {
		auto* value = reinterpret_cast<float*>(floatStatsBase + (statIndex * sizeof(float)));
		*value = AlmostMaxWeaponSkill;
	}
}

void MemoryManager::UpdateHealthRecovery(bool playerInControl) {
	const ULONGLONG now = GetTickCount64();
	if (!settingsManager->enableHealthRecovery) {
		healthRecoveryPlayerPointer = 0;
		lastHealthRecoverySampleTime = 0;
		lastHealthDamageTime = 0;
		healthRecoveryControlPauseStartTime = 0;
		lastObservedPlayerHealth = -1.0f;
		healthRecoveryHealthAddress = 0;
		healthRecoveryMaxHealthAddress = 0;
		healthRecoveryLayoutValid = false;
		return;
	}

	if (!playerInControl) {
		if (healthRecoveryControlPauseStartTime == 0)
			healthRecoveryControlPauseStartTime = now;
		return;
	}
	if (healthRecoveryControlPauseStartTime != 0) {
		if (lastHealthDamageTime != 0)
			lastHealthDamageTime += now - healthRecoveryControlPauseStartTime;
		healthRecoveryControlPauseStartTime = 0;
	}
	if (now - lastHealthRecoverySampleTime < HealthRecoverySampleIntervalMs)
		return;
	lastHealthRecoverySampleTime = now;
	if (!playerBulletDamageReductionLogged
		&& InterlockedCompareExchange(&playerBulletDamageReductionCount, 0, 0) > 0) {
		playerBulletDamageReductionLogged = true;
		uevr::API::get()->log_info("%s", "[PlayerHealth] bullet-only damage resistance active for CJ");
	}

	UpdateCombatAssistPlayerPointer();
	const uintptr_t playerPed = cachedPlayerPointer;
	if (playerPed == 0) {
		healthRecoveryPlayerPointer = 0;
		healthRecoveryHealthAddress = 0;
		healthRecoveryMaxHealthAddress = 0;
		healthRecoveryLayoutValid = false;
		lastObservedPlayerHealth = -1.0f;
		return;
	}

	if (healthRecoveryPlayerPointer != playerPed) {
		healthRecoveryPlayerPointer = playerPed;
		healthRecoveryHealthAddress = playerPed + PlayerHealthOffset;
		healthRecoveryMaxHealthAddress = playerPed + PlayerMaxHealthOffset;
		healthRecoveryLayoutValid =
			IsWritableMemory(healthRecoveryHealthAddress, sizeof(float)) &&
			IsReadableMemory(healthRecoveryMaxHealthAddress, sizeof(float));
		lastObservedPlayerHealth = -1.0f;
		lastHealthDamageTime = now;
	}
	if (!healthRecoveryLayoutValid)
		return;

	float health = 0.0f;
	float maxHealth = 0.0f;
	std::memcpy(&health, reinterpret_cast<const void*>(healthRecoveryHealthAddress), sizeof(health));
	std::memcpy(&maxHealth, reinterpret_cast<const void*>(healthRecoveryMaxHealthAddress), sizeof(maxHealth));
	if (!std::isfinite(health) || !std::isfinite(maxHealth)
		|| maxHealth < 1.0f || maxHealth > 1000.0f
		|| health < 0.0f || health > maxHealth * 2.0f) {
		lastObservedPlayerHealth = -1.0f;
		return;
	}

	if (lastObservedPlayerHealth < 0.0f) {
		lastObservedPlayerHealth = health;
		lastHealthDamageTime = now;
		return;
	}

	if (health + HealthChangeEpsilon < lastObservedPlayerHealth)
		lastHealthDamageTime = now;

	const float recoveryTarget = maxHealth * HealthRecoveryFraction;
	if (health > 0.0f && health + HealthChangeEpsilon < recoveryTarget
		&& now - lastHealthDamageTime >= HealthRecoveryDelayMs) {
		const float previousHealth = health;
		std::memcpy(reinterpret_cast<void*>(healthRecoveryHealthAddress), &recoveryTarget, sizeof(recoveryTarget));
		health = recoveryTarget;
		uevr::API::get()->log_info("[PlayerHealth] recovered %.1f -> %.1f (max %.1f)",
			previousHealth, recoveryTarget, maxHealth);
	}

	lastObservedPlayerHealth = health;
}

void MemoryManager::RestoreCombatAssistPatches() {
	RestoreManualReloadState();
	RestoreCombatAssistWeaponInfoValues();

	for (auto it = combatAssistPatches.rbegin(); it != combatAssistPatches.rend(); ++it) {
		if (it->applied && it->address != 0 && !it->originalBytes.empty())
			WriteProcessBytes(it->address, it->originalBytes);
		if (it->codeCave != nullptr)
			VirtualFree(it->codeCave, 0, MEM_RELEASE);
	}

	combatAssistPatches.clear();
	manualReloadCaptureApplyAttempted = false;
	playerSemiAutoFireGateApplyAttempted = false;
	customAkimboFirePatchApplyAttempted = false;
	customAkimboFirePatchInstalled = false;
	nativeThrowableMotionPatchApplyAttempted = false;
	nativeThrowableMotionPatchInstalled = false;
	nativeThrowableLaunchProbePatchInstalled = false;
	if (gPlayerSemiAutoPullHeld == &playerSemiAutoPullHeld)
		gPlayerSemiAutoPullHeld = nullptr;
	if (gPlayerSemiAutoShotPermit == &playerSemiAutoShotPermit)
		gPlayerSemiAutoShotPermit = nullptr;
	if (gCustomAkimboEnabled == &customAkimboEnabled)
		gCustomAkimboEnabled = nullptr;
	if (gCustomAkimboWeaponType == &customAkimboWeaponType)
		gCustomAkimboWeaponType = nullptr;
	if (gCustomAkimboPendingMask == &customAkimboPendingMask)
		gCustomAkimboPendingMask = nullptr;
	if (gCustomAkimboTaskFireMask == &customAkimboTaskFireMask)
		gCustomAkimboTaskFireMask = nullptr;
	if (gCustomAkimboActiveHand == &customAkimboActiveHand)
		gCustomAkimboActiveHand = nullptr;
	if (gCustomAkimboAcceptedShotSequence == &customAkimboAcceptedShotSequence)
		gCustomAkimboAcceptedShotSequence = nullptr;
	if (gCustomAkimboAcceptedHandMask == &customAkimboAcceptedHandMask)
		gCustomAkimboAcceptedHandMask = nullptr;
	InterlockedExchange(&playerSemiAutoPullHeld, 0);
	InterlockedExchange(&playerSemiAutoPullWeaponType, 0);
	InterlockedExchange(&playerSemiAutoShotPermit, 0);
	InterlockedExchange(&playerSemiAutoBlockedCount, 0);
	ClearCustomAkimboState();
	InterlockedExchange(&customAkimboTaskInjectionSequence, 0);
	InterlockedExchange(&customAkimboAcceptedShotSequence, 0);
	InterlockedExchange(&customAkimboAcceptedHandMask, 0);
	InterlockedExchange(&nativeThrowableMotionOverrideActive, 0);
	InterlockedExchange(&nativeThrowableMotionWeapon, 0);
	InterlockedExchange(&nativeThrowableMotionPublishSequence, 0);
	InterlockedExchange(&nativeThrowableMotionConsumedSequence, 0);
	InterlockedExchange64(&nativeThrowableLiveWeaponEntry, 0);
	InterlockedExchange(&nativeThrowableLiveWeaponSequence, 0);
	InterlockedExchange(&nativeThrowableLaunchProbeSequence, 0);
	InterlockedExchange(&nativeThrowableLaunchProbeWeapon, 0);
	InterlockedExchange(&nativeThrowableLaunchProbeOverridden, 0);
	InterlockedExchange(&nativeThrowableLaunchProbeForceBits, 0);
	InterlockedExchange64(&nativeThrowableLaunchProbeDirectionPointer, 0);
	InterlockedExchange64(&nativeThrowableLaunchProbeTarget, 0);
	for (size_t axis = 0; axis < 3; ++axis) {
		InterlockedExchange(&nativeThrowableMotionOriginBits[axis], 0);
		InterlockedExchange(&nativeThrowableMotionVelocityBits[axis], 0);
		InterlockedExchange(&nativeThrowableLaunchProbeOriginBits[axis], 0);
		InterlockedExchange(&nativeThrowableLaunchProbeVelocityBits[axis], 0);
		InterlockedExchange(&nativeThrowableLaunchProbeDirectionBits[axis], 0);
	}
	lastReadNativeThrowableMotionConsumedSequence = 0;
	lastReadNativeThrowableLaunchProbeSequence = 0;
	cachedPlayerPointer = 0;
	healthRecoveryPlayerPointer = 0;
	lastHealthRecoverySampleTime = 0;
	lastHealthDamageTime = 0;
	healthRecoveryControlPauseStartTime = 0;
	lastObservedPlayerHealth = -1.0f;
	healthRecoveryHealthAddress = 0;
	healthRecoveryMaxHealthAddress = 0;
	healthRecoveryLayoutValid = false;
	combatAssistApplyAttempted = false;
	combatAssistStatsResolveAttempted = false;
	combatAssistPlayerGlobalsResolveAttempted = false;
	combatAssistPlayerGlobalsResolved = false;
	combatAssistCurrentPlayerIndexAddress = 0;
	combatAssistPlayerInfoArrayAddress = 0;
	combatAssistWeaponInfoResolveAttempted = false;
	combatAssistWeaponInfoReady = false;
	combatAssistWeaponInfoWaitLogged = false;
	lastCombatAssistWeaponInfoReadyCheckTime = 0;
	InterlockedExchange(&nativeShotSpreadBypassCountA, 0);
	InterlockedExchange(&nativeShotSpreadBypassCountB, 0);
	InterlockedExchange(&nativeShotTraceOriginOverrideEnabled, 0);
	InterlockedExchange(&nativeShotTraceOverrideConsumed, 0);
	InterlockedExchange(&nativeShotTraceVehicleOverrideArmed, 0);
	InterlockedExchange(&nativeShotTraceVehicleModeActive, 0);
	InterlockedExchange(&nativeShotTraceCallSiteRva, 0);
	InterlockedExchange(&nativeShotTrailPending, 0);
	InterlockedExchange(&nativeShotTraceCapturedOverride, 0);
	InterlockedExchange(&nativeShotTraceSequence, 0);
	lastReadNativeShotTraceSequence = 0;
	InterlockedExchange(&nativeShotEffectCapturedOverride, 0);
	InterlockedExchange(&nativeShotEffectOwnerLocal, 0);
	InterlockedExchange(&nativeShotEffectSequence, 0);
	lastReadNativeShotEffectSequence = 0;
	InterlockedExchange(&playerBulletDamageReductionCount, 0);
	playerBulletDamageReductionLogged = false;
}

//Finds address from pointer offsets found in cheat engine
uintptr_t FindDMAAddy(uintptr_t baseAddress, const std::vector<unsigned int>& offsets) {
	uintptr_t addr = baseAddress;

	for (size_t i = 0; i < offsets.size(); ++i) {
		if (addr == 0) {
			// If at any point the address is invalid, return 0
			uevr::API::get()->log_error("%s", "Cant find gunflash socket address");
			return 0;
		}
		// Dereference the pointer
		addr = *reinterpret_cast<uintptr_t*>(addr);

		// Add the offset
		addr += offsets[i];
	}
	return addr;
}

	
//Retrieves original bytes to manually set them as a variables in the code. std::vector<std::pair<uintptr_t, size_t>>
//void MemoryManager::GetAllBytes()
//{
//	WriteBytesToIniFile("aimingUpVectorInstructionsAddresses",aimingUpVectorInstructionsAddresses);
//}
//void MemoryManager::WriteBytesToIniFile(const char* header, const std::vector<std::pair<uintptr_t, size_t>>& addresses) {
//    // Open the file in append mode
//    std::ofstream file("originalBytes.ini", std::ios::app);
//    if (!file.is_open()) {
//        std::cerr << "Failed to open file: originalBytes.ini\n";
//        return;
//    }
//
//    // Write the header
//    file << "[" << header << "]\n";
//
//    for (const auto& [address, size] : addresses) {
//        // Allocate a buffer to hold the bytes
//        std::vector<uint8_t> bytes(size);
//
//        // Read the bytes from memory
//        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(address + baseAddressGameEXE), bytes.data(), size, nullptr)) {
//            // Write the address and size to the file
//            file << "0x" << std::hex << address << ", " << size << ", 0x";
//
//            // Write the bytes in contiguous hexadecimal format
//            for (size_t i = 0; i < size; ++i) {
//                file << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]);
//            }
//            file << "\n";
//        } else {
//            std::cerr << "Failed to read memory at address: 0x" << std::hex << address << "\n";
//        }
//    }
//
//    file.close();
//    std::cout << "Bytes appended to originalBytes.ini under header: " << header << "\n";
//}
// 
// Print the original bytes
//void MemoryManager::PrintOriginalBytes() const {
//    for (const auto& [offset, originalByte] : originalBytes) {
//        std::cout << "Offset: 0x" << std::hex << offset
//                  << ", Value: 0x" << static_cast<int>(originalByte.value) << "\n";
//    }
//}
