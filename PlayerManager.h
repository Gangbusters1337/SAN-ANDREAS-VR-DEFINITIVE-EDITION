#pragma once

#define GLM_FORCE_QUAT_DATA_XYZW
#include "glm/glm.hpp"
#include <glm/gtc/type_ptr.hpp>

#include "uevr/API.hpp"
#include "Utilities.h"
#include "SettingsManager.h"
#include <vector>

class PlayerManager {
private:
	SettingsManager* const settingsManager;

public:
	PlayerManager(SettingsManager* sm) : settingsManager(sm) {};
	glm::fvec3 actualPlayerPositionUE = { 0.0f, 0.0f, 0.0f };
	glm::fvec3 actualPlayerHeadPositionUE = { 0.0f, 0.0f, 0.0f };
	const glm::fvec3 defaultPlayerHeadLocalPositionUE = { 0.0f, 0.0f, 69.0f };
	const glm::fvec3 defaultBikeLocalOffsetUE = { 0.0f, -35.0f, 0.0f };
	uevr::API::UObject* playerController = nullptr;
	uevr::API::UObject* playerActor = nullptr;
	uevr::API::UObject* playerCharacter = nullptr;
	uevr::API::UObject* playerHead = nullptr;
	bool isInControl = false;
	bool wasInControl = false;
	bool isInVehicle = false;
	bool wasInVehicle = false;
	enum VehicleType {
		OnFoot = 4,
		CarOrBoat = 10,
		Bike = 13,
		Helicopter = 16,
		Plane = 19,
	};
	VehicleType vehicleType = OnFoot;
	VehicleType previousVehicleType = OnFoot;
	std::string VehicleTypeToString(VehicleType type);
	bool shootFromCarInput = false;
	bool weaponWheelEnabled = false;
	uevr::API::UObject* lastBodyVisibilityCharacter = nullptr;
	float bodyVisibilityRefreshTimer = 0.0f;
	uevr::API::UObject* handScaleCharacter = nullptr;
	uevr::API::UObject* handScaleHands = nullptr;
	uevr::API::UObject* handScaleWatch = nullptr;
	// Retained only for source compatibility with the old experimental shoe
	// path; the visible-body lifecycle no longer mutates these components.
	uevr::API::UObject* lowerBodyVisibilityProperty = nullptr;
	uevr::API::UObject* shoeVisibilityProperty = nullptr;
	std::vector<uevr::API::UObject*> shoeVisibilityComponents;
	bool trackedHandsShoeHidden = false;
	bool handsScaledVisible = true;
	bool watchScaledVisible = true;

	void FetchPlayerUObjects();
	void ProcessBodyVisibility(float delta);
	void SetHandsScaled(bool visible, bool force = false);
	void SetWatchScaled(bool visible, bool force = false);
	void SetTrackedHandsShoeHidden(bool hidden, bool force = false);
	void RepositionUnhookedUobjects();
	void AlignControllerToAimDirection(const glm::fvec3& aimDirectionUE);

private:
	void RestoreTrackedShoeComponents();
	void RefreshTrackedShoeComponents();
};
