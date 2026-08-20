#include "SettingsManager.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace {
	constexpr float CameraOffsetEpsilon = 0.001f;
	constexpr float CameraOffsetAbsoluteLimit = 75.0f;
	constexpr ULONGLONG CameraOffsetPollIntervalMs = 250;
	constexpr ULONGLONG CameraOffsetSettleTimeMs = 750;
	constexpr ULONGLONG CameraOffsetApplyGuardMs = 750;
	constexpr ULONGLONG ConfigPollIntervalMs = 500;
}

std::string GetDLLDirectory();

static const char* BoolText(bool value)
{
	return value ? "true" : "false";
}

static bool FindSettingValue(const std::string& fileContents, const std::string& key, std::string& value)
{
	size_t lineStart = 0;
	while (lineStart <= fileContents.size())
	{
		const size_t lineEnd = fileContents.find_first_of("\r\n", lineStart);
		const size_t contentEnd = lineEnd == std::string::npos ? fileContents.size() : lineEnd;
		size_t keyStart = lineStart;
		while (keyStart < contentEnd && (fileContents[keyStart] == ' ' || fileContents[keyStart] == '\t'))
			++keyStart;

		if (contentEnd - keyStart >= key.size() && fileContents.compare(keyStart, key.size(), key) == 0)
		{
			size_t cursor = keyStart + key.size();
			while (cursor < contentEnd && (fileContents[cursor] == ' ' || fileContents[cursor] == '\t'))
				++cursor;
			if (cursor < contentEnd && fileContents[cursor] == '=')
			{
				++cursor;
				while (cursor < contentEnd && (fileContents[cursor] == ' ' || fileContents[cursor] == '\t'))
					++cursor;
				size_t valueEnd = contentEnd;
				while (valueEnd > cursor && (fileContents[valueEnd - 1] == ' ' || fileContents[valueEnd - 1] == '\t'))
					--valueEnd;
				value = fileContents.substr(cursor, valueEnd - cursor);
				return true;
			}
		}

		if (lineEnd == std::string::npos)
			break;
		lineStart = lineEnd + 1;
		if (fileContents[lineEnd] == '\r' && lineStart < fileContents.size() && fileContents[lineStart] == '\n')
			++lineStart;
	}
	return false;
}

static bool ReplaceSettingValue(std::string& fileContents, const std::string& key, const std::string& value)
{
	size_t lineStart = 0;
	while (lineStart <= fileContents.size())
	{
		const size_t lineEnd = fileContents.find_first_of("\r\n", lineStart);
		const size_t contentEnd = lineEnd == std::string::npos ? fileContents.size() : lineEnd;
		size_t keyStart = lineStart;
		while (keyStart < contentEnd && (fileContents[keyStart] == ' ' || fileContents[keyStart] == '\t'))
			++keyStart;

		if (contentEnd - keyStart >= key.size() && fileContents.compare(keyStart, key.size(), key) == 0)
		{
			size_t cursor = keyStart + key.size();
			while (cursor < contentEnd && (fileContents[cursor] == ' ' || fileContents[cursor] == '\t'))
				++cursor;
			if (cursor < contentEnd && fileContents[cursor] == '=')
			{
				fileContents.replace(cursor + 1, contentEnd - cursor - 1, value);
				return true;
			}
		}

		if (lineEnd == std::string::npos)
			break;
		lineStart = lineEnd + 1;
		if (fileContents[lineEnd] == '\r' && lineStart < fileContents.size() && fileContents[lineStart] == '\n')
			++lineStart;
	}
	return false;
}

static std::string CurrentLocalTimestamp()
{
	SYSTEMTIME st{};
	GetLocalTime(&st);
	char buffer[32]{};
	sprintf_s(buffer, "%04u-%02u-%02u %02u:%02u:%02u",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	return buffer;
}

void SettingsManager::InitSettingsManager()
{
	GetAllConfigFilePaths();
	uevr::API::get()->log_info("%s", uevrConfigFilePath.c_str());
	uevr::API::get()->log_info("%s", pluginConfigFilePath.c_str());
	FetchUevrSettings(false);
	FetchPluginSettings();
}

void SettingsManager::FetchUevrSettings(bool writeToPlugin)
{
	if (debugMod) uevr::API::get()->log_info("UpdateUevrSettings()");
	
	xAxisSensitivity = SettingsManager::GetFloatValueFromFile(uevrConfigFilePath, "VR_AimSpeed", 125.0f) * 10; //*10 because the base UEVR setting is too low as is 
	joystickDeadzone = SettingsManager::GetFloatValueFromFile(uevrConfigFilePath, "VR_JoystickDeadzone", 0.1f);
	uevr_DecoupledPitch = SettingsManager::GetBoolValueFromFile(uevrConfigFilePath, "VR_DecoupledPitch", true);
	uevr_LerpPitch = SettingsManager::GetBoolValueFromFile(uevrConfigFilePath, "VR_LerpCameraPitch", true);
	uevr_LerpRoll = SettingsManager::GetBoolValueFromFile(uevrConfigFilePath, "VR_LerpCameraRoll", true);
	uevr_LerpYaw = SettingsManager::GetBoolValueFromFile(uevrConfigFilePath, "VR_LerpCameraYaw", false);
	uevr_MovementOrientation = std::clamp(
		SettingsManager::GetIntValueFromFile(uevrConfigFilePath, "VR_MovementOrientation", 0), 0, 2);

	if (writeToPlugin)
		WriteChangedSettingsToPluginConfigFile();
	if (debugMod) uevr::API::get()->log_info("UEVR Settings Updated");
}

void SettingsManager::FetchPluginSettings()
{
	if (debugMod) uevr::API::get()->log_info("UpdatePluginSettings()");

	debugMod = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "debugMod", false);
	enableCombatAssist = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableCombatAssist", true);
	enableCombatAssistAmmo = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableCombatAssistAmmo", true);
	enableManualReloadMode = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableManualReloadMode", false);
	enableCombatAssistDamage = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableCombatAssistDamage", false);
	enableCombatAssistAnimationTiming = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableCombatAssistAnimationTiming", false);
	enableCombatAssistWeaponSkill = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableCombatAssistWeaponSkill", false);
	enableWeaponNoSpread = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableWeaponNoSpread",
		SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableSilencedPistolNoSpread", false));
	enableMotionWeaponRecoil = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableMotionWeaponRecoil", false);
	enableCompactWeaponReticle = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableCompactWeaponReticle", true);
	enableVrScope = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableVrScope", true);
	enableSilencedPistolAimStability = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableSilencedPistolAimStability", true);
	enableHealthRecovery = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableHealthRecovery", true);
	enableBulletDamageResistance = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableBulletDamageResistance", true);
	debugSpreadProbe = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "DebugSpreadProbe", false);
	enableAimCalibrationProbe = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableAimCalibrationProbe", false);
	enableUtilityWeaponAimBypass = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableUtilityWeaponAimBypass", true);
	enableUtilityWeaponCycleReset = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableUtilityWeaponCycleReset", true);
	enableDirectWeaponCycle = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableDirectWeaponCycle", true);
	enableABWeaponCycleTest = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableABWeaponCycleTest", false);
	enableAimAlignment = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableAimAlignment", true);
	enableLegacyCrosshairCompensation = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableLegacyCrosshairCompensation", false);
	enableNativeShotOriginRedirects = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableNativeShotOriginRedirects", false);
	enableCameraProfiles = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableCameraProfiles", true);
	enableBodyVisibility = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableBodyVisibility", true);
	enableBulletTraceHidden = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableBulletTraceHidden", true);
	enableDualGripAimFire = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableDualGripAimFire", true);
	enableCustomAkimbo = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableCustomAkimbo", false);
	enableTwoHandStabilization = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableTwoHandStabilization", false);
	enableAlternateWeaponHandsVisibility = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableAlternateWeaponHandsVisibility", true);
	enableFreeAimWeaponHands = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableFreeAimWeaponHands", false);
	enableGripCalibration = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableGripCalibration", false);
	enableGripWeaponCycle = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableGripWeaponCycle", false);
	enablePhoneAnswerGripTap = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnablePhoneAnswerGripTap", true);
	enableLegacyDPadModulator = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableLegacyDPadModulator", false);
	enableChordPauseMenu = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableChordPauseMenu", true);
	enableChordHudToggle = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableChordHudToggle", true);
	enablePauseUiAutoShow = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnablePauseUiAutoShow", true);
	enableHudAutoHide = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableHudAutoHide", true);
	enableShortPressCameraSwitch = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableShortPressCameraSwitch", true);
	enableFirstPersonCameraLock = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableFirstPersonCameraLock", true);
	enableVehicleFaceButtonFire = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableVehicleFaceButtonFire", true);
	enableAircraftNativeControls = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableAircraftNativeControls", true);
	enableR3LeftStickDpad = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableR3LeftStickDpad", false);
	enableMotionThrowables = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableMotionThrowables", true);
	enableThrowableMotionProbe = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableThrowableMotionProbe", false);
	enableNativeMolotovMode = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableNativeMolotovMode", false);
	enableMotionMeleeImpactAudio = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableMotionMeleeImpactAudio", true);
	enableFireTaskPrewarm = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableFireTaskPrewarm", true);
	debugInputLayerProbe = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "DebugInputLayerProbe", false);
	enableHudHiddenByDefault = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableHudHiddenByDefault", true);
	enableShowUiAtStartup = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "EnableShowUiAtStartup", !enableHudHiddenByDefault);
	EnsureVisibleFeatureFlagConfigValues();
	if (!featureFlagStatusInitialized)
	{
		activeCombatAssist = enableCombatAssist;
		activeCombatAssistAmmo = enableCombatAssistAmmo;
		activeManualReloadMode = enableManualReloadMode;
		activeCompactWeaponReticle = enableCompactWeaponReticle;
		activeBulletTraceHidden = enableBulletTraceHidden;
		activeShowUiAtStartup = enableShowUiAtStartup;
		featureFlagStatusInitialized = true;
	}

	leftHandedMode = (LeftHandedMode)SettingsManager::GetIntValueFromFile(pluginConfigFilePath, "LeftHandedMode", 0);
	uevr::API::get()->dispatch_lua_event("playerIsLeftHanded", std::to_string(leftHandedMode));
	leftHandedOnlyWhileOnFoot = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "LeftHandedOnlyWhileOnFoot", true);
	uevr::API::get()->dispatch_lua_event("leftHandedOnlyWhileOnFoot", leftHandedOnlyWhileOnFoot ? "true" : "false");

	onFoot_DecoupledPitch = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "OnFoot_DecoupledPitch", false);
	onFoot_LerpPitch = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "OnFoot_LerpPitch", true);
	onFoot_LerpRoll = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "OnFoot_LerpRoll", true);
	onFoot_LerpYaw = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "OnFoot_LerpYaw", true);
	drivingCar_DecoupledPitch = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "DrivingCar_DecoupledPitch", true);
	drivingCar_LerpPitch = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "DrivingCar_LerpPitch", true);
	drivingCar_LerpRoll = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "DrivingCar_LerpRoll", true);
	drivingCar_LerpYaw = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "DrivingCar_LerpYaw", true);
	drivingBike_DecoupledPitch = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "DrivingBike_DecoupledPitch", true);
	drivingBike_LerpPitch = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "DrivingBike_LerpPitch", true);
	drivingBike_LerpRoll = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "DrivingBike_LerpRoll", true);
	drivingBike_LerpYaw = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "DrivingBike_LerpYaw", true);
	flying_DecoupledPitch = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "Flying_DecoupledPitch", true);
	flying_LerpPitch = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "Flying_LerpPitch", true);
	flying_LerpRoll = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "Flying_LerpRoll", true);
	flying_LerpYaw = SettingsManager::GetBoolValueFromFile(pluginConfigFilePath, "Flying_LerpYaw", true);

	onFoot_CameraOffset = FetchCameraOffsetProfile("OnFoot", CameraOffsetProfile{0.0f, 0.0f, 0.0f});
	drivingCar_CameraOffset = FetchCameraOffsetProfile("DrivingCar", CameraOffsetProfile{0.0f, 0.0f, 0.0f});
	drivingBike_CameraOffset = FetchCameraOffsetProfile("DrivingBike", CameraOffsetProfile{0.0f, 0.0f, 0.0f});
	flying_CameraOffset = FetchCameraOffsetProfile("Flying", CameraOffsetProfile{0.0f, 0.0f, 0.0f});

	DispatchFeatureFlagsToLua();
	WriteFeatureFlagStatus("config_load");

	if (debugMod) uevr::API::get()->log_info("Plugin Settings Updated");
}

void SettingsManager::EnsureVisibleFeatureFlagConfigValues()
{
	std::ifstream input(pluginConfigFilePath, std::ios::binary);
	if (!input)
		return;
	const std::string fileContents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	input.close();

	std::vector<std::pair<std::string, std::string>> missing;
	auto addIfMissing = [&](const char* key, bool value) {
		std::string ignored;
		if (!FindSettingValue(fileContents, key, ignored))
			missing.push_back({key, BoolText(value)});
	};

	addIfMissing("EnableCombatAssist", enableCombatAssist);
	addIfMissing("EnableAimAlignment", enableAimAlignment);
	addIfMissing("EnableLegacyCrosshairCompensation", enableLegacyCrosshairCompensation);
	addIfMissing("EnableNativeShotOriginRedirects", enableNativeShotOriginRedirects);
	addIfMissing("EnableWeaponNoSpread", enableWeaponNoSpread);
	addIfMissing("EnableCombatAssistAmmo", enableCombatAssistAmmo);
	addIfMissing("EnableManualReloadMode", enableManualReloadMode);
	addIfMissing("EnableCombatAssistDamage", enableCombatAssistDamage);
	addIfMissing("EnableHealthRecovery", enableHealthRecovery);
	addIfMissing("EnableBulletDamageResistance", enableBulletDamageResistance);
	addIfMissing("EnableAimCalibrationProbe", enableAimCalibrationProbe);
	addIfMissing("EnableVrScope", enableVrScope);
	addIfMissing("EnableCompactWeaponReticle", enableCompactWeaponReticle);
	addIfMissing("EnableBulletTraceHidden", enableBulletTraceHidden);
	addIfMissing("EnableDualGripAimFire", enableDualGripAimFire);
	addIfMissing("EnableCustomAkimbo", enableCustomAkimbo);
	addIfMissing("EnableTwoHandStabilization", enableTwoHandStabilization);
	addIfMissing("EnableAlternateWeaponHandsVisibility", enableAlternateWeaponHandsVisibility);
	addIfMissing("EnableFreeAimWeaponHands", enableFreeAimWeaponHands);
	addIfMissing("EnableGripCalibration", enableGripCalibration);
	addIfMissing("EnableGripWeaponCycle", enableGripWeaponCycle);
	addIfMissing("EnablePhoneAnswerGripTap", enablePhoneAnswerGripTap);
	addIfMissing("EnableChordPauseMenu", enableChordPauseMenu);
	addIfMissing("EnableChordHudToggle", enableChordHudToggle);
	addIfMissing("EnableVehicleFaceButtonFire", enableVehicleFaceButtonFire);
	addIfMissing("EnableAircraftNativeControls", enableAircraftNativeControls);
	addIfMissing("EnableMotionThrowables", enableMotionThrowables);
	addIfMissing("EnableThrowableMotionProbe", enableThrowableMotionProbe);
	addIfMissing("EnableNativeMolotovMode", enableNativeMolotovMode);
	addIfMissing("EnableMotionMeleeImpactAudio", enableMotionMeleeImpactAudio);
	addIfMissing("EnableFireTaskPrewarm", enableFireTaskPrewarm);
	addIfMissing("EnablePauseUiAutoShow", enablePauseUiAutoShow);
	addIfMissing("EnableHudAutoHide", enableHudAutoHide);
	addIfMissing("EnableShowUiAtStartup", enableShowUiAtStartup);
	addIfMissing("EnableFirstPersonCameraLock", enableFirstPersonCameraLock);
	addIfMissing("EnableCameraProfiles", enableCameraProfiles);
	addIfMissing("EnableBodyVisibility", enableBodyVisibility);

	if (!missing.empty())
	{
		SetMultipleValuesToFile(pluginConfigFilePath, missing);
		uevr::API::get()->log_info("[FeatureFlags] Added %llu missing user-facing config key(s)",
			static_cast<unsigned long long>(missing.size()));
	}
}

void SettingsManager::DispatchFeatureFlagsToLua() const
{
	auto dispatchFlag = [](const char* name, bool value, bool live) {
		std::string payload = std::string(name) + "=" + (value ? "true" : "false") + ";live=" + (live ? "true" : "false");
		uevr::API::get()->dispatch_lua_event("featureFlagState", payload);
	};

	dispatchFlag("EnableCombatAssist", enableCombatAssist, true);
	dispatchFlag("EnableCombatAssistAmmo", enableCombatAssistAmmo, false);
	dispatchFlag("EnableManualReloadMode", enableManualReloadMode, false);
	dispatchFlag("EnableCombatAssistDamage", enableCombatAssistDamage, true);
	dispatchFlag("EnableCombatAssistAnimationTiming", enableCombatAssistAnimationTiming, true);
	dispatchFlag("EnableCombatAssistWeaponSkill", enableCombatAssistWeaponSkill, true);
	dispatchFlag("EnableWeaponNoSpread", enableWeaponNoSpread, true);
	dispatchFlag("EnableMotionWeaponRecoil", enableMotionWeaponRecoil, true);
	dispatchFlag("EnableCompactWeaponReticle", enableCompactWeaponReticle, false);
	dispatchFlag("EnableVrScope", enableVrScope, true);
	dispatchFlag("EnableSilencedPistolAimStability", enableSilencedPistolAimStability, true);
	dispatchFlag("EnableHealthRecovery", enableHealthRecovery, true);
	dispatchFlag("EnableBulletDamageResistance", enableBulletDamageResistance, true);
	dispatchFlag("DebugSpreadProbe", debugSpreadProbe, true);
	dispatchFlag("EnableAimCalibrationProbe", enableAimCalibrationProbe, true);
	dispatchFlag("EnableUtilityWeaponAimBypass", enableUtilityWeaponAimBypass, true);
	dispatchFlag("EnableUtilityWeaponCycleReset", enableUtilityWeaponCycleReset, true);
	dispatchFlag("EnableDirectWeaponCycle", enableDirectWeaponCycle, true);
	dispatchFlag("EnableABWeaponCycleTest", enableABWeaponCycleTest, true);
	dispatchFlag("EnableBodyVisibility", enableBodyVisibility, true);
	dispatchFlag("EnableCameraProfiles", enableCameraProfiles, true);
	dispatchFlag("DebugLogging", debugMod, true);
	dispatchFlag("EnableAimAlignment", enableAimAlignment, true);
	dispatchFlag("EnableLegacyCrosshairCompensation", enableLegacyCrosshairCompensation, true);
	dispatchFlag("EnableNativeShotOriginRedirects", enableNativeShotOriginRedirects, true);
	dispatchFlag("EnableBulletTraceHidden", enableBulletTraceHidden, false);
	dispatchFlag("EnableDualGripAimFire", enableDualGripAimFire, true);
	dispatchFlag("EnableCustomAkimbo", enableCustomAkimbo, true);
	dispatchFlag("EnableTwoHandStabilization", enableTwoHandStabilization, true);
	dispatchFlag("EnableAlternateWeaponHandsVisibility", enableAlternateWeaponHandsVisibility, true);
	dispatchFlag("EnableFreeAimWeaponHands", enableFreeAimWeaponHands, true);
	dispatchFlag("EnableGripCalibration", enableGripCalibration, true);
	dispatchFlag("EnableGripWeaponCycle", enableGripWeaponCycle, true);
	dispatchFlag("EnablePhoneAnswerGripTap", enablePhoneAnswerGripTap, true);
	dispatchFlag("EnableLegacyDPadModulator", enableLegacyDPadModulator, true);
	dispatchFlag("EnableChordPauseMenu", enableChordPauseMenu, true);
	dispatchFlag("EnableChordHudToggle", enableChordHudToggle, true);
	dispatchFlag("EnablePauseUiAutoShow", enablePauseUiAutoShow, true);
	dispatchFlag("EnableHudAutoHide", enableHudAutoHide, true);
	dispatchFlag("EnableShortPressCameraSwitch", enableShortPressCameraSwitch, true);
	dispatchFlag("EnableFirstPersonCameraLock", enableFirstPersonCameraLock, true);
	dispatchFlag("EnableVehicleFaceButtonFire", enableVehicleFaceButtonFire, true);
	dispatchFlag("EnableAircraftNativeControls", enableAircraftNativeControls, true);
	dispatchFlag("EnableR3LeftStickDpad", enableR3LeftStickDpad, true);
	dispatchFlag("EnableMotionThrowables", enableMotionThrowables, true);
	dispatchFlag("EnableThrowableMotionProbe", enableThrowableMotionProbe, true);
	dispatchFlag("EnableNativeMolotovMode", enableNativeMolotovMode, true);
	dispatchFlag("EnableFireTaskPrewarm", enableFireTaskPrewarm, true);
	dispatchFlag("DebugInputLayerProbe", debugInputLayerProbe, true);
	dispatchFlag("EnableShowUiAtStartup", enableShowUiAtStartup, true);
}

void SettingsManager::ApplyHudVisibilityDefault()
{
	SetHudUiVisible(enableShowUiAtStartup);
}

bool SettingsManager::SetHudUiVisible(bool visible)
{
	// HUD visibility is transient runtime state. Persisting every pause/manual toggle
	// makes UEVR reload its profile and can feed a renderer reset back into NoControls.
	desiredHudUiVisible = visible;
	hudUiVisibilityInitialized = true;
	if (uevr::API::get()->param()->vr == nullptr || !uevr::API::VR::is_runtime_ready())
		return false;

	uevr::API::VR::set_mod_value("VR_EnableGUI", visible);
	return true;
}

bool SettingsManager::GetPause2dScreenMode(bool& enabled) const
{
	if (uevr::API::get()->param()->vr == nullptr || !uevr::API::VR::is_runtime_ready())
		return false;

	enabled = uevr::API::VR::get_mod_value<bool>("VR_2DScreenMode");
	return true;
}

bool SettingsManager::SetPause2dScreenMode(bool enabled)
{
	if (uevr::API::get()->param()->vr == nullptr || !uevr::API::VR::is_runtime_ready())
		return false;

	// A persistent ownership marker makes this transient override crash-safe.
	// It is written only when the plugin turns 2D mode on, never for a user's
	// pre-existing 2D preference.
	if (enabled && !WritePause2dOwnershipMarker())
	{
		uevr::API::get()->log_error("%s",
			"[PauseUI] refusing temporary 2D mode because ownership marker could not be written");
		return false;
	}

	// This is intentionally live-only. Pause/failure screens temporarily use
	// UEVR's complete frame compositor, then restore the user's profile state.
	uevr::API::VR::set_mod_value("VR_2DScreenMode", enabled);
	if (!enabled)
		ClearPause2dOwnershipMarker();
	return true;
}

bool SettingsManager::RecoverPluginOwnedPause2dScreenMode()
{
	const std::string markerPath = GetPause2dOwnershipMarkerPath();
	if (markerPath.empty() || GetFileAttributesA(markerPath.c_str()) == INVALID_FILE_ATTRIBUTES)
		return true;

	if (uevr::API::get()->param()->vr == nullptr || !uevr::API::VR::is_runtime_ready())
		return false;

	uevr::API::VR::set_mod_value("VR_2DScreenMode", false);
	ClearPause2dOwnershipMarker();
	uevr::API::get()->log_warn("%s",
		"[PauseUI] recovered plugin-owned temporary 2D mode after interrupted session");
	return true;
}

std::string SettingsManager::GetPause2dOwnershipMarkerPath() const
{
	if (pluginConfigFilePath.empty())
		return {};
	const size_t fileNamePos = pluginConfigFilePath.find_last_of("\\/");
	if (fileNamePos == std::string::npos)
		return "SAVR_pause_2d_owned.flag";
	return pluginConfigFilePath.substr(0, fileNamePos + 1) + "SAVR_pause_2d_owned.flag";
}

bool SettingsManager::WritePause2dOwnershipMarker() const
{
	const std::string markerPath = GetPause2dOwnershipMarkerPath();
	if (markerPath.empty())
		return false;
	std::ofstream marker(markerPath, std::ios::binary | std::ios::trunc);
	if (!marker)
		return false;
	marker << "plugin-owned temporary VR_2DScreenMode=true\n";
	marker.flush();
	return marker.good();
}

void SettingsManager::ClearPause2dOwnershipMarker() const
{
	const std::string markerPath = GetPause2dOwnershipMarkerPath();
	if (!markerPath.empty())
		std::remove(markerPath.c_str());
}

bool SettingsManager::SetFeatureFlagFromUi(const std::string& name, bool value, bool& liveApply)
{
	liveApply = false;

	if (name == "EnableCombatAssist") {
		enableCombatAssist = value;
		activeCombatAssist = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableCombatAssist", value);
	}
	else if (name == "EnableCombatAssistAmmo") {
		enableCombatAssistAmmo = value;
		SetBoolValueToFile(pluginConfigFilePath, "EnableCombatAssistAmmo", value);
	}
	else if (name == "EnableManualReloadMode") {
		enableManualReloadMode = value;
		SetBoolValueToFile(pluginConfigFilePath, "EnableManualReloadMode", value);
	}
	else if (name == "EnableCombatAssistDamage") {
		enableCombatAssistDamage = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableCombatAssistDamage", value);
	}
	else if (name == "EnableCombatAssistAnimationTiming") {
		enableCombatAssistAnimationTiming = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableCombatAssistAnimationTiming", value);
	}
	else if (name == "EnableCombatAssistWeaponSkill") {
		enableCombatAssistWeaponSkill = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableCombatAssistWeaponSkill", value);
	}
	else if (name == "EnableWeaponNoSpread" || name == "EnableSilencedPistolNoSpread") {
		enableWeaponNoSpread = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableWeaponNoSpread", value);
	}
	else if (name == "EnableMotionWeaponRecoil") {
		enableMotionWeaponRecoil = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableMotionWeaponRecoil", value);
	}
	else if (name == "EnableCompactWeaponReticle") {
		enableCompactWeaponReticle = value;
		SetBoolValueToFile(pluginConfigFilePath, "EnableCompactWeaponReticle", value);
	}
	else if (name == "EnableVrScope") {
		enableVrScope = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableVrScope", value);
	}
	else if (name == "EnableSilencedPistolAimStability") {
		enableSilencedPistolAimStability = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableSilencedPistolAimStability", value);
	}
	else if (name == "EnableHealthRecovery") {
		enableHealthRecovery = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableHealthRecovery", value);
	}
	else if (name == "EnableBulletDamageResistance") {
		enableBulletDamageResistance = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableBulletDamageResistance", value);
	}
	else if (name == "DebugSpreadProbe") {
		debugSpreadProbe = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "DebugSpreadProbe", value);
	}
	else if (name == "EnableAimCalibrationProbe") {
		enableAimCalibrationProbe = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableAimCalibrationProbe", value);
	}
	else if (name == "EnableUtilityWeaponAimBypass") {
		enableUtilityWeaponAimBypass = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableUtilityWeaponAimBypass", value);
	}
	else if (name == "EnableUtilityWeaponCycleReset") {
		enableUtilityWeaponCycleReset = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableUtilityWeaponCycleReset", value);
	}
	else if (name == "EnableDirectWeaponCycle") {
		enableDirectWeaponCycle = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableDirectWeaponCycle", value);
	}
	else if (name == "EnableABWeaponCycleTest") {
		enableABWeaponCycleTest = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableABWeaponCycleTest", value);
	}
	else if (name == "EnableBodyVisibility") {
		enableBodyVisibility = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableBodyVisibility", value);
	}
	else if (name == "EnableCameraProfiles") {
		enableCameraProfiles = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableCameraProfiles", value);
	}
	else if (name == "DebugLogging") {
		debugMod = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "debugMod", value);
	}
	else if (name == "EnableAimAlignment") {
		enableAimAlignment = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableAimAlignment", value);
	}
	else if (name == "EnableLegacyCrosshairCompensation") {
		enableLegacyCrosshairCompensation = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableLegacyCrosshairCompensation", value);
	}
	else if (name == "EnableNativeShotOriginRedirects") {
		enableNativeShotOriginRedirects = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableNativeShotOriginRedirects", value);
	}
	else if (name == "EnableBulletTraceHidden") {
		enableBulletTraceHidden = value;
		SetBoolValueToFile(pluginConfigFilePath, "EnableBulletTraceHidden", value);
	}
	else if (name == "EnableDualGripAimFire") {
		enableDualGripAimFire = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableDualGripAimFire", value);
	}
	else if (name == "EnableCustomAkimbo") {
		enableCustomAkimbo = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableCustomAkimbo", value);
	}
	else if (name == "EnableTwoHandStabilization") {
		enableTwoHandStabilization = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableTwoHandStabilization", value);
	}
	else if (name == "EnableAlternateWeaponHandsVisibility") {
		enableAlternateWeaponHandsVisibility = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableAlternateWeaponHandsVisibility", value);
	}
	else if (name == "EnableFreeAimWeaponHands") {
		enableFreeAimWeaponHands = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableFreeAimWeaponHands", value);
	}
	else if (name == "EnableGripCalibration") {
		enableGripCalibration = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableGripCalibration", value);
	}
	else if (name == "EnableGripWeaponCycle") {
		enableGripWeaponCycle = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableGripWeaponCycle", value);
	}
	else if (name == "EnablePhoneAnswerGripTap") {
		enablePhoneAnswerGripTap = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnablePhoneAnswerGripTap", value);
	}
	else if (name == "EnableLegacyDPadModulator") {
		enableLegacyDPadModulator = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableLegacyDPadModulator", value);
	}
	else if (name == "EnableChordPauseMenu") {
		enableChordPauseMenu = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableChordPauseMenu", value);
	}
	else if (name == "EnableChordHudToggle") {
		enableChordHudToggle = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableChordHudToggle", value);
	}
	else if (name == "EnablePauseUiAutoShow") {
		enablePauseUiAutoShow = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnablePauseUiAutoShow", value);
	}
	else if (name == "EnableHudAutoHide") {
		enableHudAutoHide = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableHudAutoHide", value);
	}
	else if (name == "EnableShortPressCameraSwitch") {
		enableShortPressCameraSwitch = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableShortPressCameraSwitch", value);
	}
	else if (name == "EnableFirstPersonCameraLock") {
		enableFirstPersonCameraLock = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableFirstPersonCameraLock", value);
	}
	else if (name == "EnableVehicleFaceButtonFire") {
		enableVehicleFaceButtonFire = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableVehicleFaceButtonFire", value);
	}
	else if (name == "EnableAircraftNativeControls") {
		enableAircraftNativeControls = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableAircraftNativeControls", value);
	}
	else if (name == "EnableR3LeftStickDpad") {
		enableR3LeftStickDpad = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableR3LeftStickDpad", value);
	}
	else if (name == "EnableMotionThrowables") {
		enableMotionThrowables = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableMotionThrowables", value);
	}
	else if (name == "EnableThrowableMotionProbe") {
		enableThrowableMotionProbe = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableThrowableMotionProbe", value);
	}
	else if (name == "EnableNativeMolotovMode") {
		enableNativeMolotovMode = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableNativeMolotovMode", value);
	}
	else if (name == "EnableMotionMeleeImpactAudio") {
		enableMotionMeleeImpactAudio = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableMotionMeleeImpactAudio", value);
	}
	else if (name == "EnableFireTaskPrewarm") {
		enableFireTaskPrewarm = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "EnableFireTaskPrewarm", value);
	}
	else if (name == "EnableShowUiAtStartup") {
		enableShowUiAtStartup = value;
		SetBoolValueToFile(pluginConfigFilePath, "EnableShowUiAtStartup", value);
	}
	else if (name == "DebugInputLayerProbe") {
		debugInputLayerProbe = value;
		liveApply = true;
		SetBoolValueToFile(pluginConfigFilePath, "DebugInputLayerProbe", value);
	}
	else {
		return false;
	}

	DispatchFeatureFlagsToLua();
	WriteFeatureFlagStatus("ui_event");
	return true;
}

void SettingsManager::WriteFeatureFlagStatus(const std::string& reason)
{
	if (pluginStatusFilePath.empty())
		return;

	std::ofstream out(pluginStatusFilePath, std::ios::trunc);
	if (!out.is_open())
		return;

	auto writeFlag = [&](const char* name, bool saved, bool active, const char* applyMode, const char* group, const char* label) {
		const bool needsRestart = saved != active;
		std::string displayLabel(label);
		if (std::string(applyMode) == "RestartOrReinject")
			displayLabel += " NEEDS RESTART";
		out << name << "_Label=" << displayLabel << "\n";
		out << name << "_Group=" << group << "\n";
		out << name << "_Saved=" << BoolText(saved) << "\n";
		out << name << "_Active=" << BoolText(active) << "\n";
		out << name << "_ApplyMode=" << applyMode << "\n";
		out << name << "_NeedsRestart=" << BoolText(needsRestart) << "\n";
		out << "\n";
	};

	const bool anyRestartNeeded =
		(enableCombatAssistAmmo != activeCombatAssistAmmo) ||
		(enableManualReloadMode != activeManualReloadMode) ||
		(enableCompactWeaponReticle != activeCompactWeaponReticle) ||
		(enableBulletTraceHidden != activeBulletTraceHidden) ||
		(enableShowUiAtStartup != activeShowUiAtStartup);

	out << "[Runtime]\n";
	out << "LastUpdated=" << CurrentLocalTimestamp() << "\n";
	out << "Reason=" << reason << "\n";
	out << "ConfigPath=" << pluginConfigFilePath << "\n";
	out << "StatusPath=" << pluginStatusFilePath << "\n";
	out << "AnyRestartNeeded=" << BoolText(anyRestartNeeded) << "\n";
	out << "Notes=Saved values come from UEVR_GTASADE_config.txt. Active values are what the plugin last reports for this injected session.\n";
	out << "\n";

	out << "[FeatureFlags]\n";
	writeFlag("EnableCombatAssist", enableCombatAssist, activeCombatAssist, "LiveOnConfigReload", "Combat and aiming", "Combat range and accuracy hooks [recommended]");
	writeFlag("EnableCombatAssistAmmo", enableCombatAssistAmmo, activeCombatAssistAmmo, "RestartOrReinject", "Combat and aiming", "Unlimited ammo supply (automatic reserve or manual magazines)");
	writeFlag("EnableManualReloadMode", enableManualReloadMode, activeManualReloadMode, "RestartOrReinject", "Core combat feel", "Right-grip tap manual reload [experimental; replaces automatic reload]");
	writeFlag("EnableCombatAssistDamage", enableCombatAssistDamage, enableCombatAssistDamage, "LiveOnConfigReload", "Core combat feel", "Weapon damage boost");
	writeFlag("EnableCombatAssistAnimationTiming", enableCombatAssistAnimationTiming, enableCombatAssistAnimationTiming, "LiveOnConfigReload", "Core combat feel", "Animation timing edits [experimental]");
	writeFlag("EnableCombatAssistWeaponSkill", enableCombatAssistWeaponSkill, enableCombatAssistWeaponSkill, "LiveOnConfigReload", "Core combat feel", "Weapon skill assist");
	writeFlag("EnableWeaponNoSpread", enableWeaponNoSpread, enableWeaponNoSpread, "LiveOnConfigReload", "Core combat feel", "Weapon no spread");
	writeFlag("EnableMotionWeaponRecoil", enableMotionWeaponRecoil, enableMotionWeaponRecoil, "LiveOnConfigReload", "Core combat feel", "Motion weapon recoil [recommended off for steady aim]");
	writeFlag("EnableCompactWeaponReticle", enableCompactWeaponReticle, activeCompactWeaponReticle, "RestartOrReinject", "VR visuals", "Small aiming reticle");
	writeFlag("EnableVrScope", enableVrScope, enableVrScope, "LiveOnConfigReload", "VR visuals", "VR sniper scope [recommended]");
	writeFlag("EnableSilencedPistolAimStability", enableSilencedPistolAimStability, enableSilencedPistolAimStability, "LiveOnConfigReload", "Core combat feel", "Silenced pistol aim stability");
	writeFlag("EnableHealthRecovery", enableHealthRecovery, enableHealthRecovery, "LiveOnConfigReload", "Core combat feel", "Recover health to 50% after 10 damage-free seconds");
	writeFlag("EnableBulletDamageResistance", enableBulletDamageResistance, enableCombatAssist && enableBulletDamageResistance, "LiveOnConfigReload", "Core combat feel", "50% incoming bullet damage for CJ [recommended]");
	writeFlag("DebugSpreadProbe", debugSpreadProbe, debugSpreadProbe, "LiveOnConfigReload", "Diagnostics", "Spread probe logs");
	writeFlag("EnableAimCalibrationProbe", enableAimCalibrationProbe, enableAimCalibrationProbe, "LiveOnConfigReload", "Diagnostics", "Native aim calibration probe [experimental]");
	writeFlag("EnableUtilityWeaponAimBypass", enableUtilityWeaponAimBypass, enableUtilityWeaponAimBypass, "LiveOnConfigReload", "Core combat feel", "Utility weapon aim bypass");
	writeFlag("EnableUtilityWeaponCycleReset", enableUtilityWeaponCycleReset, enableUtilityWeaponCycleReset, "LiveOnConfigReload", "VR controls", "Utility weapon cycle reset");
	writeFlag("EnableDirectWeaponCycle", enableDirectWeaponCycle, enableDirectWeaponCycle, "LiveOnConfigReload", "VR controls", "Direct weapon cycle");
	writeFlag("EnableABWeaponCycleTest", enableABWeaponCycleTest, enableABWeaponCycleTest, "LiveOnConfigReload", "VR controls", "A/B weapon cycle test");
	writeFlag("EnableAimAlignment", enableAimAlignment, enableAimAlignment, "LiveOnConfigReload", "Core combat feel", "Aim alignment");
	writeFlag("EnableLegacyCrosshairCompensation", enableLegacyCrosshairCompensation, enableLegacyCrosshairCompensation, "LiveOnConfigReload", "Core combat feel", "Legacy over-the-shoulder crosshair compensation [experimental]");
	writeFlag("EnableNativeShotOriginRedirects", enableNativeShotOriginRedirects, enableNativeShotOriginRedirects, "LiveOnConfigReload", "Core combat feel", "Experimental native shot/trail origin redirects [experimental]");
	writeFlag("EnableCameraProfiles", enableCameraProfiles, enableCameraProfiles, "LiveOnConfigReload", "VR comfort and camera", "Camera profiles");
	writeFlag("EnableBodyVisibility", enableBodyVisibility, enableBodyVisibility, "LiveOnConfigReload", "VR comfort and camera", "Body visibility");
	writeFlag("EnableDualGripAimFire", enableDualGripAimFire, enableDualGripAimFire, "LiveOnConfigReload", "VR controls", "Dual grip aim/fire");
	writeFlag("EnableCustomAkimbo", enableCustomAkimbo, enableCustomAkimbo, "LiveOnConfigReload", "VR controls", "Independent per-hand akimbo firing [experimental]");
	writeFlag("EnableTwoHandStabilization", enableTwoHandStabilization, enableTwoHandStabilization, "LiveOnConfigReload", "Core combat feel", "Two-hand long-gun stabilization [experimental]");
	writeFlag("EnableAlternateWeaponHandsVisibility", enableAlternateWeaponHandsVisibility, enableAlternateWeaponHandsVisibility, "LiveOnConfigReload", "VR controls", "Alternate hands/weapon visibility");
	writeFlag("EnableFreeAimWeaponHands", enableFreeAimWeaponHands, enableFreeAimWeaponHands, "LiveOnConfigReload", "VR controls", "Animated hands attached to controllers for unarmed and controller-driven weapons [experimental]");
	writeFlag("EnableGripCalibration", enableGripCalibration, enableGripCalibration, "LiveOnConfigReload", "VR controls", "Temporary per-weapon grip calibration [experimental; disabled by default]");
	writeFlag("EnableGripWeaponCycle", enableGripWeaponCycle, false, "DisabledCompatibilityFlag", "VR controls", "Legacy grip weapon cycle (disabled; Quest X owns cycling)");
	writeFlag("EnablePhoneAnswerGripTap", enablePhoneAnswerGripTap, enablePhoneAnswerGripTap, "LiveOnConfigReload", "VR controls", "Quick right-grip tap answers a ringing phone [recommended]");
	writeFlag("EnableLegacyDPadModulator", enableLegacyDPadModulator, enableLegacyDPadModulator, "LiveOnConfigReload", "VR controls", "Legacy grip D-pad mapping [experimental]");
	writeFlag("EnableChordPauseMenu", enableChordPauseMenu, enableChordPauseMenu, "LiveOnConfigReload", "VR controls", "Right B + Left Y pause chord");
	writeFlag("EnableChordHudToggle", enableChordHudToggle, enableChordHudToggle, "LiveOnConfigReload", "VR controls", "Right A + Left X control-guide overlay");
	writeFlag("EnablePauseUiAutoShow", enablePauseUiAutoShow, enablePauseUiAutoShow, "LiveOnConfigReload", "VR controls", "Show UI while paused");
	writeFlag("EnableHudAutoHide", enableHudAutoHide, enableHudAutoHide, "LiveOnConfigReload", "VR controls", "Auto-hide HUD after 20 seconds");
	writeFlag("EnableShortPressCameraSwitch", enableShortPressCameraSwitch, enableShortPressCameraSwitch, "LiveOnConfigReload", "VR controls", "Short press left Quest menu camera switch");
	writeFlag("EnableFirstPersonCameraLock", enableFirstPersonCameraLock, enableFirstPersonCameraLock, "LiveOnConfigReload", "VR comfort and camera", "Lock first-person camera [recommended on]");
	writeFlag("EnableVehicleFaceButtonFire", enableVehicleFaceButtonFire, enableVehicleFaceButtonFire, "LiveOnConfigReload", "VR controls", "Vehicle live-lap weapon on right controller [ordinary driving prototype]");
	writeFlag("EnableAircraftNativeControls", enableAircraftNativeControls, enableAircraftNativeControls, "LiveOnConfigReload", "VR controls", "Aircraft native controls [recommended on, experimental]");
	writeFlag("EnableR3LeftStickDpad", enableR3LeftStickDpad, enableR3LeftStickDpad, "LiveOnConfigReload", "VR controls", "R3 + left stick D-pad [experimental]");
	writeFlag("EnableMotionThrowables", enableMotionThrowables, enableMotionThrowables, "LiveOnConfigReload", "Core combat feel", "Grip-release grenade/Molotov physical flight with DE-native impact lifecycle [experimental]");
	writeFlag("EnableThrowableMotionProbe", enableThrowableMotionProbe, enableThrowableMotionProbe, "LiveOnConfigReload", "Diagnostics", "Passive Molotov release and momentum probe");
	writeFlag("EnableNativeMolotovMode", enableNativeMolotovMode, enableNativeMolotovMode, "LiveOnConfigReload", "Core combat feel", "Use GTA's native Molotov throw path instead of custom motion flight [A/B test]");
	writeFlag("EnableMotionMeleeImpactAudio", enableMotionMeleeImpactAudio, enableMotionMeleeImpactAudio, "LiveOnConfigReload", "Core combat feel", "Basic ped/vehicle motion-melee impact audio");
	writeFlag("EnableFireTaskPrewarm", enableFireTaskPrewarm, enableFireTaskPrewarm, "LiveOnConfigReload", "Core combat feel", "Prewarm native firearm task while gripping for faster first shot");
	writeFlag("DebugInputLayerProbe", debugInputLayerProbe, debugInputLayerProbe, "LiveOnConfigReload", "Diagnostics", "Control stack input log");
	writeFlag("EnableShowUiAtStartup", enableShowUiAtStartup, activeShowUiAtStartup, "RestartOrReinject", "HUD and camera", "Show HUD when game starts");
	writeFlag("EnableBulletTraceHidden", enableBulletTraceHidden, activeBulletTraceHidden, "RestartOrReinject", "Restart/reinject required", "Hide bullet trace");
	writeFlag("DebugLogging", debugMod, debugMod, "LiveOnConfigReload", "Diagnostics", "Debug logging");
}

void SettingsManager::WriteChangedSettingsToPluginConfigFile()
{
	std::vector<std::pair<std::string, std::string>> pluginSettingsToApply;
	switch (cameraModeSettings)
	{
	case OnFoot:
		pluginSettingsToApply.push_back({"OnFoot_DecoupledPitch", uevr_DecoupledPitch ? "true" : "false"});
		pluginSettingsToApply.push_back({"OnFoot_LerpPitch", uevr_LerpPitch ? "true" : "false"});
		pluginSettingsToApply.push_back({"OnFoot_LerpRoll", uevr_LerpRoll ? "true" : "false"});
		pluginSettingsToApply.push_back({"OnFoot_LerpYaw", uevr_LerpYaw ? "true" : "false"});
		break;
	case DrivingCar:
		pluginSettingsToApply.push_back({"DrivingCar_DecoupledPitch", uevr_DecoupledPitch ? "true" : "false"});
		pluginSettingsToApply.push_back({"DrivingCar_LerpPitch", uevr_LerpPitch ? "true" : "false"});
		pluginSettingsToApply.push_back({"DrivingCar_LerpRoll", uevr_LerpRoll ? "true" : "false"});
		pluginSettingsToApply.push_back({"DrivingCar_LerpYaw", uevr_LerpYaw ? "true" : "false"});
		break;
	case DrivingBike:
		pluginSettingsToApply.push_back({"DrivingBike_DecoupledPitch", uevr_DecoupledPitch ? "true" : "false"});
		pluginSettingsToApply.push_back({"DrivingBike_LerpPitch", uevr_LerpPitch ? "true" : "false"});
		pluginSettingsToApply.push_back({"DrivingBike_LerpRoll", uevr_LerpRoll ? "true" : "false"});
		pluginSettingsToApply.push_back({"DrivingBike_LerpYaw", uevr_LerpYaw ? "true" : "false"});
		break;
	case Flying:
		pluginSettingsToApply.push_back({"Flying_DecoupledPitch", uevr_DecoupledPitch ? "true" : "false"});
		pluginSettingsToApply.push_back({"Flying_LerpPitch", uevr_LerpPitch ? "true" : "false"});
		pluginSettingsToApply.push_back({"Flying_LerpRoll", uevr_LerpRoll ? "true" : "false"});
		pluginSettingsToApply.push_back({"Flying_LerpYaw", uevr_LerpYaw ? "true" : "false"});
		break;
	}

	if (!pluginSettingsToApply.empty())
	{
		SetMultipleValuesToFile(pluginConfigFilePath, pluginSettingsToApply);
	}
}

SettingsManager::CameraOffsetProfile SettingsManager::FetchCameraOffsetProfile(const std::string& prefix, CameraOffsetProfile defaultValue)
{
	CameraOffsetProfile profile = defaultValue;
	if (prefix.empty())
		return SanitizeCameraOffsetProfile(profile, "default");

	profile.forward = GetFloatValueFromFile(pluginConfigFilePath, prefix + "_ForwardOffset", defaultValue.forward);
	profile.right = GetFloatValueFromFile(pluginConfigFilePath, prefix + "_RightOffset", defaultValue.right);
	profile.up = GetFloatValueFromFile(pluginConfigFilePath, prefix + "_UpOffset", defaultValue.up);

	return SanitizeCameraOffsetProfile(profile, prefix);
}

SettingsManager::CameraOffsetProfile SettingsManager::SanitizeCameraOffsetProfile(
	const CameraOffsetProfile& profile, const std::string& profileName) const
{
	CameraOffsetProfile sanitized = profile;
	bool adjusted = false;
	auto sanitizeAxis = [&](float value) {
		if (!std::isfinite(value))
		{
			adjusted = true;
			return 0.0f;
		}
		if (value > CameraOffsetAbsoluteLimit)
		{
			adjusted = true;
			return CameraOffsetAbsoluteLimit;
		}
		if (value < -CameraOffsetAbsoluteLimit)
		{
			adjusted = true;
			return -CameraOffsetAbsoluteLimit;
		}
		return value;
	};

	sanitized.forward = sanitizeAxis(profile.forward);
	sanitized.right = sanitizeAxis(profile.right);
	sanitized.up = sanitizeAxis(profile.up);
	if (adjusted)
	{
		uevr::API::get()->log_warn(
			"[CameraProfiles] rejected unsafe offset in %s; clamped to f=%f r=%f u=%f",
			profileName.c_str(), sanitized.forward, sanitized.right, sanitized.up);
	}
	return sanitized;
}

void SettingsManager::SaveCurrentCameraOffsetProfile(const std::string& prefix)
{
	if (prefix.empty())
		return;

	std::vector<std::pair<std::string, std::string>> pluginSettings;
	pluginSettings.push_back({prefix + "_ForwardOffset", std::to_string(uevr_CameraOffset.forward)});
	pluginSettings.push_back({prefix + "_RightOffset", std::to_string(uevr_CameraOffset.right)});
	pluginSettings.push_back({prefix + "_UpOffset", std::to_string(uevr_CameraOffset.up)});
	SetMultipleValuesToFile(pluginConfigFilePath, pluginSettings);

	uevr::API::get()->log_info("[CameraProfiles] saved %s: forward=%f right=%f up=%f",
		prefix.c_str(), uevr_CameraOffset.forward, uevr_CameraOffset.right, uevr_CameraOffset.up);
}

void SettingsManager::UpdateDefaultCameraOffsetCache(const std::string& prefix, const CameraOffsetProfile& profile)
{
	if (prefix == "OnFoot")
		onFoot_CameraOffset = profile;
	else if (prefix == "DrivingCar")
		drivingCar_CameraOffset = profile;
	else if (prefix == "DrivingBike")
		drivingBike_CameraOffset = profile;
	else if (prefix == "Flying")
		flying_CameraOffset = profile;
}

void SettingsManager::PersistCurrentCameraOffsetToUevrConfig()
{
	std::vector<std::pair<std::string, std::string>> uevrSettings;
	uevrSettings.push_back({"VR_CameraForwardOffset", std::to_string(uevr_CameraOffset.forward)});
	uevrSettings.push_back({"VR_CameraRightOffset", std::to_string(uevr_CameraOffset.right)});
	uevrSettings.push_back({"VR_CameraUpOffset", std::to_string(uevr_CameraOffset.up)});
	SetMultipleValuesToFile(uevrConfigFilePath, uevrSettings);
	uevrConfigWroteByPlugin = true;
}

std::string SettingsManager::GetDefaultCameraOffsetProfilePrefix(CameraModeSettings modeSettings) const
{
	switch (modeSettings)
	{
	case OnFoot:
		return "OnFoot";
	case DrivingCar:
		return "DrivingCar";
	case DrivingBike:
		return "DrivingBike";
	case Flying:
		return "Flying";
	default:
		return "";
	}
}

void SettingsManager::ApplyCameraOffsetProfile(const CameraOffsetProfile& profile, bool liveApply)
{
	uevr_CameraOffset = SanitizeCameraOffsetProfile(profile, activeCameraOffsetProfilePrefix);

	if (liveApply && uevr::API::get()->param()->vr != nullptr && uevr::API::VR::is_runtime_ready())
	{
		uevr::API::VR::set_mod_value("VR_CameraForwardOffset", uevr_CameraOffset.forward);
		uevr::API::VR::set_mod_value("VR_CameraRightOffset", uevr_CameraOffset.right);
		uevr::API::VR::set_mod_value("VR_CameraUpOffset", uevr_CameraOffset.up);
	}
}

void SettingsManager::ApplyUevrBoolValue(const std::string& key, bool value)
{
	if (uevr::API::get()->param()->vr != nullptr && uevr::API::VR::is_runtime_ready())
		uevr::API::VR::set_mod_value(key, value);
}

void SettingsManager::ApplyUevrIntValue(const std::string& key, int value)
{
	if (uevr::API::get()->param()->vr != nullptr && uevr::API::VR::is_runtime_ready())
		uevr::API::VR::set_mod_value(key, value);
}

void SettingsManager::ApplyUevrFloatValue(const std::string& key, float value)
{
	if (uevr::API::get()->param()->vr != nullptr && uevr::API::VR::is_runtime_ready())
		uevr::API::VR::set_mod_value(key, value);
}

void SettingsManager::ApplyCameraSettings(SettingsManager::CameraModeSettings modeSettings,
	const std::string& cameraOffsetProfilePrefix, bool allowCameraOffsetSave)
{
	cameraModeSettings = modeSettings;
	lastApplyTime = GetTickCount64();
	hasPendingOffsetChange = false;
	cameraOffsetSavingEnabled = false;

	// Cutscenes and menus temporarily change control state. Keep the last gameplay
	// offset untouched so a pause cannot zero or save over a vehicle profile.
	if (modeSettings != Cutscene)
	{
		const std::string requestedProfilePrefix = enableCameraProfiles ? cameraOffsetProfilePrefix : "";
		const std::string finalPrefix = requestedProfilePrefix.empty()
			? GetDefaultCameraOffsetProfilePrefix(modeSettings)
			: requestedProfilePrefix;
		activeCameraOffsetProfilePrefix = finalPrefix;
		cameraOffsetSavingEnabled = enableCameraProfiles && allowCameraOffsetSave && !finalPrefix.empty();

		CameraOffsetProfile defaultProfile{0.0f, 0.0f, 0.0f};
		switch (modeSettings)
		{
		case OnFoot: defaultProfile = onFoot_CameraOffset; break;
		case DrivingCar: defaultProfile = drivingCar_CameraOffset; break;
		case DrivingBike: defaultProfile = drivingBike_CameraOffset; break;
		case Flying: defaultProfile = flying_CameraOffset; break;
		default: break;
		}

		const CameraOffsetProfile profile = FetchCameraOffsetProfile(finalPrefix, defaultProfile);
		ApplyCameraOffsetProfile(profile, true);
		uevr::API::get()->log_info(
			"[CameraProfiles] applied %s: f=%f r=%f u=%f save=%s",
			finalPrefix.c_str(), uevr_CameraOffset.forward, uevr_CameraOffset.right,
			uevr_CameraOffset.up, cameraOffsetSavingEnabled ? "on" : "off");
	}
	else
	{
		activeCameraOffsetProfilePrefix.clear();
	}

	std::vector<std::pair<std::string, std::string>> uevrSettingsToApply;
	auto queueBool = [&](const char* key, bool value) {
		uevrSettingsToApply.push_back({key, value ? "true" : "false"});
		ApplyUevrBoolValue(key, value);
	};
	auto queueInt = [&](const char* key, int value) {
		uevrSettingsToApply.push_back({key, std::to_string(value)});
		ApplyUevrIntValue(key, value);
	};
	auto queueFloat = [&](const char* key, float value) {
		uevrSettingsToApply.push_back({key, std::to_string(value)});
		ApplyUevrFloatValue(key, value);
	};
	if (modeSettings != Cutscene)
	{
		uevrSettingsToApply.push_back({"VR_CameraForwardOffset", std::to_string(uevr_CameraOffset.forward)});
		uevrSettingsToApply.push_back({"VR_CameraRightOffset", std::to_string(uevr_CameraOffset.right)});
		uevrSettingsToApply.push_back({"VR_CameraUpOffset", std::to_string(uevr_CameraOffset.up)});
	}

	switch (cameraModeSettings)
	{
	case Cutscene:
		queueBool("VR_DecoupledPitch", true);
		queueBool("VR_LerpCameraPitch", false);
		queueBool("VR_LerpCameraRoll", false);
		queueBool("VR_LerpCameraYaw", false);
		if (leftHandedMode == AllInputsSwap)
			queueBool("VR_SwapControllerInputs", !leftHandedOnlyWhileOnFoot);
		break;
	case OnFoot:
		queueBool("VR_DecoupledPitch", onFoot_DecoupledPitch);
		queueInt("VR_MovementOrientation", uevr_MovementOrientation);
		queueBool("VR_LerpCameraPitch", onFoot_LerpPitch);
		queueBool("VR_LerpCameraRoll", onFoot_LerpRoll);
		queueBool("VR_LerpCameraYaw", onFoot_LerpYaw);
		if (leftHandedMode == AllInputsSwap)
			queueBool("VR_SwapControllerInputs", true);
		break;
	case DrivingCar:
		queueBool("VR_DecoupledPitch", drivingCar_DecoupledPitch);
		queueInt("VR_MovementOrientation", 0);
		queueFloat("VR_AimSpeed", xAxisSensitivity / 10.0f);
		queueBool("VR_LerpCameraPitch", drivingCar_LerpPitch);
		queueBool("VR_LerpCameraRoll", drivingCar_LerpRoll);
		queueBool("VR_LerpCameraYaw", drivingCar_LerpYaw);
		if (leftHandedMode == AllInputsSwap)
			queueBool("VR_SwapControllerInputs", !leftHandedOnlyWhileOnFoot);
		break;
	case DrivingBike:
		queueBool("VR_DecoupledPitch", drivingBike_DecoupledPitch);
		queueInt("VR_MovementOrientation", 0);
		queueBool("VR_LerpCameraPitch", drivingBike_LerpPitch);
		queueBool("VR_LerpCameraRoll", drivingBike_LerpRoll);
		queueBool("VR_LerpCameraYaw", drivingBike_LerpYaw);
		if (leftHandedMode == AllInputsSwap)
			queueBool("VR_SwapControllerInputs", !leftHandedOnlyWhileOnFoot);
		break;
	case Flying:
		queueBool("VR_DecoupledPitch", flying_DecoupledPitch);
		queueInt("VR_MovementOrientation", 0);
		queueBool("VR_LerpCameraPitch", flying_LerpPitch);
		queueBool("VR_LerpCameraRoll", flying_LerpRoll);
		queueBool("VR_LerpCameraYaw", flying_LerpYaw);
		if (leftHandedMode == AllInputsSwap)
			queueBool("VR_SwapControllerInputs", !leftHandedOnlyWhileOnFoot);
		break;
	}

	// Cutscene/NoControls values are temporary. Writing them to config makes UEVR
	// reload while the renderer is already transitioning, then OnFoot reloads again.
	const bool configChanged = modeSettings != Cutscene && !uevrSettingsToApply.empty()
		? SetMultipleValuesToFile(uevrConfigFilePath, uevrSettingsToApply)
		: false;
	uevrConfigWroteByPlugin = uevrConfigWroteByPlugin || configChanged;

	// UEVR exposes offset values through set_mod_value, but its rendered camera
	// transform remains cached on the stable runtime used by this profile. Reload
	// only after a complete gameplay profile is ready; never reload for the brief
	// category fallback or for cutscene/pause states.
	const bool shouldReloadAppliedCamera = configChanged && modeSettings != Cutscene &&
		(modeSettings == OnFoot || !cameraOffsetProfilePrefix.empty() || !enableCameraProfiles);
	if (shouldReloadAppliedCamera && uevr::API::get()->param()->vr != nullptr &&
		uevr::API::VR::is_runtime_ready())
	{
		uevr::API::VR::reload_config();
		// reload_config restores persistent VR_EnableGUI too. Restore the current
		// runtime-only HUD choice without writing the profile again.
		if (hudUiVisibilityInitialized)
			uevr::API::VR::set_mod_value("VR_EnableGUI", desiredHudUiVisible);
		lastApplyTime = GetTickCount64();
		uevr::API::get()->log_info("[CameraProfiles] activated rendered camera for %s",
			activeCameraOffsetProfilePrefix.c_str());
	}
}

static float SafeGetModValueFloat(std::string_view key, float defaultValue)
{
	try
	{
		std::string valStr = uevr::API::VR::get_mod_value<std::string>(key);
		if (valStr.empty())
		{
			return defaultValue;
		}
		return std::stof(valStr);
	}
	catch (...)
	{
		return defaultValue;
	}
}

void SettingsManager::DetectAndSaveLiveCameraOffsets()
{
	if (!enableCameraProfiles || !cameraOffsetSavingEnabled || cameraModeSettings == Cutscene ||
		activeCameraOffsetProfilePrefix.empty())
	{
		hasPendingOffsetChange = false;
		return;
	}

	if (uevr::API::get()->param()->vr == nullptr || !uevr::API::VR::is_runtime_ready())
		return;

	const ULONGLONG currentTime = GetTickCount64();
	if (currentTime - lastCameraOffsetPollTime < CameraOffsetPollIntervalMs)
		return;
	lastCameraOffsetPollTime = currentTime;

	// Ignore UEVR's short-lived transition values after changing camera context.
	if (currentTime - lastApplyTime < CameraOffsetApplyGuardMs)
		return;

	CameraOffsetProfile liveOffset{
		SafeGetModValueFloat("VR_CameraForwardOffset", uevr_CameraOffset.forward),
		SafeGetModValueFloat("VR_CameraRightOffset", uevr_CameraOffset.right),
		SafeGetModValueFloat("VR_CameraUpOffset", uevr_CameraOffset.up)
	};

	const bool unsafeLiveOffset = !std::isfinite(liveOffset.forward) || !std::isfinite(liveOffset.right) ||
		!std::isfinite(liveOffset.up) || std::fabs(liveOffset.forward) > CameraOffsetAbsoluteLimit ||
		std::fabs(liveOffset.right) > CameraOffsetAbsoluteLimit || std::fabs(liveOffset.up) > CameraOffsetAbsoluteLimit;
	if (unsafeLiveOffset)
	{
		uevr::API::get()->log_warn(
			"[CameraProfiles] unsafe live offset rejected for %s: f=%f r=%f u=%f",
			activeCameraOffsetProfilePrefix.c_str(), liveOffset.forward, liveOffset.right, liveOffset.up);
		uevr::API::VR::set_mod_value("VR_CameraForwardOffset", uevr_CameraOffset.forward);
		uevr::API::VR::set_mod_value("VR_CameraRightOffset", uevr_CameraOffset.right);
		uevr::API::VR::set_mod_value("VR_CameraUpOffset", uevr_CameraOffset.up);
		lastApplyTime = currentTime;
		hasPendingOffsetChange = false;
		return;
	}

	auto differs = [](const CameraOffsetProfile& first, const CameraOffsetProfile& second) {
		return std::fabs(first.forward - second.forward) > CameraOffsetEpsilon ||
			std::fabs(first.right - second.right) > CameraOffsetEpsilon ||
			std::fabs(first.up - second.up) > CameraOffsetEpsilon;
	};

	if (!differs(liveOffset, uevr_CameraOffset))
	{
		hasPendingOffsetChange = false;
		return;
	}

	if (!hasPendingOffsetChange || differs(liveOffset, pendingCameraOffset))
	{
		pendingCameraOffset = liveOffset;
		hasPendingOffsetChange = true;
		lastCameraOffsetChangeTime = currentTime;
		return;
	}

	if (currentTime - lastCameraOffsetChangeTime < CameraOffsetSettleTimeMs)
		return;

	uevr_CameraOffset = pendingCameraOffset;
	hasPendingOffsetChange = false;
	UpdateDefaultCameraOffsetCache(activeCameraOffsetProfilePrefix, uevr_CameraOffset);
	SaveCurrentCameraOffsetProfile(activeCameraOffsetProfilePrefix);
	PersistCurrentCameraOffsetToUevrConfig();
	uevr::API::get()->log_info("[CameraProfiles] committed stable live edit to %s",
		activeCameraOffsetProfilePrefix.c_str());
}

void SettingsManager::UpdateSettingsIfModifiedByPlayer()
{
	if (debugMod) uevr::API::get()->log_info("UpdateSettingsIfModified");

	DetectAndSaveLiveCameraOffsets();
	const ULONGLONG currentTime = GetTickCount64();
	if (currentTime - lastConfigPollTime < ConfigPollIntervalMs)
		return;
	lastConfigPollTime = currentTime;

	CheckSettingsModificationAndUpdate(pluginConfigFilePath, false);
	if (uevrConfigWroteByPlugin) //Skip uevr config check if plugin write this frame
	{
		uevrConfigWroteByPlugin = false;
		return;
	}
	CheckSettingsModificationAndUpdate(uevrConfigFilePath, true);
}

bool SettingsManager::CheckSettingsModificationAndUpdate(const std::string& filePath, bool uevr)
{
	if (debugMod) uevr::API::get()->log_info("CheckSettingsModificationAndUpdate");

	HANDLE hFile = CreateFileA(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		if (uevr)
		{
			uevr::API::get()->log_info("File not found: %s", filePath.c_str());
			return false;
		}
			
		// File does not exist, create it with default values
		uevr::API::get()->log_info("File not found: %s. Creating default config.", filePath.c_str());

		std::string defaultContent =
			"[Feature Flags :] -- Live flags can be changed in-game. Restart flags are saved here and apply on next launch.\n"
			"EnableCombatAssist=true\n"
			"EnableAimAlignment=true\n"
			"EnableLegacyCrosshairCompensation=false\n"
			"EnableNativeShotOriginRedirects=false\n"
			"EnableWeaponNoSpread=false\n"
			"EnableCombatAssistAmmo=true\n"
			"EnableManualReloadMode=false\n"
			"EnableCombatAssistDamage=false\n"
			"EnableHealthRecovery=true\n"
			"EnableBulletDamageResistance=true\n"
			"EnableAimCalibrationProbe=false\n"
			"EnableCompactWeaponReticle=true\n"
			"EnableVrScope=true\n"
			"EnableDualGripAimFire=true\n"
			"EnableCustomAkimbo=false\n"
			"EnableTwoHandStabilization=false\n"
			"EnableAlternateWeaponHandsVisibility=true\n"
			"EnableFreeAimWeaponHands=false\n"
			"EnableGripCalibration=false\n"
			"EnableGripWeaponCycle=true\n"
			"EnablePhoneAnswerGripTap=true\n"
			"EnableChordPauseMenu=true\n"
			"EnableChordHudToggle=true\n"
			"EnablePauseUiAutoShow=true\n"
			"EnableHudAutoHide=true\n"
			"EnableFirstPersonCameraLock=true\n"
			"EnableVehicleFaceButtonFire=true\n"
			"EnableAircraftNativeControls=true\n"
			"EnableMotionThrowables=true\n"
			"EnableThrowableMotionProbe=false\n"
			"EnableNativeMolotovMode=false\n"
			"EnableMotionMeleeImpactAudio=true\n"
			"EnableFireTaskPrewarm=true\n"
			"EnableShowUiAtStartup=true\n"
			"EnableCameraProfiles=true\n"
			"EnableBodyVisibility=true\n"
			"EnableBulletTraceHidden=true\n"
			"debugMod=false\n"
			"\n"
			"[Left Handed Mode :] -- Must be configured here. 0 = disabled, 1 = Triggers Swap, 2 = Full inputs Swap\n"
			"LeftHandedMode=0\n"
			"LeftHandedOnlyWhileOnFoot=true\n"
			"\n"
			"[Camera Settings :] -- Can be set directly ingame from uevr menu. The plugin will auto save each camera configuration for each vehicle type here\n"
			"OnFoot_DecoupledPitch=false\n"
			"OnFoot_LerpPitch=true\n"
			"OnFoot_LerpRoll=true\n"
			"OnFoot_LerpYaw=false\n"
			"DrivingCar_DecoupledPitch=false\n"
			"DrivingCar_LerpPitch=true\n"
			"DrivingCar_LerpRoll=true\n"
			"DrivingCar_LerpYaw=false\n"
			"DrivingBike_DecoupledPitch=true\n"
			"DrivingBike_LerpPitch=false\n"
			"DrivingBike_LerpRoll=false\n"
			"DrivingBike_LerpYaw=false\n"
			"Flying_DecoupledPitch=false\n"
			"Flying_LerpPitch=true\n"
			"Flying_LerpRoll=true\n"
			"Flying_LerpYaw=true\n";

		HANDLE hCreateFile = CreateFileA(filePath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hCreateFile != INVALID_HANDLE_VALUE)
		{
			DWORD bytesWritten;
			WriteFile(hCreateFile, defaultContent.c_str(), static_cast<DWORD>(defaultContent.size()), &bytesWritten, NULL);
			CloseHandle(hCreateFile);
			uevr::API::get()->log_info("Default config created at: %s", filePath.c_str());
		}
		else
		{
			uevr::API::get()->log_info("Failed to create default config at: %s", filePath.c_str());
		}
		return false;
	}

	FILETIME currentWriteTime;
	if (GetFileTime(hFile, NULL, NULL, &currentWriteTime))
	{
		if (CompareFileTime(uevr ? &uevrLastWriteTime : &pluginLastWriteTime, &currentWriteTime) != 0)
		{
			CloseHandle(hFile);
			if (uevr)
			{
				uevrLastWriteTime = currentWriteTime;  // Update last write time
				FetchUevrSettings(true);
			}
			else
			{
				pluginLastWriteTime = currentWriteTime; 
				FetchPluginSettings();
			}
			uevr::API::get()->log_error("setting file has been modified");
			return true;  // File has been modified
		}
	}

	CloseHandle(hFile);
	return false;  // No change
}

void SettingsManager::SetBoolValueToFile(const std::string& filePath, const std::string& key, bool value)
{
	SetMultipleValuesToFile(filePath, {{key, value ? "true" : "false"}});
	if (debugMod)
		uevr::API::get()->log_info("Updated %s to %s", key.c_str(), value ? "true" : "false");
}

bool SettingsManager::GetBoolValueFromFile(const std::string& filePath, const std::string& key, bool defaultValue)
{
	if (debugMod) uevr::API::get()->log_info("GetBoolValueFromFile()");

	HANDLE hFile = CreateFileA(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		uevr::API::get()->log_info("Failed to open %s", filePath.c_str());
		return defaultValue;
	}

	DWORD bytesRead;
	char buffer[1024];  // Buffer to read the file content
	std::string fileContents;

	// Read the file into memory
	while (ReadFile(hFile, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
	{
		buffer[bytesRead] = '\0'; // Null terminate the string
		fileContents.append(buffer);
	}
	CloseHandle(hFile);

	std::string valueStr;
	if (FindSettingValue(fileContents, key, valueStr))
	{
		std::transform(valueStr.begin(), valueStr.end(), valueStr.begin(), ::tolower);
		if (valueStr == "true") return true;
		if (valueStr == "false") return false;
		uevr::API::get()->log_info("Error: Invalid bool value for key: %s", key.c_str());
	}

	return defaultValue;  // Return default if the key is not found or invalid
}

float SettingsManager::GetFloatValueFromFile(const std::string& filePath, const std::string& key, float defaultValue)
{
	if (debugMod) uevr::API::get()->log_info("GetFloatValueFromFile()");

	HANDLE hFile = CreateFileA(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		uevr::API::get()->log_info("Failed to open %s", filePath.c_str());
		return defaultValue;
	}

	DWORD bytesRead;
	char buffer[1024];  // Buffer to read the file content
	std::string fileContents;

	// Read the file into memory
	while (ReadFile(hFile, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
	{
		buffer[bytesRead] = '\0'; // Null terminate the string
		fileContents.append(buffer);
	}
	CloseHandle(hFile);

	// Look for the key in the file contents
	size_t pos = fileContents.find(key);
	if (pos != std::string::npos)
	{
		size_t equalPos = fileContents.find('=', pos);
		if (equalPos != std::string::npos)
		{
			std::string valueStr = fileContents.substr(equalPos + 1);
			try
			{
				return std::stof(valueStr); // Convert the string to float
			}
			catch (const std::invalid_argument&)
			{
				uevr::API::get()->log_info("Error: Invalid float value for key: %s", key.c_str());
				return defaultValue;
			}
		}
	}

	return defaultValue;  // Return default if the key is not found
}

void SettingsManager::SetIntValueToFile(const std::string& filePath, const std::string& key, int value)
{
	if (debugMod) uevr::API::get()->log_info("SetIntValueToFile()");

	HANDLE hFile = CreateFileA(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		uevr::API::get()->log_info("Failed to open %s for reading", filePath.c_str());
		return;
	}

	DWORD bytesRead;
	char buffer[1024];
	std::string fileContents;

	while (ReadFile(hFile, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
	{
		buffer[bytesRead] = '\0';
		fileContents.append(buffer);
	}
	CloseHandle(hFile);

	size_t pos = fileContents.find(key);
	if (pos != std::string::npos)
	{
		size_t equalPos = fileContents.find('=', pos);
		if (equalPos != std::string::npos)
		{
			size_t endOfLine = fileContents.find_first_of("\r\n", equalPos);
			std::string before = fileContents.substr(0, equalPos + 1);
			std::string after = (endOfLine != std::string::npos) ? fileContents.substr(endOfLine) : "";

			// Replace value
			std::string newContents = before + std::to_string(value) + after;
			
			// Write it back
			HANDLE hWriteFile = CreateFileA(filePath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			if (hWriteFile != INVALID_HANDLE_VALUE)
			{
				DWORD bytesWritten;
				WriteFile(hWriteFile, newContents.c_str(), static_cast<DWORD>(newContents.size()), &bytesWritten, NULL);
				
				FILETIME lastWriteTime;
				if (GetFileTime(hWriteFile, NULL, NULL, &lastWriteTime))
				{
					if (filePath == uevrConfigFilePath)
						uevrLastWriteTime = lastWriteTime;
					else if (filePath == pluginConfigFilePath)
						pluginLastWriteTime = lastWriteTime;
				}

				CloseHandle(hWriteFile);
				uevr::API::get()->log_info("Updated %s to %s", key.c_str(), std::to_string(value).c_str());
			}
			else
			{
				uevr::API::get()->log_info("Failed to open %s for writing", filePath.c_str());
			}
			return;
		}
	}
}

void SettingsManager::SetFloatValueToFile(const std::string& filePath, const std::string& key, float value)
{
	if (debugMod) uevr::API::get()->log_info("SetFloatValueToFile()");

	HANDLE hFile = CreateFileA(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		uevr::API::get()->log_info("Failed to open %s for reading", filePath.c_str());
		return;
	}

	DWORD bytesRead;
	char buffer[1024];
	std::string fileContents;

	while (ReadFile(hFile, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
	{
		buffer[bytesRead] = '\0';
		fileContents.append(buffer);
	}
	CloseHandle(hFile);

	const std::string valueString = std::to_string(value);
	size_t pos = fileContents.find(key);
	if (pos != std::string::npos)
	{
		size_t equalPos = fileContents.find('=', pos);
		if (equalPos != std::string::npos)
		{
			size_t endOfLine = fileContents.find_first_of("\r\n", equalPos);
			std::string before = fileContents.substr(0, equalPos + 1);
			std::string after = (endOfLine != std::string::npos) ? fileContents.substr(endOfLine) : "";
			fileContents = before + valueString + after;
		}
	}
	else
	{
		if (!fileContents.empty() && fileContents.back() != '\n')
			fileContents += "\n";
		fileContents += key + "=" + valueString + "\n";
	}

	HANDLE hWriteFile = CreateFileA(filePath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hWriteFile != INVALID_HANDLE_VALUE)
	{
		DWORD bytesWritten;
		WriteFile(hWriteFile, fileContents.c_str(), static_cast<DWORD>(fileContents.size()), &bytesWritten, NULL);
		
		FILETIME lastWriteTime;
		if (GetFileTime(hWriteFile, NULL, NULL, &lastWriteTime))
		{
			if (filePath == uevrConfigFilePath)
				uevrLastWriteTime = lastWriteTime;
			else if (filePath == pluginConfigFilePath)
				pluginLastWriteTime = lastWriteTime;
		}

		CloseHandle(hWriteFile);
		uevr::API::get()->log_info("Updated %s to %s", key.c_str(), valueString.c_str());
	}
	else
	{
		uevr::API::get()->log_info("Failed to open %s for writing", filePath.c_str());
	}
}

bool SettingsManager::SetMultipleValuesToFile(const std::string& filePath, const std::vector<std::pair<std::string, std::string>>& keyValuePairs)
{
	if (debugMod) uevr::API::get()->log_info("SetMultipleValuesToFile() for %s", filePath.c_str());

	HANDLE hFile = CreateFileA(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	std::string fileContents;
	if (hFile != INVALID_HANDLE_VALUE)
	{
		DWORD bytesRead;
		char buffer[1024];
		while (ReadFile(hFile, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
		{
			buffer[bytesRead] = '\0';
			fileContents.append(buffer);
		}
		CloseHandle(hFile);
	}
	else
	{
		uevr::API::get()->log_info("Failed to open %s for reading, starting with empty", filePath.c_str());
	}

	const std::string originalContents = fileContents;
	for (const auto& pair : keyValuePairs)
	{
		const std::string& key = pair.first;
		const std::string& valueString = pair.second;

		if (!ReplaceSettingValue(fileContents, key, valueString))
		{
			if (!fileContents.empty() && fileContents.back() != '\n')
				fileContents += "\n";
			fileContents += key + "=" + valueString + "\n";
		}
	}
	if (fileContents == originalContents)
		return false;

	HANDLE hWriteFile = CreateFileA(filePath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hWriteFile != INVALID_HANDLE_VALUE)
	{
		DWORD bytesWritten;
		WriteFile(hWriteFile, fileContents.c_str(), static_cast<DWORD>(fileContents.size()), &bytesWritten, NULL);
		
		FILETIME lastWriteTime;
		if (GetFileTime(hWriteFile, NULL, NULL, &lastWriteTime))
		{
			if (filePath == uevrConfigFilePath)
				uevrLastWriteTime = lastWriteTime;
			else if (filePath == pluginConfigFilePath)
				pluginLastWriteTime = lastWriteTime;
		}
		
		CloseHandle(hWriteFile);
		return true;
	}
	else
	{
		uevr::API::get()->log_info("Failed to open %s for writing", filePath.c_str());
		return false;
	}
}

int SettingsManager::GetIntValueFromFile(const std::string& filePath, const std::string& key, int defaultValue)
{
	if (debugMod) uevr::API::get()->log_info("GetFloatValueFromFile()");

	HANDLE hFile = CreateFileA(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		uevr::API::get()->log_info("Failed to open %s", filePath.c_str());
		return defaultValue;
	}

	DWORD bytesRead;
	char buffer[1024];  // Buffer to read the file content
	std::string fileContents;

	// Read the file into memory
	while (ReadFile(hFile, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
	{
		buffer[bytesRead] = '\0'; // Null terminate the string
		fileContents.append(buffer);
	}
	CloseHandle(hFile);

	// Look for the key in the file contents
	size_t pos = fileContents.find(key);
	if (pos != std::string::npos)
	{
		size_t equalPos = fileContents.find('=', pos);
		if (equalPos != std::string::npos)
		{
			std::string valueStr = fileContents.substr(equalPos + 1);
			try
			{
				return std::stoi(valueStr); // Convert the string to float
			}
			catch (const std::invalid_argument&)
			{
				uevr::API::get()->log_info("Error: Invalid int value for key: %s", key.c_str());
				return defaultValue;
			}
		}
	}

	return defaultValue;  // Return default if the key is not found
}

std::string GetDLLDirectory()
{
	char path[MAX_PATH];
	HMODULE hModule = GetModuleHandleA("UEVR_GTASADE.dll"); // Get handle to the loaded DLL

	if (hModule)
	{
		GetModuleFileNameA(hModule, path, MAX_PATH); // Get full DLL path
		std::string fullPath = path;

		// Remove the DLL filename to get the directory
		size_t pos = fullPath.find_last_of("\\/");
		if (pos != std::string::npos)
		{
			return fullPath.substr(0, pos + 1); // Keep the trailing slash
		}
	}
	else
		uevr::API::get()->log_info("Failed to get module handle for UEVR_GTASADE.dll");

	return "Unknown";
}

void SettingsManager::GetAllConfigFilePaths()
{
	if (debugMod) uevr::API::get()->log_info("GetAllConfigFilePaths");
	uevrConfigFilePath = GetConfigFilePath(true);
	pluginConfigFilePath = GetConfigFilePath(false);
	pluginStatusFilePath = GetConfigFilePath(false);
	const size_t fileNamePos = pluginStatusFilePath.find_last_of("\\/");
	if (fileNamePos != std::string::npos)
		pluginStatusFilePath = pluginStatusFilePath.substr(0, fileNamePos + 1) + pluginStatusFileName;
}

std::string SettingsManager::GetGripCalibrationFilePath() const
{
	if (pluginConfigFilePath.empty())
		return {};
	const size_t fileNamePos = pluginConfigFilePath.find_last_of("\\/");
	if (fileNamePos == std::string::npos)
		return "UEVR_GTASADE_grip_calibration.txt";
	return pluginConfigFilePath.substr(0, fileNamePos + 1) + "UEVR_GTASADE_grip_calibration.txt";
}

std::string SettingsManager::GetHolsterAnchorsFilePath() const
{
	if (pluginConfigFilePath.empty())
		return {};
	const size_t fileNamePos = pluginConfigFilePath.find_last_of("\\/");
	if (fileNamePos == std::string::npos)
		return "UEVR_GTASADE_holster_anchors.txt";
	return pluginConfigFilePath.substr(0, fileNamePos + 1) + "UEVR_GTASADE_holster_anchors.txt";
}

std::string SettingsManager::GetConfigFilePath(bool uevr)
{
	if (debugMod) uevr::API::get()->log_info("GetConfigFilePath()");

	std::string fullPath = GetDLLDirectory();

	// Remove "SanAndreas\plugins\UEVR_GTASADE.dll" part, leaving "SanAndreas"
	size_t pos = fullPath.find_last_of("\\/");
	if (pos != std::string::npos)
	{
		fullPath = fullPath.substr(0, pos); // Remove "\plugins"
		pos = fullPath.find_last_of("\\/");
		if (pos != std::string::npos)
		{
			fullPath = fullPath.substr(0, pos + 1); // Keep "SanAndreas\"
		}
	}

	return fullPath + (uevr ? uevrSettingsFileName : pluginSettingsFileName); // Append "config.txt" or "UEVR_GTASADE_config.txt"
}
