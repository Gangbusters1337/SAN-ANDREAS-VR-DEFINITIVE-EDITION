#include "PlayerManager.h"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>

namespace {
	struct ParameterSetVisibility {
		bool bNewVisibility = true;
		bool bPropagateToChildren = true;
	};

	struct ParameterSetHiddenInGame {
		bool bNewHidden = false;
		bool bPropagateToChildren = true;
	};

	struct ParameterSetScale3D {
		glm::fvec3 newScale3D = { 1.0f, 1.0f, 1.0f };
	};

	std::wstring ToLower(std::wstring value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
			return static_cast<wchar_t>(std::towlower(ch));
		});
		return value;
	}

	bool NameLooksLikeBodyPart(const std::wstring& name)
	{
		const std::wstring lower = ToLower(name);
		const wchar_t* terms[] = {
			L"body", L"torso", L"hand", L"arm", L"leg", L"trouser",
			L"pants", L"foot", L"feet", L"shoe", L"watch", L"tattoo"
		};

		for (const auto* term : terms) {
			if (lower.find(term) != std::wstring::npos)
				return true;
		}
		return false;
	}

	uevr::API::UObject* GetObjectPropertyIfPresent(uevr::API::UObject* owner, const wchar_t* propertyName)
	{
		if (owner == nullptr || !uevr::API::UObjectHook::exists(owner))
			return nullptr;

		auto ownerClass = owner->get_class();
		if (ownerClass == nullptr || ownerClass->find_property(propertyName) == nullptr)
			return nullptr;

		auto propertyData = owner->get_property_data<uevr::API::UObject*>(propertyName);
		if (propertyData == nullptr)
			return nullptr;

		return *propertyData;
	}

	void SetComponentScale(uevr::API::UObject* object, bool visible)
	{
		if (object == nullptr || !uevr::API::UObjectHook::exists(object))
			return;

		auto objectClass = object->get_class();
		if (objectClass == nullptr)
			return;

		ParameterSetScale3D params{};
		params.newScale3D = visible ? glm::fvec3(1.0f, 1.0f, 1.0f) : glm::fvec3(0.001f, 0.001f, 0.001f);

		if (objectClass->find_function(L"SetWorldScale3D") != nullptr)
			object->call_function(L"SetWorldScale3D", &params);
		else if (objectClass->find_function(L"K2_SetWorldScale3D") != nullptr)
			object->call_function(L"K2_SetWorldScale3D", &params);
	}

	void SetComponentPresentationVisible(uevr::API::UObject* object, bool visible)
	{
		if (object == nullptr || !uevr::API::UObjectHook::exists(object))
			return;

		auto objectClass = object->get_class();
		if (objectClass == nullptr)
			return;
		if (objectClass->find_property(L"bVisible") != nullptr)
			object->set_bool_property(L"bVisible", visible);
		if (objectClass->find_property(L"bHiddenInGame") != nullptr)
			object->set_bool_property(L"bHiddenInGame", !visible);

		if (objectClass->find_function(L"SetVisibility") != nullptr) {
			ParameterSetVisibility params{};
			params.bNewVisibility = visible;
			params.bPropagateToChildren = false;
			object->call_function(L"SetVisibility", &params);
		}
		if (objectClass->find_function(L"SetHiddenInGame") != nullptr) {
			ParameterSetHiddenInGame params{};
			params.bNewHidden = !visible;
			params.bPropagateToChildren = false;
			object->call_function(L"SetHiddenInGame", &params);
		}
	}

	void ForceComponentVisible(uevr::API::UObject* object)
	{
		if (object == nullptr || !uevr::API::UObjectHook::exists(object))
			return;

		auto objectClass = object->get_class();
		if (objectClass == nullptr)
			return;

		if (objectClass->find_property(L"bHiddenInGame") != nullptr)
			object->set_bool_property(L"bHiddenInGame", false);
		if (objectClass->find_property(L"bVisible") != nullptr)
			object->set_bool_property(L"bVisible", true);
		if (objectClass->find_property(L"bOwnerNoSee") != nullptr)
			object->set_bool_property(L"bOwnerNoSee", false);
		if (objectClass->find_property(L"bOnlyOwnerSee") != nullptr)
			object->set_bool_property(L"bOnlyOwnerSee", false);

		if (objectClass->find_function(L"SetHiddenInGame") != nullptr) {
			ParameterSetHiddenInGame params{};
			object->call_function(L"SetHiddenInGame", &params);
		}
		if (objectClass->find_function(L"SetVisibility") != nullptr) {
			ParameterSetVisibility params{};
			object->call_function(L"SetVisibility", &params);
		}
		if (objectClass->find_function(L"SetOwnerNoSee") != nullptr) {
			Utilities::ParameterSingleBool params{};
			params.boolValue = false;
			object->call_function(L"SetOwnerNoSee", &params);
		}
	}

	bool NameLooksLikeNeckAccessory(const std::wstring& name)
	{
		const std::wstring lower = ToLower(name);
		return lower.find(L"necklace") != std::wstring::npos || lower.find(L"chain") != std::wstring::npos;
	}

	void ForceComponentHidden(uevr::API::UObject* object)
	{
		if (object == nullptr || !uevr::API::UObjectHook::exists(object))
			return;

		auto objectClass = object->get_class();
		if (objectClass == nullptr)
			return;

		if (objectClass->find_property(L"bHiddenInGame") != nullptr)
			object->set_bool_property(L"bHiddenInGame", true);
		if (objectClass->find_property(L"bVisible") != nullptr)
			object->set_bool_property(L"bVisible", false);
		if (objectClass->find_property(L"bOwnerNoSee") != nullptr)
			object->set_bool_property(L"bOwnerNoSee", true);
		if (objectClass->find_property(L"bOnlyOwnerSee") != nullptr)
			object->set_bool_property(L"bOnlyOwnerSee", false);

		if (objectClass->find_function(L"SetHiddenInGame") != nullptr) {
			ParameterSetHiddenInGame params{};
			params.bNewHidden = true;
			object->call_function(L"SetHiddenInGame", &params);
		}
		if (objectClass->find_function(L"SetVisibility") != nullptr) {
			ParameterSetVisibility params{};
			params.bNewVisibility = false;
			object->call_function(L"SetVisibility", &params);
		}
		if (objectClass->find_function(L"SetOwnerNoSee") != nullptr) {
			Utilities::ParameterSingleBool params{};
			params.boolValue = true;
			object->call_function(L"SetOwnerNoSee", &params);
		}
	}
}

void PlayerManager::FetchPlayerUObjects()
{
	if (settingsManager->debugMod) uevr::API::get()->log_info("FetchPlayerUObjects()");
	playerController = uevr::API::get()->get_player_controller(0);
	if (playerController == nullptr || !uevr::API::UObjectHook::exists(playerController))
	{
		playerActor = nullptr;
		playerCharacter = nullptr;
		playerHead = nullptr;
		return;
	}

	auto gta_playerActor_c = uevr::API::get()->find_uobject<uevr::API::UClass>(L"Class /Script/GTABase.GTAPlayerActor");
	auto gta_BPplayerCharacter_c = uevr::API::get()->find_uobject<uevr::API::UClass>(L"BlueprintGeneratedClass /Game/SanAndreas/Characters/Player/BP_player_character.BP_Player_Character_C");
	if (gta_playerActor_c == nullptr && gta_BPplayerCharacter_c == nullptr)
	{
		playerActor = nullptr;
		playerCharacter = nullptr;
		playerHead = nullptr;
		return;
	}

	const auto& children = playerController->get_property<uevr::API::TArray<uevr::API::UObject*>>(L"Children");
	playerActor = nullptr;
	playerCharacter = nullptr;
	playerHead = nullptr;

	for (auto child : children) {
		if (child == nullptr || !uevr::API::UObjectHook::exists(child))
			continue;

		if (gta_playerActor_c != nullptr && playerActor == nullptr && child->is_a(gta_playerActor_c)) {
			playerActor = child;
			playerHead = playerActor->get_property<uevr::API::UObject*>(L"head");
			//API::get()->log_info("playerHead : %ls", playerHead->get_full_name().c_str());
		}

		if (gta_BPplayerCharacter_c != nullptr && playerCharacter == nullptr && child->is_a(gta_BPplayerCharacter_c)) {
			playerCharacter = child;
		}
	}
}

void PlayerManager::DiscardPlayerObjectCaches()
{
	// A save/checkpoint load destroys the old world before the plugin sees the
	// replacement character. Never call methods on those stale components here.
	lastBodyVisibilityCharacter = nullptr;
	bodyVisibilityRefreshTimer = 0.0f;
	handScaleCharacter = nullptr;
	handScaleHands = nullptr;
	handScaleWatch = nullptr;
	lowerBodyVisibilityProperty = nullptr;
	shoeVisibilityProperty = nullptr;
	shoeVisibilityComponents.clear();
	trackedHandsShoeHidden = false;
	handsScaledVisible = true;
	watchScaledVisible = true;
}

void PlayerManager::RestoreTrackedShoeComponents()
{
	SetComponentPresentationVisible(lowerBodyVisibilityProperty, true);
	SetComponentPresentationVisible(shoeVisibilityProperty, true);
	for (auto component : shoeVisibilityComponents)
		SetComponentPresentationVisible(component, true);
}

void PlayerManager::RefreshTrackedShoeComponents()
{
	if (playerCharacter == nullptr || !uevr::API::UObjectHook::exists(playerCharacter))
		return;

	auto trousers = GetObjectPropertyIfPresent(playerCharacter, L"Trousers");
	if (trousers != lowerBodyVisibilityProperty)
	{
		SetComponentPresentationVisible(lowerBodyVisibilityProperty, true);
		lowerBodyVisibilityProperty = trousers;
		if (trackedHandsShoeHidden)
			SetComponentPresentationVisible(lowerBodyVisibilityProperty, false);
	}

	auto shoes = GetObjectPropertyIfPresent(playerCharacter, L"Shoes");
	if (shoes != shoeVisibilityProperty)
	{
		SetComponentPresentationVisible(shoeVisibilityProperty, true);
		shoeVisibilityComponents.erase(
			std::remove(shoeVisibilityComponents.begin(), shoeVisibilityComponents.end(), shoeVisibilityProperty),
			shoeVisibilityComponents.end());
		shoeVisibilityProperty = shoes;
		if (trackedHandsShoeHidden)
			SetComponentPresentationVisible(shoeVisibilityProperty, false);
	}

	auto playerClass = playerCharacter->get_class();
	if (playerClass == nullptr || playerClass->find_property(L"Components") == nullptr)
		return;

	auto components = playerCharacter->get_property_data<uevr::API::TArray<uevr::API::UObject*>>(L"Components");
	if (components == nullptr || components->empty())
		return;

	for (auto component : *components) {
		if (component == nullptr || !uevr::API::UObjectHook::exists(component))
			continue;

		auto componentClass = component->get_class();
		if (componentClass == nullptr)
			continue;

		auto skeletalMesh = componentClass->find_property(L"SkeletalMesh") != nullptr
			? component->get_property<uevr::API::UObject*>(L"SkeletalMesh")
			: componentClass->find_property(L"SkinnedAsset") != nullptr
			? component->get_property<uevr::API::UObject*>(L"SkinnedAsset") : nullptr;
		if (skeletalMesh == nullptr || !uevr::API::UObjectHook::exists(skeletalMesh))
			continue;

		const auto assetName = ToLower(skeletalMesh->get_full_name());
		if (assetName.find(L"sk_feet") == std::wstring::npos)
			continue;
		if (component == shoeVisibilityProperty)
			continue;

		if (std::find(shoeVisibilityComponents.begin(), shoeVisibilityComponents.end(), component) == shoeVisibilityComponents.end())
		{
			shoeVisibilityComponents.push_back(component);
			if (trackedHandsShoeHidden)
				SetComponentPresentationVisible(component, false);
		}
	}
}

void PlayerManager::ProcessBodyVisibility(float delta)
{
	if (playerCharacter == nullptr || !uevr::API::UObjectHook::exists(playerCharacter)) {
		lastBodyVisibilityCharacter = nullptr;
		bodyVisibilityRefreshTimer = 0.0f;
		return;
	}

	const bool characterChanged = playerCharacter != lastBodyVisibilityCharacter;
	bodyVisibilityRefreshTimer -= delta;
	if (!characterChanged && bodyVisibilityRefreshTimer > 0.0f)
		return;

	lastBodyVisibilityCharacter = playerCharacter;
	bodyVisibilityRefreshTimer = 1.0f;

	const wchar_t* bodyProperties[] = {
		L"hands", L"Trousers", L"watch",
		L"Tattoo1", L"Tattoo2", L"Tattoo3", L"Tattoo4", L"Tattoo5",
		L"Tattoo6", L"Tattoo7", L"Tattoo8", L"Tattoo9"
	};

	for (const auto* propertyName : bodyProperties)
		ForceComponentVisible(GetObjectPropertyIfPresent(playerCharacter, propertyName));

	ForceComponentHidden(GetObjectPropertyIfPresent(playerCharacter, L"Necklace"));

	auto playerCharacterClass = playerCharacter->get_class();
	if (playerCharacterClass == nullptr || playerCharacterClass->find_property(L"Components") == nullptr)
		return;

	auto components = playerCharacter->get_property_data<uevr::API::TArray<uevr::API::UObject*>>(L"Components");
	if (components == nullptr || components->empty())
		return;
	for (auto component : *components) {
		if (component == nullptr || !uevr::API::UObjectHook::exists(component))
			continue;

		const std::wstring componentName = component->get_fname()->to_string();
		if (NameLooksLikeNeckAccessory(componentName))
			ForceComponentHidden(component);
		else if (NameLooksLikeBodyPart(componentName))
			ForceComponentVisible(component);
	}
}

void PlayerManager::SetHandsScaled(bool visible, bool force)
{
	if (playerCharacter == nullptr || !uevr::API::UObjectHook::exists(playerCharacter))
		return;

	if (playerCharacter != handScaleCharacter) {
		handScaleCharacter = playerCharacter;
		handScaleHands = GetObjectPropertyIfPresent(playerCharacter, L"hands");
		handScaleWatch = GetObjectPropertyIfPresent(playerCharacter, L"watch");
		handsScaledVisible = true;
		watchScaledVisible = true;
	}

	if (!force && handsScaledVisible == visible)
		return;

	handsScaledVisible = visible;
	SetComponentScale(handScaleHands, visible);
	watchScaledVisible = visible;
	SetComponentScale(handScaleWatch, visible);
}

void PlayerManager::SetWatchScaled(bool visible, bool force)
{
	if (handScaleCharacter != playerCharacter)
		SetHandsScaled(true, true);
	if (handScaleWatch == nullptr && playerCharacter != nullptr)
		handScaleWatch = GetObjectPropertyIfPresent(playerCharacter, L"watch");
	if (!force && watchScaledVisible == visible)
		return;
	watchScaledVisible = visible;
	SetComponentScale(handScaleWatch, visible);
}

void PlayerManager::SetTrackedHandsShoeHidden(bool hidden, bool force)
{
	(void)hidden;
	(void)force;
	// Compatibility no-op. The stable body path leaves the native lower body
	// visible and must not toggle clothing or any component hierarchy from the
	// tracked-hand update path.
}

void PlayerManager::RepositionUnhookedUobjects()
{
	if (settingsManager->debugMod) uevr::API::get()->log_info("RepositionUnhookedUobjects()");

	if (playerHead == nullptr)
		return;
	//Reset head position during cutscene
	Utilities::Parameter_K2_SetWorldOrRelativeLocation setRelativeLocation_params{};
	setRelativeLocation_params.bSweep = false;
	setRelativeLocation_params.bTeleport = true;
	setRelativeLocation_params.newLocation = glm::fvec3(0.0f, 0.0f, 0.0f);
	playerHead->call_function(L"K2_SetRelativeLocation", &setRelativeLocation_params);
}

void PlayerManager::AlignControllerToAimDirection(const glm::fvec3& aimDirectionUE)
{
	if (playerController == nullptr)
		return;

	glm::fvec2 horizontalDirection = { aimDirectionUE.x, aimDirectionUE.y };
	if (glm::length(horizontalDirection) <= 0.001f)
		return;

	horizontalDirection = glm::normalize(horizontalDirection);

	Utilities::ParameterSetControlRotation setControlRotation_params{};
	setControlRotation_params.newRotation.pitch = 0.0f;
	constexpr float radiansToDegrees = 57.2957795f;
	setControlRotation_params.newRotation.yaw = atan2f(horizontalDirection.y, horizontalDirection.x) * radiansToDegrees;
	setControlRotation_params.newRotation.roll = 0.0f;
	playerController->call_function(L"SetControlRotation", &setControlRotation_params);

	Utilities::Parameter_K2_SetActorRotation setActorRotation_params{};
	setActorRotation_params.newRotation = setControlRotation_params.newRotation;
	setActorRotation_params.bTeleportPhysics = false;

	if (playerActor != nullptr)
		playerActor->call_function(L"K2_SetActorRotation", &setActorRotation_params);

	if (playerCharacter != nullptr && playerCharacter != playerActor)
		playerCharacter->call_function(L"K2_SetActorRotation", &setActorRotation_params);
}

std::string PlayerManager::VehicleTypeToString(VehicleType type) {
	switch (type) {
	case OnFoot:      return "OnFoot";
	case CarOrBoat:   return "CarOrBoat";
	case Bike:        return "Bike";
	case Helicopter:  return "Helicopter";
	case Plane:       return "Plane";
	default:          return "OnFoot";
	}
};
