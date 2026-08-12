#pragma once
#include <iostream>
#include <windows.h>
#include <string>
#include <vector>
#include <utility>
#include "uevr/API.hpp"

class SettingsManager {
public:
	bool debugMod = false;
	bool enableCombatAssist = true;
	bool enableCombatAssistAmmo = true;
	bool enableManualReloadMode = false;
	bool enableCombatAssistDamage = false;
	bool enableCombatAssistAnimationTiming = false;
	bool enableCombatAssistWeaponSkill = false;
	bool enableWeaponNoSpread = false;
	bool enableMotionWeaponRecoil = false;
	bool enableCompactWeaponReticle = true;
	bool enableVrScope = true;
	bool enableSilencedPistolAimStability = true;
	bool enableHealthRecovery = true;
	bool enableBulletDamageResistance = true;
	bool debugSpreadProbe = false;
	bool enableAimCalibrationProbe = false;
	bool enableUtilityWeaponAimBypass = true;
	bool enableUtilityWeaponCycleReset = true;
	bool enableDirectWeaponCycle = true;
	bool enableABWeaponCycleTest = false;
	bool enableAimAlignment = true;
	bool enableLegacyCrosshairCompensation = false;
	bool enableNativeShotOriginRedirects = false;
	bool enableCameraProfiles = true;
	bool enableBodyVisibility = true;
	bool enableBulletTraceHidden = true;
	bool enableDualGripAimFire = true;
	bool enableTwoHandStabilization = false;
	bool enableAlternateWeaponHandsVisibility = true;
	bool enableFreeAimWeaponHands = false;
	bool enableGripCalibration = false;
	bool enableGripWeaponCycle = false;
	bool enablePhoneAnswerGripTap = true;
	bool enableLegacyDPadModulator = false;
	bool enableChordPauseMenu = true;
	bool enableChordHudToggle = true;
	bool enablePauseUiAutoShow = true;
	bool enableHudAutoHide = true;
	bool enableShortPressCameraSwitch = true;
	bool enableFirstPersonCameraLock = true;
	// Bounded ordinary-driving live-lap weapon presentation/aim prototype.
	// The legacy name is retained for config compatibility.
	bool enableVehicleFaceButtonFire = true;
	bool enableAircraftNativeControls = true;
	bool enableR3LeftStickDpad = false;
	bool debugInputLayerProbe = false;
	bool enableShowUiAtStartup = true;
	bool enableHudHiddenByDefault = true;
	bool activeCombatAssist = true;
	bool activeCombatAssistAmmo = true;
	bool activeManualReloadMode = false;
	bool activeCompactWeaponReticle = true;
	bool activeBulletTraceHidden = true;
	bool activeShowUiAtStartup = true;

	enum LeftHandedMode {
		Disabled = 0,
		TriggerSwap = 1,
		AllInputsSwap = 2
	};
	LeftHandedMode leftHandedMode = Disabled;
	bool leftHandedOnlyWhileOnFoot = true;

	float xAxisSensitivity = 125.0f;
	float joystickDeadzone = 0.1f;

	enum CameraModeSettings {
		Cutscene = 0,
		OnFoot = 1,
		DrivingCar = 2,
		DrivingBike = 3,
		Flying = 4
	};

	void InitSettingsManager();
	void GetAllConfigFilePaths();
	void UpdateSettingsIfModifiedByPlayer();
	void ApplyCameraSettings(CameraModeSettings cameraModeSettings, const std::string& cameraOffsetProfilePrefix = "", bool allowCameraOffsetSave = true);
	bool SetFeatureFlagFromUi(const std::string& name, bool value, bool& liveApply);
	void DispatchFeatureFlagsToLua() const;
	void WriteFeatureFlagStatus(const std::string& reason);
	void ApplyHudVisibilityDefault();
	bool SetHudUiVisible(bool visible);
	bool GetPause2dScreenMode(bool& enabled) const;
	bool SetPause2dScreenMode(bool enabled);
	bool RecoverPluginOwnedPause2dScreenMode();
	std::string GetGripCalibrationFilePath() const;
	std::string GetHolsterAnchorsFilePath() const;

private:
	struct CameraOffsetProfile {
		float forward = 0.0f;
		float right = 0.0f;
		float up = 0.0f;
	};

	bool CheckSettingsModificationAndUpdate(const std::string& filePath, bool uevr);
	std::string GetConfigFilePath(bool uevr);
	std::string GetPause2dOwnershipMarkerPath() const;
	bool WritePause2dOwnershipMarker() const;
	void ClearPause2dOwnershipMarker() const;
	std::string uevrSettingsFileName = "config.txt";
	std::string pluginSettingsFileName = "UEVR_GTASADE_config.txt";
	std::string pluginStatusFileName = "UEVR_GTASADE_status.txt";

	std::string uevrConfigFilePath;
	FILETIME uevrLastWriteTime{};
	bool uevrConfigWroteByPlugin = false;
	std::string pluginConfigFilePath;
	std::string pluginStatusFilePath;
	FILETIME pluginLastWriteTime{};
	bool featureFlagStatusInitialized = false;

	//Would need some rework if lots of config values to read. Now it opens the config.txt file each time we call these :
	float GetFloatValueFromFile(const std::string& filePath, const std::string& key, float defaultValue);
	bool GetBoolValueFromFile(const std::string& filePath, const std::string& key, bool defaultValue);
	void SetBoolValueToFile(const std::string& filePath, const std::string& key, bool value);
	int GetIntValueFromFile(const std::string& filePath, const std::string& key, int defaultValue);
	void SetIntValueToFile(const std::string& filePath, const std::string& key, int value);
	void SetFloatValueToFile(const std::string& filePath, const std::string& key, float value);
	bool SetMultipleValuesToFile(const std::string& filePath, const std::vector<std::pair<std::string, std::string>>& keyValuePairs);
	void DetectAndSaveLiveCameraOffsets();

	void FetchUevrSettings(bool writeToPlugin);
	void FetchPluginSettings();
	void EnsureVisibleFeatureFlagConfigValues();
	void WriteChangedSettingsToPluginConfigFile();
	void SaveCurrentCameraOffsetProfile(const std::string& prefix);
	void PersistCurrentCameraOffsetToUevrConfig();
	void UpdateDefaultCameraOffsetCache(const std::string& prefix, const CameraOffsetProfile& profile);
	void ApplyCameraOffsetProfile(const CameraOffsetProfile& profile, bool liveApply);
	CameraOffsetProfile FetchCameraOffsetProfile(const std::string& prefix, CameraOffsetProfile defaultValue);
	CameraOffsetProfile SanitizeCameraOffsetProfile(const CameraOffsetProfile& profile, const std::string& profileName) const;
	std::string GetDefaultCameraOffsetProfilePrefix(CameraModeSettings modeSettings) const;
	void ApplyUevrBoolValue(const std::string& key, bool value);
	void ApplyUevrIntValue(const std::string& key, int value);
	void ApplyUevrFloatValue(const std::string& key, float value);

	bool uevr_DecoupledPitch = false;
	bool uevr_LerpPitch = false;
	bool uevr_LerpRoll = false;
	bool uevr_LerpYaw = false;
	int uevr_MovementOrientation = 0;
	CameraOffsetProfile uevr_CameraOffset{};

	bool onFoot_DecoupledPitch = false;
	bool onFoot_LerpPitch = false;
	bool onFoot_LerpRoll = false;
	bool onFoot_LerpYaw = false;
	CameraOffsetProfile onFoot_CameraOffset{};

	bool drivingCar_DecoupledPitch = false;
	bool drivingCar_LerpPitch = false;
	bool drivingCar_LerpRoll = false;
	bool drivingCar_LerpYaw = false;
	CameraOffsetProfile drivingCar_CameraOffset{};
		 
	bool drivingBike_DecoupledPitch = false;
	bool drivingBike_LerpPitch = false;
	bool drivingBike_LerpRoll = false;
	bool drivingBike_LerpYaw = false;
	CameraOffsetProfile drivingBike_CameraOffset{};

	bool flying_DecoupledPitch = false;
	bool flying_LerpPitch = false;
	bool flying_LerpRoll = false;
	bool flying_LerpYaw = false;
	CameraOffsetProfile flying_CameraOffset{};

	CameraModeSettings cameraModeSettings = CameraModeSettings::Cutscene;
	std::string activeCameraOffsetProfilePrefix;
	bool cameraOffsetSavingEnabled = false;
	bool hudUiVisibilityInitialized = false;
	bool desiredHudUiVisible = false;

	ULONGLONG lastCameraOffsetChangeTime = 0;
	ULONGLONG lastCameraOffsetPollTime = 0;
	ULONGLONG lastConfigPollTime = 0;
	ULONGLONG lastApplyTime = 0;
	CameraOffsetProfile pendingCameraOffset{};
	bool hasPendingOffsetChange = false;
};
