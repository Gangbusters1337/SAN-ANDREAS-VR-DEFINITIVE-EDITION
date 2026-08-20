#include "WeaponManager.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/matrix_major_storage.hpp>

namespace
{
	// One bounded ordinary-driving prototype. It only moves the live weapon mesh
	// already attached to the selected local player; it never enters drive-by mode
	// or retains vehicle UObject pointers across frames.
	constexpr bool kVehicleFreeAimExperimentalEnabled = true;

	struct ParameterSetVisibility {
		bool bNewVisibility = true;
		bool bPropagateToChildren = true;
	};

	struct ParameterSetScale3D {
		glm::fvec3 newScale3D = { 1.0f, 1.0f, 1.0f };
	};

	struct ParameterGetLocalBounds {
		glm::fvec3 min{};
		glm::fvec3 max{};
	};

	struct ReflectedScriptArray {
		void* data = nullptr;
		int32_t count = 0;
		int32_t capacity = 0;
	};

	glm::fvec3 NormalizeOrZero(const glm::fvec3& value)
	{
		const float length = glm::length(value);
		if (length <= 0.0001f)
			return glm::fvec3(0.0f);
		return value / length;
	}

	float AngleDegreesOrZero(const glm::fvec3& a, const glm::fvec3& b)
	{
		const glm::fvec3 an = NormalizeOrZero(a);
		const glm::fvec3 bn = NormalizeOrZero(b);
		if (glm::length(an) <= 0.0f || glm::length(bn) <= 0.0f)
			return 0.0f;

		const float dot = (std::max)(-1.0f, (std::min)(1.0f, glm::dot(an, bn)));
		return std::acos(dot) * 57.2957795f;
	}

	float QuaternionAngleDegrees(const glm::fquat& value)
	{
		if (!std::isfinite(value.w) || !std::isfinite(value.x)
			|| !std::isfinite(value.y) || !std::isfinite(value.z)
			|| glm::length(value) <= 0.0001f)
			return 0.0f;

		const glm::fquat normalized = glm::normalize(value);
		const float absoluteW = (std::min)(1.0f, (std::max)(0.0f, std::abs(normalized.w)));
		return 2.0f * std::acos(absoluteW) * 57.2957795f;
	}

	bool IsFiniteVector(const glm::fvec3& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	}

	bool IsFiniteQuaternion(const glm::fquat& value)
	{
		return std::isfinite(value.w) && std::isfinite(value.x)
			&& std::isfinite(value.y) && std::isfinite(value.z);
	}

	glm::fquat RotationBetweenDirections(const glm::fvec3& from, const glm::fvec3& to)
	{
		const glm::fvec3 fromNormal = NormalizeOrZero(from);
		const glm::fvec3 toNormal = NormalizeOrZero(to);
		const float directionDot = (std::max)(-1.0f, (std::min)(1.0f, glm::dot(fromNormal, toNormal)));
		if (directionDot > 0.9999f)
			return glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);

		if (directionDot < -0.9999f)
		{
			glm::fvec3 axis = glm::cross(fromNormal, glm::fvec3(0.0f, 0.0f, 1.0f));
			if (glm::length(axis) <= 0.0001f)
				axis = glm::cross(fromNormal, glm::fvec3(0.0f, 1.0f, 0.0f));
			return glm::angleAxis(3.14159265f, glm::normalize(axis));
		}

		const glm::fvec3 axis = glm::cross(fromNormal, toNormal);
		return glm::normalize(glm::fquat::wxyz(1.0f + directionDot, axis.x, axis.y, axis.z));
	}

	const glm::fquat& UEVRQuatConverter()
	{
		// Reproduce UObjectHook.cpp's actual construction instead of guessing
		// whether its matrix literal should be read as rows or columns:
		// glm::quat{Matrix4x4f{0,0,-1,0, 1,0,0,0, 0,1,0,0, 0,0,0,1}}.
		static const glm::fquat converter = glm::normalize(glm::fquat(glm::fmat4{
			0.0f, 0.0f, -1.0f, 0.0f,
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f
		}));
		return converter;
	}

	glm::fvec3 VRSpaceToComponentSpace(const glm::fvec3& value)
	{
		return UEVRQuatConverter() * value;
	}

	glm::fquat VRQuaternionToComponentSpace(const glm::fquat& value)
	{
		const glm::fquat normalizedValue = glm::normalize(value);
		// The same converter must be applied on both sides for an orientation;
		// multiplying quaternion components or using the raw reflection matrix
		// would not preserve a proper rotation.
		return glm::normalize(UEVRQuatConverter() * normalizedValue
			* glm::inverse(UEVRQuatConverter()));
	}

	glm::fquat ComponentQuaternionToVRSpace(const glm::fquat& value)
	{
		const glm::fquat normalizedValue = glm::normalize(value);
		return glm::normalize(glm::inverse(UEVRQuatConverter()) * normalizedValue
			* UEVRQuatConverter());
	}

	glm::fquat FlattenViewInverse(const UEVR_Rotatorf& rotation)
	{
		const glm::fmat4 viewMatrixInverse = glm::yawPitchRoll(
			glm::radians(-rotation.yaw),
			glm::radians(rotation.pitch),
			glm::radians(-rotation.roll));
		const glm::fquat viewInverse = glm::normalize(glm::fquat(viewMatrixInverse));
		const glm::fvec3 forward = NormalizeOrZero(viewInverse * glm::fvec3(0.0f, 0.0f, 1.0f));
		const glm::fvec3 flattenedForward = NormalizeOrZero(glm::fvec3(forward.x, 0.0f, forward.z));
		if (glm::length(flattenedForward) <= 0.0001f)
			return glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);

		// This is the same left-handed look-at conversion used by UEVR's
		// utility::math::flatten() helper for controller attachment.
		return glm::normalize(glm::fquat(glm::rowMajor4(glm::lookAtLH(
			glm::fvec3(0.0f), flattenedForward, glm::fvec3(0.0f, 1.0f, 0.0f)))));
	}

	glm::fquat RotationFromWeaponBasis(const glm::fvec3& forward, const glm::fvec3& right, const glm::fvec3& up)
	{
		glm::fmat4 basis(1.0f);
		basis[0] = glm::fvec4(forward, 0.0f);
		basis[1] = glm::fvec4(right, 0.0f);
		basis[2] = glm::fvec4(up, 0.0f);
		return glm::normalize(glm::quat_cast(basis));
	}

	bool NormalizeHolsterPose(bool longGun, int verticalAxis, int hand, glm::fvec3& position,
		glm::fvec3& forward, glm::fvec3& right, glm::fvec3& up)
	{
		verticalAxis = std::clamp(verticalAxis, 0, 2);
		bool changed = false;
		// ReadMagneticBodyFrame explicitly constructs local +X as anatomical forward
		// and local +Y as anatomical right from CJ's thigh positions. Keep the waist
		// symmetric around Y=0. The previous
		// one-sample -15.4 cm "belt centre" and reversed hand tie-breaker sent left-
		// hand releases to the right/buckle slot and collapsed most poses together.
		const float sideSign = std::isfinite(position.y) && std::abs(position.y) >= 5.0f
			? (position.y < 0.0f ? -1.0f : 1.0f)
			: (hand == 0 ? -1.0f : 1.0f);
		const glm::fvec3 targetPosition = longGun
			? glm::fvec3(8.0f, sideSign * 22.0f, -68.0f)
			: glm::fvec3(10.0f, sideSign * 19.0f, -62.0f);
		const float releaseDistance = IsFiniteVector(position)
			? glm::length(position - targetPosition) : 1000.0f;
		const float targetHeight = targetPosition.z;
		if (std::abs(position.z - targetHeight) > 0.01f)
		{
			position.z = targetHeight;
			changed = true;
		}

		const float minimumSide = longGun ? 21.0f : 18.0f;
		const float maximumSide = longGun ? 30.0f : 26.0f;
		const float sourceSide = std::isfinite(position.y)
			? std::abs(position.y)
			: std::abs(targetPosition.y);
		const float desiredSide = std::clamp(sourceSide, minimumSide, maximumSide);
		const float desiredLocalY = sideSign * desiredSide;
		if (!std::isfinite(position.y) || std::abs(position.y - desiredLocalY) > 0.01f)
		{
			position.y = desiredLocalY;
			changed = true;
		}
		// Preserve front-half placement while preventing belt-buckle and rear-body
		// intersections. These bounds contain the first-release hip anchors and the
		// previously accepted dynamic front-waist samples.
		const float clampedForward = std::clamp(position.x,
			longGun ? 2.0f : 4.0f, longGun ? 22.0f : 26.0f);
		if (!std::isfinite(position.x) || std::abs(position.x - clampedForward) > 0.01f)
		{
			position.x = std::isfinite(clampedForward) ? clampedForward : targetPosition.x;
			changed = true;
		}

		forward = NormalizeOrZero(forward);
		right = NormalizeOrZero(right);
		up = NormalizeOrZero(up);
		// Preserve exact orientation only when the release is both genuinely
		// nose-down and close to its side slot. Farther drops require a steeper
		// barrel, preventing chest-height/extended-arm poses from being saved.
		const float distanceFactor = std::clamp(releaseDistance / 55.0f, 0.0f, 1.0f);
		const float requiredDown = longGun
			? -(0.80f + 0.15f * distanceFactor)
			: -(0.72f + 0.20f * distanceFactor);
		const glm::fvec3 principalAxis = verticalAxis == 2 ? up
			: verticalAxis == 1 ? right : forward;
		if (principalAxis.z <= requiredDown)
			return changed;

		glm::fvec2 heading(principalAxis.x, principalAxis.y);
		if (!std::isfinite(glm::length(heading)) || glm::length(heading) < 0.1f)
			heading = glm::fvec2(1.0f, 0.0f);
		else
			heading = glm::normalize(heading);
		const float horizontalForward = longGun || verticalAxis != 0 ? 0.12f : 0.32f;
		const glm::fvec3 axisDown = glm::normalize(glm::fvec3(
			heading.x * horizontalForward, heading.y * horizontalForward, -1.0f));
		if (verticalAxis == 0)
		{
			forward = axisDown;
			right -= forward * glm::dot(right, forward);
			if (glm::length(right) < 0.1f)
			{
				right = glm::fvec3(0.0f, 1.0f, 0.0f);
				right -= forward * glm::dot(right, forward);
			}
			if (glm::length(right) < 0.1f)
			{
				right = glm::fvec3(1.0f, 0.0f, 0.0f);
				right -= forward * glm::dot(right, forward);
			}
			right = NormalizeOrZero(right);
			up = NormalizeOrZero(glm::cross(forward, right));
		}
		else if (verticalAxis == 1)
		{
			right = axisDown;
			forward -= right * glm::dot(forward, right);
			if (glm::length(forward) < 0.1f)
			{
				forward = glm::fvec3(1.0f, 0.0f, 0.0f);
				forward -= right * glm::dot(forward, right);
			}
			forward = NormalizeOrZero(forward);
			up = NormalizeOrZero(glm::cross(forward, right));
		}
		else
		{
			up = axisDown;
			right -= up * glm::dot(right, up);
			if (glm::length(right) < 0.1f)
			{
				right = glm::fvec3(0.0f, -1.0f, 0.0f);
				right -= up * glm::dot(right, up);
			}
			right = NormalizeOrZero(right);
			forward = NormalizeOrZero(glm::cross(right, up));
		}
		return true;
	}

	struct ParameterMakeRotationFromAxes
	{
		glm::fvec3 forward{};
		glm::fvec3 right{};
		glm::fvec3 up{};
		Utilities::FRotator returnValue{};
	};

	// The custom hand-thrown bottle deliberately uses some of the same reflected
	// SpawnExplosion helpers as the game. Keep a per-thread suppression depth so
	// a passive observer can distinguish our own setup calls from a genuine
	// native Molotov trigger. This changes neither call nor result.
	thread_local uint32_t motionThrowableNativeProbeSuppressDepth = 0;

	class ScopedMotionThrowableNativeProbeSuppression
	{
	public:
		ScopedMotionThrowableNativeProbeSuppression()
		{
			++motionThrowableNativeProbeSuppressDepth;
		}

		~ScopedMotionThrowableNativeProbeSuppression()
		{
			if (motionThrowableNativeProbeSuppressDepth > 0)
				--motionThrowableNativeProbeSuppressDepth;
		}

		ScopedMotionThrowableNativeProbeSuppression(
			const ScopedMotionThrowableNativeProbeSuppression&) = delete;
		ScopedMotionThrowableNativeProbeSuppression& operator=(
			const ScopedMotionThrowableNativeProbeSuppression&) = delete;
	};

	uevr::API::FProperty* FindFunctionParameter(uevr::API::UFunction* function, const wchar_t* name)
	{
		return function != nullptr ? function->find_property(name) : nullptr;
	}

	bool SetReflectedObjectParameter(uevr::API::UFunction* function, std::vector<uint8_t>& params,
		const wchar_t* name, uevr::API::UObject* value)
	{
		auto property = FindFunctionParameter(function, name);
		if (property == nullptr || property->get_offset() < 0
			|| static_cast<size_t>(property->get_offset()) + sizeof(value) > params.size())
			return false;
		std::memcpy(params.data() + property->get_offset(), &value, sizeof(value));
		return true;
	}

	bool SetReflectedBoolParameter(uevr::API::UFunction* function, std::vector<uint8_t>& params,
		const wchar_t* name, bool value)
	{
		auto property = FindFunctionParameter(function, name);
		if (property == nullptr || property->get_offset() < 0)
			return false;
		const auto propertyClassName = property->get_class() != nullptr
			? property->get_class()->get_fname()->to_string() : L"";
		if (propertyClassName == L"BoolProperty")
		{
			auto boolProperty = reinterpret_cast<uevr::API::FBoolProperty*>(property);
			const size_t byteIndex = static_cast<size_t>(property->get_offset()) + boolProperty->get_byte_offset();
			if (byteIndex >= params.size())
				return false;
			const uint8_t mask = static_cast<uint8_t>(boolProperty->get_byte_mask());
			if (value)
				params[byteIndex] |= mask;
			else
				params[byteIndex] &= static_cast<uint8_t>(~mask);
			return true;
		}
		if (static_cast<size_t>(property->get_offset()) + sizeof(bool) > params.size())
			return false;
		std::memcpy(params.data() + property->get_offset(), &value, sizeof(value));
		return true;
	}

	bool SetReflectedIntParameter(uevr::API::UFunction* function, std::vector<uint8_t>& params,
		const wchar_t* name, int32_t value)
	{
		auto property = FindFunctionParameter(function, name);
		if (property == nullptr || property->get_offset() < 0
			|| static_cast<size_t>(property->get_offset()) + sizeof(value) > params.size())
			return false;
		std::memcpy(params.data() + property->get_offset(), &value, sizeof(value));
		return true;
	}

	bool SetReflectedFloatParameter(uevr::API::UFunction* function,
		std::vector<uint8_t>& params, const wchar_t* name, float value)
	{
		auto property = FindFunctionParameter(function, name);
		if (property == nullptr || property->get_offset() < 0
			|| property->get_class() == nullptr
			|| property->get_class()->get_fname()->to_string() != L"FloatProperty"
			|| static_cast<size_t>(property->get_offset()) + sizeof(value) > params.size()
			|| !std::isfinite(value))
			return false;
		std::memcpy(params.data() + property->get_offset(), &value, sizeof(value));
		return true;
	}

	bool SetReflectedVectorParameter(uevr::API::UFunction* function,
		std::vector<uint8_t>& params, const wchar_t* name, const glm::fvec3& value)
	{
		auto property = FindFunctionParameter(function, name);
		if (property == nullptr || property->get_offset() < 0
			|| property->get_class() == nullptr
			|| property->get_class()->get_fname()->to_string() != L"StructProperty"
			|| !IsFiniteVector(value))
			return false;
		auto structProperty = reinterpret_cast<uevr::API::FStructProperty*>(property);
		const int32_t structSize = structProperty->get_struct() != nullptr
			? structProperty->get_struct()->get_struct_size() : 0;
		if (structSize < static_cast<int32_t>(sizeof(value))
			|| static_cast<size_t>(property->get_offset()) + structSize > params.size())
			return false;
		std::memset(params.data() + property->get_offset(), 0, structSize);
		std::memcpy(params.data() + property->get_offset(), &value, sizeof(value));
		return true;
	}

	bool SetReflectedArrayParameter(uevr::API::UFunction* function,
		std::vector<uint8_t>& params, const wchar_t* name, void* data, int32_t count)
	{
		auto property = FindFunctionParameter(function, name);
		if (property == nullptr || property->get_offset() < 0
			|| property->get_class() == nullptr
			|| property->get_class()->get_fname()->to_string() != L"ArrayProperty"
			|| static_cast<size_t>(property->get_offset()) + sizeof(ReflectedScriptArray) > params.size())
			return false;
		ReflectedScriptArray value{ data, count, count };
		std::memcpy(params.data() + property->get_offset(), &value, sizeof(value));
		return true;
	}

	bool SetReflectedByteParameter(uevr::API::UFunction* function, std::vector<uint8_t>& params,
		const wchar_t* name, uint8_t value)
	{
		auto property = FindFunctionParameter(function, name);
		if (property == nullptr || property->get_offset() < 0
			|| static_cast<size_t>(property->get_offset()) + sizeof(value) > params.size())
			return false;
		std::memcpy(params.data() + property->get_offset(), &value, sizeof(value));
		return true;
	}

	bool SetReflectedFNameParameter(uevr::API::UFunction* function, std::vector<uint8_t>& params,
		const wchar_t* name, const uevr::API::FName& value)
	{
		auto property = FindFunctionParameter(function, name);
		if (property == nullptr || property->get_offset() < 0
			|| static_cast<size_t>(property->get_offset()) + sizeof(uevr::API::FName) > params.size())
			return false;
		// Copy an engine-returned FName. The UEVR FName constructor is unavailable
		// for this game build, so constructing names from text produces NAME_None.
		std::memcpy(params.data() + property->get_offset(), &value, sizeof(value));
		return true;
	}

	bool SetReflectedIdentityTransform(uevr::API::UFunction* function, std::vector<uint8_t>& params,
		const wchar_t* name)
	{
		auto property = FindFunctionParameter(function, name);
		const auto propertyClassName = property != nullptr && property->get_class() != nullptr
			? property->get_class()->get_fname()->to_string() : L"";
		if (propertyClassName != L"StructProperty" || property->get_offset() < 0)
			return false;
		auto structProperty = reinterpret_cast<uevr::API::FStructProperty*>(property);
		const int32_t structSize = structProperty->get_struct() != nullptr
			? structProperty->get_struct()->get_struct_size() : 0;
		if (structSize < 40 || static_cast<size_t>(property->get_offset()) + structSize > params.size())
			return false;
		std::memset(params.data() + property->get_offset(), 0, structSize);
		float identityTransform[12] = { 0.0f, 0.0f, 0.0f, 1.0f,
			0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f };
		std::memcpy(params.data() + property->get_offset(), identityTransform,
			(std::min)(static_cast<size_t>(structSize), sizeof(identityTransform)));
		return true;
	}

	bool SetReflectedTransformParameter(uevr::API::UFunction* function, std::vector<uint8_t>& params,
		const wchar_t* name, const glm::fvec3& position, const glm::fquat& rotation,
		const glm::fvec3& scale = glm::fvec3(1.0f))
	{
		auto property = FindFunctionParameter(function, name);
		const auto propertyClassName = property != nullptr && property->get_class() != nullptr
			? property->get_class()->get_fname()->to_string() : L"";
		if (propertyClassName != L"StructProperty" || property->get_offset() < 0)
			return false;
		auto structProperty = reinterpret_cast<uevr::API::FStructProperty*>(property);
		const int32_t structSize = structProperty->get_struct() != nullptr
			? structProperty->get_struct()->get_struct_size() : 0;
		if (structSize < 40 || static_cast<size_t>(property->get_offset()) + structSize > params.size()
			|| !IsFiniteVector(position) || !IsFiniteQuaternion(rotation)
			|| !IsFiniteVector(scale))
			return false;
		std::memset(params.data() + property->get_offset(), 0, structSize);
		const glm::fquat normalized = glm::normalize(rotation);
		// UE4 FTransform: quaternion XYZW, 16-byte aligned translation, then scale.
		const float transform[12] = {
			normalized.x, normalized.y, normalized.z, normalized.w,
			position.x, position.y, position.z, 0.0f,
			scale.x, scale.y, scale.z, 0.0f
		};
		std::memcpy(params.data() + property->get_offset(), transform,
			(std::min)(static_cast<size_t>(structSize), sizeof(transform)));
		return true;
	}

	bool ReadReflectedObjectReturn(uevr::API::UFunction* function, const std::vector<uint8_t>& params,
		uevr::API::UObject*& value)
	{
		auto property = FindFunctionParameter(function, L"ReturnValue");
		if (property == nullptr || property->get_offset() < 0
			|| static_cast<size_t>(property->get_offset()) + sizeof(value) > params.size())
			return false;
		std::memcpy(&value, params.data() + property->get_offset(), sizeof(value));
		return true;
	}

	bool ReadReflectedObjectParameter(uevr::API::UFunction* function,
		const std::vector<uint8_t>& params, const wchar_t* name, uevr::API::UObject*& value)
	{
		auto property = FindFunctionParameter(function, name);
		if (property == nullptr || property->get_offset() < 0
			|| static_cast<size_t>(property->get_offset()) + sizeof(value) > params.size())
			return false;
		std::memcpy(&value, params.data() + property->get_offset(), sizeof(value));
		return true;
	}

	bool ReadReflectedBoolParameter(uevr::API::UFunction* function,
		const std::vector<uint8_t>& params, const wchar_t* name, bool& value)
	{
		auto property = FindFunctionParameter(function, name);
		if (property == nullptr || property->get_offset() < 0)
			return false;
		const auto propertyClassName = property->get_class() != nullptr
			? property->get_class()->get_fname()->to_string() : L"";
		if (propertyClassName == L"BoolProperty")
		{
			auto boolProperty = reinterpret_cast<uevr::API::FBoolProperty*>(property);
			const size_t byteIndex = static_cast<size_t>(property->get_offset())
				+ boolProperty->get_byte_offset();
			if (byteIndex >= params.size())
				return false;
			value = (params[byteIndex]
				& static_cast<uint8_t>(boolProperty->get_byte_mask())) != 0;
			return true;
		}
		if (static_cast<size_t>(property->get_offset()) + sizeof(bool) > params.size())
			return false;
		std::memcpy(&value, params.data() + property->get_offset(), sizeof(value));
		return true;
	}

	bool ReadReflectedVectorField(uevr::API::UFunction* function,
		const std::vector<uint8_t>& params, const wchar_t* structParameterName,
		const wchar_t* fieldName, glm::fvec3& value)
	{
		auto property = FindFunctionParameter(function, structParameterName);
		if (property == nullptr || property->get_offset() < 0
			|| property->get_class() == nullptr
			|| property->get_class()->get_fname()->to_string() != L"StructProperty")
			return false;
		auto structProperty = reinterpret_cast<uevr::API::FStructProperty*>(property);
		auto scriptStruct = structProperty->get_struct();
		if (scriptStruct == nullptr)
			return false;
		auto field = scriptStruct->find_property(fieldName);
		if (field == nullptr || field->get_offset() < 0
			|| static_cast<size_t>(field->get_offset()) + sizeof(value)
				> static_cast<size_t>(scriptStruct->get_struct_size()))
			return false;
		const size_t byteIndex = static_cast<size_t>(property->get_offset())
			+ static_cast<size_t>(field->get_offset());
		if (byteIndex + sizeof(value) > params.size())
			return false;
		std::memcpy(&value, params.data() + byteIndex, sizeof(value));
		return IsFiniteVector(value);
	}

	bool ReadReflectedBoolField(uevr::API::UFunction* function,
		const std::vector<uint8_t>& params, const wchar_t* structParameterName,
		const wchar_t* fieldName, bool& value)
	{
		auto property = FindFunctionParameter(function, structParameterName);
		if (property == nullptr || property->get_offset() < 0
			|| property->get_class() == nullptr
			|| property->get_class()->get_fname()->to_string() != L"StructProperty")
			return false;
		auto structProperty = reinterpret_cast<uevr::API::FStructProperty*>(property);
		auto scriptStruct = structProperty->get_struct();
		if (scriptStruct == nullptr)
			return false;
		auto field = scriptStruct->find_property(fieldName);
		if (field == nullptr || field->get_offset() < 0
			|| field->get_class() == nullptr
			|| field->get_class()->get_fname()->to_string() != L"BoolProperty")
			return false;
		auto boolProperty = reinterpret_cast<uevr::API::FBoolProperty*>(field);
		const size_t byteIndex = static_cast<size_t>(property->get_offset())
			+ static_cast<size_t>(field->get_offset())
			+ boolProperty->get_byte_offset();
		if (byteIndex >= params.size())
			return false;
		value = (params[byteIndex]
			& static_cast<uint8_t>(boolProperty->get_byte_mask())) != 0;
		return true;
	}

	struct MotionThrowableWorldSweepHit
	{
		glm::fvec3 point{};
		glm::fvec3 normal{};
	};

	bool MoveMotionThrowableCollisionProxy(uevr::API::UObject* proxy,
		const glm::fvec3& position, MotionThrowableWorldSweepHit& hit)
	{
		hit = MotionThrowableWorldSweepHit{};
		if (proxy == nullptr || !uevr::API::UObjectHook::exists(proxy)
			|| !IsFiniteVector(position) || proxy->get_class() == nullptr)
			return false;
		auto function = proxy->get_class()->find_function(L"K2_SetWorldLocation");
		if (function == nullptr)
			return false;
		std::vector<uint8_t> params(function->get_properties_size());
		if (!SetReflectedVectorParameter(function, params, L"NewLocation", position)
			|| !SetReflectedBoolParameter(function, params, L"bSweep", true)
			|| !SetReflectedBoolParameter(function, params, L"bTeleport", false))
			return false;
		function->call(proxy, params.data());
		bool blockingHit = false;
		if (!ReadReflectedBoolField(function, params, L"SweepHitResult",
			L"bBlockingHit", blockingHit) || !blockingHit)
			return false;
		if (!ReadReflectedVectorField(function, params, L"SweepHitResult",
			L"ImpactPoint", hit.point)
			&& !ReadReflectedVectorField(function, params, L"SweepHitResult",
				L"Location", hit.point))
			return false;
		ReadReflectedVectorField(function, params, L"SweepHitResult",
			L"ImpactNormal", hit.normal);
		return IsFiniteVector(hit.point);
	}

	bool SweepMotionThrowableWorld(uevr::API::UObject* worldContext,
		const glm::fvec3& start, const glm::fvec3& end, float radius,
		uint8_t traceChannel,
		bool& traceAvailable, MotionThrowableWorldSweepHit& hit)
	{
		traceAvailable = false;
		hit = MotionThrowableWorldSweepHit{};
		if (worldContext == nullptr || !IsFiniteVector(start)
			|| !IsFiniteVector(end) || !std::isfinite(radius) || radius <= 0.0f)
			return false;
		auto traceClass = uevr::API::get()->find_uobject<uevr::API::UClass>(
			L"Class /Script/Engine.KismetSystemLibrary");
		auto traceObject = traceClass != nullptr
			? traceClass->get_class_default_object() : nullptr;
		auto function = traceClass != nullptr
			? traceClass->find_function(L"SphereTraceSingle") : nullptr;
		if (traceObject == nullptr || function == nullptr
			|| FindFunctionParameter(function, L"OutHit") == nullptr
			|| FindFunctionParameter(function, L"ReturnValue") == nullptr)
			return false;

		std::vector<uint8_t> params(function->get_properties_size());
		if (!SetReflectedObjectParameter(function, params, L"WorldContextObject", worldContext)
			|| !SetReflectedVectorParameter(function, params, L"Start", start)
			|| !SetReflectedVectorParameter(function, params, L"End", end)
			|| !SetReflectedFloatParameter(function, params, L"Radius", radius)
			|| !SetReflectedByteParameter(function, params, L"TraceChannel", traceChannel)
			|| !SetReflectedBoolParameter(function, params, L"bTraceComplex", false)
			|| !SetReflectedArrayParameter(function, params, L"ActorsToIgnore", nullptr, 0)
			|| !SetReflectedByteParameter(function, params, L"DrawDebugType", 0)
			|| !SetReflectedBoolParameter(function, params, L"bIgnoreSelf", true))
			return false;
		if (FindFunctionParameter(function, L"DrawTime") != nullptr
			&& !SetReflectedFloatParameter(function, params, L"DrawTime", 0.0f))
			return false;

		traceAvailable = true;
		function->call(traceObject, params.data());
		bool returnedHit = false;
		if (!ReadReflectedBoolParameter(function, params, L"ReturnValue", returnedHit)
			|| !returnedHit)
			return false;
		if (!ReadReflectedVectorField(function, params, L"OutHit", L"ImpactPoint", hit.point)
			&& !ReadReflectedVectorField(function, params, L"OutHit", L"Location", hit.point))
			return false;
		ReadReflectedVectorField(function, params, L"OutHit", L"ImpactNormal", hit.normal);
		if (glm::length(hit.normal) <= 0.01f)
			ReadReflectedVectorField(function, params, L"OutHit", L"Normal", hit.normal);
		return IsFiniteVector(hit.point);
	}

	bool CallSetSkeletalMesh(uevr::API::UObject* component, uevr::API::UObject* skeletalMesh)
	{
		if (component == nullptr || skeletalMesh == nullptr)
			return false;
		auto function = component->get_class()->find_function(L"SetSkeletalMesh");
		if (function == nullptr)
			return false;
		std::vector<uint8_t> params(function->get_properties_size());
		if (!SetReflectedObjectParameter(function, params, L"NewMesh", skeletalMesh))
			return false;
		SetReflectedBoolParameter(function, params, L"bReinitPose", true);
		function->call(component, params.data());
		return true;
	}

	size_t ReflectedPropertySpan(uevr::API::UFunction* function,
		uevr::API::FProperty* property)
	{
		if (function == nullptr || property == nullptr || property->get_offset() < 0)
			return 0;
		const size_t begin = static_cast<size_t>(property->get_offset());
		size_t end = static_cast<size_t>(function->get_properties_size());
		for (auto field = function->get_child_properties(); field != nullptr; field = field->get_next())
		{
			auto candidate = reinterpret_cast<uevr::API::FProperty*>(field);
			if (candidate == property || candidate->get_offset() <= property->get_offset())
				continue;
			end = (std::min)(end, static_cast<size_t>(candidate->get_offset()));
		}
		return end > begin ? end - begin : 0;
	}

	bool SetReflectedStringParameter(uevr::API::UFunction* function,
		std::vector<uint8_t>& params, const wchar_t* name,
		std::vector<wchar_t>& storage)
	{
		auto property = FindFunctionParameter(function, name);
		if (property == nullptr || property->get_offset() < 0
			|| property->get_class() == nullptr
			|| property->get_class()->get_fname()->to_string() != L"StrProperty")
			return false;
		struct ReflectedString
		{
			wchar_t* data;
			int32_t count;
			int32_t capacity;
		};
		const ReflectedString value{ storage.data(),
			static_cast<int32_t>(storage.size()), static_cast<int32_t>(storage.size()) };
		if (static_cast<size_t>(property->get_offset()) + sizeof(value) > params.size())
			return false;
		std::memcpy(params.data() + property->get_offset(), &value, sizeof(value));
		return true;
	}

	bool CopyReflectedParameterBytes(uevr::API::UFunction* function,
		std::vector<uint8_t>& params, const wchar_t* name,
		const std::vector<uint8_t>& source)
	{
		auto property = FindFunctionParameter(function, name);
		const size_t span = ReflectedPropertySpan(function, property);
		if (property == nullptr || property->get_offset() < 0 || span == 0
			|| source.empty() || source.size() > span
			|| static_cast<size_t>(property->get_offset()) + span > params.size())
			return false;
		std::memcpy(params.data() + property->get_offset(), source.data(), source.size());
		return true;
	}

	bool ReadReflectedParameterBytes(uevr::API::UFunction* function,
		const std::vector<uint8_t>& params, const wchar_t* name,
		std::vector<uint8_t>& destination)
	{
		auto property = FindFunctionParameter(function, name);
		const size_t span = ReflectedPropertySpan(function, property);
		if (property == nullptr || property->get_offset() < 0 || span == 0
			|| static_cast<size_t>(property->get_offset()) + span > params.size())
			return false;
		destination.assign(params.begin() + property->get_offset(),
			params.begin() + property->get_offset() + span);
		return true;
	}

	uevr::API::UObject* LoadStaticMeshAssetBlocking(const wchar_t* objectPath)
	{
		if (objectPath == nullptr || objectPath[0] == L'\0')
			return nullptr;
		const std::wstring fullName = std::wstring(L"StaticMesh ") + objectPath;
		if (auto loaded = uevr::API::get()->find_uobject<uevr::API::UObject>(fullName.c_str()))
			return loaded;
		const auto fail = [&](const char* stage) -> uevr::API::UObject*
		{
			uevr::API::get()->log_warn(
				"[ClenchedAssets] load failed stage=%s path=%ls", stage, objectPath);
			return nullptr;
		};

		// Shipping builds sometimes retain the native OBJ loader even when an asset
		// was not indexed in the base AssetRegistry. Try that bounded package load
		// first, then use the reflected soft-reference path below.
		std::wstring packagePath(objectPath);
		if (const auto dot = packagePath.rfind(L'.'); dot != std::wstring::npos)
			packagePath.resize(dot);
		const std::wstring loadCommand = L"obj load package=" + packagePath;
		uevr::API::get()->execute_command(loadCommand.c_str());
		if (auto loaded = uevr::API::get()->find_uobject<uevr::API::UObject>(fullName.c_str()))
			return loaded;

		auto systemClass = uevr::API::get()->find_uobject<uevr::API::UClass>(
			L"Class /Script/Engine.KismetSystemLibrary");
		auto systemObject = systemClass != nullptr ? systemClass->get_class_default_object() : nullptr;
		if (systemClass == nullptr || systemObject == nullptr)
			return fail("kismet-system-class");
		auto makePath = systemClass->find_function(L"MakeSoftObjectPath");
		auto convertPath = systemClass->find_function(L"Conv_SoftObjPathToSoftObjRef");
		auto loadAsset = systemClass->find_function(L"LoadAsset_Blocking");
		if (makePath == nullptr || convertPath == nullptr || loadAsset == nullptr)
			return fail("kismet-functions");

		std::vector<wchar_t> pathStorage(objectPath, objectPath + std::wcslen(objectPath));
		pathStorage.push_back(L'\0');
		std::vector<uint8_t> makeParams(makePath->get_properties_size());
		if (!SetReflectedStringParameter(makePath, makeParams, L"PathString", pathStorage))
			return fail("make-path-input");
		makePath->call(systemObject, makeParams.data());
		std::vector<uint8_t> softPath;
		if (!ReadReflectedParameterBytes(makePath, makeParams, L"ReturnValue", softPath))
			return fail("make-path-output");

		std::vector<uint8_t> convertParams(convertPath->get_properties_size());
		if (!CopyReflectedParameterBytes(convertPath, convertParams, L"SoftObjectPath", softPath))
			return fail("convert-path-input");
		convertPath->call(systemObject, convertParams.data());
		std::vector<uint8_t> softReference;
		if (!ReadReflectedParameterBytes(convertPath, convertParams, L"ReturnValue", softReference))
			return fail("convert-path-output");

		std::vector<uint8_t> loadParams(loadAsset->get_properties_size());
		if (!CopyReflectedParameterBytes(loadAsset, loadParams, L"Asset", softReference))
			return fail("load-asset-input");
		loadAsset->call(systemObject, loadParams.data());
		uevr::API::UObject* result = nullptr;
		if (!ReadReflectedObjectReturn(loadAsset, loadParams, result))
			return fail("load-asset-output");
		if (result == nullptr)
			return fail("load-asset-null");
		uevr::API::get()->log_info(
			"[ClenchedAssets] loaded path=%ls object=%p name=%ls",
			objectPath, result, result->get_full_name().c_str());
		return result;
	}

	uevr::API::UObject* FindOrLoadSoundWave(const wchar_t* objectPath)
	{
		if (objectPath == nullptr || objectPath[0] == L'\0')
			return nullptr;
		const std::wstring fullName = std::wstring(L"SoundWave ") + objectPath;
		if (auto loaded = uevr::API::get()->find_uobject<uevr::API::UObject>(fullName.c_str()))
			return loaded;

		// These sounds ship in the game's own collision bank. Ask the engine to
		// load only the exact package, then require the expected SoundWave object.
		std::wstring packagePath(objectPath);
		if (const auto dot = packagePath.rfind(L'.'); dot != std::wstring::npos)
			packagePath.resize(dot);
		const std::wstring loadCommand = L"obj load package=" + packagePath;
		uevr::API::get()->execute_command(loadCommand.c_str());
		return uevr::API::get()->find_uobject<uevr::API::UObject>(fullName.c_str());
	}

	uevr::API::UObject* FindOrLoadParticleSystem(const wchar_t* objectPath)
	{
		if (objectPath == nullptr || objectPath[0] == L'\0')
			return nullptr;
		const std::wstring fullName = std::wstring(L"ParticleSystem ") + objectPath;
		if (auto loaded = uevr::API::get()->find_uobject<uevr::API::UObject>(fullName.c_str()))
			return loaded;

		std::wstring packagePath(objectPath);
		if (const auto dot = packagePath.rfind(L'.'); dot != std::wstring::npos)
			packagePath.resize(dot);
		uevr::API::get()->execute_command((std::wstring(L"obj load package=") + packagePath).c_str());
		return uevr::API::get()->find_uobject<uevr::API::UObject>(fullName.c_str());
	}

	uevr::API::UClass* FindOrLoadBlueprintGeneratedClass(const wchar_t* objectPath)
	{
		if (objectPath == nullptr || objectPath[0] == L'\0')
			return nullptr;
		const std::wstring fullName = std::wstring(L"BlueprintGeneratedClass ") + objectPath;
		if (auto loaded = uevr::API::get()->find_uobject<uevr::API::UClass>(fullName.c_str()))
			return loaded;

		std::wstring packagePath(objectPath);
		if (const auto dot = packagePath.rfind(L'.'); dot != std::wstring::npos)
			packagePath.resize(dot);
		uevr::API::get()->execute_command((std::wstring(L"obj load package=") + packagePath).c_str());
		return uevr::API::get()->find_uobject<uevr::API::UClass>(fullName.c_str());
	}

	uevr::API::UObject* SpawnNativeActorAtLocation(uevr::API::UObject* worldContext,
		uevr::API::UClass* actorClass, const glm::fvec3& location)
	{
		if (worldContext == nullptr || actorClass == nullptr || !IsFiniteVector(location))
			return nullptr;
		auto gameplayStaticsClass = uevr::API::get()->find_uobject<uevr::API::UClass>(
			L"Class /Script/Engine.GameplayStatics");
		auto gameplayStatics = gameplayStaticsClass != nullptr
			? gameplayStaticsClass->get_class_default_object() : nullptr;
		auto beginSpawn = gameplayStaticsClass != nullptr
			? gameplayStaticsClass->find_function(L"BeginDeferredActorSpawnFromClass") : nullptr;
		auto finishSpawn = gameplayStaticsClass != nullptr
			? gameplayStaticsClass->find_function(L"FinishSpawningActor") : nullptr;
		if (gameplayStatics == nullptr || beginSpawn == nullptr || finishSpawn == nullptr)
			return nullptr;

		const glm::fquat identity = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
		std::vector<uint8_t> beginParams(beginSpawn->get_properties_size());
		if (!SetReflectedObjectParameter(beginSpawn, beginParams, L"WorldContextObject", worldContext)
			|| !SetReflectedObjectParameter(beginSpawn, beginParams, L"ActorClass", actorClass)
			|| !SetReflectedTransformParameter(beginSpawn, beginParams, L"SpawnTransform",
				location, identity))
			return nullptr;
		// This enum value is the one already used by the project's proven Lua
		// actor-spawn helper: always create at the measured impact rather than
		// silently rejecting a wall, vehicle, or floor collision.
		if (FindFunctionParameter(beginSpawn, L"CollisionHandlingOverride") != nullptr
			&& !SetReflectedByteParameter(beginSpawn, beginParams,
				L"CollisionHandlingOverride", 1))
			return nullptr;
		if (FindFunctionParameter(beginSpawn, L"Owner") != nullptr
			&& !SetReflectedObjectParameter(beginSpawn, beginParams, L"Owner", worldContext))
			return nullptr;
		beginSpawn->call(gameplayStatics, beginParams.data());
		uevr::API::UObject* actor = nullptr;
		if (!ReadReflectedObjectReturn(beginSpawn, beginParams, actor)
			|| actor == nullptr || !uevr::API::UObjectHook::exists(actor))
			return nullptr;

		std::vector<uint8_t> finishParams(finishSpawn->get_properties_size());
		if (!SetReflectedObjectParameter(finishSpawn, finishParams, L"Actor", actor)
			|| !SetReflectedTransformParameter(finishSpawn, finishParams, L"SpawnTransform",
				location, identity))
			return nullptr;
		finishSpawn->call(gameplayStatics, finishParams.data());
		return uevr::API::UObjectHook::exists(actor) ? actor : nullptr;
	}

	bool ResolveNativeMolotovExplosionSelection(uevr::API::UObject* worldContext,
		uevr::API::UClass* spawnLibraryClass, uevr::API::UClass*& actorClass,
		uevr::API::UObject*& debrisTemplate, uint8_t& explosionType)
	{
		actorClass = nullptr;
		debrisTemplate = nullptr;
		explosionType = 0;
		if (worldContext == nullptr || spawnLibraryClass == nullptr)
			return false;
		ScopedMotionThrowableNativeProbeSuppression suppressProbe;
		auto* spawnLibrary = spawnLibraryClass->get_class_default_object();
		auto* selector = spawnLibraryClass->find_function(L"Get Explosion to Spawn");
		if (spawnLibrary == nullptr || selector == nullptr)
			return false;

		// Type is a raw byte in this cooked Blueprint, so its DE enum labels are
		// unavailable through reflection. The function itself is a selector: query
		// its small explosion range once and make the returned assets authoritative.
		// A generic actor plus a Molotov debris template is a valid native Molotov
		// selection; the prior implementation incorrectly rejected that combination.
		constexpr uint8_t FirstExplosionType = 0;
		constexpr uint8_t LastExplosionType = 31;
		const auto hasMolotovName = [](const std::wstring& name) {
			return name.find(L"Molotov") != std::wstring::npos
				|| name.find(L"molotov") != std::wstring::npos;
		};

		uevr::API::UClass* classOnlyFallback = nullptr;
		uevr::API::UObject* classOnlyFallbackDebris = nullptr;
		uint8_t classOnlyFallbackType = 0;
		for (uint16_t candidate = FirstExplosionType;
			candidate <= LastExplosionType; ++candidate)
		{
			std::vector<uint8_t> params(selector->get_properties_size());
			if (!SetReflectedByteParameter(selector, params, L"Type",
				static_cast<uint8_t>(candidate))
				|| !SetReflectedByteParameter(selector, params, L"Surface", 0)
				|| !SetReflectedObjectParameter(selector, params, L"ExplodingActor", nullptr)
				|| !SetReflectedObjectParameter(selector, params, L"__WorldContext", worldContext))
				return false;
			selector->call(spawnLibrary, params.data());

			uevr::API::UObject* selectedActor = nullptr;
			uevr::API::UObject* selectedDebris = nullptr;
			if (!ReadReflectedObjectParameter(selector, params, L"Actor", selectedActor)
				|| !ReadReflectedObjectParameter(selector, params, L"DebrisParticle", selectedDebris)
				|| selectedActor == nullptr || !uevr::API::UObjectHook::exists(selectedActor))
				continue;

			auto* selectedClass = selectedActor->dcast<uevr::API::UClass>();
			if (selectedClass == nullptr)
				continue;
			const bool debrisValid = selectedDebris != nullptr
				&& uevr::API::UObjectHook::exists(selectedDebris);
			const auto className = selectedClass->get_full_name();
			const auto debrisName = debrisValid ? selectedDebris->get_full_name()
				: std::wstring(L"<null>");
			const bool classIsMolotov = hasMolotovName(className);
			const bool debrisIsMolotov = debrisValid && hasMolotovName(debrisName);
			uevr::API::get()->log_info(
				"[MotionThrowableNativeMolotov] selector candidate type=%u class=%ls debris=%ls",
				static_cast<unsigned int>(candidate), className.c_str(), debrisName.c_str());
			if (classIsMolotov || debrisIsMolotov)
			{
				uevr::API::get()->log_info(
					"[MotionThrowableNativeMolotov] selector candidate type=%u class=%p name=%ls debris=%p debrisName=%ls match=%s",
					static_cast<unsigned int>(candidate), selectedClass, className.c_str(),
					selectedDebris, debrisName.c_str(), debrisIsMolotov ? "debris" : "class");
				if (debrisIsMolotov || (classIsMolotov && debrisValid))
				{
					actorClass = selectedClass;
					debrisTemplate = selectedDebris;
					explosionType = static_cast<uint8_t>(candidate);
					return true;
				}
				if (classOnlyFallback == nullptr)
				{
					classOnlyFallback = selectedClass;
					classOnlyFallbackDebris = debrisValid ? selectedDebris : nullptr;
					classOnlyFallbackType = static_cast<uint8_t>(candidate);
				}
			}
		}

		if (classOnlyFallback != nullptr)
		{
			actorClass = classOnlyFallback;
			debrisTemplate = classOnlyFallbackDebris;
			explosionType = classOnlyFallbackType;
			uevr::API::get()->log_warn(
				"[MotionThrowableNativeMolotov] selector using class-only fallback type=%u class=%ls debris=%p",
				static_cast<unsigned int>(classOnlyFallbackType),
				classOnlyFallback->get_full_name().c_str(), classOnlyFallbackDebris);
			return true;
		}
		return false;
	}

	bool SetupNativeSpawnedExplosion(uevr::API::UObject* worldContext,
		uevr::API::UClass* spawnLibraryClass, uevr::API::UObject* explosion,
		uevr::API::UObject* debrisTemplate)
	{
		if (worldContext == nullptr || spawnLibraryClass == nullptr || explosion == nullptr)
			return false;
		ScopedMotionThrowableNativeProbeSuppression suppressProbe;
		auto* spawnLibrary = spawnLibraryClass->get_class_default_object();
		auto* setup = spawnLibraryClass->find_function(L"SetupSpawnedExplosion");
		if (spawnLibrary == nullptr || setup == nullptr)
			return false;
		std::vector<uint8_t> params(setup->get_properties_size());
		// Collision contacts in this plugin identify the DE-native CEntity, not a
		// reflected Unreal actor. Passing it through an ObjectProperty would be
		// unsafe; null is the native world-impact case and the Blueprint handles it.
		if (!SetReflectedObjectParameter(setup, params, L"Explosion", explosion)
			|| !SetReflectedObjectParameter(setup, params, L"ExplodingActor", nullptr)
			|| !SetReflectedObjectParameter(setup, params, L"DebrisTemplate", debrisTemplate)
			|| !SetReflectedBoolParameter(setup, params, L"bSuppressLight", false)
			|| !SetReflectedObjectParameter(setup, params, L"__WorldContext", worldContext))
			return false;
		setup->call(spawnLibrary, params.data());
		return uevr::API::UObjectHook::exists(explosion);
	}

	void LogBlueprintDeclaredMembers(uevr::API::UClass* blueprintClass, const char* tag)
	{
		if (blueprintClass == nullptr || tag == nullptr)
			return;
		// Only inspect the Blueprint's own declarations. Its engine parent has a
		// large generic member list that would obscure the activation entry point.
		size_t functionCount = 0;
		for (auto* field = blueprintClass->get_children(); field != nullptr
			&& functionCount < 32; field = field->get_next())
		{
			auto* function = field->dcast<uevr::API::UFunction>();
			if (function == nullptr || function->get_fname() == nullptr)
				continue;
			uevr::API::get()->log_info("[%s] declared-function=%ls flags=0x%X params=%d",
				tag, function->get_fname()->to_string().c_str(),
				function->get_function_flags(), function->get_properties_size());
			size_t parameterCount = 0;
			for (auto* parameter = function->get_child_properties(); parameter != nullptr
				&& parameterCount < 32; parameter = parameter->get_next())
			{
				if (parameter->get_fname() == nullptr)
					continue;
				auto* reflectedParameter = static_cast<uevr::API::FProperty*>(parameter);
				const auto* parameterClass = parameter->get_class();
				const auto parameterClassName = parameterClass != nullptr
					&& parameterClass->get_fname() != nullptr
					? parameterClass->get_fname()->to_string() : L"<null>";
				std::wstring structName;
				if (parameterClassName == L"StructProperty")
				{
					auto* structProperty = reinterpret_cast<uevr::API::FStructProperty*>(parameter);
					if (auto* scriptStruct = structProperty->get_struct(); scriptStruct != nullptr
						&& scriptStruct->get_fname() != nullptr)
						structName = scriptStruct->get_fname()->to_string();
				}
				uevr::API::get()->log_info(
					"[%s] function=%ls parameter=%ls type=%ls struct=%ls offset=%d param=%d out=%d return=%d ref=%d",
					tag, function->get_fname()->to_string().c_str(),
					parameter->get_fname()->to_string().c_str(), parameterClassName.c_str(),
					structName.empty() ? L"<none>" : structName.c_str(),
					reflectedParameter->get_offset(), reflectedParameter->is_param() ? 1 : 0,
					reflectedParameter->is_out_param() ? 1 : 0,
					reflectedParameter->is_return_param() ? 1 : 0,
					reflectedParameter->is_reference_param() ? 1 : 0);
				++parameterCount;
			}
			++functionCount;
		}
		size_t propertyCount = 0;
		for (auto* property = blueprintClass->get_child_properties(); property != nullptr
			&& propertyCount < 32; property = property->get_next())
		{
			if (property->get_fname() == nullptr)
				continue;
			const auto* propertyClass = property->get_class();
			uevr::API::get()->log_info("[%s] declared-property=%ls class=%ls offset=%d",
				tag, property->get_fname()->to_string().c_str(),
				propertyClass != nullptr && propertyClass->get_fname() != nullptr
					? propertyClass->get_fname()->to_string().c_str() : L"<null>",
				static_cast<uevr::API::FProperty*>(property)->get_offset());
			++propertyCount;
		}
		uevr::API::get()->log_info("[%s] inventory functions=%zu properties=%zu",
			tag, functionCount, propertyCount);
	}

	void LogRelevantClassHierarchyMembers(uevr::API::UClass* blueprintClass,
		const char* tag)
	{
		if (blueprintClass == nullptr || tag == nullptr)
			return;
		const auto isRelevant = [](const std::wstring& name) {
			static constexpr std::array<const wchar_t*, 12> Terms{
				L"Fire", L"Burn", L"Ignit", L"Explosion", L"Damage", L"Life",
				L"Start", L"Attach", L"Creator", L"Extinguish", L"Kill", L"System" };
			for (const auto* term : Terms)
			{
				if (name.find(term) != std::wstring::npos)
					return true;
			}
			return false;
		};

		size_t loggedFunctions = 0;
		size_t loggedProperties = 0;
		int depth = 0;
		for (auto* type = static_cast<uevr::API::UStruct*>(blueprintClass);
			type != nullptr && depth < 8; type = type->get_super(), ++depth)
		{
			const auto typeName = type->get_full_name();
			for (auto* field = type->get_children(); field != nullptr;
				field = field->get_next())
			{
				auto* function = field->dcast<uevr::API::UFunction>();
				if (function == nullptr || function->get_fname() == nullptr)
					continue;
				const auto functionName = function->get_fname()->to_string();
				if (!isRelevant(functionName))
					continue;
				uevr::API::get()->log_info(
					"[%s] hierarchy-function depth=%d owner=%ls name=%ls flags=0x%X params=%d",
					tag, depth, typeName.c_str(), functionName.c_str(),
					function->get_function_flags(), function->get_properties_size());
				for (auto* parameter = function->get_child_properties(); parameter != nullptr;
					parameter = parameter->get_next())
				{
					if (parameter->get_fname() == nullptr)
						continue;
					auto* reflectedParameter = static_cast<uevr::API::FProperty*>(parameter);
					const auto* parameterClass = parameter->get_class();
					const auto parameterType = parameterClass != nullptr
						&& parameterClass->get_fname() != nullptr
						? parameterClass->get_fname()->to_string() : L"<null>";
					uevr::API::get()->log_info(
						"[%s] hierarchy-parameter function=%ls name=%ls type=%ls offset=%d param=%d out=%d return=%d",
						tag, functionName.c_str(),
						parameter->get_fname()->to_string().c_str(), parameterType.c_str(),
						reflectedParameter->get_offset(), reflectedParameter->is_param() ? 1 : 0,
						reflectedParameter->is_out_param() ? 1 : 0,
						reflectedParameter->is_return_param() ? 1 : 0);
				}
				++loggedFunctions;
			}
			for (auto* property = type->get_child_properties(); property != nullptr;
				property = property->get_next())
			{
				if (property->get_fname() == nullptr)
					continue;
				const auto propertyName = property->get_fname()->to_string();
				if (!isRelevant(propertyName))
					continue;
				const auto* propertyClass = property->get_class();
				uevr::API::get()->log_info(
					"[%s] hierarchy-property depth=%d owner=%ls name=%ls type=%ls offset=%d",
					tag, depth, typeName.c_str(), propertyName.c_str(),
					propertyClass != nullptr && propertyClass->get_fname() != nullptr
						? propertyClass->get_fname()->to_string().c_str() : L"<null>",
					static_cast<uevr::API::FProperty*>(property)->get_offset());
				++loggedProperties;
			}
		}
		uevr::API::get()->log_info(
			"[%s] hierarchy-inventory relevantFunctions=%zu relevantProperties=%zu",
			tag, loggedFunctions, loggedProperties);
	}

	uevr::API::UObject* SpawnParticleSystemAtLocation(uevr::API::UObject* worldContext,
		uevr::API::UObject* particleSystem, const glm::fvec3& location,
		const glm::fvec3& scale = glm::fvec3(1.0f), bool autoDestroy = true)
	{
		if (worldContext == nullptr || particleSystem == nullptr || !IsFiniteVector(location))
			return nullptr;
		auto gameplayStaticsClass = uevr::API::get()->find_uobject<uevr::API::UClass>(
			L"Class /Script/Engine.GameplayStatics");
		auto gameplayStatics = gameplayStaticsClass != nullptr
			? gameplayStaticsClass->get_class_default_object() : nullptr;
		auto function = gameplayStaticsClass != nullptr
			? gameplayStaticsClass->find_function(L"SpawnEmitterAtLocation") : nullptr;
		if (gameplayStatics == nullptr || function == nullptr)
			return nullptr;

		std::vector<uint8_t> params(function->get_properties_size());
		if (!SetReflectedObjectParameter(function, params, L"WorldContextObject", worldContext)
			|| !SetReflectedObjectParameter(function, params, L"EmitterTemplate", particleSystem)
			|| !SetReflectedVectorParameter(function, params, L"Location", location))
			return nullptr;
		// SpawnEmitterAtLocation has a Rotation input on this build. Leaving the
		// zeroed FRotator untouched can produce a valid call with an invalid
		// transform, so explicitly provide the identity rotator when reflected.
		if (FindFunctionParameter(function, L"Rotation") != nullptr
			&& !SetReflectedVectorParameter(function, params, L"Rotation",
				glm::fvec3(0.0f)))
			return nullptr;
		if (FindFunctionParameter(function, L"Scale") != nullptr
			&& !SetReflectedVectorParameter(function, params, L"Scale", scale))
			return nullptr;
		if (FindFunctionParameter(function, L"bAutoDestroy") != nullptr
			&& !SetReflectedBoolParameter(function, params, L"bAutoDestroy", autoDestroy))
			return nullptr;
		if (FindFunctionParameter(function, L"bAutoActivate") != nullptr
			&& !SetReflectedBoolParameter(function, params, L"bAutoActivate", true))
			return nullptr;
		function->call(gameplayStatics, params.data());
		uevr::API::UObject* spawnedComponent = nullptr;
		if (!ReadReflectedObjectReturn(function, params, spawnedComponent)
			|| spawnedComponent == nullptr
			|| !uevr::API::UObjectHook::exists(spawnedComponent))
			return nullptr;
		return spawnedComponent;
	}

	bool PlaySoundWaveAtLocation(uevr::API::UObject* worldContext,
		uevr::API::UObject* sound, const glm::fvec3& location,
		float volume, float pitch)
	{
		if (worldContext == nullptr || sound == nullptr || !IsFiniteVector(location))
			return false;
		auto gameplayStaticsClass = uevr::API::get()->find_uobject<uevr::API::UClass>(
			L"Class /Script/Engine.GameplayStatics");
		auto gameplayStatics = gameplayStaticsClass != nullptr
			? gameplayStaticsClass->get_class_default_object() : nullptr;
		auto function = gameplayStaticsClass != nullptr
			? gameplayStaticsClass->find_function(L"PlaySoundAtLocation") : nullptr;
		if (gameplayStatics == nullptr || function == nullptr)
			return false;

		std::vector<uint8_t> params(function->get_properties_size());
		if (!SetReflectedObjectParameter(function, params, L"WorldContextObject", worldContext)
			|| !SetReflectedObjectParameter(function, params, L"Sound", sound)
			|| !SetReflectedVectorParameter(function, params, L"Location", location)
			|| !SetReflectedFloatParameter(function, params, L"VolumeMultiplier", volume)
			|| !SetReflectedFloatParameter(function, params, L"PitchMultiplier", pitch)
			|| !SetReflectedFloatParameter(function, params, L"StartTime", 0.0f))
			return false;
		function->call(gameplayStatics, params.data());
		return true;
	}

	bool CallSetStaticMesh(uevr::API::UObject* component, uevr::API::UObject* staticMesh)
	{
		if (component == nullptr || staticMesh == nullptr || component->get_class() == nullptr)
			return false;
		auto function = component->get_class()->find_function(L"SetStaticMesh");
		if (function == nullptr)
			return false;
		std::vector<uint8_t> params(function->get_properties_size());
		if (!SetReflectedObjectParameter(function, params, L"NewMesh", staticMesh))
			return false;
		function->call(component, params.data());
		return true;
	}

	uevr::API::UObject* ReadComponentMaterial(uevr::API::UObject* component, int32_t elementIndex)
	{
		if (component == nullptr || component->get_class() == nullptr)
			return nullptr;
		auto function = component->get_class()->find_function(L"GetMaterial");
		if (function == nullptr)
			return nullptr;
		std::vector<uint8_t> params(function->get_properties_size());
		if (!SetReflectedIntParameter(function, params, L"ElementIndex", elementIndex))
			return nullptr;
		function->call(component, params.data());
		uevr::API::UObject* material = nullptr;
		return ReadReflectedObjectReturn(function, params, material) ? material : nullptr;
	}

	bool SetComponentMaterial(uevr::API::UObject* component, int32_t elementIndex,
		uevr::API::UObject* material)
	{
		if (component == nullptr || material == nullptr || component->get_class() == nullptr)
			return false;
		auto function = component->get_class()->find_function(L"SetMaterial");
		if (function == nullptr)
			return false;
		std::vector<uint8_t> params(function->get_properties_size());
		if (!SetReflectedIntParameter(function, params, L"ElementIndex", elementIndex)
			|| !SetReflectedObjectParameter(function, params, L"Material", material))
			return false;
		function->call(component, params.data());
		return true;
	}

	uevr::API::UObject* AddSkeletalComponent(uevr::API::UObject* actor, uevr::API::UClass* componentClass)
	{
		if (actor == nullptr || componentClass == nullptr)
			return nullptr;
		return uevr::API::get()->add_component_by_class(actor, componentClass, false);
	}

	bool ReadBoneTransform(uevr::API::UObject* component,
		const uevr::API::FName& boneName, glm::fvec3& translation,
		glm::fquat& rotation, uint8_t transformSpace);

	bool AttachStaticHandToDriver(uevr::API::UObject* staticHand,
		uevr::API::UObject* driver, const uevr::API::FName& driverHandBone)
	{
		if (staticHand == nullptr || driver == nullptr
			|| staticHand->get_class() == nullptr || driverHandBone.comparison_index == 0)
			return false;
		glm::fvec3 handPosition{};
		glm::fquat handRotation{};
		if (!ReadBoneTransform(driver, driverHandBone, handPosition, handRotation, 2))
			return false;
		auto attachFunction = staticHand->get_class()->find_function(L"AttachToComponent");
		if (attachFunction == nullptr)
			attachFunction = staticHand->get_class()->find_function(L"K2_AttachToComponent");
		if (attachFunction == nullptr)
			return false;
		std::vector<uint8_t> attachParams(attachFunction->get_properties_size());
		if (!SetReflectedObjectParameter(attachFunction, attachParams, L"Parent", driver)
			|| !SetReflectedFNameParameter(attachFunction, attachParams, L"SocketName", uevr::API::FName{})
			|| !SetReflectedByteParameter(attachFunction, attachParams, L"LocationRule", 0)
			|| !SetReflectedByteParameter(attachFunction, attachParams, L"RotationRule", 0)
			|| !SetReflectedByteParameter(attachFunction, attachParams, L"ScaleRule", 0))
			return false;
		SetReflectedBoolParameter(attachFunction, attachParams, L"bWeldSimulatedBodies", false);
		SetReflectedBoolParameter(attachFunction, attachParams, L"WeldSimulatedBodies", false);
		attachFunction->call(staticHand, attachParams.data());

		if (Utilities::KismetMathLibrary == nullptr)
			Utilities::InitHelperClasses();
		if (Utilities::KismetMathLibrary == nullptr)
			return false;
		const glm::fvec3 forward = NormalizeOrZero(handRotation * glm::fvec3(1.0f, 0.0f, 0.0f));
		const glm::fvec3 right = NormalizeOrZero(handRotation * glm::fvec3(0.0f, 1.0f, 0.0f));
		const glm::fvec3 up = NormalizeOrZero(handRotation * glm::fvec3(0.0f, 0.0f, 1.0f));
		ParameterMakeRotationFromAxes rotationParams{};
		rotationParams.forward = forward;
		rotationParams.right = right;
		rotationParams.up = up;
		Utilities::KismetMathLibrary->call_function(L"MakeRotationFromAxes", &rotationParams);
		Utilities::Parameter_K2_SetWorldOrRelativeLocation locationParams{};
		locationParams.newLocation = handPosition;
		locationParams.bTeleport = true;
		staticHand->call_function(L"K2_SetRelativeLocation", &locationParams);
		Utilities::Parameter_K2_SetWorldOrRelativeRotation relativeRotationParams{};
		relativeRotationParams.newRotation = rotationParams.returnValue;
		relativeRotationParams.bTeleport = true;
		staticHand->call_function(L"K2_SetRelativeRotation", &relativeRotationParams);
		return true;
	}

	bool SetComponentTickEnabled(uevr::API::UObject* component, bool enabled)
	{
		if (component == nullptr || component->get_class() == nullptr)
			return false;
		auto function = component->get_class()->find_function(L"SetComponentTickEnabled");
		if (function == nullptr)
			return false;
		std::vector<uint8_t> params(function->get_properties_size());
		if (!SetReflectedBoolParameter(function, params, L"bEnabled", enabled))
			return false;
		function->call(component, params.data());
		return true;
	}

	bool SetBoneTransformInSpace(uevr::API::UObject* component, const uevr::API::FName& boneName,
		const glm::fvec3& position, const glm::fquat& rotation,
		const glm::fvec3& scale, uint8_t boneSpace)
	{
		if (component == nullptr || component->get_class() == nullptr)
			return false;
		auto function = component->get_class()->find_function(L"SetBoneTransformByName");
		if (function == nullptr)
			return false;
		std::vector<uint8_t> params(function->get_properties_size());
		const bool transformSet = SetReflectedTransformParameter(function, params,
			L"InTransform", position, rotation, scale)
			|| SetReflectedTransformParameter(function, params,
				L"Transform", position, rotation, scale);
		const bool spaceSet = SetReflectedByteParameter(function, params, L"BoneSpace", boneSpace);
		if (!SetReflectedFNameParameter(function, params, L"BoneName", boneName)
			|| !transformSet || !spaceSet)
			return false;
		function->call(component, params.data());
		return true;
	}

	bool SetBoneComponentTransform(uevr::API::UObject* component, const uevr::API::FName& boneName,
		const glm::fvec3& position, const glm::fquat& rotation,
		const glm::fvec3& scale = glm::fvec3(1.0f))
	{
		return SetBoneTransformInSpace(component, boneName, position, rotation, scale, 1);
	}

	bool SetBoneWorldTransform(uevr::API::UObject* component, const uevr::API::FName& boneName,
		const glm::fvec3& position, const glm::fquat& rotation)
	{
		return SetBoneTransformInSpace(component, boneName, position, rotation,
			glm::fvec3(1.0f), 0);
	}

	bool HideBone(uevr::API::UObject* component, const uevr::API::FName& boneName)
	{
		if (component == nullptr || component->get_class()->find_function(L"HideBoneByName") == nullptr)
			return false;
		auto function = component->get_class()->find_function(L"HideBoneByName");
		std::vector<uint8_t> params(function->get_properties_size());
		// UE4's reflected parameter is PhysBodyOption.  Keep the older alias as a
		// fallback for engine variants, but do not reject a valid UE4.26 call by
		// looking only for the abbreviated C++-side name.
		const bool bodyOptionSet = SetReflectedByteParameter(function, params, L"PhysBodyOption", 0)
			|| SetReflectedByteParameter(function, params, L"PhysBodyOp", 0);
		return SetReflectedFNameParameter(function, params, L"BoneName", boneName)
			&& bodyOptionSet
			&& (function->call(component, params.data()), true);
	}

	bool UnhideBone(uevr::API::UObject* component, const uevr::API::FName& boneName)
	{
		if (component == nullptr)
			return false;
		auto klass = component->get_class();
		if (klass == nullptr)
			return false;
		auto function = klass->find_function(L"UnHideBoneByName");
		if (function == nullptr)
			return false;
		std::vector<uint8_t> params(function->get_properties_size());
		if (!SetReflectedFNameParameter(function, params, L"BoneName", boneName))
			return false;
		function->call(component, params.data());
		return true;
	}

	bool TryReadBoneHiddenByName(uevr::API::UObject* component, const uevr::API::FName& boneName, bool& hidden)
	{
		hidden = false;
		if (component == nullptr)
			return false;
		auto function = component->get_class()->find_function(L"IsBoneHiddenByName");
		if (function == nullptr)
			return false;
		std::vector<uint8_t> params(function->get_properties_size());
		if (!SetReflectedFNameParameter(function, params, L"BoneName", boneName))
			return false;
		function->call(component, params.data());
		auto returnProperty = FindFunctionParameter(function, L"ReturnValue");
		if (returnProperty == nullptr || returnProperty->get_offset() < 0
			|| returnProperty->get_class() == nullptr
			|| returnProperty->get_class()->get_fname()->to_string() != L"BoolProperty")
			return false;
		auto boolProperty = reinterpret_cast<uevr::API::FBoolProperty*>(returnProperty);
		const size_t byteIndex = static_cast<size_t>(returnProperty->get_offset())
			+ boolProperty->get_byte_offset();
		if (byteIndex >= params.size())
			return false;
		hidden = (params[byteIndex] & static_cast<uint8_t>(boolProperty->get_byte_mask())) != 0;
		return true;
	}

	bool ReadBoneCount(uevr::API::UObject* component, int32_t& boneCount)
	{
		boneCount = -1;
		if (component == nullptr)
			return false;
		auto function = component->get_class()->find_function(L"GetNumBones");
		if (function == nullptr)
			return false;
		std::vector<uint8_t> params(function->get_properties_size());
		function->call(component, params.data());
		auto returnProperty = FindFunctionParameter(function, L"ReturnValue");
		if (returnProperty == nullptr || returnProperty->get_offset() < 0
			|| static_cast<size_t>(returnProperty->get_offset()) + sizeof(boneCount) > params.size())
			return false;
		std::memcpy(&boneCount, params.data() + returnProperty->get_offset(), sizeof(boneCount));
		return true;
	}

	struct ResolvedBone
	{
		int32_t index = -1;
		uevr::API::FName name{};
		std::wstring text;
	};

	bool ReadBoneNameAtIndex(uevr::API::UObject* component, int32_t boneIndex,
		std::wstring& boneName, uevr::API::FName* rawName = nullptr)
	{
		boneName.clear();
		if (component == nullptr)
			return false;
		auto function = component->get_class()->find_function(L"GetBoneName");
		if (function == nullptr)
			return false;
		std::vector<uint8_t> params(function->get_properties_size());
		if (!SetReflectedIntParameter(function, params, L"BoneIndex", boneIndex))
			return false;
		function->call(component, params.data());
		auto returnProperty = FindFunctionParameter(function, L"ReturnValue");
		if (returnProperty == nullptr || returnProperty->get_offset() < 0
			|| static_cast<size_t>(returnProperty->get_offset()) + sizeof(uevr::API::FName) > params.size())
			return false;
		uevr::API::FName result{};
		std::memcpy(&result, params.data() + returnProperty->get_offset(), sizeof(result));
		boneName = result.to_string();
		if (rawName != nullptr)
			*rawName = result;
		return !boneName.empty();
	}

	bool ResolveBone(uevr::API::UObject* component, const wchar_t* wantedName, ResolvedBone& result)
	{
		result = ResolvedBone{};
		int32_t boneCount = -1;
		if (!ReadBoneCount(component, boneCount) || boneCount <= 0)
			return false;
		const int32_t cappedCount = (std::min)(boneCount, 256);
		for (int32_t index = 0; index < cappedCount; ++index)
		{
			std::wstring name;
			uevr::API::FName rawName{};
			if (!ReadBoneNameAtIndex(component, index, name, &rawName))
				continue;
			if (name == wantedName)
			{
				result.index = index;
				result.name = rawName;
				result.text = std::move(name);
				return true;
			}
		}
		return false;
	}

	bool ResolveFirstBone(uevr::API::UObject* component,
		std::initializer_list<const wchar_t*> wantedNames, ResolvedBone& result)
	{
		for (const auto* wantedName : wantedNames)
		{
			if (ResolveBone(component, wantedName, result))
				return true;
		}
		result = ResolvedBone{};
		return false;
	}

	void LogHandSkeletalInventory(const char* label, uevr::API::UObject* component)
	{
		if (component == nullptr || component->get_class() == nullptr)
		{
			uevr::API::get()->log_warn("[FreeAimHands] inventory label=%s component=null", label);
			return;
		}
		auto skeletalMesh = component->get_property<uevr::API::UObject*>(L"SkeletalMesh");
		int32_t boneCount = -1;
		const bool countRead = ReadBoneCount(component, boneCount);
		const bool nameFunctionAvailable = component->get_class()->find_function(L"GetBoneName") != nullptr;
		uevr::API::get()->log_info("[FreeAimHands] inventory label=%s class=%ls object=%ls asset=%ls countRead=%d count=%d getBoneName=%d",
			label, component->get_class()->get_full_name().c_str(), component->get_full_name().c_str(),
			skeletalMesh != nullptr ? skeletalMesh->get_full_name().c_str() : L"null",
			countRead, boneCount, nameFunctionAvailable);
		if (!countRead || boneCount <= 0 || !nameFunctionAvailable)
			return;
		const int32_t cappedCount = (std::min)(boneCount, 256);
		for (int32_t index = 0; index < cappedCount; ++index)
		{
			std::wstring name;
			if (!ReadBoneNameAtIndex(component, index, name))
				continue;
			const bool relevant = name.find(L"Hand") != std::wstring::npos
				|| name.find(L"hand") != std::wstring::npos
				|| name.find(L"Arm") != std::wstring::npos
				|| name.find(L"arm") != std::wstring::npos
				|| name.find(L"Wrist") != std::wstring::npos
				|| name.find(L"wrist") != std::wstring::npos
				|| name.find(L"Clav") != std::wstring::npos
				|| name.find(L"clav") != std::wstring::npos;
			if (index < 12 || relevant)
				uevr::API::get()->log_info("[FreeAimHands] inventory label=%s bone[%d]=%ls", label, index, name.c_str());
		}
	}

	bool ReadBoneTransform(uevr::API::UObject* component, const uevr::API::FName& boneName,
		glm::fvec3& translation, glm::fquat& rotation, uint8_t transformSpace = 2)
	{
		if (component == nullptr)
			return false;
		const auto readTransformReturn = [&](uevr::API::UFunction* function,
			const std::vector<uint8_t>& params) -> bool
		{
			auto returnProperty = FindFunctionParameter(function, L"ReturnValue");
			const auto returnClassName = returnProperty != nullptr && returnProperty->get_class() != nullptr
				? returnProperty->get_class()->get_fname()->to_string() : L"";
			auto structProperty = returnProperty != nullptr
				? reinterpret_cast<uevr::API::FStructProperty*>(returnProperty) : nullptr;
			if (returnClassName != L"StructProperty" || returnProperty->get_offset() < 0
				|| structProperty->get_struct() == nullptr || structProperty->get_struct()->get_struct_size() < 40)
				return false;
			std::array<float, 12> raw{};
			std::memcpy(raw.data(), params.data() + returnProperty->get_offset(),
				(std::min)(sizeof(raw), static_cast<size_t>(structProperty->get_struct()->get_struct_size())));
			rotation = glm::normalize(glm::fquat::wxyz(raw[3], raw[0], raw[1], raw[2]));
			translation = glm::fvec3(raw[4], raw[5], raw[6]);
			return IsFiniteVector(translation) && IsFiniteQuaternion(rotation)
				&& glm::length(rotation) > 0.5f && glm::length(translation) > 0.01f;
		};

		// GetSocketTransform is Blueprint-exposed in UE4 and accepts bone names as
		// well as sockets.  This is more reliable through UEVR reflection than the
		// native GetBoneTransform overload used by the original prototype.
		auto socketFunction = component->get_class()->find_function(L"GetSocketTransform");
		if (socketFunction != nullptr)
		{
			std::vector<uint8_t> socketParams(socketFunction->get_properties_size());
			const bool nameSet = SetReflectedFNameParameter(socketFunction, socketParams, L"InSocketName", boneName)
				|| SetReflectedFNameParameter(socketFunction, socketParams, L"SocketName", boneName);
			const bool spaceSet = SetReflectedByteParameter(socketFunction, socketParams, L"TransformSpace", transformSpace)
				|| SetReflectedByteParameter(socketFunction, socketParams, L"Space", transformSpace);
			if (nameSet && spaceSet)
			{
				socketFunction->call(component, socketParams.data());
				if (readTransformReturn(socketFunction, socketParams))
					return true;
			}
		}

		return false;
	}

	bool ReadBoneWorldTransform(uevr::API::UObject* component, const uevr::API::FName& boneName,
		glm::fvec3& translation, glm::fquat& rotation)
	{
		return ReadBoneTransform(component, boneName, translation, rotation, 0);
	}

	struct CapturedGripBone
	{
		const wchar_t* name = L"";
		int parent = -1;
		glm::fvec3 position{};
		glm::fquat rotation{};
	};

	const std::array<CapturedGripBone, 6>& CapturedVehicleGripBones(int hand)
	{
		// Settled native steering pose captured from GTAPoseableComponent hands on
		// 2026-08-12.  Positions/rotations are component-space source data.  We
		// derive same-rig parent-local relations below, so existing controller and
		// per-weapon calibration remain authoritative for final placement.
		static const std::array<CapturedGripBone, 6> left = {{
			{ L"L_Hand", -1, {-19.046440f, -30.546392f, 36.806976f}, glm::fquat::wxyz(-0.2643142f, -0.6824937f, 0.6778587f, 0.0696287f) },
			{ L"LThumb1", 0, {-13.812113f, -31.628971f, 40.731544f}, glm::fquat::wxyz(-0.5455648f, 0.3662382f, 0.2162129f, -0.7221363f) },
			{ L"LThumb2", 1, {-12.939327f, -36.854279f, 42.525272f}, glm::fquat::wxyz(0.4331086f, -0.7964645f, 0.2601637f, 0.3322292f) },
			{ L"L_Finger", 0, {-18.466402f, -38.444386f, 38.967026f}, glm::fquat::wxyz(-0.1797713f, -0.9451519f, 0.1791810f, 0.2055831f) },
			{ L"L_Finger01", 3, {-14.928052f, -40.149456f, 37.623699f}, glm::fquat::wxyz(-0.0539415f, -0.9093494f, -0.3138497f, 0.2677172f) },
			{ L"L_Finger0Nub", 4, {-12.461994f, -38.116325f, 35.665619f}, glm::fquat::wxyz(0.0539420f, 0.9093489f, 0.3138497f, -0.2677186f) }
		}};
		static const std::array<CapturedGripBone, 6> right = {{
			{ L"R_Hand", -1, {18.787901f, -30.801706f, 38.321873f}, glm::fquat::wxyz(0.5842338f, 0.0821629f, -0.2386883f, -0.7713288f) },
			{ L"RThumb1", 0, {14.200489f, -31.616087f, 41.868973f}, glm::fquat::wxyz(-0.2244088f, 0.8459628f, 0.4140857f, -0.2500416f) },
			{ L"RThumb2", 1, {11.102139f, -36.028500f, 43.290676f}, glm::fquat::wxyz(0.3739921f, -0.8132555f, -0.0945300f, -0.4356714f) },
			{ L"R_Finger", 0, {16.295494f, -38.518932f, 39.570023f}, glm::fquat::wxyz(-0.0095865f, 0.2109961f, -0.1385766f, -0.9675667f) },
			{ L"R_Finger01", 3, {12.520937f, -38.690578f, 37.867455f}, glm::fquat::wxyz(-0.4366697f, 0.2505321f, -0.0309272f, -0.8634796f) },
			{ L"R_Finger0Nub", 4, {10.661525f, -35.904903f, 36.138832f}, glm::fquat::wxyz(0.8634790f, -0.0309282f, -0.2505341f, -0.4366696f) }
		}};
		return hand == 0 ? left : right;
	}

	bool ApplyHandPoseFromComponentTransforms(uevr::API::UObject* component, int hand,
		const uevr::API::FName& handBoneName,
		const std::array<glm::fvec3, 6>& sourcePositions,
		const std::array<glm::fquat, 6>& sourceRotations)
	{
		if (component == nullptr || (hand != 0 && hand != 1)
			|| !SetComponentTickEnabled(component, false))
			return false;
		glm::fvec3 rootPosition{};
		glm::fquat rootRotation{};
		if (!ReadBoneTransform(component, handBoneName, rootPosition, rootRotation, 2))
			return false;
		const auto& source = CapturedVehicleGripBones(hand);
		std::array<glm::fvec3, 6> desiredPositions{};
		std::array<glm::fquat, 6> desiredRotations{};
		desiredPositions[0] = rootPosition;
		desiredRotations[0] = glm::normalize(rootRotation);
		for (size_t index = 1; index < source.size(); ++index)
		{
			const int parent = source[index].parent;
			if (parent < 0 || parent >= static_cast<int>(index))
				return false;
			const glm::fquat sourceParentRotation = glm::normalize(
				sourceRotations[static_cast<size_t>(parent)]);
			const glm::fquat localRotation = glm::normalize(
				glm::inverse(sourceParentRotation) * glm::normalize(sourceRotations[index]));
			const glm::fvec3 localPosition = glm::inverse(sourceParentRotation)
				* (sourcePositions[index] - sourcePositions[static_cast<size_t>(parent)]);
			desiredRotations[index] = glm::normalize(desiredRotations[static_cast<size_t>(parent)] * localRotation);
			desiredPositions[index] = desiredPositions[static_cast<size_t>(parent)]
				+ desiredRotations[static_cast<size_t>(parent)] * localPosition;
			ResolvedBone target;
			if (!ResolveBone(component, source[index].name, target)
				|| !SetBoneComponentTransform(component, target.name,
					desiredPositions[index], desiredRotations[index]))
				return false;
		}
		return true;
	}

	bool ApplyCapturedVehicleGripPose(uevr::API::UObject* component, int hand,
		const uevr::API::FName& handBoneName)
	{
		if (hand != 0 && hand != 1)
			return false;
		const auto& source = CapturedVehicleGripBones(hand);
		std::array<glm::fvec3, 6> sourcePositions{};
		std::array<glm::fquat, 6> sourceRotations{};
		for (size_t index = 0; index < source.size(); ++index)
		{
			sourcePositions[index] = source[index].position;
			sourceRotations[index] = source[index].rotation;
		}
		return ApplyHandPoseFromComponentTransforms(component, hand, handBoneName,
			sourcePositions, sourceRotations);
	}

	bool ApplyReferenceHandPose(uevr::API::UObject* component,
		uevr::API::UObject* referenceComponent, int hand,
		const uevr::API::FName& handBoneName)
	{
		if (component == nullptr || referenceComponent == nullptr
			|| (hand != 0 && hand != 1))
			return false;
		const auto& source = CapturedVehicleGripBones(hand);
		std::array<glm::fvec3, 6> sourcePositions{};
		std::array<glm::fquat, 6> sourceRotations{};
		for (size_t index = 0; index < source.size(); ++index)
		{
			ResolvedBone sourceBone;
			if (!ResolveBone(referenceComponent, source[index].name, sourceBone)
				|| !ReadBoneTransform(referenceComponent, sourceBone.name,
					sourcePositions[index], sourceRotations[index], 2))
				return false;
		}
		return ApplyHandPoseFromComponentTransforms(component, hand, handBoneName,
			sourcePositions, sourceRotations);
	}

	struct NativeFunctionFrame
	{
		void* vtable = nullptr;
		bool bool1 = false;
		bool bool2 = false;
		void* node = nullptr;
		void* object = nullptr;
		void* code = nullptr;
		void* locals = nullptr;
	};

	struct MotionThrowableNativeProbeHook
	{
		uevr::API::UFunction* function = nullptr;
		const char* label = nullptr;
	};

	std::array<MotionThrowableNativeProbeHook, 3> motionThrowableNativeProbeHooks{};
	std::atomic<uint32_t> motionThrowableNativeProbeHookCount{ 0 };
	std::atomic<uint32_t> motionThrowableNativeProbeEventCount{ 0 };
	bool motionThrowableNativeProbeHooksInitialized = false;

	template <typename TValue>
	bool ReadNativeFunctionFrameParameter(uevr::API::UFunction* function, void* locals,
		const wchar_t* name, TValue& value)
	{
		if (function == nullptr || locals == nullptr || name == nullptr)
			return false;
		auto* property = function->find_property(name);
		const int32_t offset = property != nullptr ? property->get_offset() : -1;
		const int32_t propertySize = function->get_properties_size();
		if (offset < 0 || propertySize < 0
			|| static_cast<size_t>(offset) + sizeof(TValue) > static_cast<size_t>(propertySize))
			return false;
		std::memcpy(&value, reinterpret_cast<const uint8_t*>(locals) + offset, sizeof(value));
		return true;
	}

	const char* MotionThrowableNativeProbeLabel(uevr::API::UFunction* function)
	{
		const uint32_t count = motionThrowableNativeProbeHookCount.load(std::memory_order_acquire);
		for (uint32_t index = 0; index < count && index < motionThrowableNativeProbeHooks.size(); ++index)
		{
			if (motionThrowableNativeProbeHooks[index].function == function)
				return motionThrowableNativeProbeHooks[index].label;
		}
		return "unknown";
	}

	bool MotionThrowableNativeMolotovProbePreHook(uevr::API::UFunction* function,
		uevr::API::UObject* object, void* framePointer, void*)
	{
		if (motionThrowableNativeProbeSuppressDepth != 0 || function == nullptr
			|| framePointer == nullptr)
			return true;

		const uint32_t event = motionThrowableNativeProbeEventCount.fetch_add(
			1, std::memory_order_acq_rel) + 1;
		// The source path can be called by broad explosion systems. Keep the probe
		// finite and event-level; a native Molotov trigger needs only one or two
		// records to identify whether this Blueprint route is the right owner.
		if (event > 32)
			return true;

		auto* frame = reinterpret_cast<NativeFunctionFrame*>(framePointer);
		if (frame->locals == nullptr)
			return true;

		uint8_t type = 0xFF;
		uint8_t surface = 0xFF;
		uevr::API::UObject* explosion = nullptr;
		uevr::API::UObject* explodingActor = nullptr;
		uevr::API::UObject* debris = nullptr;
		uevr::API::UObject* worldContext = nullptr;
		const bool hasType = ReadNativeFunctionFrameParameter(function, frame->locals,
			L"Type", type);
		const bool hasSurface = ReadNativeFunctionFrameParameter(function, frame->locals,
			L"Surface", surface);
		const bool hasExplosion = ReadNativeFunctionFrameParameter(function, frame->locals,
			L"Explosion", explosion);
		const bool hasExplodingActor = ReadNativeFunctionFrameParameter(function, frame->locals,
			L"ExplodingActor", explodingActor);
		const bool hasDebris = ReadNativeFunctionFrameParameter(function, frame->locals,
			L"DebrisTemplate", debris);
		const bool hasWorldContext = ReadNativeFunctionFrameParameter(function, frame->locals,
			L"__WorldContext", worldContext);
		const std::wstring objectName = object != nullptr && uevr::API::UObjectHook::exists(object)
			? object->get_full_name() : L"<invalid>";
		uevr::API::get()->log_info(
			"[NativeMolotovProbe] event=%u function=%s object=%p objectName=%ls type=%s%u surface=%s%u explosion=%s%p explodingActor=%s%p debris=%s%p world=%s%p",
			event, MotionThrowableNativeProbeLabel(function), object, objectName.c_str(),
			hasType ? "" : "n/a:", static_cast<unsigned int>(type),
			hasSurface ? "" : "n/a:", static_cast<unsigned int>(surface),
			hasExplosion ? "" : "n/a:", explosion,
			hasExplodingActor ? "" : "n/a:", explodingActor,
			hasDebris ? "" : "n/a:", debris,
			hasWorldContext ? "" : "n/a:", worldContext);
		return true;
	}

	void EnsureMotionThrowableNativeMolotovProbeHooks(uevr::API::UClass* spawnLibraryClass)
	{
		if (motionThrowableNativeProbeHooksInitialized || spawnLibraryClass == nullptr)
			return;

		const struct { const wchar_t* name; const char* label; } candidates[] = {
			{ L"Get Explosion to Spawn", "GetExplosionToSpawn" },
			{ L"DetermineExplosionTransform", "DetermineExplosionTransform" },
			{ L"SetupSpawnedExplosion", "SetupSpawnedExplosion" }
		};
		uint32_t installed = 0;
		for (const auto& candidate : candidates)
		{
			if (installed >= motionThrowableNativeProbeHooks.size())
				break;
			auto* function = spawnLibraryClass->find_function(candidate.name);
			if (function == nullptr)
				continue;
			auto& hook = motionThrowableNativeProbeHooks[installed];
			hook.function = function;
			hook.label = candidate.label;
			if (function->hook_ptr(MotionThrowableNativeMolotovProbePreHook, nullptr))
				++installed;
			else
			{
				hook = {};
				uevr::API::get()->log_warn(
					"[NativeMolotovProbe] hook install failed function=%s", candidate.label);
			}
		}
		motionThrowableNativeProbeHookCount.store(installed, std::memory_order_release);
		motionThrowableNativeProbeHooksInitialized = true;
		uevr::API::get()->log_info(
			"[NativeMolotovProbe] hook initialization installed=%u; custom selector/setup calls are suppressed",
			installed);
	}

	struct RawArray
	{
		void* data = nullptr;
		int32_t count = 0;
		int32_t capacity = 0;
	};

	struct BulletTraceMeshHook
	{
		uevr::API::UFunction* function = nullptr;
		void* originalNativeFunction = nullptr;
		int32_t verticesOffset = -1;
		int32_t vectorStride = 0;
		const char* name = nullptr;
	};

	std::array<BulletTraceMeshHook, 4> bulletTraceMeshHooks{};
	std::atomic<uint32_t> bulletTraceMeshHookCount{ 0 };
	std::atomic<uevr::API::UObject*> bulletTraceMeshComponent{ nullptr };
	std::atomic<uint32_t> bulletTraceMuzzleSequence{ 0 };
	std::array<std::atomic<uint32_t>, 3> bulletTraceMuzzleBits{};
	std::atomic<uint64_t> bulletTraceMuzzleTimestamp{ 0 };
	std::atomic<bool> bulletTraceMuzzleEligible{ false };
	std::atomic<bool> bulletTraceDebugEnabled{ false };
	bool bulletTraceHooksInitialized = false;
	bool bulletTraceNativeHooksInitialized = false;
	uint64_t lastBulletTraceComponentResolveTime = 0;
	uint32_t pendingBulletTraceCorrectionSequence = 0;
	uint32_t appliedBulletTraceCorrectionSequence = 0;
	glm::fvec3 pendingBulletTraceRawStartWorld{};
	glm::fvec3 pendingBulletTraceDeltaWorld{};
	glm::fvec3 bulletTraceComponentBaseline{};
	glm::fvec3 bulletTraceComponentLastApplied{};
	uint64_t bulletTraceCorrectionExpiresAt = 0;
	bool bulletTraceComponentCorrectionActive = false;

	glm::fvec3 ReadMeshVertex(const uint8_t* data, int32_t index, int32_t stride)
	{
		if (stride == 12)
		{
			glm::fvec3 value{};
			std::memcpy(&value, data + static_cast<size_t>(index) * stride, sizeof(value));
			return value;
		}

		const double* value = reinterpret_cast<const double*>(data + static_cast<size_t>(index) * stride);
		return glm::fvec3(static_cast<float>(value[0]), static_cast<float>(value[1]), static_cast<float>(value[2]));
	}

	void WriteMeshVertex(uint8_t* data, int32_t index, int32_t stride, const glm::fvec3& value)
	{
		if (stride == 12)
		{
			std::memcpy(data + static_cast<size_t>(index) * stride, &value, sizeof(value));
			return;
		}

		double* destination = reinterpret_cast<double*>(data + static_cast<size_t>(index) * stride);
		destination[0] = value.x;
		destination[1] = value.y;
		destination[2] = value.z;
	}

	bool ReadBulletTraceMuzzleSnapshot(glm::fvec3& muzzle)
	{
		if (!bulletTraceMuzzleEligible.load(std::memory_order_acquire))
			return false;
		const uint64_t publishedAt = bulletTraceMuzzleTimestamp.load(std::memory_order_acquire);
		const uint64_t now = GetTickCount64();
		if (publishedAt == 0 || now < publishedAt || now - publishedAt > 500)
			return false;

		for (int attempt = 0; attempt < 3; ++attempt)
		{
			const uint32_t sequenceBefore = bulletTraceMuzzleSequence.load(std::memory_order_acquire);
			if ((sequenceBefore & 1U) != 0)
				continue;

			std::array<uint32_t, 3> bits{};
			for (size_t i = 0; i < bits.size(); ++i)
				bits[i] = bulletTraceMuzzleBits[i].load(std::memory_order_relaxed);
			const uint32_t sequenceAfter = bulletTraceMuzzleSequence.load(std::memory_order_acquire);
			if (sequenceBefore != sequenceAfter || (sequenceAfter & 1U) != 0)
				continue;

			std::memcpy(&muzzle.x, &bits[0], sizeof(float));
			std::memcpy(&muzzle.y, &bits[1], sizeof(float));
			std::memcpy(&muzzle.z, &bits[2], sizeof(float));
			return IsFiniteVector(muzzle) && glm::length(muzzle) > 1.0f;
		}
		return false;
	}

	bool AdjustBulletTraceVertices(RawArray* vertices, int32_t vectorStride, const char* path)
	{
		if (vertices == nullptr || vertices->data == nullptr
			|| vertices->count < 2 || vertices->count > 4096
			|| vertices->capacity < vertices->count
			|| (vectorStride != 12 && vectorStride != 24))
			return false;

		glm::fvec3 muzzle{};
		if (!ReadBulletTraceMuzzleSnapshot(muzzle))
			return false;

		auto data = reinterpret_cast<uint8_t*>(vertices->data);
		float nearestDistance = std::numeric_limits<float>::infinity();
		float farthestDistance = 0.0f;
		for (int32_t i = 0; i < vertices->count; ++i)
		{
			const glm::fvec3 vertex = ReadMeshVertex(data, i, vectorStride);
			if (!IsFiniteVector(vertex))
				return false;
			const float distance = glm::length(vertex - muzzle);
			nearestDistance = (std::min)(nearestDistance, distance);
			farthestDistance = (std::max)(farthestDistance, distance);
		}

		// A native trace begins roughly at CJ's unmoved gun, normally within a
		// meter or two of the mock muzzle. Require a distinct far end so a very
		// short impact cannot have its entire quad translated.
		constexpr float maximumStartDistance = 250.0f;
		constexpr float startClusterTolerance = 8.0f;
		const bool geometryEligible = std::isfinite(nearestDistance)
			&& nearestDistance <= maximumStartDistance
			&& farthestDistance - nearestDistance >= 25.0f;
		int32_t selectedCount = 0;
		glm::fvec3 startCentroid(0.0f);
		if (geometryEligible)
		{
			for (int32_t i = 0; i < vertices->count; ++i)
			{
				const glm::fvec3 vertex = ReadMeshVertex(data, i, vectorStride);
				if (glm::length(vertex - muzzle) <= nearestDistance + startClusterTolerance)
				{
					startCentroid += vertex;
					++selectedCount;
				}
			}
		}

		bool modified = false;
		glm::fvec3 correction(0.0f);
		if (selectedCount > 0 && selectedCount <= 16 && selectedCount < vertices->count)
		{
			startCentroid /= static_cast<float>(selectedCount);
			correction = muzzle - startCentroid;
			if (IsFiniteVector(correction) && glm::length(correction) <= maximumStartDistance)
			{
				for (int32_t i = 0; i < vertices->count; ++i)
				{
					const glm::fvec3 vertex = ReadMeshVertex(data, i, vectorStride);
					if (glm::length(vertex - muzzle) <= nearestDistance + startClusterTolerance)
						WriteMeshVertex(data, i, vectorStride, vertex + correction);
				}
				modified = true;
			}
		}

		if (bulletTraceDebugEnabled.load(std::memory_order_relaxed))
		{
			const glm::fvec3 first = ReadMeshVertex(data, 0, vectorStride);
			uevr::API::get()->log_info(
				"[BulletTraceMeshHook] path=%s vertices=%d stride=%d nearest=%.3f farthest=%.3f selected=%d correction=(%.3f %.3f %.3f) first=(%.3f %.3f %.3f) modified=%s",
				path, vertices->count, vectorStride, nearestDistance, farthestDistance,
				selectedCount, correction.x, correction.y, correction.z,
				first.x, first.y, first.z, modified ? "true" : "false");
		}
		return modified;
	}

	bool BulletTraceMeshPreHook(uevr::API::UFunction* function, uevr::API::UObject* object,
		void* framePointer, void*)
	{
		if (object == nullptr || object != bulletTraceMeshComponent.load(std::memory_order_acquire)
			|| framePointer == nullptr)
			return true;

		const BulletTraceMeshHook* hook = nullptr;
		const uint32_t hookCount = bulletTraceMeshHookCount.load(std::memory_order_acquire);
		for (uint32_t i = 0; i < hookCount; ++i)
		{
			if (bulletTraceMeshHooks[i].function == function)
			{
				hook = &bulletTraceMeshHooks[i];
				break;
			}
		}
		if (hook == nullptr || hook->verticesOffset < 0
			|| (hook->vectorStride != 12 && hook->vectorStride != 24))
			return true;

		auto frame = reinterpret_cast<NativeFunctionFrame*>(framePointer);
		if (frame->locals == nullptr)
			return true;
		auto vertices = reinterpret_cast<RawArray*>(
			reinterpret_cast<uint8_t*>(frame->locals) + hook->verticesOffset);
		AdjustBulletTraceVertices(vertices, hook->vectorStride, hook->name);
		return true;
	}

	void PublishBulletTraceMuzzle(const glm::fvec3& muzzle, bool eligible, bool debug)
	{
		bulletTraceDebugEnabled.store(debug, std::memory_order_relaxed);
		bulletTraceMuzzleSequence.fetch_add(1, std::memory_order_acq_rel);
		if (eligible && IsFiniteVector(muzzle))
		{
			const float values[] = { muzzle.x, muzzle.y, muzzle.z };
			for (size_t i = 0; i < bulletTraceMuzzleBits.size(); ++i)
			{
				uint32_t bits = 0;
				std::memcpy(&bits, &values[i], sizeof(bits));
				bulletTraceMuzzleBits[i].store(bits, std::memory_order_relaxed);
			}
			bulletTraceMuzzleTimestamp.store(GetTickCount64(), std::memory_order_relaxed);
		}
		bulletTraceMuzzleEligible.store(eligible, std::memory_order_relaxed);
		bulletTraceMuzzleSequence.fetch_add(1, std::memory_order_release);
	}

	void QueueBulletTraceComponentCorrection(const MemoryManager::NativeShotTraceProbe& probe)
	{
		if (!probe.overridden)
			return;
		const glm::fvec3 rawStartWorld = {
			probe.rawStart[0] * 100.0f, -probe.rawStart[1] * 100.0f, probe.rawStart[2] * 100.0f
		};
		const glm::fvec3 appliedStartWorld = {
			probe.appliedStart[0] * 100.0f, -probe.appliedStart[1] * 100.0f, probe.appliedStart[2] * 100.0f
		};
		const glm::fvec3 deltaWorld = appliedStartWorld - rawStartWorld;
		const float deltaLength = glm::length(deltaWorld);
		if (!IsFiniteVector(rawStartWorld) || !IsFiniteVector(appliedStartWorld)
			|| !IsFiniteVector(deltaWorld) || !std::isfinite(deltaLength)
			|| deltaLength < 0.1f || deltaLength > 300.0f)
			return;
		pendingBulletTraceRawStartWorld = rawStartWorld;
		pendingBulletTraceDeltaWorld = deltaWorld;
		pendingBulletTraceCorrectionSequence = probe.sequence;
		bulletTraceCorrectionExpiresAt = GetTickCount64() + 500;
	}

	void ApplyBulletTraceComponentCorrection(bool debug)
	{
		auto component = bulletTraceMeshComponent.load(std::memory_order_acquire);
		if (component == nullptr || !uevr::API::UObjectHook::exists(component))
			return;
		auto componentClass = component->get_class();
		if (componentClass == nullptr
			|| componentClass->find_function(L"K2_GetComponentLocation") == nullptr
			|| componentClass->find_function(L"K2_SetWorldLocation") == nullptr)
			return;

		Utilities::ParameterSingleVector3 locationParams{};
		component->call_function(L"K2_GetComponentLocation", &locationParams);
		const glm::fvec3 currentLocation = locationParams.vec3Value;
		if (!IsFiniteVector(currentLocation))
			return;

		const uint64_t now = GetTickCount64();
		if (bulletTraceComponentCorrectionActive && now > bulletTraceCorrectionExpiresAt)
		{
			if (glm::length(currentLocation - bulletTraceComponentLastApplied) <= 50.0f)
			{
				Utilities::Parameter_K2_SetWorldOrRelativeLocation restoreParams{};
				restoreParams.newLocation = bulletTraceComponentBaseline;
				restoreParams.bSweep = false;
				restoreParams.bTeleport = true;
				component->call_function(L"K2_SetWorldLocation", &restoreParams);
			}
			bulletTraceComponentCorrectionActive = false;
		}

		if (pendingBulletTraceCorrectionSequence == 0
			|| pendingBulletTraceCorrectionSequence == appliedBulletTraceCorrectionSequence)
			return;

		if (!bulletTraceComponentCorrectionActive)
		{
			const bool worldSpaceMesh = glm::length(currentLocation) <= 500.0f;
			const bool nativeOriginComponent = glm::length(currentLocation - pendingBulletTraceRawStartWorld) <= 250.0f;
			if (!worldSpaceMesh && !nativeOriginComponent)
			{
				if (debug)
					uevr::API::get()->log_info(
						"[BulletTraceComponent] seq=%u skipped location=(%.3f %.3f %.3f) rawStartWorld=(%.3f %.3f %.3f)",
						pendingBulletTraceCorrectionSequence,
						currentLocation.x, currentLocation.y, currentLocation.z,
						pendingBulletTraceRawStartWorld.x, pendingBulletTraceRawStartWorld.y,
						pendingBulletTraceRawStartWorld.z);
				appliedBulletTraceCorrectionSequence = pendingBulletTraceCorrectionSequence;
				return;
			}
			bulletTraceComponentBaseline = currentLocation;
		}
		else if (glm::length(currentLocation - bulletTraceComponentLastApplied) > 50.0f
			&& glm::length(currentLocation - bulletTraceComponentBaseline) > 50.0f)
		{
			return;
		}

		const glm::fvec3 desiredLocation = bulletTraceComponentBaseline + pendingBulletTraceDeltaWorld;
		if (!IsFiniteVector(desiredLocation))
			return;
		Utilities::Parameter_K2_SetWorldOrRelativeLocation setParams{};
		setParams.newLocation = desiredLocation;
		setParams.bSweep = false;
		setParams.bTeleport = true;
		component->call_function(L"K2_SetWorldLocation", &setParams);
		bulletTraceComponentLastApplied = desiredLocation;
		bulletTraceComponentCorrectionActive = true;
		appliedBulletTraceCorrectionSequence = pendingBulletTraceCorrectionSequence;

		if (debug)
		{
			const auto name = component->get_full_name();
			uevr::API::get()->log_info(
				"[BulletTraceComponent] seq=%u component=%ls baseline=(%.3f %.3f %.3f) delta=(%.3f %.3f %.3f) applied=(%.3f %.3f %.3f)",
				appliedBulletTraceCorrectionSequence, name.c_str(),
				bulletTraceComponentBaseline.x, bulletTraceComponentBaseline.y, bulletTraceComponentBaseline.z,
				pendingBulletTraceDeltaWorld.x, pendingBulletTraceDeltaWorld.y, pendingBulletTraceDeltaWorld.z,
				desiredLocation.x, desiredLocation.y, desiredLocation.z);
		}
	}

	struct RelativeCallCandidate
	{
		uintptr_t target = 0;
		size_t offset = 0;
	};

	bool IsExecutableAddress(uintptr_t address)
	{
		MEMORY_BASIC_INFORMATION memory{};
		if (address == 0 || VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) == 0
			|| memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
			return false;
		const DWORD protection = memory.Protect & 0xFF;
		return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ
			|| protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
	}

	size_t DecodeSupportedPrologueInstruction(const uint8_t* code)
	{
		if (code == nullptr)
			return 0;
		size_t offset = 0;
		while (offset < 2 && code[offset] >= 0x40 && code[offset] <= 0x4F)
			++offset;
		const uint8_t opcode = code[offset++];
		if (opcode >= 0x50 && opcode <= 0x57)
			return offset;
		if (opcode != 0x89 && opcode != 0x8B && opcode != 0x8D
			&& opcode != 0x81 && opcode != 0x83)
			return 0;

		const uint8_t modrm = code[offset++];
		const uint8_t mod = modrm >> 6;
		const uint8_t rm = modrm & 7;
		if (mod != 3 && rm == 4)
		{
			const uint8_t sib = code[offset++];
			if (mod == 0 && (sib & 7) == 5)
				offset += 4;
		}
		if (mod == 0 && rm == 5)
			offset += 4;
		else if (mod == 1)
			offset += 1;
		else if (mod == 2)
			offset += 4;
		if (opcode == 0x81)
			offset += 4;
		else if (opcode == 0x83)
			offset += 1;
		return offset <= 15 ? offset : 0;
	}

	size_t GetSupportedPrologueLength(uintptr_t target)
	{
		if (!IsExecutableAddress(target))
			return 0;
		const auto code = reinterpret_cast<const uint8_t*>(target);
		size_t total = 0;
		while (total < 5)
		{
			const size_t instructionLength = DecodeSupportedPrologueInstruction(code + total);
			if (instructionLength == 0)
				return 0;
			total += instructionLength;
		}
		return total <= 16 ? total : 0;
	}

	std::vector<RelativeCallCandidate> CollectRelativeCalls(void* originalNativeFunction,
		size_t& scannedLength)
	{
		std::vector<RelativeCallCandidate> calls;
		scannedLength = 0;
		if (originalNativeFunction == nullptr)
			return calls;
		const auto nativeFunction = reinterpret_cast<const uint8_t*>(originalNativeFunction);
		if (!IsExecutableAddress(reinterpret_cast<uintptr_t>(nativeFunction)))
			return calls;

		MEMORY_BASIC_INFORMATION memory{};
		VirtualQuery(nativeFunction, &memory, sizeof(memory));
		const size_t available = static_cast<size_t>(
			reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize
			- reinterpret_cast<uintptr_t>(nativeFunction));
		const size_t limit = (std::min)(available, static_cast<size_t>(768));
		for (size_t offset = 0; offset + 5 <= limit; ++offset)
		{
			if (nativeFunction[offset] == 0xE8)
			{
				int32_t displacement = 0;
				std::memcpy(&displacement, nativeFunction + offset + 1, sizeof(displacement));
				const uintptr_t target = reinterpret_cast<uintptr_t>(nativeFunction + offset + 5)
					+ static_cast<int64_t>(displacement);
				if (IsExecutableAddress(target))
					calls.push_back({ target, offset });
			}
			if (offset >= 32 && (nativeFunction[offset] == 0xC3 || nativeFunction[offset] == 0xC2))
			{
				scannedLength = offset + 1;
				break;
			}
		}
		if (scannedLength == 0)
			scannedLength = limit;
		return calls;
	}

	void EnsureBulletTraceNativeHooks(MemoryManager* memoryManager)
	{
		if (bulletTraceNativeHooksInitialized || memoryManager == nullptr
			|| bulletTraceMeshComponent.load(std::memory_order_acquire) == nullptr)
			return;

		const uint32_t hookCount = bulletTraceMeshHookCount.load(std::memory_order_acquire);
		if (hookCount == 0)
			return;
		bulletTraceNativeHooksInitialized = true;

		std::array<std::vector<RelativeCallCandidate>, 4> callsByFunction{};
		std::array<size_t, 4> scanLengths{};
		std::array<uintptr_t, 4> selectedTargets{};
		std::array<size_t, 4> overwriteSizes{};
		for (uint32_t i = 0; i < hookCount && i < callsByFunction.size(); ++i)
		{
			callsByFunction[i] = CollectRelativeCalls(
				bulletTraceMeshHooks[i].originalNativeFunction, scanLengths[i]);
		}

		for (uint32_t i = 0; i < hookCount && i < callsByFunction.size(); ++i)
		{
			for (auto candidate = callsByFunction[i].rbegin(); candidate != callsByFunction[i].rend(); ++candidate)
			{
				uint32_t functionOccurrences = 0;
				for (uint32_t other = 0; other < hookCount && other < callsByFunction.size(); ++other)
				{
					const bool found = std::any_of(callsByFunction[other].begin(), callsByFunction[other].end(),
						[&](const RelativeCallCandidate& value) { return value.target == candidate->target; });
					functionOccurrences += found ? 1U : 0U;
				}
				const size_t overwriteSize = GetSupportedPrologueLength(candidate->target);
				if (functionOccurrences == 1 && overwriteSize >= 5)
				{
					selectedTargets[i] = candidate->target;
					overwriteSizes[i] = overwriteSize;
					break;
				}
			}
		}

		const char* names[] = {
			"BulletTrace native CreateMeshSection",
			"BulletTrace native CreateMeshSectionLinear",
			"BulletTrace native UpdateMeshSection",
			"BulletTrace native UpdateMeshSectionLinear"
		};
		uint32_t installedCount = 0;
		for (uint32_t i = 0; i < hookCount && i < selectedTargets.size(); ++i)
		{
			bool duplicate = false;
			for (uint32_t previous = 0; previous < i; ++previous)
				duplicate = duplicate || (selectedTargets[i] != 0 && selectedTargets[i] == selectedTargets[previous]);
			const bool installed = !duplicate && selectedTargets[i] != 0
				&& memoryManager->InstallRuntimeArrayCallbackHook(names[i], selectedTargets[i], overwriteSizes[i],
					&bulletTraceMeshComponent, bulletTraceMeshHooks[i].vectorStride,
					bulletTraceMeshHooks[i].name, reinterpret_cast<void*>(&AdjustBulletTraceVertices));
			installedCount += installed ? 1U : 0U;
			uevr::API::get()->log_info(
				"[BulletTraceNativeHook] fn=%s thunk=%p calls=%zu scanned=%zu target=%p overwrite=%zu duplicate=%s installed=%s",
				bulletTraceMeshHooks[i].name, bulletTraceMeshHooks[i].originalNativeFunction,
				callsByFunction[i].size(), scanLengths[i], reinterpret_cast<void*>(selectedTargets[i]),
				overwriteSizes[i], duplicate ? "true" : "false", installed ? "true" : "false");
		}
		uevr::API::get()->log_info("[BulletTraceNativeHook] initialization installed=%u", installedCount);
	}

	void EnsureBulletTraceMeshHooks(bool debug, MemoryManager* memoryManager)
	{
		bulletTraceDebugEnabled.store(debug, std::memory_order_relaxed);
		if (!bulletTraceHooksInitialized)
		{
			auto proceduralMeshClass = uevr::API::get()->find_uobject<uevr::API::UClass>(
				L"Class /Script/ProceduralMeshComponent.ProceduralMeshComponent");
			if (proceduralMeshClass != nullptr)
			{
				const struct { const wchar_t* wideName; const char* name; } candidates[] = {
					{ L"CreateMeshSection", "CreateMeshSection" },
					{ L"CreateMeshSection_LinearColor", "CreateMeshSection_LinearColor" },
					{ L"UpdateMeshSection", "UpdateMeshSection" },
					{ L"UpdateMeshSection_LinearColor", "UpdateMeshSection_LinearColor" }
				};
				uint32_t installedCount = 0;
				for (const auto& candidate : candidates)
				{
					auto function = proceduralMeshClass->find_function(candidate.wideName);
					auto verticesProperty = function != nullptr ? function->find_property(L"Vertices") : nullptr;
					if (function == nullptr || verticesProperty == nullptr)
						continue;

					auto arrayProperty = reinterpret_cast<uevr::API::FArrayProperty*>(verticesProperty);
					auto innerProperty = arrayProperty->get_inner();
					auto structProperty = reinterpret_cast<uevr::API::FStructProperty*>(innerProperty);
					auto vectorStruct = structProperty != nullptr ? structProperty->get_struct() : nullptr;
					const int32_t vectorStride = vectorStruct != nullptr ? vectorStruct->get_struct_size() : 0;
					if (vectorStride != 12 && vectorStride != 24)
						continue;

					auto& hook = bulletTraceMeshHooks[installedCount];
					hook.function = function;
					hook.originalNativeFunction = function->get_native_function();
					hook.verticesOffset = verticesProperty->get_offset();
					hook.vectorStride = vectorStride;
					hook.name = candidate.name;
					if (function->hook_ptr(BulletTraceMeshPreHook, nullptr))
						++installedCount;
				}
				bulletTraceMeshHookCount.store(installedCount, std::memory_order_release);
				bulletTraceHooksInitialized = true;
				uevr::API::get()->log_info(
					"[BulletTraceMeshHook] initialization installed=%u", installedCount);
			}
		}

		auto currentComponent = bulletTraceMeshComponent.load(std::memory_order_acquire);
		if (currentComponent == nullptr || !uevr::API::UObjectHook::exists(currentComponent))
		{
			const uint64_t now = GetTickCount64();
			if (now - lastBulletTraceComponentResolveTime < 1000)
				return;
			lastBulletTraceComponentResolveTime = now;

			auto owner = uevr::API::get()->find_uobject<uevr::API::UObject>(
				L"BP_Water_Base_C /Game/SanAndreas/Maps/SAWorld/SAWorld.SAWorld.PersistentLevel.BP_Water_Base_4");
			auto ownerClass = owner != nullptr ? owner->get_class() : nullptr;
			auto component = ownerClass != nullptr && ownerClass->find_property(L"BulletTrace") != nullptr
				? owner->get_property<uevr::API::UObject*>(L"BulletTrace")
				: nullptr;
			if (component != nullptr && uevr::API::UObjectHook::exists(component))
			{
				bulletTraceMeshComponent.store(component, std::memory_order_release);
				const auto name = component->get_full_name();
				uevr::API::get()->log_info("[BulletTraceMeshHook] component resolved=%ls", name.c_str());
			}
		}
		// The native-thunk scan was unable to resolve a safe target in this build.
		// Keep the high-level hooks and the guarded component-transform correction;
		// do not install speculative internal-call hooks.
	}

}

void WeaponManager::ProcessBulletTracePostTick()
{
	// The native trace and tracer bridges consume their correlated snapshots in
	// the engine hooks. No component scan or passive post-tick probe is needed.
}

void WeaponManager::SetGripState(bool leftGripHeld, bool rightGripHeld)
{
	const uint8_t mask = static_cast<uint8_t>((leftGripHeld ? 1 : 0) | (rightGripHeld ? 2 : 0));
	const uint8_t previousMask = gripStateMask.load(std::memory_order_relaxed);
	for (int hand = 0; hand < 2; ++hand)
	{
		const uint8_t bit = static_cast<uint8_t>(1U << hand);
		if ((mask & bit) != 0 && (previousMask & bit) == 0)
			gripPressGeneration[static_cast<size_t>(hand)].fetch_add(1, std::memory_order_acq_rel);
	}
	if (mask == 0)
	{
		twoHandFirstGripHandSnapshot.store(-1, std::memory_order_release);
		twoHandPrimaryHand.store(-1, std::memory_order_release);
		gripStateMask.store(0, std::memory_order_release);
	}
	else
	{
		int firstGripHand = twoHandFirstGripHandSnapshot.load(std::memory_order_acquire);
		// A direct hand transfer can move from one sole-grip mask to the other
		// without an intervening zero sample. The old latch then names a hand that
		// is no longer held and the next support grip solves around the wrong
		// primary. Relatch only this impossible single-grip state; ordinary grip
		// order, calibration data, and weapon-class behavior remain unchanged.
		if ((mask == 1U || mask == 2U) && firstGripHand >= 0
			&& (mask & static_cast<uint8_t>(1U << firstGripHand)) == 0)
		{
			const int staleHand = firstGripHand;
			firstGripHand = mask == 1U ? 0 : 1;
			twoHandFirstGripHandSnapshot.store(static_cast<int8_t>(firstGripHand), std::memory_order_release);
			twoHandPrimaryHand.store(static_cast<int8_t>(firstGripHand), std::memory_order_release);
			if (settingsManager->debugInputLayerProbe)
				uevr::API::get()->log_info(
					"[TwoHand] sole-grip relatch stale=%d primary=%d mask=%u",
					staleHand, firstGripHand, static_cast<unsigned int>(mask));
		}
		if (firstGripHand < 0)
		{
			// Prefer the hand that was already held if this callback observes the
			// transition to both grips in one sample. A simultaneous first sample
			// uses the game-thread-published configured attachment hand.
			if ((previousMask & 1) != 0 || mask == 1)
				firstGripHand = 0;
			else if ((previousMask & 2) != 0 || mask == 2)
				firstGripHand = 1;
			else
				firstGripHand = twoHandConfiguredHandSnapshot.load(std::memory_order_acquire);

			if (firstGripHand >= 0)
				twoHandFirstGripHandSnapshot.store(static_cast<int8_t>(firstGripHand), std::memory_order_release);
		}

		// Only publish the T-0021 primary latch while the game-thread snapshot
		// says this is an eligible single long gun on foot. The first-grip
		// snapshot above is input-only state used to bridge callback/frame order.
		if (twoHandLatchEligibleSnapshot.load(std::memory_order_acquire)
			&& firstGripHand >= 0)
			twoHandPrimaryHand.store(static_cast<int8_t>(firstGripHand), std::memory_order_release);
	}
	// Capture the first nonzero grip without reading game-thread state.  The
	// game-thread eligibility snapshot gates every consumer of this latch, so
	// pistols, dual wield, vehicles, and ordinary hand switching are unchanged.
	gripStateMask.store(mask, std::memory_order_release);

	if (previousMask != mask && settingsManager->debugInputLayerProbe)
	{
		uevr::API::get()->log_info(
			"[TwoHand] grip mask=%u previous=%u eligible=%s latchedHand=%d configuredHand=%d",
			static_cast<unsigned int>(mask), static_cast<unsigned int>(previousMask),
			twoHandLatchEligibleSnapshot.load(std::memory_order_acquire) ? "true" : "false",
			static_cast<int>(twoHandPrimaryHand.load(std::memory_order_acquire)),
			static_cast<int>(twoHandConfiguredHandSnapshot.load(std::memory_order_acquire)));
	}
}

void WeaponManager::SetTwoHandViewRotation(const UEVR_Rotatorf& rotation)
{
	if (!std::isfinite(rotation.pitch) || !std::isfinite(rotation.yaw) || !std::isfinite(rotation.roll))
		return;

	twoHandViewPitch.store(rotation.pitch, std::memory_order_relaxed);
	twoHandViewYaw.store(rotation.yaw, std::memory_order_relaxed);
	twoHandViewRoll.store(rotation.roll, std::memory_order_relaxed);
	twoHandViewRotationValid.store(true, std::memory_order_release);
}

bool WeaponManager::IsTwoHandLongGun() const
{
	switch (currentWeaponEquipped)
	{
	case Shotgun:
	case Spas12:
	case Mp5:
	case Ak47:
	case M4:
	case Rifle:
	case Sniper:
	case RocketLauncher:
	case RocketLauncherHs:
	case Flamethrower:
	case Minigun:
		return true;
	default:
		return false;
	}
}

bool WeaponManager::ShouldPreservePrimaryHandForTwoHand() const
{
	return twoHandLatchEligibleSnapshot.load(std::memory_order_acquire)
		&& gripStateMask.load(std::memory_order_acquire) != 0
		&& twoHandPrimaryHand.load(std::memory_order_acquire) >= 0;
}

void WeaponManager::RestoreTwoHandRotationOffset()
{
	// The primary fake hand is only weapon-local while the stabilized support
	// solve owns the weapon. Release it with the same transition so the next
	// presentation pass can restore raw controller following immediately.
	RestorePrimaryFakeHandAttachment();
	if (twoHandOffsetApplied && twoHandAppliedWeaponMesh != nullptr
		&& uevr::API::UObjectHook::exists(twoHandAppliedWeaponMesh))
	{
		if (auto motionState = uevr::API::UObjectHook::get_motion_controller_state(twoHandAppliedWeaponMesh))
		{
			int restoreHand = motionConfiguredFirstHand;
			if (restoreHand < 0)
				restoreHand = twoHandPrimaryHand.load(std::memory_order_acquire);
			const glm::fquat restoreOffset = settingsManager != nullptr && restoreHand >= 0
				? GetWeaponGripRotationOffset(restoreHand)
				: glm::normalize(glm::fquat(defaultWeaponRotationEuler));
			const UEVR_Quaternionf restoreOffsetUevr = {
				restoreOffset.w, restoreOffset.x, restoreOffset.y, restoreOffset.z
			};
			motionState->set_rotation_offset(&restoreOffsetUevr);
		}
	}

	twoHandSupportActive = false;
	twoHandOffsetApplied = false;
	twoHandRotationOffset = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	twoHandAppliedWeaponMesh = nullptr;
	twoHandPrimaryBasisValid = false;
	twoHandWeaponRelativeToPrimary = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	twoHandPrimaryBasisWeaponMesh = nullptr;
	twoHandNeutralActualForward = glm::fvec3(0.0f);
	twoHandPostSettleElapsed = 0.0f;
	twoHandPostSettleLogged = false;
	twoHandLastDesiredForward = glm::fvec3(0.0f);
	twoHandFilteredSupportDirection = glm::fvec3(0.0f);
	twoHandFilteredSupportDirectionValid = false;
	twoHandStableTargetValid = false;
	twoHandStableTargetRotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	twoHandWristOverrideActive = false;
	twoHandWristPrimaryHand = -1;
	twoHandWristRotationDelta = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	twoHandWristPrimaryPoseRotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
}

void WeaponManager::SetMeleeClenchState(bool leftTriggerHeld, bool rightTriggerHeld)
{
	const uint8_t mask = static_cast<uint8_t>((leftTriggerHeld ? 1U : 0U)
		| (rightTriggerHeld ? 2U : 0U));
	motionMeleeClenchMask.store(mask, std::memory_order_release);
}

void WeaponManager::SetCustomAkimboInputState(uint8_t heldMask,
	uint8_t edgeMask, int luaWeaponId)
{
	customAkimboHeldMaskSnapshot.store(heldMask & 0x03U, std::memory_order_release);
	if ((edgeMask & 0x03U) != 0)
		customAkimboEdgeMaskPending.fetch_or(edgeMask & 0x03U, std::memory_order_acq_rel);
	customAkimboInputWeapon.store(luaWeaponId, std::memory_order_release);
}

void WeaponManager::ProcessCustomAkimboState()
{
	const int weapon = static_cast<int>(currentWeaponEquipped);
	const bool supported = weapon == Pistol || weapon == Sawnoff
		|| weapon == MicroUzi || weapon == Tec9;
	int rejection = 0;
	if (settingsManager == nullptr || !settingsManager->enableCustomAkimbo)
		rejection = 1;
	else if (!supported)
		rejection = 2;
	else if (playerManager == nullptr || !playerManager->isInControl
		|| playerManager->isInVehicle || playerManager->weaponWheelEnabled)
		rejection = 3;
	else if (firstWeaponMesh == nullptr || secondWeaponMesh == nullptr
		|| !uevr::API::UObjectHook::exists(firstWeaponMesh)
		|| !uevr::API::UObjectHook::exists(secondWeaponMesh))
		rejection = 4;
	else if ((gripStateMask.load(std::memory_order_acquire) & 0x03U) != 0x03U)
		rejection = 5;
	else if (customAkimboInputWeapon.load(std::memory_order_acquire) >= 0
		&& customAkimboInputWeapon.load(std::memory_order_acquire) != weapon)
		rejection = 6;

	const bool active = rejection == 0;
	const uint8_t heldMask = customAkimboHeldMaskSnapshot.load(std::memory_order_acquire);
	const uint8_t edgeMask = customAkimboEdgeMaskPending.exchange(0, std::memory_order_acq_rel);
	if (active && !customAkimboActive)
	{
		// UpdateActualWeaponMesh has already assigned the duplicate to the hand
		// opposite the live GTA mesh. ProcessMagneticIdleWeapon then suspends its
		// ownership and clears motionConfiguredFirstHand, so recover the primary
		// from that surviving opposite-hand assignment before forcing both visuals
		// into controller ownership below.
		if (motionConfiguredSecondWeaponMesh == customAkimboVisualMesh
			&& motionConfiguredSecondHand >= 0 && motionConfiguredSecondHand <= 1)
			customAkimboPrimaryHand = 1 - motionConfiguredSecondHand;
		else
			customAkimboPrimaryHand = settingsManager->leftHandedMode != SettingsManager::Disabled ? 0 : 1;
	}
	if (memoryManager != nullptr)
		memoryManager->SetCustomAkimboState(active, weapon, heldMask, edgeMask);
	if (active != customAkimboActive || rejection != customAkimboLastRejection)
	{
		uevr::API::get()->log_info(
			"[CustomAkimbo] %s weapon=%d rejection=%d grips=%u heldNativeMask=%u first=%p second=%p",
			active ? "active" : "inactive", weapon, rejection,
			static_cast<unsigned int>(gripStateMask.load(std::memory_order_relaxed) & 0x03U),
			static_cast<unsigned int>(heldMask), firstWeaponMesh, secondWeaponMesh);
		customAkimboActive = active;
		customAkimboLastRejection = rejection;
	}

	if (active)
	{
		// Magnetic suspension intentionally restores GTA's native attachment and
		// removes the live mesh's motion state. Akimbo must reclaim both visuals
		// after that transition; otherwise the duplicate follows one controller
		// while the original follows CJ's body animation.
		const bool firstReady = uevr::API::UObjectHook::get_motion_controller_state(firstWeaponMesh) != nullptr;
		const bool secondReady = uevr::API::UObjectHook::get_motion_controller_state(secondWeaponMesh) != nullptr;
		if (!motionWeaponTrackingEnabled || !visualWeaponTrackingEnabled
			|| !firstReady || !secondReady)
		{
			// Do not expose either mesh at its native/default transform during the
			// one-frame ownership handoff. Both are revealed together only after
			// their controller states exist.
			SetComponentVisibility(firstWeaponMesh, false);
			SetComponentVisibility(secondWeaponMesh, false);
			motionWeaponTrackingEnabled = true;
			visualWeaponTrackingEnabled = true;
			magneticIdleWeaponActive = false;
			if (!firstReady)
			{
				motionConfiguredFirstWeaponMesh = nullptr;
				motionConfiguredFirstHand = -1;
				motionConfiguredFirstCalibrationRole = -1;
			}
			if (!secondReady)
			{
				motionConfiguredSecondWeaponMesh = nullptr;
				motionConfiguredSecondHand = -1;
			}
			UpdateActualWeaponMesh();
			uevr::API::get()->log_info(
				"[CustomAkimbo] controller ownership repaired primaryHand=%d firstReady=%s secondReady=%s",
				customAkimboPrimaryHand,
				uevr::API::UObjectHook::get_motion_controller_state(firstWeaponMesh) != nullptr ? "true" : "false",
				uevr::API::UObjectHook::get_motion_controller_state(secondWeaponMesh) != nullptr ? "true" : "false");
		}
		const bool bothReady = firstWeaponMesh != nullptr && secondWeaponMesh != nullptr
			&& uevr::API::UObjectHook::get_motion_controller_state(firstWeaponMesh) != nullptr
			&& uevr::API::UObjectHook::get_motion_controller_state(secondWeaponMesh) != nullptr;
		SetComponentVisibility(firstWeaponMesh, bothReady && weaponScaledVisible);
		SetComponentVisibility(secondWeaponMesh, bothReady && weaponScaledVisible);
	}
	else
	{
		if (customAkimboVisualMesh != nullptr
			&& uevr::API::UObjectHook::exists(customAkimboVisualMesh))
			SetComponentVisibility(customAkimboVisualMesh, false);
		customAkimboPrimaryHand = -1;
	}
}

void WeaponManager::RemoveCustomAkimboVisual(const char* reason)
{
	auto* visual = customAkimboVisualMesh;
	if (visual != nullptr && uevr::API::UObjectHook::exists(visual))
	{
		uevr::API::UObjectHook::remove_motion_controller_state(visual);
		SetComponentVisibility(visual, false);
		auto* componentClass = visual->get_class();
		auto* destroy = componentClass != nullptr
			? componentClass->find_function(L"DestroyComponent") : nullptr;
		if (destroy != nullptr)
		{
			std::vector<uint8_t> params(destroy->get_properties_size());
			SetReflectedBoolParameter(destroy, params, L"bPromoteChildren", false);
			destroy->call(visual, params.data());
		}
	}
	if (secondWeaponMesh == visual)
	{
		secondWeaponMesh = nullptr;
		secondWeaponStaticMesh = nullptr;
		secondWeaponContainer = nullptr;
	}
	if (visibilityAppliedSecondWeaponMesh == visual)
		visibilityAppliedSecondWeaponMesh = nullptr;
	if (motionConfiguredSecondWeaponMesh == visual)
	{
		motionConfiguredSecondWeaponMesh = nullptr;
		motionConfiguredSecondHand = -1;
	}
	if (visual != nullptr)
		uevr::API::get()->log_info("[CustomAkimbo] visual removed reason=%s mesh=%p weapon=%d",
			reason != nullptr ? reason : "unknown", visual, customAkimboVisualWeapon);
	customAkimboVisualMesh = nullptr;
	customAkimboVisualStaticMesh = nullptr;
	customAkimboVisualCharacter = nullptr;
	customAkimboVisualWeapon = -1;
}

bool WeaponManager::EnsureCustomAkimboVisual()
{
	const int weapon = static_cast<int>(currentWeaponEquipped);
	const bool supported = weapon == Pistol || weapon == Sawnoff
		|| weapon == MicroUzi || weapon == Tec9;
	auto* character = playerManager != nullptr ? playerManager->playerCharacter : nullptr;
	if (!supported || character == nullptr || firstWeaponStaticMesh == nullptr
		|| !uevr::API::UObjectHook::exists(character)
		|| !uevr::API::UObjectHook::exists(firstWeaponStaticMesh))
		return false;

	if (customAkimboVisualMesh != nullptr
		&& customAkimboVisualCharacter == character
		&& customAkimboVisualStaticMesh == firstWeaponStaticMesh
		&& customAkimboVisualWeapon == weapon
		&& uevr::API::UObjectHook::exists(customAkimboVisualMesh))
	{
		secondWeaponMesh = customAkimboVisualMesh;
		secondWeaponStaticMesh = customAkimboVisualStaticMesh;
		secondWeaponContainer = nullptr;
		return true;
	}

	RemoveCustomAkimboVisual("replace");
	auto* staticMeshClass = uevr::API::get()->find_uobject<uevr::API::UClass>(
		L"Class /Script/Engine.StaticMeshComponent");
	auto* visual = staticMeshClass != nullptr
		? AddSkeletalComponent(character, staticMeshClass) : nullptr;
	if (visual == nullptr || !CallSetStaticMesh(visual, firstWeaponStaticMesh))
	{
		if (visual != nullptr && uevr::API::UObjectHook::exists(visual))
			SetComponentVisibility(visual, false);
		uevr::API::get()->log_warn("[CustomAkimbo] visual creation failed weapon=%d", weapon);
		return false;
	}

	customAkimboVisualMesh = visual;
	customAkimboVisualStaticMesh = firstWeaponStaticMesh;
	customAkimboVisualCharacter = character;
	customAkimboVisualWeapon = weapon;
	secondWeaponMesh = visual;
	secondWeaponStaticMesh = firstWeaponStaticMesh;
	secondWeaponContainer = nullptr;
	SetComponentVisibility(visual, false);
	uevr::API::get()->log_info("[CustomAkimbo] visual created weapon=%d mesh=%p asset=%p character=%p",
		weapon, visual, firstWeaponStaticMesh, character);
	return true;
}

void WeaponManager::QueuePhysicalThrowableProbeEvent(int eventType,
	uint32_t sequence, uint8_t handMask, uint8_t gripMask,
	uint32_t holdMilliseconds, int luaWeaponId)
{
	// Called from Lua/custom-event context: publish plain values only. UObject and
	// controller-pose reads remain on the engine thread in ProcessPhysicalThrowableProbe.
	handMask &= 3U;
	gripMask &= 3U;
	if (eventType == 1 && sequence != 0 && handMask != 0)
	{
		throwableProbeEdgeSequence.store(sequence, std::memory_order_relaxed);
		throwableProbeEdgeHandMask.store(handMask, std::memory_order_relaxed);
		throwableProbeEdgeGripMask.store(gripMask, std::memory_order_relaxed);
		throwableProbeEdgeLuaWeapon.store(luaWeaponId, std::memory_order_relaxed);
		throwableProbeEdgePending.store(true, std::memory_order_release);
	}
	else if (eventType == 2 && sequence != 0 && handMask != 0)
	{
		throwableProbeReleaseSequence.store(sequence, std::memory_order_relaxed);
		throwableProbeReleaseHandMask.store(handMask, std::memory_order_relaxed);
		throwableProbeReleaseGripMask.store(gripMask, std::memory_order_relaxed);
		throwableProbeReleaseHoldMilliseconds.store(holdMilliseconds, std::memory_order_relaxed);
		throwableProbeReleaseLuaWeapon.store(luaWeaponId, std::memory_order_relaxed);
		throwableProbeReleasePending.store(true, std::memory_order_release);
	}
	else if (eventType == 3)
	{
		throwableProbeCancelPending.store(true, std::memory_order_release);
	}
}

void WeaponManager::BeginInteractionEngineTick()
{
	// Advance once at the outer engine-tick boundary, before any forced/free-hand
	// presentation pre-pass. Primary attachment priming must survive one complete
	// outer tick; ProcessMotionMelee runs too late to be that boundary.
	++interactionEngineTickGeneration;
}

bool WeaponManager::PrepareForExplicitWeaponCycle()
{
	if (playerManager == nullptr || !playerManager->isInControl
		|| playerManager->isInVehicle || playerManager->weaponWheelEnabled)
		return false;

	const uint8_t heldGrips = gripStateMask.load(std::memory_order_acquire);
	const bool ownedPresentation = magneticIdleWeaponActive || magneticGripHand >= 0
		|| magneticIdleWeaponDetached;
	if (ownedPresentation && !explicitWeaponCyclePending)
	{
		// GTA replaces/selects weapons through its native attachment hierarchy. Keep
		// the complete body-local pose in place, but briefly return the current mesh
		// to that hierarchy so UpdateActualWeaponMesh can discover the replacement.
		explicitWeaponCyclePending = true;
		explicitWeaponCycleSourceWeaponId = static_cast<int>(currentWeaponEquipped);
		explicitWeaponCycleRestoreAnchorHand = magneticIdleAnchorHand;
		explicitWeaponCycleRestoreAnchorBucket = magneticIdleAnchorBucket;
		explicitWeaponCycleDeadline = GetTickCount64() + 750;
		SuspendMagneticIdleSlot();
		magneticIdleAnchorHand = explicitWeaponCycleRestoreAnchorHand;
		magneticIdleAnchorBucket = explicitWeaponCycleRestoreAnchorBucket;
		ConsumeCurrentGripPressGenerations();
		magneticProcessedGripMask = heldGrips;
		magneticTriggerBlockedSnapshot.store(true, std::memory_order_release);
	}
	uevr::API::get()->log_info(
		"[MagneticWeapon] explicit cycle accepted native-window=%s grips=%u weapon=%d anchor=%s",
		ownedPresentation ? "true" : "false", static_cast<unsigned int>(heldGrips),
		static_cast<int>(currentWeaponEquipped), magneticCustomAnchorValid ? "captured" : "fallback");
	return true;
}

glm::fquat WeaponManager::ComposeTwoHandRotationOffset(const glm::fquat& baseOffset, uevr::API::UObject* weaponMesh) const
{
	if (!twoHandOffsetApplied || twoHandAppliedWeaponMesh != weaponMesh)
		return baseOffset;

	const glm::fquat defaultOffset = glm::normalize(glm::fquat(defaultWeaponRotationEuler));
	// UObjectHook applies handRotation * inverse(rotation_offset).  The
	// two-hand correction therefore composes on the right, in offset space.
	int primaryHand = motionConfiguredFirstHand;
	if (primaryHand < 0)
		primaryHand = twoHandPrimaryHand.load(std::memory_order_acquire);
	const glm::fquat neutralOffset = settingsManager != nullptr && primaryHand >= 0
		? GetWeaponGripRotationOffset(primaryHand) : defaultOffset;
	return glm::normalize(baseOffset * glm::inverse(neutralOffset) * twoHandRotationOffset);
}

void WeaponManager::ProcessTwoHandStabilization(float delta)
{
	if (IsGripCalibrationActive())
		return;
	const glm::fquat defaultOffset = glm::normalize(glm::fquat(defaultWeaponRotationEuler));
	glm::fquat neutralOffset = defaultOffset;
	int neutralHand = motionConfiguredFirstHand;
	if (neutralHand < 0)
		neutralHand = twoHandPrimaryHand.load(std::memory_order_acquire);
	if (settingsManager->enableGripCalibration && neutralHand >= 0)
		neutralOffset = GetWeaponGripRotationOffset(neutralHand);
	const float safeDelta = (std::max)(0.0f, (std::min)(delta, 0.1f));
	const bool hasEligibleWeapon = firstWeaponMesh != nullptr
		&& uevr::API::UObjectHook::exists(firstWeaponMesh)
		&& secondWeaponMesh == nullptr
		&& IsTwoHandLongGun();
	const bool baseEligible = settingsManager->enableTwoHandStabilization
		&& playerManager->isInControl
		&& !playerManager->isInVehicle
		&& !playerManager->weaponWheelEnabled
		&& cameraController->currentCameraMode != CameraController::Camera
		&& hasEligibleWeapon;

	if (!settingsManager->enableTwoHandStabilization)
	{
		if (twoHandOffsetApplied)
			RestoreTwoHandRotationOffset();
		return;
	}

	if (twoHandOffsetApplied && twoHandAppliedWeaponMesh != firstWeaponMesh)
		RestoreTwoHandRotationOffset();

	bool supportPoseValid = false;
	glm::fquat targetOffset = defaultOffset;
	bool targetOffsetValid = false;
	float handDistance = 0.0f;
	float forwardDot = -1.0f;
	glm::fvec3 rawHandVector(0.0f);
	glm::fvec3 convertedHandVector(0.0f);
	glm::fvec3 primaryForward(0.0f);
	glm::fvec3 supportDirection(0.0f);
	glm::fvec3 desiredForward(0.0f);
	glm::fvec3 diagnosticActualComponentForward = twoHandNeutralActualForward;
	int diagnosticSelectedHand = -1;
	float supportRawCorrectionDegrees = 0.0f;
	float supportAppliedCorrectionDegrees = 0.0f;
	bool supportCalibrationUsed = false;
	bool supportContactRecordUsed = false;
	float supportContactSpan = 0.0f;
	glm::fquat diagnosticRawPrimaryGripRotation(1.0f, 0.0f, 0.0f, 0.0f);
	glm::fquat diagnosticRawPrimaryAimRotation(1.0f, 0.0f, 0.0f, 0.0f);
	glm::fquat diagnosticTrackingRotation(1.0f, 0.0f, 0.0f, 0.0f);
	glm::fquat diagnosticPrimaryPoseRotation(1.0f, 0.0f, 0.0f, 0.0f);
	bool diagnosticTrackingRotationValid = false;
	const bool bothGripsHeld = gripStateMask.load(std::memory_order_acquire) == 3;
	if (!bothGripsHeld && !twoHandOffsetApplied)
	{
		twoHandPrimaryBasisValid = false;
		twoHandPrimaryBasisWeaponMesh = nullptr;
		twoHandNeutralActualForward = glm::fvec3(0.0f);
		twoHandWristOverrideActive = false;
		twoHandWristPrimaryHand = -1;
		twoHandWristRotationDelta = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
		twoHandWristPrimaryPoseRotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	}
	if (baseEligible && bothGripsHeld)
	{
		int primaryHand = twoHandPrimaryHand.load(std::memory_order_acquire);
		if (primaryHand < 0)
		{
			const int firstGripHand = twoHandFirstGripHandSnapshot.load(std::memory_order_acquire);
			const int configuredHand = twoHandConfiguredHandSnapshot.load(std::memory_order_acquire);
			if (firstGripHand >= 0)
				primaryHand = firstGripHand;
			else if (configuredHand >= 0)
				primaryHand = configuredHand;
			else
				primaryHand = settingsManager->leftHandedMode != SettingsManager::Disabled ? 0 : 1;
			twoHandPrimaryHand.store(static_cast<int8_t>(primaryHand), std::memory_order_release);
		}
			if (settingsManager->enableGripCalibration && primaryHand >= 0)
				neutralOffset = GetWeaponGripRotationOffset(primaryHand);
			diagnosticSelectedHand = primaryHand;
		const auto primaryIndex = primaryHand == 0
			? uevr::API::VR::get_left_controller_index()
			: uevr::API::VR::get_right_controller_index();
		const auto supportIndex = primaryHand == 0
			? uevr::API::VR::get_right_controller_index()
			: uevr::API::VR::get_left_controller_index();
		const bool viewRotationReady = twoHandViewRotationValid.load(std::memory_order_acquire);
		if (primaryIndex >= 0 && supportIndex >= 0 && viewRotationReady)
		{
			// UObjectHook uses grip positions but aim rotations for attachment.
			// Keep the two pose paths separate for the same reason here.
			const auto primaryGripPose = uevr::API::VR::get_grip_pose(primaryIndex);
			const auto primaryAimPose = uevr::API::VR::get_aim_pose(primaryIndex);
			const auto supportGripPose = uevr::API::VR::get_grip_pose(supportIndex);
			const glm::fvec3 primaryPosition(
				primaryGripPose.position.x, primaryGripPose.position.y, primaryGripPose.position.z);
			const glm::fvec3 supportPosition(
				supportGripPose.position.x, supportGripPose.position.y, supportGripPose.position.z);
			const UEVR_Rotatorf viewRotationEuler = {
				twoHandViewPitch.load(std::memory_order_relaxed),
				twoHandViewYaw.load(std::memory_order_relaxed),
				twoHandViewRoll.load(std::memory_order_relaxed)
			};
			const glm::fquat viewInverse = FlattenViewInverse(viewRotationEuler);
			const auto vrRotationOffsetUevr = uevr::API::VR::get_rotation_offset();
			const glm::fquat vrRotationOffset = glm::fquat::wxyz(
				vrRotationOffsetUevr.w, vrRotationOffsetUevr.x,
				vrRotationOffsetUevr.y, vrRotationOffsetUevr.z);
			const glm::fquat rawPrimaryGripRotation = glm::fquat::wxyz(
				primaryGripPose.rotation.w, primaryGripPose.rotation.x,
				primaryGripPose.rotation.y, primaryGripPose.rotation.z);
			const glm::fquat rawPrimaryAimRotation = glm::fquat::wxyz(
				primaryAimPose.rotation.w, primaryAimPose.rotation.x,
				primaryAimPose.rotation.y, primaryAimPose.rotation.z);
			glm::fquat primaryPoseRotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
			const bool trackingRotationValid = IsFiniteQuaternion(viewInverse)
				&& glm::length(viewInverse) > 0.0001f
				&& IsFiniteQuaternion(vrRotationOffset)
				&& glm::length(vrRotationOffset) > 0.0001f
				&& IsFiniteQuaternion(rawPrimaryAimRotation)
				&& glm::length(rawPrimaryAimRotation) > 0.0001f;
			diagnosticRawPrimaryGripRotation = rawPrimaryGripRotation;
			diagnosticRawPrimaryAimRotation = rawPrimaryAimRotation;
			if (trackingRotationValid)
			{
				const glm::fquat trackingRotation = glm::normalize(
					glm::normalize(viewInverse) * glm::normalize(vrRotationOffset)
					* glm::normalize(rawPrimaryAimRotation));
				diagnosticTrackingRotation = trackingRotation;
				// UEVR feeds this composed hand quaternion through
				// euler_angles_from_steamvr() before setting the Unreal component
				// rotation. Convert it with the same quat_converter basis change
				// before combining it with Unreal component vectors.
				primaryPoseRotation = VRQuaternionToComponentSpace(trackingRotation);
				diagnosticPrimaryPoseRotation = primaryPoseRotation;
			}
			diagnosticTrackingRotationValid = trackingRotationValid;

			if (twoHandPrimaryBasisValid && twoHandPrimaryBasisWeaponMesh != firstWeaponMesh)
			{
				twoHandPrimaryBasisValid = false;
				twoHandPrimaryBasisWeaponMesh = nullptr;
				twoHandNeutralActualForward = glm::fvec3(0.0f);
			}

			// Capture the actual weapon/primary relation only while the normal
			// attachment offset is active. Never read back a component affected by
			// twoHandRotationOffset to reconstruct the next frame's neutral pose.
			if (!twoHandPrimaryBasisValid && !twoHandOffsetApplied
				&& trackingRotationValid
				&& IsFiniteQuaternion(primaryPoseRotation)
				&& glm::length(primaryPoseRotation) > 0.0001f)
			{
				Utilities::ParameterSingleVector3 weaponForwardParams{};
				Utilities::ParameterSingleVector3 weaponRightParams{};
				Utilities::ParameterSingleVector3 weaponUpParams{};
				firstWeaponMesh->call_function(L"GetForwardVector", &weaponForwardParams);
				firstWeaponMesh->call_function(L"GetRightVector", &weaponRightParams);
				firstWeaponMesh->call_function(L"GetUpVector", &weaponUpParams);
				const glm::fvec3 currentWeaponForward = NormalizeOrZero(weaponForwardParams.vec3Value);
				const glm::fvec3 currentWeaponRight = NormalizeOrZero(weaponRightParams.vec3Value);
				const glm::fvec3 currentWeaponUp = NormalizeOrZero(weaponUpParams.vec3Value);
				if (IsFiniteVector(currentWeaponForward) && IsFiniteVector(currentWeaponRight)
					&& IsFiniteVector(currentWeaponUp)
					&& glm::length(currentWeaponForward) > 0.0001f
					&& glm::length(currentWeaponRight) > 0.0001f
					&& glm::length(currentWeaponUp) > 0.0001f)
				{
					const glm::fquat currentComponentRotation = RotationFromWeaponBasis(
						currentWeaponForward, currentWeaponRight, currentWeaponUp);
					if (IsFiniteQuaternion(currentComponentRotation)
						&& glm::length(currentComponentRotation) > 0.0001f)
					{
						// R = inverse(H0) * C0, where C0 is the actual component
						// orientation before the two-hand offset is applied.
							twoHandWeaponRelativeToPrimary = glm::normalize(
								glm::inverse(primaryPoseRotation) * currentComponentRotation);
						twoHandNeutralActualForward = currentWeaponForward;
						twoHandPrimaryBasisWeaponMesh = firstWeaponMesh;
						twoHandPrimaryBasisValid = true;
					}
				}
			}

			const bool neutralBasisValid = twoHandPrimaryBasisValid
				&& twoHandPrimaryBasisWeaponMesh == firstWeaponMesh
				&& trackingRotationValid
				&& IsFiniteQuaternion(primaryPoseRotation)
				&& glm::length(primaryPoseRotation) > 0.0001f
				&& IsFiniteQuaternion(twoHandWeaponRelativeToPrimary)
				&& glm::length(twoHandWeaponRelativeToPrimary) > 0.0001f;
			const glm::fquat baseComponentRotation = neutralBasisValid
				? glm::normalize(primaryPoseRotation * twoHandWeaponRelativeToPrimary)
				: glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
			const glm::fvec3 baseWeaponForward = NormalizeOrZero(
				baseComponentRotation * glm::fvec3(1.0f, 0.0f, 0.0f));

			if (IsFiniteVector(primaryPosition) && IsFiniteVector(supportPosition)
				&& IsFiniteVector(baseWeaponForward)
				&& neutralBasisValid
				&& IsFiniteQuaternion(baseComponentRotation) && glm::length(baseComponentRotation) > 0.0001f
				&& glm::length(baseWeaponForward) > 0.0001f)
			{
				rawHandVector = supportPosition - primaryPosition;
				handDistance = glm::length(rawHandVector);
				if (handDistance > 0.0001f)
				{
					// UObjectHook transforms each grip and then subtracts that
					// transformed position from final_position. Therefore the actual
					// component-space (support - primary) vector is the negative of the
					// transformed raw (support - primary) delta.
					convertedHandVector = -VRSpaceToComponentSpace(
						viewInverse * (glm::normalize(vrRotationOffset) * rawHandVector));
					supportDirection = NormalizeOrZero(convertedHandVector);
					primaryForward = baseWeaponForward;
					const float minimumDistance = twoHandSupportActive ? 0.10f : 0.14f;
					const float maximumDistance = twoHandSupportActive ? 0.90f : 0.78f;
					supportPoseValid = handDistance >= minimumDistance
						&& handDistance <= maximumDistance;

					if (supportPoseValid)
					{
						// Remove sub-degree optical/controller noise without restoring the
						// old dead-zone or delayed steering model. Engagement and deliberate
						// direction changes remain immediate; only tiny frame-to-frame motion
						// receives a short adaptive low-pass.
						if (!twoHandFilteredSupportDirectionValid || !twoHandSupportActive)
						{
							twoHandFilteredSupportDirection = supportDirection;
							twoHandFilteredSupportDirectionValid = true;
						}
						else
						{
							const float directionDot = glm::clamp(glm::dot(
								twoHandFilteredSupportDirection, supportDirection), -1.0f, 1.0f);
							const float directionDeltaDegrees = std::acos(directionDot) * 57.2957795f;
							const bool sniperStability = currentWeaponEquipped == Sniper;
							const float deliberate = sniperStability
								? glm::clamp((directionDeltaDegrees - 0.30f) / 1.70f, 0.0f, 1.0f)
								: glm::clamp((directionDeltaDegrees - 0.15f) / 1.35f, 0.0f, 1.0f);
							// Sniper optics magnify tiny controller noise. Use a much calmer low-angle
							// response for weapon 34, while large deliberate movement ramps back to
							// near-normal speed. Other two-hand weapons retain their existing feel.
							const float responseRate = sniperStability
								? 8.0f + (82.0f * deliberate)
								: 24.0f + (66.0f * deliberate);
							const float alpha = glm::clamp(1.0f - std::exp(-responseRate * safeDelta), 0.0f, 1.0f);
							twoHandFilteredSupportDirection = NormalizeOrZero(glm::mix(
								twoHandFilteredSupportDirection, supportDirection, alpha));
						}
						if (glm::length(twoHandFilteredSupportDirection) > 0.0001f)
							supportDirection = twoHandFilteredSupportDirection;

						glm::fquat desiredComponentRotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
						bool desiredComponentRotationValid = false;
						// Solve the actual calibrated contact segment, not a fictional
						// barrel axis through both raw controller centers. The UEVR
						// location offset represents weapon-root displacement from the
						// primary controller, so its negated component-space value is the
						// controller contact in weapon-local coordinates.
						GripCalibrationTransform calibratedSupportContact;
						const int supportHand = 1 - primaryHand;
						const bool hasCalibratedSupportContact = settingsManager->enableGripCalibration
							&& GetSupportContactForHand(supportHand, calibratedSupportContact);
						const glm::fvec3 primaryContactLocal = -VRSpaceToComponentSpace(
							GetWeaponGripPositionOffset(primaryHand));
						const glm::fvec3 localContactDelta = hasCalibratedSupportContact
							? calibratedSupportContact.position - primaryContactLocal
							: glm::fvec3(1.0f, 0.0f, 0.0f);
						supportContactRecordUsed = hasCalibratedSupportContact;
						supportContactSpan = glm::length(localContactDelta);
						glm::fvec3 localContactDirection = hasCalibratedSupportContact
							? NormalizeOrZero(localContactDelta)
							: glm::fvec3(1.0f, 0.0f, 0.0f);
						if (glm::length(localContactDirection) <= 0.0001f)
							localContactDirection = glm::fvec3(1.0f, 0.0f, 0.0f);
						const glm::fvec3 neutralContactDirection = NormalizeOrZero(
							baseComponentRotation * localContactDirection);
						// The segment is directed from primary to support. Rejecting the
						// opposite hemisphere prevents a support-hand crossover flip.
						if (glm::dot(supportDirection, neutralContactDirection) < 0.0f)
							supportDirection = -supportDirection;
						forwardDot = glm::dot(neutralContactDirection, supportDirection);
						const glm::fquat contactCorrection = RotationBetweenDirections(
							neutralContactDirection, supportDirection);
						desiredComponentRotation = glm::normalize(
							contactCorrection * baseComponentRotation);
						desiredComponentRotationValid = IsFiniteQuaternion(desiredComponentRotation)
							&& glm::length(desiredComponentRotation) > 0.0001f;
						if (desiredComponentRotationValid)
							desiredForward = NormalizeOrZero(
								desiredComponentRotation * glm::fvec3(1.0f, 0.0f, 0.0f));
						supportCalibrationUsed = desiredComponentRotationValid;

						if (desiredComponentRotationValid)
						{
							twoHandStableTargetRotation = desiredComponentRotation;
							twoHandStableTargetValid = true;
							twoHandWristPrimaryHand = static_cast<int8_t>(primaryHand);
							twoHandWristPrimaryPoseRotation = primaryPoseRotation;
							twoHandWristRotationDelta = glm::normalize(
								desiredComponentRotation * glm::inverse(baseComponentRotation));
							twoHandWristOverrideActive = IsFiniteQuaternion(twoHandWristRotationDelta)
								&& glm::length(twoHandWristRotationDelta) > 0.0001f;
							// UObjectHook applies adjusted = H * inverse(O), then converts
							// adjusted through euler_angles_from_steamvr. Let O0 be the
							// normal offset and R be the fixed weapon-mesh relation captured
							// as twoHandWeaponRelativeToPrimary. The neutral relation is
							// C0 = H * O0^-1 * R, so the target solve is
							// O = O0 * Rcap * inverse(D) * H. Keeping every factor in
							// component space prevents the old neutral-basis residual.
							const glm::fquat defaultOffsetComponent =
								VRQuaternionToComponentSpace(neutralOffset);
							const glm::fquat targetOffsetComponent = glm::normalize(
								defaultOffsetComponent * twoHandWeaponRelativeToPrimary
								* glm::inverse(desiredComponentRotation) * primaryPoseRotation);
							const glm::fquat candidateTargetOffset = ComponentQuaternionToVRSpace(targetOffsetComponent);
							if (IsFiniteQuaternion(candidateTargetOffset)
								&& glm::length(candidateTargetOffset) > 0.0001f)
							{
								targetOffset = candidateTargetOffset;
								targetOffsetValid = true;
							}
						}
					}
			}
		}
	}
	}

	const bool wasSupportActive = twoHandSupportActive;
	twoHandSupportActive = supportPoseValid && targetOffsetValid && supportCalibrationUsed;
	if (wasSupportActive != twoHandSupportActive)
	{
		uevr::API::get()->log_info(
			"[TwoHand] %s primary=%d contact=%s spanCm=%.2f handDistanceM=%.3f",
			twoHandSupportActive ? "engaged" : "released",
			static_cast<int>(twoHandPrimaryHand.load(std::memory_order_acquire)),
			supportContactRecordUsed ? "calibrated" : "barrel-axis-fallback",
			supportContactSpan, handDistance);
	}
	if (!twoHandSupportActive)
	{
		twoHandWristOverrideActive = false;
		twoHandWristPrimaryHand = -1;
		twoHandWristRotationDelta = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
		twoHandWristPrimaryPoseRotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	}
	if (twoHandSupportActive && IsFiniteVector(desiredForward)
		&& glm::length(desiredForward) > 0.0001f)
	{
		twoHandLastDesiredForward = NormalizeOrZero(desiredForward);
	}
	if (!twoHandSupportActive)
	{
		twoHandPostSettleElapsed = 0.0f;
		twoHandPostSettleLogged = false;
		twoHandLastDesiredForward = glm::fvec3(0.0f);
		twoHandFilteredSupportDirection = glm::fvec3(0.0f);
		twoHandFilteredSupportDirectionValid = false;
	}
	else if (!wasSupportActive)
	{
		twoHandPostSettleElapsed = 0.0f;
		twoHandPostSettleLogged = false;
	}
	if (wasSupportActive != twoHandSupportActive && settingsManager->debugInputLayerProbe)
	{
		uevr::API::get()->log_info(
			"[TwoHand] primary-wrist-follow=%s primary=%d",
			twoHandWristOverrideActive ? "active" : "restored",
			static_cast<int>(twoHandWristPrimaryHand));
		// This is diagnostic-only and transition-gated. It never participates in
		// neutral reconstruction, so it cannot reintroduce the feedback loop.
		if (firstWeaponMesh != nullptr && uevr::API::UObjectHook::exists(firstWeaponMesh))
		{
			Utilities::ParameterSingleVector3 actualForwardParams{};
			firstWeaponMesh->call_function(L"GetForwardVector", &actualForwardParams);
			const glm::fvec3 transitionActualForward = NormalizeOrZero(actualForwardParams.vec3Value);
			if (IsFiniteVector(transitionActualForward) && glm::length(transitionActualForward) > 0.0001f)
				diagnosticActualComponentForward = transitionActualForward;
		}

		uevr::API::get()->log_info(
			"[TwoHand] %s weapon=%i distance=%.3f forwardDot=%.3f desiredF=(%.3f %.3f %.3f) neutralF=(%.3f %.3f %.3f) actualF=(%.3f %.3f %.3f) targetUQ=(%.5f %.5f %.5f %.5f) rawDelta=(%.3f %.3f %.3f) convertedDelta=(%.3f %.3f %.3f) supportDir=(%.3f %.3f %.3f) supportRawDeg=%.2f supportAppliedDeg=%.2f supportCalibrated=%s rawGripQ=(%.5f %.5f %.5f %.5f) rawAimQ=(%.5f %.5f %.5f %.5f) trackingQ=(%.5f %.5f %.5f %.5f) componentPrimaryQ=(%.5f %.5f %.5f %.5f) latchedHand=%d configuredHand=%d selectedHand=%d trackingValid=%s",
			twoHandSupportActive ? "engaged" : "released",
			static_cast<int>(currentWeaponEquipped), handDistance, forwardDot,
			desiredForward.x, desiredForward.y, desiredForward.z,
			primaryForward.x, primaryForward.y, primaryForward.z,
			diagnosticActualComponentForward.x, diagnosticActualComponentForward.y, diagnosticActualComponentForward.z,
			targetOffset.w, targetOffset.x, targetOffset.y, targetOffset.z,
			rawHandVector.x, rawHandVector.y, rawHandVector.z,
			convertedHandVector.x, convertedHandVector.y, convertedHandVector.z,
			supportDirection.x, supportDirection.y, supportDirection.z,
			supportRawCorrectionDegrees, supportAppliedCorrectionDegrees,
			supportCalibrationUsed ? "true" : "false",
			diagnosticRawPrimaryGripRotation.w, diagnosticRawPrimaryGripRotation.x,
			diagnosticRawPrimaryGripRotation.y, diagnosticRawPrimaryGripRotation.z,
			diagnosticRawPrimaryAimRotation.w, diagnosticRawPrimaryAimRotation.x,
			diagnosticRawPrimaryAimRotation.y, diagnosticRawPrimaryAimRotation.z,
			diagnosticTrackingRotation.w, diagnosticTrackingRotation.x,
			diagnosticTrackingRotation.y, diagnosticTrackingRotation.z,
			diagnosticPrimaryPoseRotation.w, diagnosticPrimaryPoseRotation.x,
			diagnosticPrimaryPoseRotation.y, diagnosticPrimaryPoseRotation.z,
			static_cast<int>(twoHandPrimaryHand.load(std::memory_order_acquire)),
			motionConfiguredFirstHand, diagnosticSelectedHand,
			diagnosticTrackingRotationValid ? "true" : "false");
	}

	if (!twoHandOffsetApplied)
		twoHandRotationOffset = neutralOffset;

	// Apply a valid support solve immediately. There is intentionally no
	// engagement-neutral capture, dead zone, influence cap, or frame smoothing:
	// the barrel follows the current primary-to-support controller line one-to-one.
	if (twoHandSupportActive)
	{
		if (glm::dot(twoHandRotationOffset, targetOffset) < 0.0f)
			targetOffset = -targetOffset;
		twoHandRotationOffset = glm::normalize(targetOffset);
	}
	else if (twoHandOffsetApplied)
	{
		twoHandRotationOffset = neutralOffset;
	}

	if (!twoHandSupportActive && !twoHandOffsetApplied)
		return;
	if (firstWeaponMesh == nullptr || !uevr::API::UObjectHook::exists(firstWeaponMesh))
	{
		RestoreTwoHandRotationOffset();
		return;
	}

	auto motionState = uevr::API::UObjectHook::get_motion_controller_state(firstWeaponMesh);
	if (motionState == nullptr)
	{
		RestoreTwoHandRotationOffset();
		return;
	}

	const UEVR_Quaternionf appliedOffset = {
		twoHandRotationOffset.w, twoHandRotationOffset.x,
		twoHandRotationOffset.y, twoHandRotationOffset.z
	};
	motionState->set_rotation_offset(&appliedOffset);
	motionState->set_permanent(true);
	twoHandOffsetApplied = true;
	twoHandAppliedWeaponMesh = firstWeaponMesh;

	// The transition diagnostic above is intentionally pre-blend.  This one
	// samples only once after the blend has had time to settle, so it can show
	// whether the component actually converged to the requested direction.
	if (twoHandSupportActive && !twoHandPostSettleLogged)
	{
		twoHandPostSettleElapsed += safeDelta;
		if (twoHandPostSettleElapsed >= 0.5f)
		{
			glm::fvec3 settledActualForward(0.0f);
			if (uevr::API::UObjectHook::exists(firstWeaponMesh))
			{
				Utilities::ParameterSingleVector3 settledForwardParams{};
				firstWeaponMesh->call_function(L"GetForwardVector", &settledForwardParams);
				settledActualForward = NormalizeOrZero(settledForwardParams.vec3Value);
			}
			if (settingsManager->debugInputLayerProbe)
			{
				uevr::API::get()->log_info(
					"[TwoHand] post-settle desiredF=(%.3f %.3f %.3f) actualF=(%.3f %.3f %.3f) residualDeg=%.2f",
					twoHandLastDesiredForward.x, twoHandLastDesiredForward.y, twoHandLastDesiredForward.z,
					settledActualForward.x, settledActualForward.y, settledActualForward.z,
					AngleDegreesOrZero(twoHandLastDesiredForward, settledActualForward));
			}
			twoHandPostSettleLogged = true;
		}
	}

	if (!twoHandSupportActive && std::abs(glm::dot(twoHandRotationOffset, neutralOffset)) > 0.99999f)
	{
		const bool logReleaseComplete = settingsManager->debugInputLayerProbe;
		RestoreTwoHandRotationOffset();
		if (logReleaseComplete)
			uevr::API::get()->log_info("%s", "[TwoHand] release blend complete");
	}
}

void WeaponManager::SetComponentVisibility(uevr::API::UObject* object, bool visible,
	bool propagateToChildren)
{
	if (object == nullptr || !uevr::API::UObjectHook::exists(object))
		return;

	auto objectClass = object->get_class();
	if (objectClass == nullptr)
		return;

	ParameterSetVisibility params{};
	params.bNewVisibility = visible;
	params.bPropagateToChildren = propagateToChildren;

	if (objectClass->find_function(L"SetVisibility") != nullptr)
		object->call_function(L"SetVisibility", &params);

	// Keep the direct property in sync for meshes whose native visibility call
	// is bypassed by GTA's own update path. This deliberately never edits scale.
	if (objectClass->find_property(L"bVisible") != nullptr)
		object->set_bool_property(L"bVisible", visible);
}

const char* WeaponManager::RuntimeHandRoleName(RuntimeHandRole role) const
{
	switch (role)
	{
	case RuntimeHandRole::FreeTracked: return "free";
	case RuntimeHandRole::PrimaryWeapon: return "primary";
	case RuntimeHandRole::SupportWeapon: return "support";
	case RuntimeHandRole::VehicleNative: return "vehicle-native";
	case RuntimeHandRole::VehiclePrimary: return "vehicle-primary";
	case RuntimeHandRole::CalibrationPrimary: return "calibration-primary";
	case RuntimeHandRole::CalibrationSupport: return "calibration-support";
	case RuntimeHandRole::Inactive:
	default: return "inactive";
	}
}

void WeaponManager::ResetRuntimeHandState(const char* reason, bool restoreTransient, bool cancelCalibration)
{
	const bool hadRuntimeState = !runtimeHandStateReset
		|| runtimeHandStates[0].role != RuntimeHandRole::Inactive
		|| runtimeHandStates[1].role != RuntimeHandRole::Inactive;
	const bool hadCalibration = IsGripCalibrationActive();
	const bool hadCalibrationInput = gripCalibrationButtonMask.load(std::memory_order_acquire) != 0
		|| gripCalibrationProcessedButtonMask != 0;
	const bool hadTransient = freeAimSupportHandAttached || freeAimPrimaryHandAttached
		|| twoHandOffsetApplied;
	if (!hadRuntimeState && !hadCalibration && !hadCalibrationInput && !hadTransient)
		return;

	if (restoreTransient)
	{
		RestoreSupportFakeHandAttachment();
		RestorePrimaryFakeHandAttachment();
		RestoreTwoHandRotationOffset();
	}
	if (cancelCalibration)
	{
		gripCalibrationSessions = {};
		gripCalibrationButtonMask.store(0, std::memory_order_release);
		gripCalibrationProcessedButtonMask = 0;
		motionConfiguredFirstWeaponMesh = nullptr;
		motionConfiguredFirstHand = -1;
		motionConfiguredFirstCalibrationRole = -1;
	}

	++runtimeHandGeneration;
	for (auto& state : runtimeHandStates)
	{
		state = RuntimeHandState{};
		state.generation = runtimeHandGeneration;
	}
	runtimeHandStateReset = true;
	uevr::API::get()->log_info(
		"[HandRuntime] reset generation=%u reason=%s calibration=%s transient=%s",
		runtimeHandGeneration, reason != nullptr ? reason : "unspecified",
		hadCalibration ? "true" : "false", hadTransient ? "true" : "false");
}

void WeaponManager::RefreshRuntimeHandRoles(const char* reason)
{
	if (playerManager == nullptr || !playerManager->isInControl)
	{
		ResetRuntimeHandState("control-loss", true, true);
		return;
	}
	if (playerManager->weaponWheelEnabled)
	{
		ResetRuntimeHandState("weapon-wheel", true, true);
		return;
	}

	const bool vehicleFreeAim = playerManager->isInVehicle && IsVehicleFreeAimActive();
	if (playerManager->isInVehicle && !vehicleFreeAim)
	{
		ResetRuntimeHandState("vehicle-transition", true, true);
		return;
	}
	const bool weaponExpected = currentWeaponEquipped != Unarmed;
	if (weaponExpected && (firstWeaponMesh == nullptr
		|| !uevr::API::UObjectHook::exists(firstWeaponMesh)))
	{
		ResetRuntimeHandState("weapon-missing", true, true);
		return;
	}

	const uint8_t grips = gripStateMask.load(std::memory_order_acquire);
	std::array<RuntimeHandState, 2> desired{};
	for (int hand = 0; hand < 2; ++hand)
	{
		desired[static_cast<size_t>(hand)].weaponId = -1;
		desired[static_cast<size_t>(hand)].gripHeld = (grips & (1U << hand)) != 0;
		if (freeAimFakeHandsActive)
			desired[static_cast<size_t>(hand)].role = RuntimeHandRole::FreeTracked;
	}

	if (vehicleFreeAim)
	{
		desired[0].role = RuntimeHandRole::VehicleNative;
		desired[1].role = RuntimeHandRole::VehiclePrimary;
		desired[1].weaponId = static_cast<int>(currentWeaponEquipped);
	}
	else
	{
		int primaryHand = magneticGripHand;
		if (primaryHand < 0 && motionWeaponTrackingEnabled)
			primaryHand = motionConfiguredFirstHand;
		if (primaryHand >= 0 && primaryHand <= 1 && motionWeaponTrackingEnabled)
		{
			desired[static_cast<size_t>(primaryHand)].role = RuntimeHandRole::PrimaryWeapon;
			desired[static_cast<size_t>(primaryHand)].weaponId = static_cast<int>(currentWeaponEquipped);
			const int latchedPrimary = twoHandPrimaryHand.load(std::memory_order_acquire);
			if (grips == 3U && latchedPrimary == primaryHand)
			{
				const int supportHand = 1 - primaryHand;
				desired[static_cast<size_t>(supportHand)].role = RuntimeHandRole::SupportWeapon;
				desired[static_cast<size_t>(supportHand)].weaponId = static_cast<int>(currentWeaponEquipped);
			}
		}
	}

	for (int hand = 0; hand < 2; ++hand)
	{
		const auto& session = gripCalibrationSessions[static_cast<size_t>(hand)];
		if (!session.active)
			continue;
		desired[static_cast<size_t>(hand)].role = session.record == GripCalibrationRecord::SupportContact
			? RuntimeHandRole::CalibrationSupport : RuntimeHandRole::CalibrationPrimary;
		desired[static_cast<size_t>(hand)].weaponId = session.weaponId;
	}

	bool changed = runtimeHandStateReset;
	for (int hand = 0; hand < 2 && !changed; ++hand)
	{
		const auto& current = runtimeHandStates[static_cast<size_t>(hand)];
		const auto& next = desired[static_cast<size_t>(hand)];
		changed = current.role != next.role || current.weaponId != next.weaponId
			|| current.gripHeld != next.gripHeld;
	}
	if (!changed)
		return;

	++runtimeHandGeneration;
	for (auto& state : desired)
		state.generation = runtimeHandGeneration;
	runtimeHandStates = desired;
	runtimeHandStateReset = false;
	uevr::API::get()->log_info(
		"[HandRuntime] transition generation=%u reason=%s weapon=%d grips=%u left=%s right=%s",
		runtimeHandGeneration, reason != nullptr ? reason : "runtime",
		static_cast<int>(currentWeaponEquipped), static_cast<unsigned int>(grips),
		RuntimeHandRoleName(runtimeHandStates[0].role), RuntimeHandRoleName(runtimeHandStates[1].role));
}

const char* WeaponManager::GripCalibrationRecordName(GripCalibrationRecord record) const
{
	return record == GripCalibrationRecord::SupportContact ? "support-contact" : "primary-grip";
}

int WeaponManager::GripCalibrationRecordIndex(GripCalibrationRecord record) const
{
	const int index = static_cast<int>(record);
	return index >= 0 && index < static_cast<int>(GripCalibrationRecordCount) ? index : -1;
}

bool WeaponManager::IsGripCalibrationEligible(int controllerHand) const
{
	if (!settingsManager->enableGripCalibration || controllerHand < 0 || controllerHand > 1)
		return false;
	const uint8_t mask = gripStateMask.load(std::memory_order_acquire);
	const bool meleeWeapon = currentWeaponEquipped >= BrassKnuckles
		&& currentWeaponEquipped <= Cane;
	const bool throwable = currentWeaponEquipped >= Grenade
		&& currentWeaponEquipped <= Molotov;
	const bool firearm = currentWeaponEquipped >= Pistol
		&& currentWeaponEquipped <= Minigun;
	const bool controllerHeldUtility = IsControllerHeldUtility();
	if ((mask & (1U << controllerHand)) == 0
		|| !playerManager->isInControl || playerManager->isInVehicle
		|| playerManager->weaponWheelEnabled || (!meleeWeapon && !throwable && !firearm && !controllerHeldUtility)
		// Melee calibration is deliberately single-hand until its two-hand
		// semantics exist; firearm support keeps the existing support path.
		|| ((meleeWeapon || throwable || controllerHeldUtility) && mask == 3U) || firstWeaponMesh == nullptr
		|| !uevr::API::UObjectHook::exists(firstWeaponMesh))
		return false;
	const bool supportHand = mask == 3U
		&& twoHandPrimaryHand.load(std::memory_order_acquire) >= 0
		&& controllerHand != twoHandPrimaryHand.load(std::memory_order_acquire);
	return !supportHand || freeAimFakeHandsActive;
}

bool WeaponManager::ReadGripCalibrationTransform(int weaponId, GripCalibrationRecord record,
	int requestedHand, GripCalibrationTransform& result, int* sourceHand) const
{
	result = GripCalibrationTransform{};
	if (sourceHand != nullptr)
		*sourceHand = -1;
	const int slot = GripCalibrationRecordIndex(record);
	if (weaponId < 0 || slot < 0)
		return false;
	const auto weaponIt = gripCalibrationTransforms.find(weaponId);
	if (weaponIt == gripCalibrationTransforms.end())
		return false;
	const GripCalibrationTransform* entry = nullptr;
	int resolvedHand = -1;
	if (record == GripCalibrationRecord::PrimaryGrip)
	{
		if (requestedHand < 0 || requestedHand > 1)
			return false;
		entry = &weaponIt->second.primaryGrip[static_cast<size_t>(requestedHand)];
		resolvedHand = requestedHand;
		if (!entry->valid)
		{
			resolvedHand = 1 - requestedHand;
			entry = &weaponIt->second.primaryGrip[static_cast<size_t>(resolvedHand)];
		}
	}
	else
	{
		if (requestedHand < 0 || requestedHand > 1)
			return false;
		entry = &weaponIt->second.supportContact[static_cast<size_t>(requestedHand)];
		resolvedHand = requestedHand;
		if (!entry->valid)
		{
			resolvedHand = 1 - requestedHand;
			entry = &weaponIt->second.supportContact[static_cast<size_t>(resolvedHand)];
		}
	}
	if (entry == nullptr)
		return false;
	if (!entry->valid || !IsFiniteVector(entry->position) || glm::length(entry->position) > 500.0f
		|| !IsFiniteQuaternion(entry->rotation) || glm::length(entry->rotation) < 0.5f)
		return false;
	result = *entry;
	result.rotation = glm::normalize(result.rotation);
	if (sourceHand != nullptr)
		*sourceHand = resolvedHand;
	return true;
}

WeaponManager::GripCalibrationTransform WeaponManager::MirrorCanonicalTransform(
	const GripCalibrationTransform& value) const
{
	GripCalibrationTransform mirrored = value;
	if (!value.valid)
		return mirrored;
	// Canonical axes are weapon/component X=forward, Y=right, Z=up. Reflect
	// across the lateral Y=0 plane and conjugate the rotation matrix so the
	// result remains a proper rotation rather than a guessed quaternion flip.
	mirrored.position.y = -mirrored.position.y;
	mirrored.rotation = MirrorLateralRotation(value.rotation);
	mirrored.valid = IsFiniteVector(mirrored.position) && IsFiniteQuaternion(mirrored.rotation)
		&& glm::length(mirrored.rotation) > 0.5f;
	return mirrored;
}

glm::fquat WeaponManager::MirrorLateralRotation(const glm::fquat& value) const
{
	if (!IsFiniteQuaternion(value) || glm::length(value) <= 0.0001f)
		return glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	glm::fmat3 mirror(1.0f);
	mirror[1][1] = -1.0f;
	const glm::fmat3 reflected = mirror * glm::mat3_cast(glm::normalize(value)) * mirror;
	return glm::normalize(glm::quat_cast(reflected));
}

bool WeaponManager::GetCanonicalPrimaryGripForHand(int hand, GripCalibrationTransform& result) const
{
	result = GripCalibrationTransform{};
	if (hand != 0 && hand != 1)
		return false;
	GripCalibrationTransform stored;
	int sourceHand = -1;
	if (!ReadGripCalibrationTransform(static_cast<int>(currentWeaponEquipped),
		GripCalibrationRecord::PrimaryGrip, hand, stored, &sourceHand))
		return false;
	if (stored.palmFramed)
		return false;

	// v5 stored a raw canonical controller offset. Convert that legacy value to
	// an orientation-only palm relation before reconstructing either hand. v6
	// stores the relation directly. Both paths deliberately leave position as
	// the compact controller/weapon offset; the old full skeleton translation
	// is never reintroduced.
	glm::fquat canonicalPalmRelation = stored.rotation;
	if (!stored.anatomicalFramed)
	{
		ControllerPalmAdapter sourceAdapter;
		if (!ReadControllerPalmAdapter(sourceHand, sourceAdapter))
		{
			result = stored;
			if (sourceHand != hand)
				result.rotation = MirrorLateralRotation(result.rotation);
			return result.valid;
		}
		const glm::fquat legacyRawRotation = ComponentQuaternionToVRSpace(stored.rotation);
		const glm::fquat relationVR = glm::normalize(
			sourceAdapter.rotation * glm::inverse(legacyRawRotation));
		canonicalPalmRelation = VRQuaternionToComponentSpace(relationVR);
	}
	if (!IsFiniteQuaternion(canonicalPalmRelation)
		|| glm::length(canonicalPalmRelation) <= 0.0001f)
		return false;
	// Exact left/right records remain independent. Reflection is used only when
	// the requested hand has no record and the opposite hand seeds it.
	if (sourceHand != hand)
		canonicalPalmRelation = MirrorLateralRotation(canonicalPalmRelation);

	ControllerPalmAdapter destinationAdapter;
	if (!ReadControllerPalmAdapter(hand, destinationAdapter))
	{
		if (stored.anatomicalFramed)
			return false;
		result = stored;
		if (hand == 0)
			result.rotation = MirrorLateralRotation(result.rotation);
		return result.valid;
	}
	const glm::fquat targetRelationVR = ComponentQuaternionToVRSpace(canonicalPalmRelation);
	const glm::fquat targetRawRotation = glm::normalize(
		glm::inverse(targetRelationVR) * destinationAdapter.rotation);
	if (!IsFiniteQuaternion(targetRawRotation) || glm::length(targetRawRotation) <= 0.0001f)
		return false;
	result = stored;
	// OpenXR's controller-relative location already carries the left/right
	// controller basis. Only the anatomical orientation relation is mirrored;
	// reflecting this position again produces the observed double horizontal
	// offset on the opposite primary hand.
	result.position = stored.position;
	result.rotation = VRQuaternionToComponentSpace(targetRawRotation);
	result.palmFramed = false;
	result.anatomicalFramed = false;
	result.valid = IsFiniteVector(result.position) && IsFiniteQuaternion(result.rotation)
		&& glm::length(result.rotation) > 0.5f;
	return result.valid;
}

bool WeaponManager::GetSupportContactForHand(int hand, GripCalibrationTransform& result) const
{
	result = GripCalibrationTransform{};
	if (hand != 0 && hand != 1)
		return false;
	int sourceHand = -1;
	if (!ReadGripCalibrationTransform(static_cast<int>(currentWeaponEquipped),
		GripCalibrationRecord::SupportContact, hand, result, &sourceHand))
		return false;
	// Support contacts are anatomical and must be calibrated independently.
	// A mirrored opposite-hand record is not safe to attach to the barrel.
	if (sourceHand != hand)
		return false;
	return result.valid;
}

bool WeaponManager::ReadGripCalibrationFile(const std::string& path, GripCalibrationMap& result,
	int& loaded, int& legacyIgnored) const
{
	result.clear();
	loaded = 0;
	legacyIgnored = 0;
	std::ifstream input(path);
	if (!input.is_open())
		return false;

	std::string line;
	while (std::getline(input, line))
	{
		std::istringstream row(line);
		std::string tag;
		row >> tag;
		if (tag.empty() || tag[0] == '#')
			continue;
		if (tag != "entry")
			continue;

		std::vector<std::string> fields;
		std::string field;
		while (row >> field)
			fields.push_back(field);

		int weaponId = -1;
		int recordValue = -1;
		int storageHand = -1;
		int role = -1;
		int targetValue = -1;
		int controllerHand = -1;
		GripCalibrationTransform entry;
		try
		{
			// v8 stores independent primary and support records for hand 0/1.
			// v7 support hand=-1 migrates into the left slot and may seed right.
			if (fields.size() == 12 && (fields[0] == "7" || fields[0] == "8"))
			{
				const int schemaVersion = std::stoi(fields[0]);
				weaponId = std::stoi(fields[1]);
				recordValue = std::stoi(fields[2]);
				storageHand = std::stoi(fields[3]);
				if (schemaVersion == 7
					&& recordValue == static_cast<int>(GripCalibrationRecord::SupportContact)
					&& storageHand == -1)
					storageHand = 0;
				const int anatomicalFrameValue = std::stoi(fields[4]);
				entry.position.x = std::stof(fields[5]);
				entry.position.y = std::stof(fields[6]);
				entry.position.z = std::stof(fields[7]);
				entry.rotation.w = std::stof(fields[8]);
				entry.rotation.x = std::stof(fields[9]);
				entry.rotation.y = std::stof(fields[10]);
				entry.rotation.z = std::stof(fields[11]);
				entry.anatomicalFramed = anatomicalFrameValue == 1
					&& recordValue == static_cast<int>(GripCalibrationRecord::PrimaryGrip);
			}
			// v4/v6 and v3 stored one canonical right-primary record. Load it into
			// the right slot; the left slot may use it only as a runtime seed.
			else if (fields.size() == 11 && (fields[0] == "4" || fields[0] == "6"))
			{
				const int schemaVersion = std::stoi(fields[0]);
				weaponId = std::stoi(fields[1]);
				recordValue = std::stoi(fields[2]);
				storageHand = recordValue == static_cast<int>(GripCalibrationRecord::PrimaryGrip) ? 1 : -1;
				const int frameValue = std::stoi(fields[3]);
				entry.position.x = std::stof(fields[4]);
				entry.position.y = std::stof(fields[5]);
				entry.position.z = std::stof(fields[6]);
				entry.rotation.w = std::stof(fields[7]);
				entry.rotation.x = std::stof(fields[8]);
				entry.rotation.y = std::stof(fields[9]);
				entry.rotation.z = std::stof(fields[10]);
				entry.palmFramed = schemaVersion == 4 && frameValue == 1
					&& recordValue == static_cast<int>(GripCalibrationRecord::PrimaryGrip);
				entry.anatomicalFramed = schemaVersion == 6 && frameValue == 1
					&& recordValue == static_cast<int>(GripCalibrationRecord::PrimaryGrip);
			}
			else if (fields.size() == 10 && fields[0] == "3")
			{
				weaponId = std::stoi(fields[1]);
				recordValue = std::stoi(fields[2]);
				storageHand = recordValue == static_cast<int>(GripCalibrationRecord::PrimaryGrip) ? 1 : -1;
				entry.position.x = std::stof(fields[3]);
				entry.position.y = std::stof(fields[4]);
				entry.position.z = std::stof(fields[5]);
				entry.rotation.w = std::stof(fields[6]);
				entry.rotation.x = std::stof(fields[7]);
				entry.rotation.y = std::stof(fields[8]);
				entry.rotation.z = std::stof(fields[9]);
			}
			// Unambiguous v1/v2 solo primaries retain the side that captured them.
			else if (fields.size() == 9 || fields.size() == 11)
			{
				weaponId = std::stoi(fields[0]);
				role = std::stoi(fields[1]);
				if (fields.size() == 9)
				{
					controllerHand = role;
					entry.position.x = std::stof(fields[2]);
					entry.position.y = std::stof(fields[3]);
					entry.position.z = std::stof(fields[4]);
					entry.rotation.w = std::stof(fields[5]);
					entry.rotation.x = std::stof(fields[6]);
					entry.rotation.y = std::stof(fields[7]);
					entry.rotation.z = std::stof(fields[8]);
				}
				else
				{
					targetValue = std::stoi(fields[2]);
					controllerHand = std::stoi(fields[3]);
					entry.position.x = std::stof(fields[4]);
					entry.position.y = std::stof(fields[5]);
					entry.position.z = std::stof(fields[6]);
					entry.rotation.w = std::stof(fields[7]);
					entry.rotation.x = std::stof(fields[8]);
					entry.rotation.y = std::stof(fields[9]);
					entry.rotation.z = std::stof(fields[10]);
				}
				if (role < 0 || role > 1 || controllerHand != role
					|| (fields.size() == 11 && targetValue != 0))
				{
					++legacyIgnored;
					continue;
				}
				recordValue = static_cast<int>(GripCalibrationRecord::PrimaryGrip);
				storageHand = controllerHand;
				entry.position = VRSpaceToComponentSpace(entry.position);
				entry.rotation = VRQuaternionToComponentSpace(entry.rotation);
			}
			else
			{
				return false;
			}
		}
		catch (...)
		{
			return false;
		}

		const auto record = static_cast<GripCalibrationRecord>(recordValue);
		const int slot = GripCalibrationRecordIndex(record);
		if (record == GripCalibrationRecord::SupportContact && storageHand == -1)
			storageHand = 0;
		if (record == GripCalibrationRecord::PrimaryGrip && entry.palmFramed)
		{
			++legacyIgnored;
			continue;
		}
		if (weaponId < 0 || slot < 0
			|| storageHand < 0 || storageHand > 1
			|| !IsFiniteVector(entry.position) || glm::length(entry.position) > 500.0f
			|| !IsFiniteQuaternion(entry.rotation) || glm::length(entry.rotation) < 0.5f)
		{
			++legacyIgnored;
			continue;
		}
		entry.rotation = glm::normalize(entry.rotation);
		entry.valid = true;
		auto& weapon = result[weaponId];
		if (record == GripCalibrationRecord::PrimaryGrip)
			weapon.primaryGrip[static_cast<size_t>(storageHand)] = entry;
		else
			weapon.supportContact[static_cast<size_t>(storageHand)] = entry;
		++loaded;
	}
	return input.eof() || input.good();
}

bool WeaponManager::WriteGripCalibrationFile(const std::string& path,
	const GripCalibrationMap& values) const
{
	std::ofstream output(path, std::ios::trunc);
	if (!output.is_open())
		return false;
	output << "# UEVR GTA SA DE grip calibration per-hand-v8\n";
	output << "# entry 8 weaponId record hand anatomicalFramed posX posY posZ rotW rotX rotY rotZ\n";
	output << "# record 0=primary-grip hand 0/1, record 1=support-contact hand 0/1\n";
	output.setf(std::ios::fixed);
	output.precision(6);
	std::vector<int> weaponIds;
	weaponIds.reserve(values.size());
	for (const auto& [weaponId, unused] : values)
		weaponIds.push_back(weaponId);
	std::sort(weaponIds.begin(), weaponIds.end());
	for (const int weaponId : weaponIds)
	{
		const auto& weapon = values.at(weaponId);
		for (int hand = 0; hand < 2; ++hand)
		{
			const auto& entry = weapon.primaryGrip[static_cast<size_t>(hand)];
			if (!entry.valid)
				continue;
			output << "entry 8 " << weaponId << ' '
				<< static_cast<int>(GripCalibrationRecord::PrimaryGrip) << ' ' << hand << ' '
				<< (entry.anatomicalFramed ? 1 : 0) << ' '
				<< entry.position.x << ' ' << entry.position.y << ' ' << entry.position.z << ' '
				<< entry.rotation.w << ' ' << entry.rotation.x << ' '
				<< entry.rotation.y << ' ' << entry.rotation.z << "\n";
		}
		for (int hand = 0; hand < 2; ++hand)
		{
			const auto& support = weapon.supportContact[static_cast<size_t>(hand)];
			if (!support.valid)
				continue;
			output << "entry 8 " << weaponId << ' '
				<< static_cast<int>(GripCalibrationRecord::SupportContact) << ' ' << hand << " 0 "
				<< support.position.x << ' ' << support.position.y << ' ' << support.position.z << ' '
				<< support.rotation.w << ' ' << support.rotation.x << ' '
				<< support.rotation.y << ' ' << support.rotation.z << "\n";
		}
	}
	output.flush();
	return output.good();
}

bool WeaponManager::ValidateGripCalibrationRoundTrip(const GripCalibrationMap& expected,
	const GripCalibrationMap& actual) const
{
	const auto equivalent = [](const GripCalibrationTransform& a, const GripCalibrationTransform& b) {
		if (a.valid != b.valid)
			return false;
		if (!a.valid)
			return true;
		if (a.palmFramed != b.palmFramed || a.anatomicalFramed != b.anatomicalFramed
			|| glm::length(a.position - b.position) > 0.00001f)
			return false;
		const glm::fquat qa = glm::normalize(a.rotation);
		const glm::fquat qb = glm::normalize(b.rotation);
		return std::abs(glm::dot(qa, qb)) >= 0.999999f;
	};
	size_t expectedRecords = 0;
	size_t actualRecords = 0;
	for (const auto& [weaponId, weapon] : expected)
	{
		const auto found = actual.find(weaponId);
		for (int hand = 0; hand < 2; ++hand)
		{
			const auto& entry = weapon.primaryGrip[static_cast<size_t>(hand)];
			if (entry.valid)
				++expectedRecords;
			if (found == actual.end() || !equivalent(entry,
				found->second.primaryGrip[static_cast<size_t>(hand)]))
				return false;
		}
		for (int hand = 0; hand < 2; ++hand)
		{
			const auto& support = weapon.supportContact[static_cast<size_t>(hand)];
			if (support.valid)
				++expectedRecords;
			if (found == actual.end() || !equivalent(support,
				found->second.supportContact[static_cast<size_t>(hand)]))
				return false;
		}
	}
	for (const auto& [unusedWeaponId, weapon] : actual)
	{
		for (const auto& entry : weapon.primaryGrip)
			if (entry.valid)
				++actualRecords;
		for (const auto& entry : weapon.supportContact)
			if (entry.valid)
				++actualRecords;
	}
	return expectedRecords == actualRecords;
}

void WeaponManager::LoadGripCalibration()
{
	if (gripCalibrationLoaded)
		return;
	gripCalibrationLoaded = true;
	const std::string path = settingsManager->GetGripCalibrationFilePath();
	if (path.empty())
		return;
	GripCalibrationMap loadedValues;
	int loaded = 0;
	int legacyIgnored = 0;
	if (!ReadGripCalibrationFile(path, loadedValues, loaded, legacyIgnored))
	{
		uevr::API::get()->log_info("[GripCalibration] mode=%s file=%s entries=0",
			settingsManager->enableGripCalibration ? "enabled" : "disabled", path.c_str());
		return;
	}
	gripCalibrationTransforms = std::move(loadedValues);
	uevr::API::get()->log_info("[GripCalibration] mode=%s file=%s schema=per-hand-v8 entries=%d legacy-ignored=%d",
		settingsManager->enableGripCalibration ? "enabled" : "disabled", path.c_str(), loaded, legacyIgnored);
}

bool WeaponManager::SaveGripCalibration()
{
	const std::string path = settingsManager->GetGripCalibrationFilePath();
	if (path.empty())
		return false;
	const std::filesystem::path target(path);
	const std::filesystem::path temporary = target.wstring() + L".tmp";
	std::error_code error;
	std::filesystem::remove(temporary, error);
	if (!WriteGripCalibrationFile(temporary.string(), gripCalibrationTransforms))
	{
		uevr::API::get()->log_warn("[GripCalibration] save failed stage=temp-write file=%s", path.c_str());
		std::filesystem::remove(temporary, error);
		return false;
	}

	GripCalibrationMap roundTrip;
	int roundTripLoaded = 0;
	int roundTripIgnored = 0;
	if (!ReadGripCalibrationFile(temporary.string(), roundTrip, roundTripLoaded, roundTripIgnored)
		|| roundTripIgnored != 0
		|| !ValidateGripCalibrationRoundTrip(gripCalibrationTransforms, roundTrip))
	{
		uevr::API::get()->log_warn("[GripCalibration] save failed stage=temp-validation file=%s", path.c_str());
		std::filesystem::remove(temporary, error);
		return false;
	}

	std::filesystem::path backup;
	if (std::filesystem::exists(target, error) && !error)
	{
		GripCalibrationMap previous;
		int previousLoaded = 0;
		int previousIgnored = 0;
		if (!ReadGripCalibrationFile(path, previous, previousLoaded, previousIgnored))
		{
			uevr::API::get()->log_warn("[GripCalibration] save failed stage=existing-validation file=%s", path.c_str());
			std::filesystem::remove(temporary, error);
			return false;
		}
		SYSTEMTIME now{};
		GetLocalTime(&now);
		std::wostringstream suffix;
		suffix << L".known-good-" << std::setfill(L'0')
			<< std::setw(4) << now.wYear << std::setw(2) << now.wMonth << std::setw(2) << now.wDay
			<< L'-' << std::setw(2) << now.wHour << std::setw(2) << now.wMinute
			<< std::setw(2) << now.wSecond << L'-' << std::setw(3) << now.wMilliseconds;
		backup = target.parent_path() /
			(target.stem().wstring() + suffix.str() + target.extension().wstring());
		std::filesystem::copy_file(target, backup,
			std::filesystem::copy_options::overwrite_existing, error);
		if (error)
		{
			uevr::API::get()->log_warn("[GripCalibration] save failed stage=backup error=%d file=%s",
				error.value(), path.c_str());
			std::filesystem::remove(temporary, error);
			return false;
		}
	}

	if (!MoveFileExW(temporary.c_str(), target.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		const DWORD moveError = GetLastError();
		uevr::API::get()->log_warn("[GripCalibration] save failed stage=atomic-replace error=%lu file=%s",
			static_cast<unsigned long>(moveError), path.c_str());
		std::filesystem::remove(temporary, error);
		return false;
	}
	uevr::API::get()->log_info("[GripCalibration] save committed schema=per-hand-v8 entries=%d backup=%s",
		roundTripLoaded, backup.empty() ? "none" : backup.string().c_str());
	return true;
}

bool WeaponManager::ReadControllerCalibrationPose(int controllerHand, glm::fvec3& gripPosition, glm::fquat& aimRotation) const
{
	gripPosition = glm::fvec3(0.0f);
	aimRotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	const auto controllerIndex = controllerHand == 0
		? uevr::API::VR::get_left_controller_index()
		: controllerHand == 1 ? uevr::API::VR::get_right_controller_index() : -1;
	if (controllerIndex < 0)
		return false;
	const auto gripPose = uevr::API::VR::get_grip_pose(controllerIndex);
	const auto aimPose = uevr::API::VR::get_aim_pose(controllerIndex);
	gripPosition = glm::fvec3(gripPose.position.x, gripPose.position.y, gripPose.position.z);
	aimRotation = glm::normalize(glm::fquat::wxyz(
		aimPose.rotation.w, aimPose.rotation.x, aimPose.rotation.y, aimPose.rotation.z));
	return IsFiniteVector(gripPosition) && IsFiniteQuaternion(aimRotation)
		&& glm::length(aimRotation) > 0.5f;
}

bool WeaponManager::ReadCurrentWeaponWorldPose(glm::fvec3& position, Utilities::FRotator& rotation) const
{
	position = glm::fvec3(0.0f);
	rotation = Utilities::FRotator{};
	if (firstWeaponMesh == nullptr || !uevr::API::UObjectHook::exists(firstWeaponMesh))
		return false;
	const auto componentClass = firstWeaponMesh->get_class();
	if (componentClass == nullptr || componentClass->find_function(L"K2_GetComponentLocation") == nullptr
		|| componentClass->find_function(L"GetForwardVector") == nullptr
		|| componentClass->find_function(L"GetRightVector") == nullptr
		|| componentClass->find_function(L"GetUpVector") == nullptr
		|| Utilities::KismetMathLibrary == nullptr)
		return false;
	Utilities::ParameterSingleVector3 locationParams{};
	Utilities::ParameterSingleVector3 forwardParams{};
	Utilities::ParameterSingleVector3 rightParams{};
	Utilities::ParameterSingleVector3 upParams{};
	firstWeaponMesh->call_function(L"K2_GetComponentLocation", &locationParams);
	firstWeaponMesh->call_function(L"GetForwardVector", &forwardParams);
	firstWeaponMesh->call_function(L"GetRightVector", &rightParams);
	firstWeaponMesh->call_function(L"GetUpVector", &upParams);
	const glm::fvec3 forward = NormalizeOrZero(forwardParams.vec3Value);
	const glm::fvec3 right = NormalizeOrZero(rightParams.vec3Value);
	const glm::fvec3 up = NormalizeOrZero(upParams.vec3Value);
	if (!IsFiniteVector(locationParams.vec3Value) || glm::length(forward) < 0.5f
		|| glm::length(right) < 0.5f || glm::length(up) < 0.5f
		|| Utilities::KismetMathLibrary->get_class()->find_function(L"MakeRotationFromAxes") == nullptr)
		return false;
	ParameterMakeRotationFromAxes makeRotationParams{};
	makeRotationParams.forward = forward;
	makeRotationParams.right = right;
	makeRotationParams.up = up;
	Utilities::KismetMathLibrary->call_function(L"MakeRotationFromAxes", &makeRotationParams);
	position = locationParams.vec3Value;
	rotation = makeRotationParams.returnValue;
	return IsFiniteVector(position) && std::isfinite(rotation.pitch)
		&& std::isfinite(rotation.yaw) && std::isfinite(rotation.roll);
}

bool WeaponManager::ReadCurrentWeaponWorldTransform(glm::fvec3& position, glm::fquat& rotation) const
{
	position = glm::fvec3(0.0f);
	rotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	if (firstWeaponMesh == nullptr || !uevr::API::UObjectHook::exists(firstWeaponMesh))
		return false;
	const auto componentClass = firstWeaponMesh->get_class();
	if (componentClass == nullptr || componentClass->find_function(L"K2_GetComponentLocation") == nullptr
		|| componentClass->find_function(L"GetForwardVector") == nullptr
		|| componentClass->find_function(L"GetRightVector") == nullptr
		|| componentClass->find_function(L"GetUpVector") == nullptr)
		return false;
	Utilities::ParameterSingleVector3 locationParams{};
	Utilities::ParameterSingleVector3 forwardParams{};
	Utilities::ParameterSingleVector3 rightParams{};
	Utilities::ParameterSingleVector3 upParams{};
	firstWeaponMesh->call_function(L"K2_GetComponentLocation", &locationParams);
	firstWeaponMesh->call_function(L"GetForwardVector", &forwardParams);
	firstWeaponMesh->call_function(L"GetRightVector", &rightParams);
	firstWeaponMesh->call_function(L"GetUpVector", &upParams);
	const glm::fvec3 forward = NormalizeOrZero(forwardParams.vec3Value);
	const glm::fvec3 right = NormalizeOrZero(rightParams.vec3Value);
	const glm::fvec3 up = NormalizeOrZero(upParams.vec3Value);
	if (!IsFiniteVector(locationParams.vec3Value) || glm::length(forward) < 0.5f
		|| glm::length(right) < 0.5f || glm::length(up) < 0.5f)
		return false;
	position = locationParams.vec3Value;
	rotation = RotationFromWeaponBasis(forward, right, up);
	return IsFiniteVector(position) && IsFiniteQuaternion(rotation)
		&& glm::length(rotation) > 0.5f;
}

bool WeaponManager::SetCurrentWeaponWorldPose(const glm::fvec3& position, const Utilities::FRotator& rotation) const
{
	if (firstWeaponMesh == nullptr || !uevr::API::UObjectHook::exists(firstWeaponMesh))
		return false;
	const auto componentClass = firstWeaponMesh->get_class();
	if (componentClass == nullptr || componentClass->find_function(L"K2_SetWorldLocation") == nullptr
		|| componentClass->find_function(L"K2_SetWorldRotation") == nullptr)
		return false;
	Utilities::Parameter_K2_SetWorldOrRelativeLocation locationParams{};
	locationParams.newLocation = position;
	locationParams.bSweep = false;
	locationParams.bTeleport = true;
	firstWeaponMesh->call_function(L"K2_SetWorldLocation", &locationParams);
	Utilities::Parameter_K2_SetWorldOrRelativeRotation rotationParams{};
	rotationParams.newRotation = rotation;
	rotationParams.bSweep = false;
	rotationParams.bTeleport = true;
	firstWeaponMesh->call_function(L"K2_SetWorldRotation", &rotationParams);
	return true;
}

WeaponManager::ControllerPalmAdapter WeaponManager::BuildControllerPalmAdapter(
	int hand, const glm::fvec3& boneTranslation, const glm::fquat& boneRotation,
	bool applyPalmOffset) const
{
	ControllerPalmAdapter adapter;
	if (hand != 0 && hand != 1)
		return adapter;
	if (!IsFiniteVector(boneTranslation) || !IsFiniteQuaternion(boneRotation)
		|| glm::length(boneRotation) <= 0.0001f)
		return adapter;

	adapter.position = -(glm::inverse(UEVRQuatConverter()) * boneTranslation);
	adapter.rotation = ComponentQuaternionToVRSpace(boneRotation);
	if (applyPalmOffset && hand == 0)
	{
		// The left skeletal hand bone has the opposite local roll. Correct only
		// that orientation here. A left-only positional palm shove would make a
		// single mirrored weapon calibration mathematically inconsistent.
		const glm::fquat localForwardRollCorrection = glm::angleAxis(
			3.14159265358979323846f, glm::fvec3(0.0f, 0.0f, 1.0f));
		adapter.rotation = glm::normalize(adapter.rotation * localForwardRollCorrection);
	}
	adapter.valid = IsFiniteVector(adapter.position) && IsFiniteQuaternion(adapter.rotation)
		&& glm::length(adapter.rotation) > 0.5f;
	return adapter;
}

bool WeaponManager::ReadControllerPalmAdapter(int hand, ControllerPalmAdapter& result) const
{
	result = ControllerPalmAdapter{};
	if (hand != 0 && hand != 1)
		return false;
	const auto component = hand == 0 ? freeAimFakeLeftHand : freeAimFakeRightHand;
	const auto& boneName = hand == 0 ? freeAimFakeLeftHandBoneName : freeAimFakeRightHandBoneName;
	if (component == nullptr || boneName.comparison_index == 0
		|| !uevr::API::UObjectHook::exists(component))
		return false;
	const size_t cacheIndex = static_cast<size_t>(hand);
	if (cachedControllerPalmAdapterComponents[cacheIndex] == component
		&& cachedControllerPalmAdapterBones[cacheIndex] == boneName.comparison_index
		&& cachedControllerPalmAdapters[cacheIndex].valid)
	{
		result = cachedControllerPalmAdapters[cacheIndex];
		return true;
	}
	glm::fvec3 boneTranslation{};
	glm::fquat boneRotation{};
	if (!ReadBoneTransform(component, boneName, boneTranslation, boneRotation))
		return false;
	result = BuildControllerPalmAdapter(hand, boneTranslation, boneRotation);
	if (result.valid)
	{
		cachedControllerPalmAdapterComponents[cacheIndex] = component;
		cachedControllerPalmAdapterBones[cacheIndex] = boneName.comparison_index;
		cachedControllerPalmAdapters[cacheIndex] = result;
	}
	return result.valid;
}

bool WeaponManager::ReadHandAnatomicalBasisRotation(int hand, glm::fquat& result) const
{
	result = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	ControllerPalmAdapter adapter;
	if (!ReadControllerPalmAdapter(hand, adapter))
		return false;
	result = VRQuaternionToComponentSpace(adapter.rotation);
	return IsFiniteQuaternion(result) && glm::length(result) > 0.0001f;
}

bool WeaponManager::ConvertMirroredHandContact(const GripCalibrationTransform& source,
	int sourceHand, int destinationHand, GripCalibrationTransform& result) const
{
	result = GripCalibrationTransform{};
	if (!source.valid || (sourceHand != 0 && sourceHand != 1)
		|| (destinationHand != 0 && destinationHand != 1) || sourceHand == destinationHand)
		return false;
	glm::fquat sourceBasis;
	glm::fquat destinationBasis;
	if (!ReadHandAnatomicalBasisRotation(sourceHand, sourceBasis)
		|| !ReadHandAnatomicalBasisRotation(destinationHand, destinationBasis))
		return false;
	result = MirrorCanonicalTransform(source);
	const glm::fquat mirroredSourceBasis = MirrorLateralRotation(sourceBasis);
	// The reflection handles the weapon's spatial left/right frame. The
	// right-multiplied correction then replaces the reflected source hand basis
	// with the actual destination palm basis. It is orientation-only: no
	// skeleton root-to-hand translation is introduced.
	const glm::fquat anatomicalCorrection = glm::normalize(
		glm::inverse(mirroredSourceBasis) * destinationBasis);
	result.rotation = glm::normalize(result.rotation * anatomicalCorrection);
	result.valid = IsFiniteVector(result.position) && IsFiniteQuaternion(result.rotation)
		&& glm::length(result.rotation) > 0.5f;
	return result.valid;
}

bool WeaponManager::AttachSkeletalComponentToWeapon(uevr::API::UObject* component,
	const uevr::API::FName& boneName, const GripCalibrationTransform& contact)
{
	const auto fail = [&](int stage, const char* reason) -> bool
	{
		if (freeAimSupportFailureStageLogged != stage)
		{
			freeAimSupportFailureStageLogged = stage;
			uevr::API::get()->log_warn(
				"[GripCalibration] support-contact attach failed stage=%d reason=%s component=%p bone=%d weapon=%p",
				stage, reason, component, boneName.comparison_index, firstWeaponMesh);
		}
		return false;
	};
	if (!contact.valid
		|| firstWeaponMesh == nullptr || !uevr::API::UObjectHook::exists(firstWeaponMesh))
		return fail(1, "invalid-contact-or-weapon");
	if (component == nullptr || boneName.comparison_index == 0
		|| !uevr::API::UObjectHook::exists(component))
		return fail(2, "invalid-component-or-bone");

	glm::fvec3 bonePosition{};
	glm::fquat boneRotation{};
	if (!ReadBoneTransform(component, boneName, bonePosition, boneRotation))
		return fail(3, "bone-transform");
	const glm::fquat relativeRotation = glm::normalize(contact.rotation * glm::inverse(boneRotation));
	const glm::fvec3 relativePosition = contact.position - relativeRotation * bonePosition;
	if (!IsFiniteVector(relativePosition) || !IsFiniteQuaternion(relativeRotation)
		|| glm::length(relativeRotation) <= 0.5f)
		return fail(4, "relative-contact");
	const glm::fvec3 forward = NormalizeOrZero(relativeRotation * glm::fvec3(1.0f, 0.0f, 0.0f));
	const glm::fvec3 right = NormalizeOrZero(relativeRotation * glm::fvec3(0.0f, 1.0f, 0.0f));
	const glm::fvec3 up = NormalizeOrZero(relativeRotation * glm::fvec3(0.0f, 0.0f, 1.0f));
	if (Utilities::KismetMathLibrary == nullptr)
		Utilities::InitHelperClasses();
	if (Utilities::KismetMathLibrary == nullptr
		|| Utilities::KismetMathLibrary->get_class() == nullptr
		|| Utilities::KismetMathLibrary->get_class()->find_function(L"MakeRotationFromAxes") == nullptr)
		return fail(5, "kismet-rotation");
	ParameterMakeRotationFromAxes makeRotationParams{};
	makeRotationParams.forward = forward;
	makeRotationParams.right = right;
	makeRotationParams.up = up;
	Utilities::KismetMathLibrary->call_function(L"MakeRotationFromAxes", &makeRotationParams);

	uevr::API::UObjectHook::remove_motion_controller_state(component);
	const auto componentClass = component->get_class();
	if (componentClass == nullptr)
		return fail(6, "component-class");
	// UE4.26 exposes the Blueprint-safe attachment wrapper on this runtime
	// component class instead of the native AttachToComponent symbol.
	const auto nativeAttachFunction = componentClass->find_function(L"AttachToComponent");
	const auto k2AttachFunction = componentClass->find_function(L"K2_AttachToComponent");
	const auto attachFunction = nativeAttachFunction != nullptr ? nativeAttachFunction : k2AttachFunction;
	const bool relativeLocationAvailable = componentClass->find_function(L"K2_SetRelativeLocation") != nullptr;
	const bool relativeRotationAvailable = componentClass->find_function(L"K2_SetRelativeRotation") != nullptr;
	if (attachFunction == nullptr || !relativeLocationAvailable || !relativeRotationAvailable)
	{
		if (freeAimSupportFailureStageLogged != 7)
		{
			freeAimSupportFailureStageLogged = 7;
			uevr::API::get()->log_warn(
				"[GripCalibration] support-contact attach failed stage=7 reason=attach-functions native=%d k2=%d relativeLocation=%d relativeRotation=%d component=%p bone=%d weapon=%p",
				nativeAttachFunction != nullptr ? 1 : 0, k2AttachFunction != nullptr ? 1 : 0,
				relativeLocationAvailable ? 1 : 0, relativeRotationAvailable ? 1 : 0,
				component, boneName.comparison_index, firstWeaponMesh);
		}
		return false;
	}
	std::vector<uint8_t> attachParams(attachFunction->get_properties_size());
	const bool parentSet = SetReflectedObjectParameter(attachFunction, attachParams, L"Parent", firstWeaponMesh);
	const bool socketSet = SetReflectedFNameParameter(attachFunction, attachParams, L"SocketName", uevr::API::FName{});
	const bool locationRuleSet = SetReflectedByteParameter(attachFunction, attachParams, L"LocationRule", 0);
	const bool rotationRuleSet = SetReflectedByteParameter(attachFunction, attachParams, L"RotationRule", 0);
	const bool scaleRuleSet = SetReflectedByteParameter(attachFunction, attachParams, L"ScaleRule", 0);
	// K2_AttachToComponent names this parameter bWeldSimulatedBodies;
	// older/native wrappers may expose the unprefixed spelling.
	SetReflectedBoolParameter(attachFunction, attachParams, L"bWeldSimulatedBodies", false);
	SetReflectedBoolParameter(attachFunction, attachParams, L"WeldSimulatedBodies", false);
	if (!parentSet || !socketSet || !locationRuleSet || !rotationRuleSet || !scaleRuleSet)
		return fail(8, "attach-parameters");
	attachFunction->call(component, attachParams.data());

	Utilities::Parameter_K2_SetWorldOrRelativeLocation locationParams{};
	locationParams.newLocation = relativePosition;
	locationParams.bSweep = false;
	locationParams.bTeleport = true;
	component->call_function(L"K2_SetRelativeLocation", &locationParams);
	Utilities::Parameter_K2_SetWorldOrRelativeRotation rotationParams{};
	rotationParams.newRotation = makeRotationParams.returnValue;
	rotationParams.bSweep = false;
	rotationParams.bTeleport = true;
	component->call_function(L"K2_SetRelativeRotation", &rotationParams);
	SetComponentVisibility(component, true);
	return true;
}

bool WeaponManager::RealignSkeletalComponentToWeapon(uevr::API::UObject* component,
	const uevr::API::FName& boneName, const GripCalibrationTransform& contact)
{
	const auto fail = [&](int stage, const char* reason) -> bool
	{
		if (freeAimSupportFailureStageLogged != stage)
		{
			freeAimSupportFailureStageLogged = stage;
			uevr::API::get()->log_warn(
				"[GripCalibration] support-contact realign failed stage=%d reason=%s component=%p bone=%d weapon=%p",
				stage, reason, component, boneName.comparison_index, firstWeaponMesh);
		}
		return false;
	};
	if (!contact.valid || firstWeaponMesh == nullptr
		|| !uevr::API::UObjectHook::exists(firstWeaponMesh)
		|| component == nullptr || boneName.comparison_index == 0
		|| !uevr::API::UObjectHook::exists(component))
		return fail(20, "invalid-contact-or-component");

	glm::fvec3 bonePosition{};
	glm::fquat boneRotation{};
	if (!ReadBoneTransform(component, boneName, bonePosition, boneRotation))
		return fail(21, "bone-transform");
	const glm::fquat relativeRotation = glm::normalize(contact.rotation * glm::inverse(boneRotation));
	const glm::fvec3 relativePosition = contact.position - relativeRotation * bonePosition;
	if (!IsFiniteVector(relativePosition) || !IsFiniteQuaternion(relativeRotation)
		|| glm::length(relativeRotation) <= 0.5f)
		return fail(22, "relative-contact");
	const auto componentClass = component->get_class();
	if (componentClass == nullptr
		|| componentClass->find_function(L"K2_SetRelativeLocation") == nullptr
		|| componentClass->find_function(L"K2_SetRelativeRotation") == nullptr
		|| Utilities::KismetMathLibrary == nullptr
		|| Utilities::KismetMathLibrary->get_class() == nullptr)
		return fail(23, "relative-functions");
	const glm::fvec3 forward = NormalizeOrZero(relativeRotation * glm::fvec3(1.0f, 0.0f, 0.0f));
	const glm::fvec3 right = NormalizeOrZero(relativeRotation * glm::fvec3(0.0f, 1.0f, 0.0f));
	const glm::fvec3 up = NormalizeOrZero(relativeRotation * glm::fvec3(0.0f, 0.0f, 1.0f));
	if (glm::length(forward) <= 0.5f || glm::length(right) <= 0.5f || glm::length(up) <= 0.5f
		|| Utilities::KismetMathLibrary->get_class()->find_function(L"MakeRotationFromAxes") == nullptr)
		return fail(24, "kismet-rotation");
	ParameterMakeRotationFromAxes makeRotationParams{};
	makeRotationParams.forward = forward;
	makeRotationParams.right = right;
	makeRotationParams.up = up;
	Utilities::KismetMathLibrary->call_function(L"MakeRotationFromAxes", &makeRotationParams);

	Utilities::Parameter_K2_SetWorldOrRelativeLocation locationParams{};
	locationParams.newLocation = relativePosition;
	locationParams.bSweep = false;
	locationParams.bTeleport = true;
	component->call_function(L"K2_SetRelativeLocation", &locationParams);
	Utilities::Parameter_K2_SetWorldOrRelativeRotation rotationParams{};
	rotationParams.newRotation = makeRotationParams.returnValue;
	rotationParams.bSweep = false;
	rotationParams.bTeleport = true;
	component->call_function(L"K2_SetRelativeRotation", &rotationParams);
	return true;
}

bool WeaponManager::AttachSupportFakeHandToWeapon(int controllerHand, const GripCalibrationTransform& contact)
{
	const auto fail = [&](int stage, const char* reason) -> bool
	{
		if (freeAimSupportFailureStageLogged != stage)
		{
			freeAimSupportFailureStageLogged = stage;
			uevr::API::get()->log_warn(
				"[GripCalibration] support-contact request failed stage=%d reason=%s hand=%d weapon=%p",
				stage, reason, controllerHand, firstWeaponMesh);
		}
		return false;
	};
	if ((controllerHand != 0 && controllerHand != 1) || !contact.valid
		|| firstWeaponMesh == nullptr || !uevr::API::UObjectHook::exists(firstWeaponMesh))
		return fail(30, "invalid-hand-contact-or-weapon");
	const auto component = controllerHand == 0 ? freeAimFakeLeftHand : freeAimFakeRightHand;
	const auto& boneName = controllerHand == 0 ? freeAimFakeLeftHandBoneName : freeAimFakeRightHandBoneName;
	if (component == nullptr || boneName.comparison_index == 0
		|| !uevr::API::UObjectHook::exists(component))
		return fail(31, "support-hand-component-or-bone");
	const bool watchNeeded = controllerHand == 0 && freeAimFakeWatch != nullptr
		&& freeAimFakeWatchHandBoneName.comparison_index != 0
		&& uevr::API::UObjectHook::exists(freeAimFakeWatch);
	if (freeAimSupportHandAttached && freeAimSupportAttachedHand == controllerHand
		&& freeAimSupportAttachedWeapon == firstWeaponMesh
		&& (!watchNeeded || freeAimSupportWatchAttached))
		return true;
	RestoreSupportFakeHandAttachment();
	if (!AttachSkeletalComponentToWeapon(component, boneName, contact))
		return false;
	// The watch is skinned to the native left hand. When that hand becomes the
	// weapon's support hand, attach the watch to the same gun-local contact so it
	// cannot remain behind on the left controller. A watch failure does not make
	// the hand attachment fail; the normal presentation path remains the safe
	// fallback for the accessory.
	freeAimSupportWatchAttached = false;
	if (watchNeeded)
		freeAimSupportWatchAttached = AttachSkeletalComponentToWeapon(
			freeAimFakeWatch, freeAimFakeWatchHandBoneName, contact);
	freeAimSupportHandAttached = true;
	freeAimSupportAttachedHand = static_cast<int8_t>(controllerHand);
	freeAimSupportAttachedWeapon = firstWeaponMesh;
	if (!watchNeeded || freeAimSupportWatchAttached)
		freeAimSupportFailureStageLogged = -1;
	uevr::API::get()->log_info("[GripCalibration] support-contact attached hand=%d watch=%d weapon=%p",
		controllerHand, freeAimSupportWatchAttached ? 1 : 0, firstWeaponMesh);
	return true;
}

bool WeaponManager::RealignSupportFakeHandToWeapon(int controllerHand,
	const GripCalibrationTransform& contact)
{
	if (!freeAimSupportHandAttached || freeAimSupportAttachedHand != controllerHand
		|| freeAimSupportAttachedWeapon != firstWeaponMesh)
		return false;
	const auto component = controllerHand == 0 ? freeAimFakeLeftHand : freeAimFakeRightHand;
	const auto& boneName = controllerHand == 0 ? freeAimFakeLeftHandBoneName : freeAimFakeRightHandBoneName;
	if (!RealignSkeletalComponentToWeapon(component, boneName, contact))
		return false;
	if (freeAimSupportWatchAttached
		&& !RealignSkeletalComponentToWeapon(freeAimFakeWatch, freeAimFakeWatchHandBoneName, contact))
		return false;
	return true;
}

void WeaponManager::RestoreSupportFakeHandAttachment()
{
	if (!freeAimSupportHandAttached)
		return;
	if (freeAimSupportWatchAttached && freeAimFakeWatch != nullptr
		&& uevr::API::UObjectHook::exists(freeAimFakeWatch)
		&& freeAimFakeWatch->get_class() != nullptr
		&& freeAimFakeWatch->get_class()->find_function(L"DetachFromParent") != nullptr)
	{
		Utilities::ParameterDetachFromParent detachParams{};
		detachParams.maintainWorldPosition = true;
		detachParams.callModify = false;
		freeAimFakeWatch->call_function(L"DetachFromParent", &detachParams);
		uevr::API::UObjectHook::remove_motion_controller_state(freeAimFakeWatch);
	}
	const auto component = freeAimSupportAttachedHand == 0 ? freeAimFakeLeftHand : freeAimFakeRightHand;
	if (component != nullptr && uevr::API::UObjectHook::exists(component)
		&& component->get_class() != nullptr
		&& component->get_class()->find_function(L"DetachFromParent") != nullptr)
	{
		Utilities::ParameterDetachFromParent detachParams{};
		detachParams.maintainWorldPosition = true;
		detachParams.callModify = false;
		component->call_function(L"DetachFromParent", &detachParams);
	}
	if (component != nullptr && uevr::API::UObjectHook::exists(component))
		uevr::API::UObjectHook::remove_motion_controller_state(component);
	freeAimSupportWatchAttached = false;
	freeAimSupportHandAttached = false;
	freeAimSupportAttachedHand = -1;
	freeAimSupportAttachedWeapon = nullptr;
	uevr::API::get()->log_info("%s", "[GripCalibration] support-contact restored to controller");
}

bool WeaponManager::BuildPrimaryWeaponLocalContact(int controllerHand,
	GripCalibrationTransform& contact) const
{
	contact = GripCalibrationTransform{};
	if ((controllerHand != 0 && controllerHand != 1)
		|| firstWeaponMesh == nullptr || !uevr::API::UObjectHook::exists(firstWeaponMesh))
		return false;
	const auto component = controllerHand == 0 ? freeAimFakeLeftHand : freeAimFakeRightHand;
	const auto& boneName = controllerHand == 0 ? freeAimFakeLeftHandBoneName : freeAimFakeRightHandBoneName;
	if (component == nullptr || boneName.comparison_index == 0
		|| !uevr::API::UObjectHook::exists(component))
		return false;

	glm::fvec3 weaponPosition{};
	glm::fquat weaponRotation{};
	glm::fvec3 handPosition{};
	glm::fquat handRotation{};
	if (!ReadCurrentWeaponWorldTransform(weaponPosition, weaponRotation)
		|| !ReadBoneWorldTransform(component, boneName, handPosition, handRotation))
		return false;
	if (!IsFiniteVector(weaponPosition) || !IsFiniteQuaternion(weaponRotation)
		|| glm::length(weaponRotation) <= 0.5f
		|| !IsFiniteVector(handPosition) || !IsFiniteQuaternion(handRotation)
		|| glm::length(handRotation) <= 0.5f)
		return false;

	// The caller guarantees one complete controller-tracked priming frame. The
	// visible bone rotation therefore already contains the anatomical adapter and
	// the two-hand wrist correction. Applying the wrist delta again here caused
	// the grip-side-dependent palm/knuckle flip seen in the runtime logs.

	const glm::fquat inverseWeaponRotation = glm::inverse(glm::normalize(weaponRotation));
	contact.position = inverseWeaponRotation * (handPosition - weaponPosition);
	contact.rotation = glm::normalize(inverseWeaponRotation * glm::normalize(handRotation));
	contact.valid = IsFiniteVector(contact.position)
		&& glm::length(contact.position) <= 500.0f
		&& IsFiniteQuaternion(contact.rotation)
		&& glm::length(contact.rotation) > 0.5f;
	return contact.valid;
}

bool WeaponManager::AttachPrimaryFakeHandToWeapon(int controllerHand)
{
	const auto readAttachParent = [](uevr::API::UObject* object,
		bool& parentKnown) -> uevr::API::UObject*
	{
		parentKnown = false;
		if (object == nullptr || !uevr::API::UObjectHook::exists(object)
			|| object->get_class() == nullptr
			|| object->get_class()->find_property(L"AttachParent") == nullptr)
			return nullptr;
		parentKnown = true;
		return object->get_property<uevr::API::UObject*>(L"AttachParent");
	};
	const auto component = (controllerHand == 0) ? freeAimFakeLeftHand
		: (controllerHand == 1 ? freeAimFakeRightHand : nullptr);
	const auto logFailure = [&](const char* reason) -> bool
	{
		if (!freeAimPrimaryAttachFailureDiagnosticLogged)
		{
			bool parentKnown = false;
			const auto parent = readAttachParent(component, parentKnown);
			const auto motionState = component != nullptr
				&& uevr::API::UObjectHook::exists(component)
				? uevr::API::UObjectHook::get_motion_controller_state(component) : nullptr;
			uevr::API::get()->log_warn(
				"[GripCalibration] primary-grip attach failed hand=%d reason=%s component=%p parent=%p parentKnown=%d weapon=%p motionStateRemoved=%d",
				controllerHand, reason, component, parent, parentKnown ? 1 : 0, firstWeaponMesh,
				motionState != nullptr ? 0 : 1);
			freeAimPrimaryAttachFailureDiagnosticLogged = true;
		}
		return false;
	};
	if ((controllerHand != 0 && controllerHand != 1)
		|| firstWeaponMesh == nullptr || !uevr::API::UObjectHook::exists(firstWeaponMesh))
		return logFailure("invalid-hand-or-weapon");
	const auto& boneName = controllerHand == 0 ? freeAimFakeLeftHandBoneName : freeAimFakeRightHandBoneName;
	if (component == nullptr || boneName.comparison_index == 0
		|| !uevr::API::UObjectHook::exists(component))
		return logFailure("invalid-component-or-bone");
	if (freeAimPrimaryHandAttached && freeAimPrimaryAttachedHand == controllerHand
		&& freeAimPrimaryAttachedWeapon == firstWeaponMesh)
		return true;

	GripCalibrationTransform contact;
	if (!BuildPrimaryWeaponLocalContact(controllerHand, contact))
		return logFailure("primary-local-contact");
	RestorePrimaryFakeHandAttachment();
	if (!AttachSkeletalComponentToWeapon(component, boneName, contact))
		return logFailure("contact-attach");

	bool parentKnown = false;
	const auto parent = readAttachParent(component, parentKnown);
	const bool parentMatchesWeapon = parentKnown && parent == firstWeaponMesh;
	if (parentKnown && !parentMatchesWeapon)
	{
		const bool failed = logFailure("parent-mismatch");
		if (component->get_class() != nullptr
			&& component->get_class()->find_function(L"DetachFromParent") != nullptr)
		{
			Utilities::ParameterDetachFromParent detachParams{};
			detachParams.maintainWorldPosition = true;
			detachParams.callModify = false;
			component->call_function(L"DetachFromParent", &detachParams);
		}
		uevr::API::UObjectHook::remove_motion_controller_state(component);
		return failed;
	}
	const auto motionState = uevr::API::UObjectHook::get_motion_controller_state(component);
	const bool motionStateRemoved = motionState == nullptr;
	freeAimPrimaryHandAttached = true;
	freeAimPrimaryAttachedHand = static_cast<int8_t>(controllerHand);
	freeAimPrimaryAttachedWeapon = firstWeaponMesh;
	freeAimPrimaryAttachFailureDiagnosticLogged = false;
	uevr::API::get()->log_info(
		"[GripCalibration] primary-grip attached hand=%d support=%d weaponId=%d generation=%u role=%s component=%p parent=%p parentKnown=%d parentIsWeapon=%d weapon=%p motionStateRemoved=%d contactP=(%.2f %.2f %.2f) contactQ=(%.5f %.5f %.5f %.5f)",
		controllerHand, 1 - controllerHand, static_cast<int>(currentWeaponEquipped),
		runtimeHandGeneration,
		RuntimeHandRoleName(runtimeHandStates[static_cast<size_t>(controllerHand)].role),
		component, parent, parentKnown ? 1 : 0, parentMatchesWeapon ? 1 : 0,
		firstWeaponMesh, motionStateRemoved ? 1 : 0,
		contact.position.x, contact.position.y, contact.position.z,
		contact.rotation.w, contact.rotation.x, contact.rotation.y, contact.rotation.z);
	return true;
}

void WeaponManager::RestorePrimaryFakeHandAttachment()
{
	freeAimPrimaryAttachPrimeFramesRemaining = 0;
	freeAimPrimaryAttachPrimeHand = -1;
	freeAimPrimaryAttachPrimeWeapon = nullptr;
	freeAimPrimaryAttachPrimeGeneration = 0;
	freeAimPrimaryAttachPrimeEngineTick = 0;
	if (!freeAimPrimaryHandAttached)
	{
		freeAimPrimaryAttachFailureDiagnosticLogged = false;
		return;
	}
	const auto component = freeAimPrimaryAttachedHand == 0
		? freeAimFakeLeftHand : freeAimFakeRightHand;
	if (component != nullptr && uevr::API::UObjectHook::exists(component)
		&& component->get_class() != nullptr
		&& component->get_class()->find_function(L"DetachFromParent") != nullptr)
	{
		Utilities::ParameterDetachFromParent detachParams{};
		detachParams.maintainWorldPosition = true;
		detachParams.callModify = false;
		component->call_function(L"DetachFromParent", &detachParams);
	}
	if (component != nullptr && uevr::API::UObjectHook::exists(component))
		uevr::API::UObjectHook::remove_motion_controller_state(component);
	freeAimPrimaryHandAttached = false;
	freeAimPrimaryAttachedHand = -1;
	freeAimPrimaryAttachedWeapon = nullptr;
	freeAimPrimaryAttachFailureDiagnosticLogged = false;
	uevr::API::get()->log_info("%s", "[GripCalibration] primary-grip restored to controller");
}

bool WeaponManager::GetFakeHandControllerOffset(int controllerHand, glm::fvec3& position, glm::fquat& rotation) const
{
	position = glm::fvec3(0.0f);
	rotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	const auto component = controllerHand == 0 ? freeAimFakeLeftHand : freeAimFakeRightHand;
	const auto& boneName = controllerHand == 0 ? freeAimFakeLeftHandBoneName : freeAimFakeRightHandBoneName;
	if (component == nullptr || boneName.comparison_index == 0 || !uevr::API::UObjectHook::exists(component))
		return false;
	glm::fvec3 boneTranslation{};
	glm::fquat boneRotation{};
	if (!ReadBoneTransform(component, boneName, boneTranslation, boneRotation))
		return false;
	rotation = ComponentQuaternionToVRSpace(boneRotation);
	if (controllerHand == 0)
		rotation = glm::normalize(rotation * glm::angleAxis(3.14159265358979323846f, glm::fvec3(0.0f, 0.0f, 1.0f)));
	position = -(glm::inverse(UEVRQuatConverter()) * boneTranslation);
	return IsFiniteVector(position) && IsFiniteQuaternion(rotation) && glm::length(rotation) > 0.5f;
}

bool WeaponManager::BeginGripCalibration(int controllerHand)
{
	if (!settingsManager->enableGripCalibration || controllerHand < 0 || controllerHand > 1)
		return false;
	if (!IsGripCalibrationEligible(controllerHand))
	{
		if (!gripCalibrationFailureLogged)
		{
			uevr::API::get()->log_warn("[GripCalibration] reject begin hand=%d weapon=%d reason=ineligible",
				controllerHand, static_cast<int>(currentWeaponEquipped));
			gripCalibrationFailureLogged = true;
		}
		return false;
	}

	const uint8_t gripMask = gripStateMask.load(std::memory_order_acquire);
	const int primaryHand = twoHandPrimaryHand.load(std::memory_order_acquire);
	// Select the calibration role from the latched grip order, not the solver's
	// transient validity. This keeps the support button a support calibration
	// even while the weapon is frozen or the two-point solve is rebuilding.
	const bool supportHand = gripMask == 3U && primaryHand >= 0 && controllerHand != primaryHand;
	const auto record = supportHand ? GripCalibrationRecord::SupportContact
		: GripCalibrationRecord::PrimaryGrip;
	if (record == GripCalibrationRecord::SupportContact)
		RestoreSupportFakeHandAttachment();
	const int slot = GripCalibrationRecordIndex(record);
	if (slot < 0 || (record == GripCalibrationRecord::SupportContact && !freeAimFakeHandsActive))
	{
		uevr::API::get()->log_warn("[GripCalibration] reject begin hand=%d weapon=%d record=%s reason=hand-presentation-unavailable",
			controllerHand, static_cast<int>(currentWeaponEquipped), GripCalibrationRecordName(record));
		return false;
	}

	GripCalibrationSession& session = gripCalibrationSessions[static_cast<size_t>(controllerHand)];
	if (session.active)
		return true;

	glm::fvec3 controllerPosition{};
	glm::fquat controllerRotation{};
	if (!ReadControllerCalibrationPose(controllerHand, controllerPosition, controllerRotation))
	{
		uevr::API::get()->log_warn("[GripCalibration] reject begin hand=%d weapon=%d record=%s reason=controller-pose",
				controllerHand, static_cast<int>(currentWeaponEquipped), GripCalibrationRecordName(record));
		return false;
	}

	if (!IsGripCalibrationActive())
	{
		if (!ReadCurrentWeaponWorldPose(session.frozenWorldPosition, session.frozenWorldRotation))
		{
			uevr::API::get()->log_warn("[GripCalibration] reject begin hand=%d weapon=%d record=%s reason=weapon-pose",
				controllerHand, static_cast<int>(currentWeaponEquipped), GripCalibrationRecordName(record));
			return false;
		}
		uevr::API::UObjectHook::remove_motion_controller_state(firstWeaponMesh);
		motionConfiguredFirstWeaponMesh = nullptr;
		motionConfiguredFirstHand = -1;
		motionConfiguredFirstCalibrationRole = -1;
		if (!SetCurrentWeaponWorldPose(session.frozenWorldPosition, session.frozenWorldRotation))
		{
			uevr::API::get()->log_warn("[GripCalibration] reject begin hand=%d weapon=%d record=%s reason=freeze",
				controllerHand, static_cast<int>(currentWeaponEquipped), GripCalibrationRecordName(record));
			return false;
		}
	}
	else
	{
		const auto& existing = gripCalibrationSessions[0].active
			? gripCalibrationSessions[0] : gripCalibrationSessions[1];
		if (existing.weaponId != static_cast<int>(currentWeaponEquipped)
			|| existing.frozenWeaponMesh != firstWeaponMesh)
			return false;
		session.frozenWorldPosition = existing.frozenWorldPosition;
		session.frozenWorldRotation = existing.frozenWorldRotation;
	}

	GripCalibrationTransform saved;
	if (record == GripCalibrationRecord::PrimaryGrip)
	{
		session.baselinePosition = GetWeaponGripPositionOffset(controllerHand);
		session.baselineRotation = GetWeaponGripRotationOffset(controllerHand);
	}
	if (record == GripCalibrationRecord::PrimaryGrip
		&& GetCanonicalPrimaryGripForHand(controllerHand, saved))
	{
		// Use the currently reconstructed controller offset as the baseline. The
		// stored v6 primary record is a palm relation, not a UEVR offset.
		session.baselinePosition = glm::inverse(UEVRQuatConverter()) * saved.position;
		session.baselineRotation = ComponentQuaternionToVRSpace(saved.rotation);
	}

	session.active = true;
	session.controllerHand = controllerHand;
	session.weaponId = static_cast<int>(currentWeaponEquipped);
	session.record = record;
	session.beginGripPosition = controllerPosition;
	session.beginAimRotation = controllerRotation;
	session.frozenWeaponMesh = firstWeaponMesh;
	RefreshRuntimeHandRoles("calibration-begin");
	for (auto& activeSession : gripCalibrationSessions)
		if (activeSession.active)
			activeSession.runtimeGeneration = runtimeHandGeneration;
	gripCalibrationFailureLogged = false;
	uevr::API::get()->log_info(
		"[GripCalibration] begin weapon=%d record=%s sourceHand=%d slot=%d frozen=true grip=(%.3f %.3f %.3f) aim=(%.4f %.4f %.4f %.4f) baselinePos=(%.2f %.2f %.2f) baselineRot=(%.4f %.4f %.4f %.4f)",
		static_cast<int>(currentWeaponEquipped), GripCalibrationRecordName(record), controllerHand, slot,
		session.beginGripPosition.x, session.beginGripPosition.y, session.beginGripPosition.z,
		session.beginAimRotation.w, session.beginAimRotation.x, session.beginAimRotation.y, session.beginAimRotation.z,
		session.baselinePosition.x, session.baselinePosition.y, session.baselinePosition.z,
		session.baselineRotation.w, session.baselineRotation.x, session.baselineRotation.y, session.baselineRotation.z);
	return true;
}

void WeaponManager::EndGripCalibration(int controllerHand, bool save)
{
	if (controllerHand < 0 || controllerHand > 1)
		return;
	GripCalibrationSession& session = gripCalibrationSessions[static_cast<size_t>(controllerHand)];
	if (!session.active)
		return;

	if (save && session.runtimeGeneration == runtimeHandGeneration
		&& firstWeaponMesh == session.frozenWeaponMesh
		&& static_cast<int>(currentWeaponEquipped) == session.weaponId)
	{
		GripCalibrationTransform canonical;
		bool captured = false;
		bool mirrored = false;
		bool anatomicalCorrectionApplied = false;
		const int canonicalHand = controllerHand;
		if (session.record == GripCalibrationRecord::PrimaryGrip)
		{
			glm::fvec3 endPosition{};
			glm::fquat endRotation{};
			if (ReadControllerCalibrationPose(controllerHand, endPosition, endRotation))
			{
				// Keep the established UEVR controller-relative offset equation, then
				// remove the source hand's orientation-only controller-to-palm adapter.
				// No full fake-skeleton root-to-hand translation enters calibration.
				constexpr float metresToUnrealUnits = 100.0f;
				const glm::fquat beginAdjustedRotation = glm::normalize(session.beginAimRotation
					* glm::inverse(session.baselineRotation));
				const glm::fquat calibratedRotation = glm::normalize(session.baselineRotation
					* glm::inverse(session.beginAimRotation) * endRotation);
				const glm::fquat endAdjustedRotation = glm::normalize(endRotation
					* glm::inverse(calibratedRotation));
				const glm::fvec3 controllerDelta = endPosition - session.beginGripPosition;
				const glm::fvec3 beginOffsetInControllerSpace = beginAdjustedRotation
					* (session.baselinePosition / metresToUnrealUnits);
				const glm::fvec3 calibratedPosition = glm::inverse(endAdjustedRotation)
					* (controllerDelta + beginOffsetInControllerSpace) * metresToUnrealUnits;
				ControllerPalmAdapter sourceAdapter;
				if (IsFiniteVector(calibratedPosition) && glm::length(calibratedPosition) <= 500.0f
					&& IsFiniteQuaternion(calibratedRotation) && glm::length(calibratedRotation) > 0.5f
					&& ReadControllerPalmAdapter(controllerHand, sourceAdapter))
				{
					canonical.position = VRSpaceToComponentSpace(calibratedPosition);
					const glm::fquat palmRelationVR = glm::normalize(
						sourceAdapter.rotation * glm::inverse(calibratedRotation));
					canonical.rotation = VRQuaternionToComponentSpace(palmRelationVR);
					canonical.palmFramed = false;
					canonical.anatomicalFramed = true;
					canonical.valid = IsFiniteVector(canonical.position)
						&& IsFiniteQuaternion(canonical.rotation)
						&& glm::length(canonical.rotation) > 0.5f;
					mirrored = false;
					anatomicalCorrectionApplied = canonical.valid;
					captured = canonical.valid = IsFiniteVector(canonical.position)
						&& glm::length(canonical.position) <= 500.0f
						&& IsFiniteQuaternion(canonical.rotation)
						&& glm::length(canonical.rotation) > 0.5f;
				}
			}
		}
		else
		{
			const auto component = controllerHand == 0 ? freeAimFakeLeftHand : freeAimFakeRightHand;
			const auto& boneName = controllerHand == 0 ? freeAimFakeLeftHandBoneName : freeAimFakeRightHandBoneName;
			glm::fvec3 weaponPosition{};
			glm::fquat weaponRotation{};
			glm::fvec3 handPosition{};
			glm::fquat handRotation{};
			if (ReadCurrentWeaponWorldTransform(weaponPosition, weaponRotation)
				&& component != nullptr && boneName.comparison_index != 0
				&& ReadBoneWorldTransform(component, boneName, handPosition, handRotation))
			{
				GripCalibrationTransform capturedContact;
				capturedContact.position = glm::inverse(weaponRotation) * (handPosition - weaponPosition);
				capturedContact.rotation = glm::normalize(glm::inverse(weaponRotation) * handRotation);
				capturedContact.valid = IsFiniteVector(capturedContact.position)
					&& IsFiniteQuaternion(capturedContact.rotation)
					&& glm::length(capturedContact.rotation) > 0.5f;
				if (capturedContact.valid)
					canonical = capturedContact;
				canonical.valid = IsFiniteVector(canonical.position)
					&& IsFiniteQuaternion(canonical.rotation)
					&& glm::length(canonical.rotation) > 0.5f;
				mirrored = false;
				captured = canonical.valid = IsFiniteVector(canonical.position)
					&& glm::length(canonical.position) <= 500.0f
					&& IsFiniteQuaternion(canonical.rotation)
					&& glm::length(canonical.rotation) > 0.5f;
			}
		}

		const int slot = GripCalibrationRecordIndex(session.record);
		if (!captured || slot < 0)
		{
			uevr::API::get()->log_warn("[GripCalibration] reject save weapon=%d record=%s sourceHand=%d reason=capture-or-validation",
				session.weaponId, GripCalibrationRecordName(session.record), session.controllerHand);
		}
		else
		{
			const auto previousIt = gripCalibrationTransforms.find(session.weaponId);
			const bool hadPrevious = previousIt != gripCalibrationTransforms.end();
			const WeaponGripCalibration previous = hadPrevious
				? previousIt->second : WeaponGripCalibration{};
			auto& weapon = gripCalibrationTransforms[session.weaponId];
			if (session.record == GripCalibrationRecord::PrimaryGrip)
				weapon.primaryGrip[static_cast<size_t>(controllerHand)] = canonical;
			else
				weapon.supportContact[static_cast<size_t>(controllerHand)] = canonical;
			if (!SaveGripCalibration())
			{
				if (hadPrevious)
					gripCalibrationTransforms[session.weaponId] = previous;
				else
					gripCalibrationTransforms.erase(session.weaponId);
				uevr::API::get()->log_warn("[GripCalibration] reject save weapon=%d record=%s sourceHand=%d reason=persistence",
					session.weaponId, GripCalibrationRecordName(session.record), session.controllerHand);
			}
			else
			uevr::API::get()->log_info("[GripCalibration] save weapon=%d record=%s sourceHand=%d destinationHand=%d canonical=true anatomy=%s mirrored=%s slot=%d pos=(%.2f %.2f %.2f) rot=(%.4f %.4f %.4f %.4f)",
				session.weaponId, GripCalibrationRecordName(session.record), session.controllerHand, canonicalHand,
				anatomicalCorrectionApplied ? "yes" : "no", mirrored ? "yes" : "no", slot, canonical.position.x, canonical.position.y,
				canonical.position.z, canonical.rotation.w, canonical.rotation.x,
				canonical.rotation.y, canonical.rotation.z);
		}
	}
	else if (save)
		uevr::API::get()->log_warn("[GripCalibration] reject save hand=%d record=%s reason=%s",
			controllerHand, GripCalibrationRecordName(session.record),
			session.runtimeGeneration != runtimeHandGeneration ? "generation-changed" : "weapon-changed");

	session = GripCalibrationSession{};
	freeAimAppliedCalibrationWeaponId = -1;
	if (!IsGripCalibrationActive())
	{
		motionConfiguredFirstWeaponMesh = nullptr;
		motionConfiguredFirstHand = -1;
		motionConfiguredFirstCalibrationRole = -1;
	}
	RefreshRuntimeHandRoles("calibration-end");
}

void WeaponManager::EnforceGripCalibrationFreeze()
{
	const GripCalibrationSession* activeSession = nullptr;
	for (const auto& session : gripCalibrationSessions)
	{
		if (session.active)
		{
			activeSession = &session;
			break;
		}
	}
	if (activeSession == nullptr || firstWeaponMesh != activeSession->frozenWeaponMesh)
		return;
	uevr::API::UObjectHook::remove_motion_controller_state(firstWeaponMesh);
	SetCurrentWeaponWorldPose(activeSession->frozenWorldPosition, activeSession->frozenWorldRotation);
}

void WeaponManager::InitializeGripCalibration()
{
	LoadGripCalibration();
	gripCalibrationModeObserved = settingsManager->enableGripCalibration;
	uevr::API::get()->log_info("[GripCalibration] mode=%s controls=both-thumb-rests:held-grip",
		settingsManager->enableGripCalibration ? "enabled" : "disabled");
}

uint8_t WeaponManager::GetDualThumbCalibrationHandMask() const
{
	if (settingsManager == nullptr || !settingsManager->enableGripCalibration)
		return 0;
	const uint8_t grips = gripStateMask.load(std::memory_order_acquire);
	if (grips == 1U || grips == 2U)
		return grips;
	if (grips == 3U)
	{
		const int primary = twoHandPrimaryHand.load(std::memory_order_acquire);
		if (primary == 0 || primary == 1)
			return static_cast<uint8_t>(1U << (1 - primary));
	}
	return 0;
}

void WeaponManager::SetCalibrationButtonState(bool leftButtonHeld, bool rightButtonHeld)
{
	if (!settingsManager->enableGripCalibration)
	{
		gripCalibrationButtonMask.store(0, std::memory_order_release);
		return;
	}
	// This is logical controller-hand state from the dual-thumb-rest gesture.
	// Face buttons are deliberately not calibration inputs.
	const bool leftEligible = IsGripCalibrationEligible(0);
	const bool rightEligible = IsGripCalibrationEligible(1);
	uint8_t handMask = 0;
	if (leftButtonHeld && leftEligible)
		handMask |= 1U;
	if (rightButtonHeld && rightEligible)
		handMask |= 2U;
	gripCalibrationButtonMask.store(handMask, std::memory_order_release);
}

void WeaponManager::ProcessGripCalibration()
{
	RefreshRuntimeHandRoles("calibration-update");
	if (!settingsManager->enableGripCalibration)
	{
		if (IsGripCalibrationActive())
			CancelGripCalibration();
		gripCalibrationButtonMask.store(0, std::memory_order_release);
		gripCalibrationProcessedButtonMask = 0;
		if (gripCalibrationModeObserved)
		{
			uevr::API::get()->log_info("%s", "[GripCalibration] mode=disabled after settings reload");
			gripCalibrationModeObserved = false;
		}
		return;
	}
	if (!gripCalibrationModeObserved)
	{
		gripCalibrationTransforms.clear();
		gripCalibrationLoaded = false;
		LoadGripCalibration();
		gripCalibrationModeObserved = true;
		uevr::API::get()->log_info("%s", "[GripCalibration] mode=enabled after settings reload");
	}
	const uint8_t currentButtons = gripCalibrationButtonMask.load(std::memory_order_acquire);
	const uint8_t pressed = static_cast<uint8_t>(currentButtons & ~gripCalibrationProcessedButtonMask);
	const uint8_t released = static_cast<uint8_t>(gripCalibrationProcessedButtonMask & ~currentButtons);
	gripCalibrationProcessedButtonMask = currentButtons;
	const uint8_t grips = gripStateMask.load(std::memory_order_acquire);
	for (int hand = 0; hand < 2; ++hand)
	{
		const uint8_t bit = static_cast<uint8_t>(1U << hand);
		if ((pressed & bit) != 0 && IsGripCalibrationEligible(hand))
			BeginGripCalibration(hand);
		if ((released & bit) != 0)
			EndGripCalibration(hand, true);
		if (gripCalibrationSessions[static_cast<size_t>(hand)].active
			&& (gripCalibrationSessions[static_cast<size_t>(hand)].runtimeGeneration != runtimeHandGeneration
				|| (grips & bit) == 0 || firstWeaponMesh != gripCalibrationSessions[static_cast<size_t>(hand)].frozenWeaponMesh
				|| static_cast<int>(currentWeaponEquipped) != gripCalibrationSessions[static_cast<size_t>(hand)].weaponId))
			EndGripCalibration(hand, false);
	}
	if (IsGripCalibrationActive())
		EnforceGripCalibrationFreeze();
}

void WeaponManager::CancelGripCalibration()
{
	const bool wasActive = IsGripCalibrationActive();
	ResetRuntimeHandState("calibration-cancelled", false, true);
	if (wasActive)
		uevr::API::get()->log_info("%s", "[GripCalibration] cancelled; normal tracking restored");
}

void WeaponManager::ResetGripCalibration()
{
	LoadGripCalibration();
	const int weaponId = static_cast<int>(currentWeaponEquipped);
	if (weaponId < 0)
		return;
	const auto previousIt = gripCalibrationTransforms.find(weaponId);
	if (previousIt == gripCalibrationTransforms.end())
		return;
	const WeaponGripCalibration previous = previousIt->second;
	gripCalibrationTransforms.erase(weaponId);
	if (!SaveGripCalibration())
	{
		gripCalibrationTransforms[weaponId] = previous;
		uevr::API::get()->log_warn("[GripCalibration] reset rejected weapon=%d reason=persistence", weaponId);
		return;
	}
	uevr::API::get()->log_info("[GripCalibration] reset weapon=%d", weaponId);
}

void WeaponManager::UpdateActualWeaponMesh()
{
	if (settingsManager->debugMod) uevr::API::get()->log_info("UpdateActualWeaponMesh()");
	const auto publishTwoHandIneligible = [this]()
	{
		twoHandLatchEligibleSnapshot.store(false, std::memory_order_release);
		twoHandConfiguredHandSnapshot.store(-1, std::memory_order_release);
		twoHandPrimaryHand.store(-1, std::memory_order_release);
	};
	const auto logVehicleResolution = [this](int reason)
	{
		if (!playerManager->isInVehicle)
		{
			vehicleFreeAimLastRejection = -1;
			return;
		}
		if (reason == vehicleFreeAimLastRejection)
			return;
		vehicleFreeAimLastRejection = reason;
		const char* result = reason == 0 ? "eligible" : "rejected";
		uevr::API::get()->log_info(
			"[VehicleFreeAim] %s reason=%d weapon=%d mesh=%p container=%p camera=%d vehicleType=%d",
			result,
			reason,
			static_cast<int>(currentWeaponEquipped),
			firstWeaponMesh,
			firstWeaponContainer,
			static_cast<int>(cameraController->currentCameraMode),
			static_cast<int>(playerManager->vehicleType));
	};

	if (cameraController->currentCameraMode == CameraController::Camera  && cameraController->previousCameraMode == CameraController::Camera )
	{
		logVehicleResolution(4);
		publishTwoHandIneligible();
		return;
	}

	//static auto gta_weapon_c = uevr::API::get()->find_uobject<uevr::API::UClass>(L"Class /Script/GTABase.GTAWeapon");
	//static auto gta_BPweapon_c = uevr::API::get()->find_uobject<uevr::API::UClass>(L"BlueprintGeneratedClass /Game/SanAndreas/GameData/Blueprints/BP_GTASA_Weapon.BP_GTASA_Weapon_C");
	auto gta_BPplayerCharacter_c = uevr::API::get()->find_uobject<uevr::API::UClass>(L"BlueprintGeneratedClass /Game/SanAndreas/Characters/Player/BP_player_character.BP_Player_Character_C");
	auto gta_StaticMeshComponent_c = uevr::API::get()->find_uobject<uevr::API::UClass>(L"Class /Script/Engine.StaticMeshComponent");
	//API::get()->log_info("gta_BPweapon_c = %ls", gta_BPweapon_c->get_full_name().c_str());

	if (playerManager->playerController == nullptr || gta_BPplayerCharacter_c == nullptr || gta_StaticMeshComponent_c == nullptr)
	{
		RemoveCustomAkimboVisual("player-unavailable");
		logVehicleResolution(8);
		currentWeaponEquipped = Unarmed;
		firstWeaponMesh = nullptr;
		firstWeaponStaticMesh = nullptr;
		secondWeaponMesh = nullptr;
		secondWeaponStaticMesh = nullptr;
		firstWeaponContainer = nullptr;
		secondWeaponContainer = nullptr;
		visibilityAppliedFirstWeaponMesh = nullptr;
		visibilityAppliedSecondWeaponMesh = nullptr;
		motionConfiguredFirstWeaponMesh = nullptr;
		motionConfiguredSecondWeaponMesh = nullptr;
		motionConfiguredFirstHand = -1;
		motionConfiguredSecondHand = -1;
		motionConfiguredFirstCalibrationRole = -1;
		publishTwoHandIneligible();
		return;
	}

	const auto& playerControllerChildren = playerManager->playerController->get_property<uevr::API::TArray<uevr::API::UObject*>>(L"Children");
	//API::get()->log_info("children = %ls", children.data[4]->get_full_name().c_str());
	uevr::API::UObject* gta_BPplayerCharacter = nullptr;
	for (auto child : playerControllerChildren) {
		if (child != nullptr && gta_BPplayerCharacter == nullptr && child->is_a(gta_BPplayerCharacter_c)) {
			gta_BPplayerCharacter = child;
		}
	}
	if (gta_BPplayerCharacter != nullptr)
		torso = gta_BPplayerCharacter->get_property<uevr::API::UObject*>(L"torso");
	else
	{
		RemoveCustomAkimboVisual("character-unavailable");
		uevr::API::get()->log_info("gta_BPplayerCharacter not found.");
		logVehicleResolution(5);
		torso = nullptr;
		currentWeaponEquipped = Unarmed;
		firstWeaponMesh = nullptr;
		firstWeaponStaticMesh = nullptr;
		secondWeaponMesh = nullptr;
		secondWeaponStaticMesh = nullptr;
		firstWeaponContainer = nullptr;
		secondWeaponContainer = nullptr;
		visibilityAppliedFirstWeaponMesh = nullptr;
		visibilityAppliedSecondWeaponMesh = nullptr;
		motionConfiguredFirstWeaponMesh = nullptr;
		motionConfiguredSecondWeaponMesh = nullptr;
		motionConfiguredFirstHand = -1;
		motionConfiguredSecondHand = -1;
		motionConfiguredFirstCalibrationRole = -1;
		publishTwoHandIneligible();
		return;
	}
	if (torso == nullptr)
	{
		RemoveCustomAkimboVisual("torso-unavailable");
		logVehicleResolution(5);
		currentWeaponEquipped = Unarmed;
		firstWeaponMesh = nullptr;
		firstWeaponStaticMesh = nullptr;
		firstWeaponContainer = nullptr;
		secondWeaponMesh = nullptr;
		secondWeaponStaticMesh = nullptr;
		secondWeaponContainer = nullptr;
		publishTwoHandIneligible();
		return;
	}

	if (magneticIdleWeaponDetached
		&& (playerManager->isInVehicle
			|| (magneticDetachedPlayerCharacter != nullptr
				&& magneticDetachedPlayerCharacter != gta_BPplayerCharacter)))
		RestoreMagneticIdleWeaponAttachment(playerManager->isInVehicle
			? "vehicle-discovery" : "character-replacement");
	const auto& torsoChildren = torso->get_property<uevr::API::TArray<uevr::API::UObject*>>(L"AttachChildren");
	uevr::API::UObject* firstWeaponMeshFetch = nullptr;
	uevr::API::UObject* secondWeaponMeshFetch = nullptr;
	uevr::API::UObject* firstWeaponContainerFetch = nullptr;
	uevr::API::UObject* secondWeaponContainerFetch = nullptr;

	// A stashed/held magnetic weapon is deliberately detached from GTA's animated
	// hand container. Retain that exact validated mesh until it is restored; a
	// torso AttachChildren scan cannot rediscover a detached component.
	if (magneticIdleWeaponDetached && magneticDetachedWeaponMesh != nullptr
		&& magneticIdleNativeParent != nullptr
		&& uevr::API::UObjectHook::exists(magneticDetachedWeaponMesh)
		&& uevr::API::UObjectHook::exists(magneticIdleNativeParent))
	{
		// A cheat or mission script can attach a replacement mesh while our old
		// presentation is body-root detached. Inspect only CJ's normal torso child
		// containers and adopt a validated, differently identified weapon. This
		// avoids the CPed selected-slot value, which is transiently zero during
		// ordinary reload/weapon states and caused repeated false invalidation.
		uevr::API::UObject* replacementMesh = nullptr;
		uevr::API::UObject* replacementContainer = nullptr;
		const int ownedWeaponId = magneticGripWeaponId >= 0
			? magneticGripWeaponId : magneticIdleWeaponId;
		for (auto child : torsoChildren)
		{
			if (child == nullptr || child->is_a(gta_StaticMeshComponent_c))
				continue;
			const auto& attached = child->get_property<uevr::API::TArray<uevr::API::UObject*>>(L"AttachChildren");
			if (attached.count <= 0 || attached.data == nullptr || attached.data[0] == nullptr
				|| attached.data[0] == magneticDetachedWeaponMesh
				|| !attached.data[0]->is_a(gta_StaticMeshComponent_c))
				continue;
			auto candidateAsset = attached.data[0]->get_property<uevr::API::UObject*>(L"StaticMesh");
			if (candidateAsset == nullptr || !uevr::API::UObjectHook::exists(candidateAsset))
				continue;
			const auto candidateIt = weaponNameToIndex.find(candidateAsset->get_fname()->to_string());
			if (candidateIt == weaponNameToIndex.end() || candidateIt->second == ownedWeaponId)
				continue;
			replacementMesh = attached.data[0];
			replacementContainer = child;
			break;
		}
		if (replacementMesh != nullptr)
		{
			RestoreMagneticIdleWeaponAttachment("external-component-replacement");
			magneticIdleWeaponActive = false;
			magneticIdleWeaponId = -1;
			magneticGripHand = -1;
			magneticGripWeaponId = -1;
			magneticGripAttached = false;
			magneticReleaseRequested = false;
			magneticAnchoredWeaponMesh = nullptr;
			// Explicit X-cycle replacement is a presentation swap, not a new
			// holster placement. Keep the transition state until
			// ProcessMagneticIdleWeapon selects the replacement's own anchor.
			if (!explicitWeaponCyclePending)
			{
				magneticCustomAnchorValid = false;
				magneticCustomAnchorWeaponId = -1;
				magneticLastHeldPoseHand = -1;
				magneticLastHeldPoseGripGeneration = 0;
				magneticStableBodyRotationValid = false;
				magneticStableBodyRotationLastUpdate = 0;
				magneticBodyFrameRebaseAt = 0;
			}
			firstWeaponMeshFetch = replacementMesh;
			firstWeaponContainerFetch = replacementContainer;
			uevr::API::get()->log_info(
				"[MagneticWeapon] external component replacement adopted previousWeapon=%d mesh=%p",
				ownedWeaponId, replacementMesh);
		}
		else
		{
			firstWeaponMeshFetch = magneticDetachedWeaponMesh;
			firstWeaponContainerFetch = magneticIdleNativeParent;
		}
	}
	else
	{
		if (magneticIdleWeaponDetached)
			RestoreMagneticIdleWeaponAttachment("invalid-detached-state");
		for (auto child : torsoChildren) {
			if (child == nullptr)
				continue;

			// If dual wield, fetch second weapon
			if (firstWeaponMeshFetch != nullptr && !child->is_a(gta_StaticMeshComponent_c)) {
				secondWeaponContainerFetch = child;
				const auto& attachChildren = child->get_property<uevr::API::TArray<uevr::API::UObject*>>(L"AttachChildren");
				if (attachChildren.count > 0 && attachChildren.data != nullptr)
					secondWeaponMeshFetch = attachChildren.data[0];
			}
			if (firstWeaponMeshFetch == nullptr && !child->is_a(gta_StaticMeshComponent_c)) {
				firstWeaponContainerFetch = child;
				const auto& attachChildren = child->get_property<uevr::API::TArray<uevr::API::UObject*>>(L"AttachChildren");
				if (attachChildren.count > 0 && attachChildren.data != nullptr)
					firstWeaponMeshFetch = attachChildren.data[0];
			}
		}
	}

	if (firstWeaponMeshFetch != nullptr && firstWeaponMeshFetch->is_a(gta_StaticMeshComponent_c))
	{
		firstWeaponMesh = firstWeaponMeshFetch;
		firstWeaponStaticMesh = firstWeaponMesh->get_property<uevr::API::UObject*>(L"StaticMesh");
		firstWeaponContainer = firstWeaponContainerFetch;
	}
	else
	{
		RemoveCustomAkimboVisual("weapon-mesh-unavailable");
		logVehicleResolution(5);
		currentWeaponEquipped = Unarmed;
		firstWeaponMesh = nullptr;
		firstWeaponStaticMesh = nullptr;
		firstWeaponContainer = nullptr;
		secondWeaponMesh = nullptr;
		secondWeaponStaticMesh = nullptr;
		secondWeaponContainer = nullptr;
		visibilityAppliedFirstWeaponMesh = nullptr;
		motionConfiguredFirstWeaponMesh = nullptr;
		motionConfiguredFirstHand = -1;
		motionConfiguredFirstCalibrationRole = -1;
		publishTwoHandIneligible();
		return;
	}

	if (secondWeaponMeshFetch != nullptr && secondWeaponMeshFetch->is_a(gta_StaticMeshComponent_c))
	{
		secondWeaponMesh = secondWeaponMeshFetch;
		secondWeaponStaticMesh = secondWeaponMesh->get_property<uevr::API::UObject*>(L"StaticMesh");
		secondWeaponContainer = secondWeaponContainerFetch;
	}
	else
	{
		secondWeaponMesh = nullptr;
		secondWeaponStaticMesh = nullptr;
		secondWeaponContainer = nullptr;
	}
	if (firstWeaponStaticMesh == nullptr
		|| !uevr::API::UObjectHook::exists(firstWeaponStaticMesh))
	{
		RemoveCustomAkimboVisual("weapon-asset-unavailable");
		logVehicleResolution(5);
		currentWeaponEquipped = Unarmed;
		firstWeaponMesh = nullptr;
		firstWeaponStaticMesh = nullptr;
		firstWeaponContainer = nullptr;
		secondWeaponMesh = nullptr;
		secondWeaponStaticMesh = nullptr;
		secondWeaponContainer = nullptr;
		publishTwoHandIneligible();
		return;
	}

	std::wstring weaponName = firstWeaponStaticMesh->get_fname()->to_string();
	// Look up the weapon before attachment selection so the T-0021 eligibility
	// snapshot is for this frame's weapon, not the previous frame's weapon.
	auto it = weaponNameToIndex.find(weaponName);
	if (it != weaponNameToIndex.end())
		currentWeaponEquipped = static_cast<WeaponType>(it->second);

	const int discoveredWeapon = static_cast<int>(currentWeaponEquipped);
	const bool customAkimboSupported = discoveredWeapon == Pistol || discoveredWeapon == Sawnoff
		|| discoveredWeapon == MicroUzi || discoveredWeapon == Tec9;
	if (secondWeaponMeshFetch != nullptr)
	{
		// Prefer GTA's live second mesh if one exists; never show a third gun.
		RemoveCustomAkimboVisual("native-second-present");
		secondWeaponMesh = secondWeaponMeshFetch;
		secondWeaponStaticMesh = secondWeaponMesh->get_property<uevr::API::UObject*>(L"StaticMesh");
		secondWeaponContainer = secondWeaponContainerFetch;
	}
	else if (settingsManager != nullptr && settingsManager->enableCustomAkimbo
		&& customAkimboSupported && playerManager->isInControl && !playerManager->isInVehicle
		&& (gripStateMask.load(std::memory_order_acquire) & 0x03U) == 0x03U)
	{
		EnsureCustomAkimboVisual();
	}
	else
	{
		RemoveCustomAkimboVisual("ineligible");
	}

	const bool wasVehicleFreeAimActive = vehicleFreeAimPresentationActive;
	const bool vehicleFreeAimActive = IsVehicleFreeAimActive();
	if (!playerManager->isInVehicle)
		vehicleFreeAimLastRejection = -1;
	else
	{
		int rejection = 0;
		if (!kVehicleFreeAimExperimentalEnabled || settingsManager == nullptr
			|| !settingsManager->enableVehicleFaceButtonFire)
			rejection = 1; // disabled by feature flag
		else if (!playerManager->isInControl)
			rejection = 2; // no local-player control
		else if (playerManager->vehicleType != PlayerManager::CarOrBoat
			&& playerManager->vehicleType != PlayerManager::Bike)
			rejection = 3; // aircraft/unsupported vehicle
		else if (cameraController->currentCameraMode == CameraController::Camera
			|| cameraController->currentCameraMode == CameraController::AimWeaponFromCar)
			rejection = 4; // preserve native camera states
		else if (firstWeaponMesh == nullptr || !uevr::API::UObjectHook::exists(firstWeaponMesh)
			|| firstWeaponContainer == nullptr
			|| !uevr::API::UObjectHook::exists(firstWeaponContainer))
			rejection = 5; // no live lap-attached mesh
		else if (!IsVehicleFreeAimSupportedWeapon())
			rejection = 6; // unsupported weapon class
		else if (secondWeaponMesh != nullptr)
			rejection = 7; // dual-wield path is intentionally not part of this proof
		logVehicleResolution(rejection);
	}
	if (vehicleFreeAimPresentationActive != vehicleFreeAimActive)
	{
		vehicleFreeAimPresentationActive = vehicleFreeAimActive;
		uevr::API::get()->dispatch_lua_event("vehicleFreeAimState",
			vehicleFreeAimActive ? "true" : "false");
		uevr::API::get()->log_info("[VehicleFreeAim] %s weapon=%d camera=%d hand=right mode=%s",
			vehicleFreeAimActive ? "active" : "inactive",
			static_cast<int>(currentWeaponEquipped),
			static_cast<int>(cameraController->currentCameraMode),
			vehicleFreeAimActive ? "controller-pistol" : "native");
	}
	if (vehicleFreeAimActive)
	{
		// The vehicle visibility script normally leaves the weapon in GTA's native
		// lap state. Reuse that same live mesh and move only its UEVR presentation.
		if (!wasVehicleFreeAimActive)
			SetWeaponScaled(true, true);
		motionWeaponTrackingEnabled = true;
		visualWeaponTrackingEnabled = true;
		magneticIdleWeaponActive = false;
		magneticIdleWeaponId = -1;
		magneticGripHand = -1;
		magneticGripWeaponId = -1;
		magneticGripAttached = false;
		magneticReleaseRequested = false;
		magneticBodyFrameRebaseAt = 0;
	}
	else if (wasVehicleFreeAimActive)
	{
		// A vehicle mesh is only controller-presented while the bounded prototype is
		// eligible. On exit or camera/weapon rejection, return that same live mesh to
		// its native attachment before the on-foot magnetic state is evaluated.
		if (firstWeaponMesh != nullptr && uevr::API::UObjectHook::exists(firstWeaponMesh))
			UnhookAndRepositionWeapon(false);
		motionConfiguredFirstWeaponMesh = nullptr;
		motionConfiguredFirstHand = -1;
		motionConfiguredFirstCalibrationRole = -1;
		motionWeaponTrackingEnabled = false;
		visualWeaponTrackingEnabled = false;
		magneticIdleWeaponActive = false;
		magneticIdleWeaponId = -1;
		magneticGripHand = -1;
		magneticGripWeaponId = -1;
		magneticGripAttached = false;
		magneticReleaseRequested = false;
		magneticBodyFrameRebaseAt = 0;
	}

	const int configuredHand = settingsManager->leftHandedMode != SettingsManager::Disabled ? 0 : 1;
	const bool twoHandEligibleNow = settingsManager->enableTwoHandStabilization
		&& playerManager->isInControl
		&& !playerManager->isInVehicle
		&& !playerManager->weaponWheelEnabled
		&& cameraController->currentCameraMode != CameraController::Camera
		&& firstWeaponMesh != nullptr
		&& uevr::API::UObjectHook::exists(firstWeaponMesh)
		&& secondWeaponMesh == nullptr
		&& IsTwoHandLongGun();
	twoHandConfiguredHandSnapshot.store(static_cast<int8_t>(configuredHand), std::memory_order_release);
	twoHandLatchEligibleSnapshot.store(twoHandEligibleNow, std::memory_order_release);
	// The game-thread attachment owner is authoritative. Repair only a stale
	// input latch that contradicts the hand currently holding this exact mesh.
	// This covers a direct left<->right transfer before a second grip arrives.
	const uint8_t currentGripMask = gripStateMask.load(std::memory_order_acquire);
	if (twoHandEligibleNow && magneticGripHand >= 0 && magneticGripHand <= 1
		&& (currentGripMask & static_cast<uint8_t>(1U << magneticGripHand)) != 0
		&& twoHandPrimaryHand.load(std::memory_order_acquire) != magneticGripHand)
	{
		const int staleHand = twoHandPrimaryHand.load(std::memory_order_relaxed);
		twoHandFirstGripHandSnapshot.store(static_cast<int8_t>(magneticGripHand), std::memory_order_release);
		twoHandPrimaryHand.store(static_cast<int8_t>(magneticGripHand), std::memory_order_release);
		if (settingsManager->debugInputLayerProbe)
			uevr::API::get()->log_info(
				"[TwoHand] attachment-owner repair stale=%d primary=%d weapon=%d mask=%u",
				staleHand, magneticGripHand, static_cast<int>(currentWeaponEquipped),
				static_cast<unsigned int>(currentGripMask));
	}
	if (twoHandEligibleNow && currentGripMask != 0
		&& twoHandPrimaryHand.load(std::memory_order_acquire) < 0)
	{
		const int firstGripHand = twoHandFirstGripHandSnapshot.load(std::memory_order_acquire);
		if (firstGripHand >= 0)
			twoHandPrimaryHand.store(static_cast<int8_t>(firstGripHand), std::memory_order_release);
	}
	if (!twoHandEligibleNow)
		twoHandPrimaryHand.store(-1, std::memory_order_release);

	// A weapon change replaces these components after the Lua visibility state
	// was already applied. Reapply only to a newly discovered component; writing
	// visibility every frame can fight GTA's native left-hand weapon state.
	if (visibilityAppliedFirstWeaponMesh != firstWeaponMesh)
	{
		SetComponentVisibility(firstWeaponMesh, weaponScaledVisible);
		visibilityAppliedFirstWeaponMesh = firstWeaponMesh;
		if (settingsManager->debugInputLayerProbe)
			uevr::API::get()->log_info("[WeaponAttach] first mesh=%p visible=%s", firstWeaponMesh, weaponScaledVisible ? "true" : "false");
	}
	if (visibilityAppliedSecondWeaponMesh != secondWeaponMesh)
	{
		const bool customVisual = secondWeaponMesh != nullptr
			&& secondWeaponMesh == customAkimboVisualMesh;
		const bool secondVisible = weaponScaledVisible && (!customVisual
			|| customAkimboActive);
		SetComponentVisibility(secondWeaponMesh, secondVisible);
		visibilityAppliedSecondWeaponMesh = secondWeaponMesh;
		if (settingsManager->debugInputLayerProbe)
			uevr::API::get()->log_info("[WeaponAttach] second mesh=%p visible=%s custom=%s",
				secondWeaponMesh, secondVisible ? "true" : "false", customVisual ? "true" : "false");
	}

	if (visualWeaponTrackingEnabled && !magneticIdleWeaponActive && !IsGripCalibrationActive()
		&& cameraController->currentCameraMode != CameraController::Camera
		&& (vehicleFreeAimActive || !playerManager->isInVehicle
			|| cameraController->currentCameraMode == CameraController::AimWeaponFromCar))
	{
		const int latchedTwoHandPrimary = twoHandPrimaryHand.load(std::memory_order_relaxed);
		const bool lockTwoHandPrimary = twoHandLatchEligibleSnapshot.load(std::memory_order_acquire)
			&& gripStateMask.load(std::memory_order_acquire) != 0
			&& latchedTwoHandPrimary >= 0;
		const bool customAkimboPresentation = secondWeaponMesh == customAkimboVisualMesh
			&& (gripStateMask.load(std::memory_order_acquire) & 0x03U) == 0x03U;
		const int desiredFirstHand = vehicleFreeAimActive
			? 1
			: customAkimboPresentation && customAkimboPrimaryHand >= 0
			? customAkimboPrimaryHand
			: magneticGripHand >= 0
			? magneticGripHand
			: lockTwoHandPrimary
			? latchedTwoHandPrimary
			: (settingsManager->leftHandedMode != SettingsManager::Disabled ? 0 : 1);
		const int desiredCalibrationRole = settingsManager->enableGripCalibration
			? desiredFirstHand : -1;
		if (motionConfiguredFirstWeaponMesh != firstWeaponMesh || motionConfiguredFirstHand != desiredFirstHand
			|| motionConfiguredFirstCalibrationRole != desiredCalibrationRole)
		{
			auto motionState = uevr::API::UObjectHook::get_or_add_motion_controller_state(firstWeaponMesh);
			const glm::fvec3 weaponPosition = GetWeaponGripPositionOffset(desiredFirstHand);
			const UEVR_Vector3f weaponPositionUevr = { weaponPosition.x, weaponPosition.y, weaponPosition.z };
			glm::fquat defaultWeaponRotationQuat = GetWeaponGripRotationOffset(desiredFirstHand);
			UEVR_Quaternionf defaultWeaponRotationQuat_UEVR = { defaultWeaponRotationQuat.w , defaultWeaponRotationQuat.x, defaultWeaponRotationQuat.y, defaultWeaponRotationQuat.z };
			motionState->set_location_offset(&weaponPositionUevr);
			motionState->set_rotation_offset(&defaultWeaponRotationQuat_UEVR);
			motionState->set_hand(desiredFirstHand);
			motionState->set_permanent(true);
			motionConfiguredFirstWeaponMesh = firstWeaponMesh;
			motionConfiguredFirstHand = desiredFirstHand;
			motionConfiguredFirstCalibrationRole = desiredCalibrationRole;
			if (magneticGripHand == desiredFirstHand)
				magneticGripAttached = true;
			if (settingsManager->enableGripCalibration)
			{
				GripCalibrationTransform canonicalPrimary;
				int calibrationSourceHand = -1;
				if (ReadGripCalibrationTransform(static_cast<int>(currentWeaponEquipped),
					GripCalibrationRecord::PrimaryGrip, desiredFirstHand,
					canonicalPrimary, &calibrationSourceHand))
				{
					const glm::fvec3 leftControllerPosition = GetWeaponGripPositionOffset(0);
					const glm::fvec3 rightControllerPosition = GetWeaponGripPositionOffset(1);
					uevr::API::get()->log_info(
						"[GripCalibration] primary-position weapon=%d requestedHand=%d sourceHand=%d seed=%s storedComponent=(%.2f %.2f %.2f) controllerLeft=(%.2f %.2f %.2f) controllerRight=(%.2f %.2f %.2f)",
						static_cast<int>(currentWeaponEquipped), desiredFirstHand,
						calibrationSourceHand, calibrationSourceHand == desiredFirstHand ? "exact" : "mirrored",
						canonicalPrimary.position.x,
						canonicalPrimary.position.y, canonicalPrimary.position.z,
						leftControllerPosition.x, leftControllerPosition.y, leftControllerPosition.z,
						rightControllerPosition.x, rightControllerPosition.y, rightControllerPosition.z);
				}
			}
			if (settingsManager->debugInputLayerProbe)
				uevr::API::get()->log_info("[WeaponAttach] first mesh=%p hand=%d gripOffset=(%.2f %.2f %.2f)",
					firstWeaponMesh, desiredFirstHand, weaponPosition.x, weaponPosition.y, weaponPosition.z);
		}
		if (secondWeaponMesh != nullptr)
		{
			const int desiredSecondHand = secondWeaponMesh == customAkimboVisualMesh
				? 1 - desiredFirstHand
				: (settingsManager->leftHandedMode != SettingsManager::Disabled ? 1 : 0);
			if (motionConfiguredSecondWeaponMesh != secondWeaponMesh || motionConfiguredSecondHand != desiredSecondHand)
			{
				auto motionState = uevr::API::UObjectHook::get_or_add_motion_controller_state(secondWeaponMesh);
				const glm::fvec3 weaponPosition = GetWeaponGripPositionOffset(desiredSecondHand);
				const UEVR_Vector3f weaponPositionUevr = { weaponPosition.x, weaponPosition.y, weaponPosition.z };
				glm::fquat defaultWeaponRotationQuat = GetWeaponGripRotationOffset(desiredSecondHand);
				UEVR_Quaternionf defaultWeaponRotationQuat_UEVR = { defaultWeaponRotationQuat.w , defaultWeaponRotationQuat.x, defaultWeaponRotationQuat.y, defaultWeaponRotationQuat.z };
				motionState->set_location_offset(&weaponPositionUevr);
				motionState->set_rotation_offset(&defaultWeaponRotationQuat_UEVR);
				motionState->set_hand(desiredSecondHand);
				motionState->set_permanent(true);
				motionConfiguredSecondWeaponMesh = secondWeaponMesh;
				motionConfiguredSecondHand = desiredSecondHand;
				if (settingsManager->debugInputLayerProbe)
					uevr::API::get()->log_info("[WeaponAttach] second mesh=%p hand=%d gripOffset=(%.2f %.2f %.2f)",
						secondWeaponMesh, desiredSecondHand, weaponPosition.x, weaponPosition.y, weaponPosition.z);
			}
		}
		else
		{
			motionConfiguredSecondWeaponMesh = nullptr;
			motionConfiguredSecondHand = -1;
		}
	}
	if ((cameraController->currentCameraMode != CameraController::Camera
		&& playerManager->isInVehicle && !playerManager->wasInVehicle)
		&& cameraController->currentCameraMode != CameraController::AimWeaponFromCar
		&& !vehicleFreeAimActive)
	{
		uevr::API::UObjectHook::remove_motion_controller_state(firstWeaponMesh);
		if (secondWeaponMesh != nullptr)
			uevr::API::UObjectHook::remove_motion_controller_state(secondWeaponMesh);
		motionConfiguredFirstWeaponMesh = nullptr;
		motionConfiguredSecondWeaponMesh = nullptr;
		motionConfiguredFirstHand = -1;
		motionConfiguredSecondHand = -1;
		motionConfiguredFirstCalibrationRole = -1;
	}

	if (previousWeaponEquipped != currentWeaponEquipped)
	{
		ResetRuntimeHandState("weapon-change", true, true);
		ResetShootingState();
		uevr::API::get()->dispatch_lua_event("currentWeapon", std::to_string(static_cast<int>(currentWeaponEquipped)));
	}
}

void WeaponManager::ResetShootingState()
{
	firstWeaponIsShooting = false;
	secondWeaponIsShooting = false;
	customAkimboHeldMaskSnapshot.store(0, std::memory_order_release);
	customAkimboEdgeMaskPending.store(0, std::memory_order_release);
	customAkimboInputWeapon.store(-1, std::memory_order_release);
	customAkimboActive = false;
	customAkimboLastRejection = -1;
	customAkimboPrimaryHand = -1;
	customAkimboMuzzleValidMask = 0;
	customAkimboFlashAlternateHand = 0;
	customAkimboLastEffectMuzzleWorld = {};
	customAkimboLastEffectMuzzleValid = false;
	customAkimboMuzzleEffectTemplate = nullptr;
	customAkimboMuzzleEffectTemplates.clear();
	customAkimboMuzzleEffectScales.clear();
	customAkimboMuzzleEffectTemplateWeapon = -1;
	customAkimboPendingEffectHandMask = 0;
	customAkimboPendingEffectSince = 0;
	customAkimboMissingEffectTemplateLogged = false;
	if (memoryManager != nullptr)
		memoryManager->ClearCustomAkimboState();
	motionMeleeNativeTriggerBlockSnapshot.store(false, std::memory_order_release);
	motionMeleeClenchMask.store(0, std::memory_order_release);
	motionMeleeLastLoggedClenchMask = 0xFF;
	motionMeleeControllerPositionValid = { false, false };
	motionMeleeContactPoseValid = { false, false };
	for (auto& entities : motionMeleeContactEntities)
		entities.clear();
	motionMeleeRejectedContactDiagnostics.clear();
	motionMeleeContactSwingGeneration = { 0, 0 };
	++motionMeleeTransitionGeneration;
	motionMeleeLastWeapon = -1;
	motionMeleeLastGripMask = 0;
	motionMeleeLastEligible = false;
	motionMeleeLowSpeedDwellRemaining = { 0.0f, 0.0f };
	motionMeleeInterSwingCooldownRemaining = { 0.0f, 0.0f };
	motionMeleeContactWindowRemaining = { 0.0f, 0.0f };
	motionMeleeHandActiveState = { false, false };
	motionMeleeHandGeometryMode = { -1, -1 };
	firstWeaponShotDone = false;
	firstWeaponLastParticleShot = nullptr;
	secondWeaponLastParticleShot = nullptr;
	shotHierarchyProbeLogged = false;
	hasStableGameAim = false;
	stableGameAimLatchFrames = 0;
	aimLatchFallbackLoggedThisShot = false;
	aimLatchJumpLoggedThisShot = false;
}

bool WeaponManager::HideBulletTrace()
{
	auto owner = uevr::API::get()->find_uobject<uevr::API::UObject>(L"BP_Water_Base_C /Game/SanAndreas/Maps/SAWorld/SAWorld.SAWorld.PersistentLevel.BP_Water_Base_4");
	if (owner == nullptr)
		return false;

	auto component = owner->get_property<uevr::API::UObject*>(L"BulletTrace");
	if (component == nullptr)
		return false;

	auto componentClass = component->get_class();
	if (componentClass == nullptr)
		return false;

	if (componentClass->find_property(L"bVisible") != nullptr)
		component->set_bool_property(L"bVisible", false);

	if (componentClass->find_property(L"bHiddenInGame") != nullptr)
		component->set_bool_property(L"bHiddenInGame", true);

	return true;
}

void WeaponManager::CacheCustomAkimboEffectTemplate(uevr::API::UObject* component)
{
	if (component == nullptr || !uevr::API::UObjectHook::exists(component)
		|| component->get_class() == nullptr)
		return;
	auto* componentClass = component->get_class();
	if (componentClass->find_property(L"Template") == nullptr)
		return;
	auto* effectTemplate = component->get_property<uevr::API::UObject*>(L"Template");
	if (effectTemplate == nullptr || !uevr::API::UObjectHook::exists(effectTemplate))
		return;

	glm::fvec3 nativeScale(1.0f);
	auto* getScale = componentClass->find_function(L"GetComponentScale");
	if (getScale == nullptr)
		getScale = componentClass->find_function(L"K2_GetComponentScale");
	if (getScale != nullptr)
	{
		Utilities::ParameterSingleVector3 scaleParams{};
		getScale->call(component, &scaleParams);
		const glm::fvec3 candidate = glm::abs(scaleParams.vec3Value);
		if (IsFiniteVector(candidate)
			&& candidate.x > 0.001f && candidate.y > 0.001f && candidate.z > 0.001f
			&& candidate.x < 100.0f && candidate.y < 100.0f && candidate.z < 100.0f)
			nativeScale = candidate;
	}

	auto existing = std::find(customAkimboMuzzleEffectTemplates.begin(),
		customAkimboMuzzleEffectTemplates.end(), effectTemplate);
	if (existing == customAkimboMuzzleEffectTemplates.end())
	{
		customAkimboMuzzleEffectTemplates.push_back(effectTemplate);
		customAkimboMuzzleEffectScales.push_back(nativeScale);
		uevr::API::get()->log_info(
			"[CustomAkimboFlash] native effect template captured index=%llu name=%ls scale=(%.2f %.2f %.2f)",
			static_cast<unsigned long long>(customAkimboMuzzleEffectTemplates.size() - 1),
			effectTemplate->get_full_name().c_str(), nativeScale.x, nativeScale.y, nativeScale.z);
	}
	else
	{
		const size_t index = static_cast<size_t>(existing - customAkimboMuzzleEffectTemplates.begin());
		if (index < customAkimboMuzzleEffectScales.size())
			customAkimboMuzzleEffectScales[index] = nativeScale;
	}
	customAkimboMuzzleEffectTemplate = effectTemplate;
	customAkimboMuzzleEffectTemplateWeapon = static_cast<int>(currentWeaponEquipped);
	customAkimboMissingEffectTemplateLogged = false;
}

void WeaponManager::UpdateShootingState(bool firstWeapon)
{
	uevr::API::UObject* weaponMesh = firstWeapon ? firstWeaponMesh : secondWeaponMesh;
	std::vector<uevr::API::UObject*>& previousParticles = firstWeapon ? firstWeaponPreviousParticles : secondWeaponPreviousParticles;

	if (!weaponMesh || !uevr::API::UObjectHook::exists(weaponMesh))
		return;

	const auto& childrenParticle = weaponMesh->get_property<uevr::API::TArray<uevr::API::UObject*>>(L"AttachChildren");
	int particleCount = childrenParticle.count;
	//uevr::API::get()->log_info("particle count %s = %i", firstWeapon ? "first weapon : " : "second weapon : ", particleCount);
	//uevr::API::get()->log_info("previousParticleCount %s = %i", firstWeapon ? "first weapon : " : "second weapon : ", previousParticleCount);

	bool newParticleDetected = false;
	uevr::API::UObject* newParticle = nullptr;
	std::vector<uevr::API::UObject*> newParticles;

    for (size_t i = 0; i < childrenParticle.count; ++i)
    {
        auto particle = childrenParticle.data[i];
        // check if this particle is in the previousParticles
        if (std::find(previousParticles.begin(), previousParticles.end(), particle) == previousParticles.end())
        {
            newParticleDetected = true;
			if (newParticle == nullptr)
				newParticle = particle;
			newParticles.push_back(particle);
			/*uevr::API::get()->log_info("childrenParticle.data[i] = %ls", childrenParticle.data[i]->get_fname()->to_string().c_str());*/
        }
    }

    // Cache the current particles for next frame
    previousParticles.clear();
    for (size_t i = 0; i < childrenParticle.count; ++i)
        previousParticles.push_back(childrenParticle.data[i]);

	// A false positive detection sometimes happens the frame after a detection. Resetting and returning here
	// allows to discard it. No weapon can shoot on two consecutive frames anyway.
	if (firstWeaponIsShooting || secondWeaponIsShooting)
	{
		firstWeaponIsShooting = false;
		secondWeaponIsShooting = false;
		// GTA's ordinary cadence cannot create legitimate consecutive-frame
		// flashes. Custom akimbo can accept the opposite hand on the next frame,
		// so do not discard that independently owned particle pulse.
		if (!customAkimboActive)
			return;
	}

	const uint64_t effectNow = GetTickCount64();
	if (customAkimboActive && memoryManager != nullptr)
	{
		const uint8_t newlyAccepted = memoryManager->ConsumeCustomAkimboAcceptedHandMask() & 0x03U;
		if (newlyAccepted != 0)
		{
			if (customAkimboPendingEffectHandMask == 0)
				customAkimboPendingEffectSince = effectNow;
			customAkimboPendingEffectHandMask |= newlyAccepted;
		}
	}
	else
	{
		customAkimboPendingEffectHandMask = 0;
		customAkimboPendingEffectSince = 0;
	}

	// Capture the exact native particle system rather than guessing an asset.
	// This is valid even before akimbo engages, so a later left-first shot can
	// reuse the already observed weapon-specific flash/smoke effect.
	for (auto* particle : newParticles)
	{
		if (particle == nullptr || !uevr::API::UObjectHook::exists(particle)
			|| particle->get_class() == nullptr)
			continue;
		CacheCustomAkimboEffectTemplate(particle);
	}

	const auto spawnCustomAkimboEffect = [this](int hand) -> bool
	{
		if (hand < 0 || hand > 1
			|| customAkimboMuzzleEffectTemplate == nullptr
			|| customAkimboMuzzleEffectTemplateWeapon != static_cast<int>(currentWeaponEquipped)
			|| !uevr::API::UObjectHook::exists(customAkimboMuzzleEffectTemplate)
			|| (customAkimboMuzzleValidMask & static_cast<uint8_t>(1U << hand)) == 0
			|| playerManager == nullptr || playerManager->playerCharacter == nullptr)
			return false;
		bool spawnedAny = false;
		for (size_t effectIndex = 0;
			effectIndex < customAkimboMuzzleEffectTemplates.size(); ++effectIndex)
		{
			auto* effectTemplate = customAkimboMuzzleEffectTemplates[effectIndex];
			if (effectTemplate == nullptr || !uevr::API::UObjectHook::exists(effectTemplate))
				continue;
			glm::fvec3 effectScale = effectIndex < customAkimboMuzzleEffectScales.size()
				? customAkimboMuzzleEffectScales[effectIndex] : glm::fvec3(1.0f);
			// The world-spawned copy lacks the native weapon component's inherited
			// scale/instance setup. A small bounded correction matches the native
			// right-hand flash without changing the native effect itself.
			effectScale *= 1.20f;
			auto* spawned = SpawnParticleSystemAtLocation(playerManager->playerCharacter,
				effectTemplate, customAkimboMuzzleWorld[hand], effectScale);
			if (spawned == nullptr)
				continue;
			EnsureMotionThrowableVisualRenderable(spawned);
			spawnedAny = true;
		}
		if (!spawnedAny)
			return false;
		lastAimMuzzleWorldPosition = customAkimboMuzzleWorld[hand];
		customAkimboLastEffectMuzzleWorld = customAkimboMuzzleWorld[hand];
		customAkimboLastEffectMuzzleValid = true;
		return true;
	};

	if (newParticleDetected)
	{
		if (customAkimboActive && memoryManager != nullptr)
		{
			const uint8_t acceptedMask = customAkimboPendingEffectHandMask;
			uint8_t movedMask = 0;
			for (size_t particleIndex = 0; particleIndex < newParticles.size(); ++particleIndex)
			{
				int flashHand = -1;
				if (acceptedMask == 0x01U)
					flashHand = 0;
				else if (acceptedMask == 0x02U)
					flashHand = 1;
				else if (acceptedMask == 0x03U)
				{
					// GTA normally creates one particle for its authoritative right lane.
					// Keep it there and synthesize the missing left visual from its exact
					// template. If two particles exist, distribute one to each muzzle.
					flashHand = newParticles.size() >= 2
						? static_cast<int>(particleIndex & 1U)
						: 1;
				}
				auto* particle = newParticles[particleIndex];
				if (flashHand < 0
					|| (customAkimboMuzzleValidMask & static_cast<uint8_t>(1U << flashHand)) == 0
					|| particle == nullptr || !uevr::API::UObjectHook::exists(particle)
					|| particle->get_class() == nullptr
					|| particle->get_class()->find_function(L"K2_SetWorldLocation") == nullptr)
					continue;

				Utilities::Parameter_K2_SetWorldOrRelativeLocation setWorldLocationParams{};
				setWorldLocationParams.newLocation = customAkimboMuzzleWorld[flashHand];
				setWorldLocationParams.bSweep = false;
				setWorldLocationParams.bTeleport = true;
				particle->call_function(L"K2_SetWorldLocation", &setWorldLocationParams);
				lastAimMuzzleWorldPosition = customAkimboMuzzleWorld[flashHand];
				customAkimboLastEffectMuzzleWorld = customAkimboMuzzleWorld[flashHand];
				customAkimboLastEffectMuzzleValid = true;
				movedMask |= static_cast<uint8_t>(1U << flashHand);
			}
			uint8_t spawnedMask = 0;
			if ((acceptedMask & 0x01U) != 0 && (movedMask & 0x01U) == 0
				&& spawnCustomAkimboEffect(0))
				spawnedMask |= 0x01U;
			customAkimboPendingEffectHandMask = 0;
			customAkimboPendingEffectSince = 0;
			if (settingsManager->debugInputLayerProbe)
				uevr::API::get()->log_info(
					"[CustomAkimboFlash] particles=%llu acceptedMask=%u movedMask=%u spawnedMask=%u muzzleL=(%.1f %.1f %.1f) muzzleR=(%.1f %.1f %.1f)",
					static_cast<unsigned long long>(newParticles.size()),
					static_cast<unsigned int>(acceptedMask), static_cast<unsigned int>(movedMask),
					static_cast<unsigned int>(spawnedMask),
					customAkimboMuzzleWorld[0].x, customAkimboMuzzleWorld[0].y, customAkimboMuzzleWorld[0].z,
					customAkimboMuzzleWorld[1].x, customAkimboMuzzleWorld[1].y, customAkimboMuzzleWorld[1].z);
		}
		memoryManager->RecordTriggerTimingMuzzleParticle(static_cast<int>(currentWeaponEquipped), firstWeapon);
		if (!shotHierarchyProbeLogged && settingsManager->debugSpreadProbe)
		{
			const auto dumpChildren = [](const char* label, uevr::API::UObject* parent)
			{
				if (parent == nullptr || !uevr::API::UObjectHook::exists(parent))
					return;
				auto parentClass = parent->get_class();
				const auto parentName = parent->get_full_name();
				const auto parentClassName = parentClass != nullptr ? parentClass->get_full_name() : L"<no class>";
				uevr::API::get()->log_info("[ShotHierarchy] %s parent=%p class=%ls name=%ls", label,
					parent, parentClassName.c_str(), parentName.c_str());
				if (parentClass == nullptr || parentClass->find_property(L"AttachChildren") == nullptr)
					return;
				const auto& children = parent->get_property<uevr::API::TArray<uevr::API::UObject*>>(L"AttachChildren");
				uevr::API::get()->log_info("[ShotHierarchy] %s childCount=%d", label, children.count);
				for (size_t i = 0; i < children.count; ++i)
				{
					auto child = children.data != nullptr ? children.data[i] : nullptr;
					if (child == nullptr || !uevr::API::UObjectHook::exists(child))
						continue;
					auto childClass = child->get_class();
					const auto childName = child->get_full_name();
					const auto childClassName = childClass != nullptr ? childClass->get_full_name() : L"<no class>";
					uevr::API::get()->log_info("[ShotHierarchy] %s child[%llu]=%p class=%ls name=%ls", label,
						static_cast<unsigned long long>(i), child, childClassName.c_str(), childName.c_str());
				}
			};

			if (newParticle != nullptr && uevr::API::UObjectHook::exists(newParticle))
			{
				auto particleClass = newParticle->get_class();
				const auto particleName = newParticle->get_full_name();
				const auto particleClassName = particleClass != nullptr ? particleClass->get_full_name() : L"<no class>";
				uevr::API::get()->log_info("[ShotHierarchy] detected hand=%s particle=%p class=%ls name=%ls",
					firstWeapon ? "first" : "second", newParticle, particleClassName.c_str(), particleName.c_str());
			}
			dumpChildren("weaponMesh", weaponMesh);
			dumpChildren("weaponContainer", firstWeapon ? firstWeaponContainer : secondWeaponContainer);
			shotHierarchyProbeLogged = true;
		}
		if (!secondWeaponMesh)
		{
			firstWeaponIsShooting = true;
			firstWeaponLastParticleShot = newParticle;
			return;
		}

		//dual wield
		if (firstWeapon)
		{
			firstWeaponIsShooting = true;
			firstWeaponLastParticleShot = newParticle;
			firstWeaponShotDone = true;
		}
		else
		{
			secondWeaponIsShooting = true;
			secondWeaponLastParticleShot = newParticle;
			firstWeaponShotDone = false;
		}
	}
	else if (customAkimboActive && (customAkimboPendingEffectHandMask & 0x01U) != 0
		&& customAkimboPendingEffectSince != 0
		&& effectNow - customAkimboPendingEffectSince >= 25)
	{
		// The presentation-only left lane has no native particle child. Wait one
		// short engine interval so a late native particle can still win, then emit
		// only the missing visual effect at the accepted left muzzle.
		const bool spawned = spawnCustomAkimboEffect(0);
		// Native muzzle smoke/effect siblings can exist even when GTA omits the
		// mesh-child flash for the left presentation lane. Keep the ordinary
		// one-shot redirect gate open for this accepted shot so those components
		// are discovered and moved to the left muzzle later in the same tick.
		firstWeaponIsShooting = true;
		customAkimboLastEffectMuzzleWorld = customAkimboMuzzleWorld[0];
		customAkimboLastEffectMuzzleValid = true;
		customAkimboPendingEffectHandMask &= ~0x01U;
		if (customAkimboPendingEffectHandMask == 0)
			customAkimboPendingEffectSince = 0;
		if (!spawned && !customAkimboMissingEffectTemplateLogged)
		{
			customAkimboMissingEffectTemplateLogged = true;
			uevr::API::get()->log_warn(
				"[CustomAkimboFlash] left visual deferred: native effect template unavailable weapon=%d",
				static_cast<int>(currentWeaponEquipped));
		}
		else if (spawned && settingsManager->debugInputLayerProbe)
		{
			uevr::API::get()->log_info(
				"[CustomAkimboFlash] particles=0 acceptedMask=1 movedMask=0 spawnedMask=1 muzzleL=(%.1f %.1f %.1f)",
				customAkimboMuzzleWorld[0].x, customAkimboMuzzleWorld[0].y,
				customAkimboMuzzleWorld[0].z);
		}
	}
}

void WeaponManager::RedirectWorldShotEffects(bool firstWeapon)
{
	// This is deliberately a one-shot test.  The old redirect was effectively
	// active for the whole aim state, which let unrelated traces/effects reuse a
	// stale mock muzzle.  Only the particle pulse detected for this frame is
	// allowed to move anything here.
	if (!firstWeapon || !firstWeaponIsShooting
		|| !settingsManager->enableNativeShotOriginRedirects
		|| !playerManager->isInControl || playerManager->isInVehicle
		|| playerManager->weaponWheelEnabled
		|| firstWeaponMesh == nullptr || firstWeaponContainer == nullptr
		|| !uevr::API::UObjectHook::exists(firstWeaponMesh)
		|| !uevr::API::UObjectHook::exists(firstWeaponContainer)
		|| (!IsFiniteVector(lastAimMuzzleWorldPosition)
			&& !(customAkimboActive && customAkimboLastEffectMuzzleValid
				&& IsFiniteVector(customAkimboLastEffectMuzzleWorld)))
		|| currentWeaponEquipped < Pistol || currentWeaponEquipped > Minigun)
		return;

	auto containerClass = firstWeaponContainer->get_class();
	if (containerClass == nullptr || containerClass->find_function(L"K2_GetComponentLocation") == nullptr)
		return;

	Utilities::ParameterSingleVector3 containerLocationParams{};
	firstWeaponContainer->call_function(L"K2_GetComponentLocation", &containerLocationParams);
	const glm::fvec3 nativeEffectOrigin = containerLocationParams.vec3Value;
	if (!IsFiniteVector(nativeEffectOrigin))
		return;
	const glm::fvec3 effectDestination = customAkimboActive
		&& customAkimboLastEffectMuzzleValid
		&& IsFiniteVector(customAkimboLastEffectMuzzleWorld)
		? customAkimboLastEffectMuzzleWorld
		: lastAimMuzzleWorldPosition;

	const auto getAttachParent = [](uevr::API::UObject* object) -> uevr::API::UObject*
	{
		if (object == nullptr || !uevr::API::UObjectHook::exists(object))
			return nullptr;
		auto objectClass = object->get_class();
		if (objectClass == nullptr || objectClass->find_property(L"AttachParent") == nullptr)
			return nullptr;
		return object->get_property<uevr::API::UObject*>(L"AttachParent");
	};

	const auto isAttachedToMockMesh = [&](uevr::API::UObject* object) -> bool
	{
		if (object == firstWeaponMesh)
			return true;

		auto parent = getAttachParent(object);
		for (int depth = 0; parent != nullptr && depth < 12; ++depth)
		{
			if (parent == firstWeaponMesh)
				return true;
			auto nextParent = getAttachParent(parent);
			if (nextParent == parent)
				break;
			parent = nextParent;
		}
		return false;
	};

	const auto redirectClassObjects = [&](uevr::API::UClass* effectClass, uint32_t& candidateCount,
		uint32_t& movedCount)
	{
		if (effectClass == nullptr)
			return;

		for (auto effect : effectClass->get_objects_matching(false))
		{
			if (effect == nullptr || !uevr::API::UObjectHook::exists(effect)
				|| effect == firstWeaponLastParticleShot || isAttachedToMockMesh(effect))
				continue;

			auto effectClassObject = effect->get_class();
			if (effectClassObject == nullptr
				|| effectClassObject->find_function(L"K2_GetComponentLocation") == nullptr
				|| effectClassObject->find_function(L"K2_SetWorldLocation") == nullptr)
				continue;
			if (effectClassObject->find_property(L"bIsActive") != nullptr
				&& !effect->get_bool_property(L"bIsActive"))
				continue;

			Utilities::ParameterSingleVector3 effectLocationParams{};
			effect->call_function(L"K2_GetComponentLocation", &effectLocationParams);
			const glm::fvec3 effectLocation = effectLocationParams.vec3Value;
			if (!IsFiniteVector(effectLocation))
				continue;

			// The native effect is expected to be near the unmoved game weapon
			// root.  This spatial guard keeps the test from touching unrelated
			// particles elsewhere in the world.
			const float rootDistance = glm::length(effectLocation - nativeEffectOrigin);
			if (!std::isfinite(rootDistance) || rootDistance > 250.0f)
				continue;

			// Cache every actual native particle-system template involved in the
			// shot, not merely the first newly attached child (which can be a shell
			// or another non-visible helper). These become the left lane's visual-only
			// flash/smoke set on subsequent accepted shots.
			if (customAkimboActive)
				CacheCustomAkimboEffectTemplate(effect);

			++candidateCount;
			Utilities::Parameter_K2_SetWorldOrRelativeLocation setWorldLocationParams{};
			setWorldLocationParams.newLocation = effectDestination;
			setWorldLocationParams.bSweep = false;
			setWorldLocationParams.bTeleport = true;
			effect->call_function(L"K2_SetWorldLocation", &setWorldLocationParams);
			++movedCount;

			if (settingsManager->debugSpreadProbe)
			{
				const auto effectName = effect->get_full_name();
				const auto effectClassName = effectClassObject->get_full_name();
				const auto parent = getAttachParent(effect);
				const auto parentName = parent != nullptr ? parent->get_full_name() : L"<world>";
				uevr::API::get()->log_info(
					"[WorldShotEffectRedirect] moved effect=%p class=%ls name=%ls parent=%ls from=(%.3f %.3f %.3f) to=(%.3f %.3f %.3f) rootDistance=%.3f",
					effect, effectClassName.c_str(), effectName.c_str(), parentName.c_str(),
					effectLocation.x, effectLocation.y, effectLocation.z,
					effectDestination.x, effectDestination.y, effectDestination.z,
					rootDistance);
			}
		}
	};

	uint32_t candidateCount = 0;
	uint32_t movedCount = 0;
	redirectClassObjects(uevr::API::get()->find_uobject<uevr::API::UClass>(L"Class /Script/Engine.ParticleSystemComponent"),
		candidateCount, movedCount);
	redirectClassObjects(uevr::API::get()->find_uobject<uevr::API::UClass>(L"Class /Script/Niagara.NiagaraComponent"),
		candidateCount, movedCount);

	if (settingsManager->debugSpreadProbe)
	{
		static uint32_t shotSequence = 0;
		uevr::API::get()->log_info(
			"[WorldShotEffectRedirect] shot=%u scanned candidateCount=%u movedCount=%u nativeRoot=(%.3f %.3f %.3f) mockMuzzle=(%.3f %.3f %.3f)",
			++shotSequence, candidateCount, movedCount,
			nativeEffectOrigin.x, nativeEffectOrigin.y, nativeEffectOrigin.z,
			effectDestination.x, effectDestination.y, effectDestination.z);
	}
}

bool WeaponManager::HasUsableWeapon(bool firstWeapon) const
{
	auto weaponMesh = firstWeapon ? firstWeaponMesh : secondWeaponMesh;
	return weaponMesh != nullptr && uevr::API::UObjectHook::exists(weaponMesh);
}

bool WeaponManager::IsVehicleFreeAimSupportedWeapon() const
{
	// The vehicle proof uses the existing bullet-weapon muzzle/trace boundary.
	// It intentionally stops at the sniper rifle; explosives, sprays, flame and
	// minigun particles do not share that validated native trace path yet.
	return currentWeaponEquipped >= Pistol && currentWeaponEquipped <= Sniper;
}

bool WeaponManager::IsVehicleFreeAimActive() const
{
	if (!kVehicleFreeAimExperimentalEnabled)
		return false;
	return settingsManager != nullptr
		&& settingsManager->enableVehicleFaceButtonFire
		&& playerManager != nullptr
		&& playerManager->isInControl
		&& playerManager->isInVehicle
		&& (playerManager->vehicleType == PlayerManager::CarOrBoat
			|| playerManager->vehicleType == PlayerManager::Bike)
		&& cameraController != nullptr
		&& cameraController->currentCameraMode != CameraController::Camera
		&& cameraController->currentCameraMode != CameraController::AimWeaponFromCar
		&& firstWeaponMesh != nullptr
		&& uevr::API::UObjectHook::exists(firstWeaponMesh)
		&& firstWeaponContainer != nullptr
		&& uevr::API::UObjectHook::exists(firstWeaponContainer)
		&& IsVehicleFreeAimSupportedWeapon()
		&& secondWeaponMesh == nullptr;
}

void WeaponManager::SetVehicleFaceButtonState(bool held)
{
	if (!IsVehicleFreeAimActive())
	{
		vehicleFaceButtonHeld.store(false, std::memory_order_release);
		vehicleFaceButtonFirePending.store(false, std::memory_order_release);
		vehicleShotTraceArmPending.store(false, std::memory_order_release);
		vehicleFaceButtonNoShotCheckPending = false;
		if (memoryManager != nullptr)
			memoryManager->SetVehicleShotTraceOverrideArmed(false);
		return;
	}

	const bool previous = vehicleFaceButtonHeld.exchange(held, std::memory_order_acq_rel);
	if (held && !previous)
	{
		++vehicleFaceButtonSequence;
		vehicleFaceButtonTraceSequenceAtPress = memoryManager != nullptr
			? memoryManager->GetNativeShotTraceSequenceSnapshot() : 0;
		vehicleFaceButtonNoShotCheckPending = false;
		vehicleFaceButtonFirePending.store(true, std::memory_order_release);
		vehicleShotTraceArmPending.store(true, std::memory_order_release);
		uevr::API::get()->log_info("[VehicleFreeAim] face-fire request pressed seq=%u weapon=%d",
			vehicleFaceButtonSequence, static_cast<int>(currentWeaponEquipped));
	}
	else if (!held && previous)
	{
		vehicleShotTraceArmPending.store(false, std::memory_order_release);
		if (memoryManager != nullptr)
			memoryManager->SetVehicleShotTraceOverrideArmed(false);
		vehicleFaceButtonNoShotCheckTime = GetTickCount64() + 150;
		vehicleFaceButtonNoShotCheckPending = true;
		uevr::API::get()->log_info("[VehicleFreeAim] face-fire request released seq=%u",
			vehicleFaceButtonSequence);
	}
}

bool WeaponManager::ApplyVehicleNativeRightArmPresentation(bool hidden)
{
	uevr::API::UObject* nativeHands = playerManager != nullptr ? playerManager->handScaleHands : nullptr;
	if (!hidden)
	{
		if (vehicleNativeRightArmHidden && vehicleNativeRightArmComponent != nullptr
			&& uevr::API::UObjectHook::exists(vehicleNativeRightArmComponent)
			&& vehicleNativeRightArmBoneName.comparison_index != 0)
		{
			if (!UnhideBone(vehicleNativeRightArmComponent, vehicleNativeRightArmBoneName))
			{
				uevr::API::get()->log_warn("[VehicleFreeAim] native right branch restore unavailable; UnHideBoneByName missing");
				return false;
			}
			bool stillHidden = true;
			if (!TryReadBoneHiddenByName(
				vehicleNativeRightArmComponent, vehicleNativeRightArmBoneName, stillHidden)
				|| stillHidden)
			{
				uevr::API::get()->log_warn("[VehicleFreeAim] native right branch restore unverifiable; keeping cleanup state for retry");
				return false;
			}
			uevr::API::get()->log_info("[VehicleFreeAim] native right hand branch restored");
		}
		vehicleNativeRightArmHidden = false;
		vehicleNativeRightArmComponent = nullptr;
		vehicleNativeRightArmBoneName = uevr::API::FName{};
		return true;
	}

	if (nativeHands == nullptr || !uevr::API::UObjectHook::exists(nativeHands))
		return false;
	if (vehicleNativeRightArmHidden && vehicleNativeRightArmComponent == nativeHands)
		return true;
	// Do not hide the native branch unless the same runtime class exposes the
	// inverse operation.  Without this guard a failed cleanup could leave CJ's
	// right hand hidden for the rest of the session.
	if (nativeHands->get_class() == nullptr
		|| nativeHands->get_class()->find_function(L"UnHideBoneByName") == nullptr)
	{
		uevr::API::get()->log_warn("[VehicleFreeAim] native right branch hide unavailable; UnHideBoneByName missing");
		return false;
	}

	if (vehicleNativeRightArmHidden
		&& !ApplyVehicleNativeRightArmPresentation(false))
	{
		uevr::API::get()->log_warn("[VehicleFreeAim] native right branch replacement deferred; previous branch not restored");
		return false;
	}

	ResolvedBone rightArm;
	if (!ResolveBone(nativeHands, L"R_UpperArm", rightArm) || rightArm.index < 0
		|| !HideBone(nativeHands, rightArm.name))
	{
		uevr::API::get()->log_warn("[VehicleFreeAim] native right branch hide failed");
		return false;
	}
	bool hiddenByName = false;
	if (!TryReadBoneHiddenByName(nativeHands, rightArm.name, hiddenByName) || !hiddenByName)
	{
		// Do not leave a native branch hidden unless the already-proven reflected
		// verification path confirms it; the steering hand is the safe fallback.
		UnhideBone(nativeHands, rightArm.name);
		uevr::API::get()->log_warn("[VehicleFreeAim] native right branch hide unverifiable; left native hands retained");
		return false;
	}

	vehicleNativeRightArmComponent = nativeHands;
	vehicleNativeRightArmBoneName = rightArm.name;
	vehicleNativeRightArmHidden = true;
	uevr::API::get()->log_info("[VehicleFreeAim] native right hand branch hidden bone=%ls index=%d; native left hand retained",
		rightArm.text.c_str(), rightArm.index);
	return true;
}

void WeaponManager::ProcessAiming(bool firstWeapon, bool applyGameAim)
{
	if (settingsManager->debugMod) uevr::API::get()->log_info("UpdateAimingVectors()");
	bool nativeShotPosePublishedThisUpdate = false;
	const bool vehicleFreeAim = IsVehicleFreeAimActive();
	// Disarm the vehicle return-site gate while rebuilding this frame's coherent
	// snapshot. A vehicle snapshot is armed again only after publication below.
	// On vehicle exit, clear the old vehicle pair before the on-foot path can
	// publish its replacement; this prevents a stale car shot from leaking out.
	if (applyGameAim)
	{
		memoryManager->SetVehicleShotTraceModeActive(vehicleFreeAim);
		if (!vehicleFreeAim && playerManager->wasInVehicle)
			memoryManager->ClearNativeShotTraceOverride();
		else
			memoryManager->SetVehicleShotTraceOverrideArmed(false);
	}
	const auto clearNativeShotSnapshot = [&]()
	{
		if (!applyGameAim)
			return;
		memoryManager->ClearNativeShotTraceOverride();
	};

	const bool shotActiveForAimLatch = IsWeaponShooting(firstWeapon);
	const bool silencedPistolAimStability = settingsManager->enableSilencedPistolAimStability && currentWeaponEquipped == PistolSilenced;
	if (applyGameAim)
	{
		if (stableGameAimLatchFrames > 0)
			--stableGameAimLatchFrames;
		if (!shotActiveForAimLatch)
		{
			aimLatchFallbackLoggedThisShot = false;
			aimLatchJumpLoggedThisShot = false;
		}
	}

	if (!HasUsableWeapon(firstWeapon))
	{
		if (!applyGameAim || !firstWeapon)
		{
			clearNativeShotSnapshot();
			return;
		}
	}

	if (cameraController->currentCameraMode == CameraController::Camera)
	{
		if (applyGameAim)
		{
			cameraController->forwardVectorUE = glm::fvec3(
				*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[0])),
				-*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[1])),
				*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[2]))
			);
		}
		clearNativeShotSnapshot();
		return;
	}

	if (applyGameAim && IsUtilityAimBypassWeapon())
	{
		hasStableGameAim = false;
		stableGameAimLatchFrames = 0;
		aimLatchFallbackLoggedThisShot = false;
		aimLatchJumpLoggedThisShot = false;
		clearNativeShotSnapshot();
		return;
	}

	// If not aiming, synchronise the aiming vector with the camera matrix (prevents the radar from following the gun orientation)
	if (!playerManager->isInVehicle && camModsRequiringAimHandling.find((int)cameraController->currentCameraMode) == camModsRequiringAimHandling.end()) //check if the current camera mode is in the aiming cam, if not, return
	{
		glm::fvec3 cameraForwardUE = {
			cameraController->cameraMatrixValues[4],
			-cameraController->cameraMatrixValues[5],
			cameraController->cameraMatrixValues[6]
		};
		if (applyGameAim)
		{
			const bool keepStableAim = hasStableGameAim
				&& IsAimStabilizedWeapon()
				&& (stableGameAimLatchFrames > 0 || shotActiveForAimLatch || silencedPistolAimStability);

			if (keepStableAim)
			{
				*(reinterpret_cast<float*>(memoryManager->cameraPositionAddresses[0])) = lastStableGameAimPosition.x;
				*(reinterpret_cast<float*>(memoryManager->cameraPositionAddresses[1])) = lastStableGameAimPosition.y;
				*(reinterpret_cast<float*>(memoryManager->cameraPositionAddresses[2])) = lastStableGameAimPosition.z;

				*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[0])) = lastStableGameAimForward.x;
				*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[1])) = lastStableGameAimForward.y;
				*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[2])) = lastStableGameAimForward.z;

				if (settingsManager->debugMod && shotActiveForAimLatch && !aimLatchFallbackLoggedThisShot)
				{
					uevr::API::get()->log_info("[AimLatch] kept stable aim during non-aim fallback; mode=%i latchFrames=%i stable=(%.3f %.3f %.3f) camera=(%.3f %.3f %.3f)",
						static_cast<int>(cameraController->currentCameraMode),
						stableGameAimLatchFrames,
						lastStableGameAimForward.x,
						lastStableGameAimForward.y,
						lastStableGameAimForward.z,
						cameraController->cameraMatrixValues[4],
						cameraController->cameraMatrixValues[5],
						cameraController->cameraMatrixValues[6]);
					aimLatchFallbackLoggedThisShot = true;
				}
			}
			else
			{
				*(reinterpret_cast<float*>(memoryManager->cameraPositionAddresses[0])) = cameraController->cameraMatrixValues[12];
				*(reinterpret_cast<float*>(memoryManager->cameraPositionAddresses[1])) = cameraController->cameraMatrixValues[13];
				*(reinterpret_cast<float*>(memoryManager->cameraPositionAddresses[2])) = cameraController->cameraMatrixValues[14];

				*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[0])) = cameraController->cameraMatrixValues[4];
				*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[1])) = cameraController->cameraMatrixValues[5];
				*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[2])) = cameraController->cameraMatrixValues[6];
			}
			memoryManager->RecordTriggerTimingAimVectorProxy(
				static_cast<int>(currentWeaponEquipped),
				firstWeapon,
				playerManager->isInVehicle,
				static_cast<int>(cameraController->currentCameraMode));
		}
		clearNativeShotSnapshot();
		return;
	}

	uevr::API::UObject* weaponMesh = firstWeapon ? firstWeaponMesh : secondWeaponMesh;

	if (weaponMesh != nullptr) {
		Utilities::ParameterSingleVector3 forwardVector_params;
		Utilities::ParameterSingleVector3 upVector_params;
		Utilities::ParameterSingleVector3 rightVector_params;
		weaponMesh->call_function(L"GetForwardVector", &forwardVector_params);
		weaponMesh->call_function(L"GetUpVector", &upVector_params);
		weaponMesh->call_function(L"GetRightVector", &rightVector_params);

		glm::fvec3 point1Offsets = { 0.0f, 0.0f, 0.0f };
		glm::fvec3 point2Offsets = { 0.0f, 0.0f, 0.0f };
		bool socketAvailable = true;
		bool sprayWeapon = false;
		bool meleeWeapon = false;
		float offset = 0.0f;
		bool firstPersonView = cameraController->currentCameraMode == CameraController::HelicannonFirstPerson;

		//mesh alignement weapon offsets
		switch (currentWeaponEquipped)
		{
			// Offsets taken from the game's 3D models in Blender by taking 2 points aligned with the barrel. Units is centimeters.
			// Y axis is inversed in Blender.
			// We then add some slight offsets manually depending on the aiming tests done ingame.
		case Pistol :
			point1Offsets = { 2.82819, -2.52103, 9.92684 };
			point2Offsets = { 21.7272, -3.89487, 12.9088 + 0.2 };
			break;
		case PistolSilenced :
			point1Offsets = { 2.80735, -2.52308, 9.9193 };
			point2Offsets = { 17.3316, -3.5591 + 0.1, 12.2129 + 0.6 };
			break;
		case DesertEagle :
			point1Offsets = { 7.06492 , -2.25853 , 11.9386 + 0.5 };
			point2Offsets = { 33.5914, -1.46079 - 0.5, 11.9439 - 0.5 };
			break;
		case Shotgun :
			point1Offsets = { 31.3429, -0.670153, 15.2663 };
			point2Offsets = { 73.6795 , 4.2357 - 1 , 22.2237 - 2 };
			break;
		case Sawnoff :
			point1Offsets = { 21.2896, -2.13098 , 13.0224 };
			point2Offsets = { 55.8867 , -2.10406 - 1, 16.3934 - 2 };
			break;
		case Spas12 :
			point1Offsets = { 51.9659 , 1.30133, 19.5475 };
			point2Offsets = { 70.459 , 3.20646 , 22.5404 };
			break;
		case MicroUzi:
			point1Offsets = { -0.267532, -2.19868 , 10.2951 };
			point2Offsets = { 12.9468 , -0.996034 + 0.4, 11.293 + 0.9 };
			break;
		case Mp5:
			point1Offsets = { 6.8924, -1.74509 , 19.3761 };
			point2Offsets = { 21.3778 , 0.000536 + 0.2, 21.2535 + 1 };
			break;
		case Ak47:
			offset = firstPersonView ? 0.0f : -0.2f;
			point1Offsets = { 3.8416 , -2.83908, 14.3539 };
			point2Offsets = { 36.3719, 0.193737 + offset, 16.1544 + offset };
			break;
		case M4:
			point1Offsets = { 5.85945 , -1.78476 , 15.1271 };
			point2Offsets = { 60.0434  , 2.99539 - 1 , 16.4006 - 1.5 };
			break;
		case Tec9:
			point1Offsets = { 1.1631 , -3.60654, 11.7162 };
			point2Offsets = { 24.9241 , -3.60654, 13.9038 - 1 };
			break;
		case Rifle : //"cuntgun"
			point1Offsets = { 7.92837 , -3.48911 , 11.4936 };
			point2Offsets = { 71.2598, 4.09339 - 0.75, 20.9391 - 1.5 };
			break;
		case Sniper :
			point1Offsets = { 5.94806 , -2.75068, 13.2024 };
			point2Offsets = { 30.6871 , -0.22823 - 0.025, 15.6848 };
			socketAvailable = false;
			break;
		case RocketLauncher :
			//point1Offsets = { 2.41748 , -3.88386 , 14.4056 };
			//point2Offsets = { 29.0589, -3.88386, 14.4056 };
			point1Offsets = { 0.0f , 0.0f, 0.0f };
			point2Offsets = { 0.0f , 0.0f, 0.0f };
			socketAvailable = false;
			break;
		case RocketLauncherHs : // RocketLauncherHeatSeek
			//point1Offsets = { -57.665 , -3.74195 , 20.2618 };
			//point2Offsets = { 34.8035, -3.52085 , 20.1928 };
			point1Offsets = { 0.0f , 0.0f, 0.0f };
			point2Offsets = { 0.0f , 0.0f, 0.0f };
			socketAvailable = false;
			break;
		case Flamethrower :
			point1Offsets = { 48.0165 , -1.65182 , 16.1683 };
			point2Offsets = { 76.7885, 0.537026 , 31.6837 };
			sprayWeapon = true;
			break;
		case Minigun :
			point1Offsets = { 48.1025 , -2.9978 , 14.3878 };
			point2Offsets = { 86.6453 , 0.429413 /*- 0.5*/ , 35.9644 /*- 0.5 */};
			break;
		case Grenade:
		case Teargas:
		case Molotov:
			// Physical throwables use the tracked mesh/controller forward while GTA
			// retains native cook/release ownership and projectile timing.
			socketAvailable = false;
			break;
		case SprayCan:
			/*point1Offsets = { 2.82819, -2.52103, 9.92684 };
			point2Offsets = { 21.7272, -3.89487, 12.9088 };*/
			point1Offsets = { 0.0f , 0.0f, 0.0f };
			point2Offsets = { 0.0f , 0.0f, 0.0f };
			sprayWeapon = true;
			socketAvailable = false;
			break;
		case Extinguisher:
			/*point1Offsets = { 2.82819, -2.52103, 9.92684 };
			point2Offsets = { 21.7272, -3.89487, 12.9088 };*/
			point1Offsets = { 0.0f , 0.0f, 0.0f };
			point2Offsets = { 0.0f , 0.0f, 0.0f };
			sprayWeapon = true;
			socketAvailable = false;
			break;
		case Camera:
			point1Offsets = { 13.8476, -11.6162, 1.72577 };
			point2Offsets = { 27.6432, -11.6162, 2.84382 };
			socketAvailable = false;
			break;

		default:
			point1Offsets = { 0.0f , 0.0f, 0.0f };
			point2Offsets = { 0.0f , 0.0f, 0.0f };
			socketAvailable = false;
			meleeWeapon = true;
			break;
		}

		glm::fvec3 point1Position = { 0.0f , 0.0f, 0.0f };
		glm::fvec3 point2Position = { 0.0f , 0.0f, 0.0f };
		glm::fvec3 aimingDirection = { 0.0f , 0.0f, 0.0f };
		glm::fvec3 rawBarrelDirection = { 0.0f, 0.0f, 0.0f };
		glm::fvec3 muzzleWorldPosition = { 0.0f, 0.0f, 0.0f };
		bool legacyCrosshairCompensationApplied = false;

		if (socketAvailable && !meleeWeapon)
		{
			Utilities::ParameterGetSocketLocation socketLocation_params;
			socketLocation_params.inSocketName = uevr::API::FName(L"gunflash");
			weaponMesh->call_function(L"GetSocketLocation", &socketLocation_params);
			muzzleWorldPosition = socketLocation_params.outLocation;

			point1Position = Utilities::OffsetLocalPositionFromWorld(socketLocation_params.outLocation, forwardVector_params.vec3Value, upVector_params.vec3Value, rightVector_params.vec3Value, point1Offsets);
			point2Position = Utilities::OffsetLocalPositionFromWorld(socketLocation_params.outLocation, forwardVector_params.vec3Value, upVector_params.vec3Value, rightVector_params.vec3Value, point2Offsets);

			aimingDirection = glm::normalize(point2Position - point1Position);
			rawBarrelDirection = aimingDirection;

			//The code below compensate for the over the shoulder camera offset. When in 3rd person, this game's crosshair is not centered.
			//The game calculate it's aiming vector with the camera direction. We move and align this camera with the gun mesh
			//so the game can still use the camera values to calculate the shoot traces.
			//Hacky but functional, if someday this game gets proper reverse engineered we should be able to do better.
			if (settingsManager->enableLegacyCrosshairCompensation && !firstPersonView)
			{
				legacyCrosshairCompensationApplied = true;
				glm::fvec3 projectedToFloorVector = glm::fvec3(aimingDirection.x, aimingDirection.y, 0.0);

				// Safeguard: Normalize projectedToFloorVector only if valid
				if (glm::length(projectedToFloorVector) > 0.0f) {
					projectedToFloorVector = glm::normalize(projectedToFloorVector);
				}
				else {
					projectedToFloorVector = glm::fvec3(1.0f, 0.0f, 0.0f); // Fallback vector
				}

				glm::fvec3 yawRight = glm::cross(glm::fvec3(0.0f, 0.0f, 1.0f), projectedToFloorVector);
				if (glm::length(yawRight) > 0.0f) {
					yawRight = glm::normalize(yawRight);
				}
				else {
					yawRight = glm::fvec3(0.0f, -1.0f, 0.0f); // Fallback vector if collinear
				}


				glm::fvec3 yawUp = glm::cross(yawRight, projectedToFloorVector);
				if (glm::length(yawUp) > 0.0f) {
					yawUp = glm::normalize(yawUp);
				}
				else {
					yawUp = glm::fvec3(0.0f, 0.0f, 1.0f); // Fallback vector
				}

				point2Position = Utilities::OffsetLocalPositionFromWorld(point2Position, projectedToFloorVector, yawUp, yawRight, crosshairOffset);
			}

			// Safeguard: Recalculate aiming direction and normalize
			aimingDirection = point2Position - point1Position;
			if (glm::length(aimingDirection) > 0.0f) {
				aimingDirection = glm::normalize(aimingDirection);
			}
			else {
				aimingDirection = glm::fvec3(1.0f, 0.0f, 0.0f); // Fallback vector
			}
		}
		else
		{
			Utilities::ParameterSingleVector3 componentToWorld_params;
			weaponMesh->call_function(L"K2_GetComponentLocation", &componentToWorld_params);

			if (glm::length(point1Offsets) > 0.0f && glm::length(point2Offsets) > 0.0f)
			{
				point1Position = Utilities::OffsetLocalPositionFromWorld(componentToWorld_params.vec3Value, forwardVector_params.vec3Value, upVector_params.vec3Value, rightVector_params.vec3Value, point1Offsets);
				point2Position = Utilities::OffsetLocalPositionFromWorld(componentToWorld_params.vec3Value, forwardVector_params.vec3Value, upVector_params.vec3Value, rightVector_params.vec3Value, point2Offsets);
				aimingDirection = glm::normalize(point2Position - point1Position);
			}
			else
			{
				point1Position = componentToWorld_params.vec3Value;
				aimingDirection = forwardVector_params.vec3Value;
			}
			muzzleWorldPosition = point1Position;
			rawBarrelDirection = aimingDirection;
		}

		// The spawned gunflash component follows the visual flash at the actual
		// barrel tip. Prefer it once available; before the first shot, the gunflash
		// socket itself is the closest stable pre-fire transform.
		uevr::API::UObject* gunflashComponent = firstWeapon
			? firstWeaponLastParticleShot : secondWeaponLastParticleShot;
		// A relocated akimbo particle must never become the next frame's muzzle
		// origin. Each hand's gunflash socket is the authoritative barrel tip.
		if (!customAkimboActive && gunflashComponent != nullptr
			&& uevr::API::UObjectHook::exists(gunflashComponent))
		{
			auto gunflashClass = gunflashComponent->get_class();
			if (gunflashClass != nullptr
				&& gunflashClass->find_function(L"K2_GetComponentLocation") != nullptr)
			{
				Utilities::ParameterSingleVector3 gunflashLocationParams{};
				gunflashComponent->call_function(L"K2_GetComponentLocation", &gunflashLocationParams);
				if (IsFiniteVector(gunflashLocationParams.vec3Value))
					muzzleWorldPosition = gunflashLocationParams.vec3Value;
			}
		}
		const bool gunflashNearController = IsFiniteVector(muzzleWorldPosition)
			&& IsFiniteVector(point1Position)
			&& std::isfinite(glm::length(muzzleWorldPosition - point1Position))
			&& glm::length(muzzleWorldPosition - point1Position) <= 500.0f;
		if (!gunflashNearController)
			muzzleWorldPosition = point1Position;

		calculatedAimForward = {aimingDirection.x, -aimingDirection.y, aimingDirection.z};
		calculatedAimPosition = { point1Position.x * 0.01f, -point1Position.y * 0.01f, point1Position.z * 0.01f};
		glm::fvec3 currentBarrelDirection = rawBarrelDirection;
		if (glm::length(currentBarrelDirection) <= 0.0001f)
			currentBarrelDirection = aimingDirection;
		const glm::fvec3 currentTraceForward = NormalizeOrZero({
			currentBarrelDirection.x, -currentBarrelDirection.y, currentBarrelDirection.z
		});
		if (customAkimboActive && currentWeaponEquipped >= Pistol
			&& currentWeaponEquipped <= Sniper && IsFiniteVector(calculatedAimPosition)
			&& IsFiniteVector(currentTraceForward)
			&& glm::length(currentTraceForward) > 0.0001f)
		{
			int hand = firstWeapon ? motionConfiguredFirstHand : motionConfiguredSecondHand;
			if (hand < 0 || hand > 1)
			{
				const int firstHand = settingsManager->leftHandedMode
					!= SettingsManager::Disabled ? 0 : 1;
				hand = firstWeapon ? firstHand : 1 - firstHand;
			}
			constexpr float customAkimboTraceRange = 15000.0f;
			const glm::fvec3 customTarget = calculatedAimPosition
				+ currentTraceForward * customAkimboTraceRange;
			if (IsFiniteVector(customTarget)
				&& memoryManager->SetCustomAkimboHandTrace(hand,
					{ calculatedAimPosition.x, calculatedAimPosition.y, calculatedAimPosition.z },
					{ customTarget.x, customTarget.y, customTarget.z }))
			{
				// The gunflash socket is at the weapon pivot on these GTA meshes.
				// The per-model calibrated second barrel point is the visible muzzle tip.
				glm::fvec3 customMuzzle = IsFiniteVector(point2Position)
					? point2Position : muzzleWorldPosition;
				// The original SMG endpoint tables stop near the rear sight on the
				// duplicated controller meshes. Correct only akimbo presentation;
				// the established one-handed aim and effect offsets remain unchanged.
				if (currentWeaponEquipped == MicroUzi)
					customMuzzle += forwardVector_params.vec3Value * 8.0f
						- upVector_params.vec3Value * 2.0f;
				else if (currentWeaponEquipped == Tec9)
					customMuzzle += forwardVector_params.vec3Value * 5.0f
						- upVector_params.vec3Value * 2.0f;
				customAkimboMuzzleWorld[hand] = customMuzzle;
				customAkimboMuzzleValidMask |= static_cast<uint8_t>(1U << hand);
			}
		}
		if (applyGameAim)
		{
			if (glm::length(rawBarrelDirection) <= 0.0001f)
				rawBarrelDirection = aimingDirection;
			lastRawBarrelAimForward = {
				rawBarrelDirection.x, -rawBarrelDirection.y, rawBarrelDirection.z
			};
			lastPreLatchAimForward = calculatedAimForward;
			lastAimMuzzlePosition = {
				muzzleWorldPosition.x * 0.01f,
				-muzzleWorldPosition.y * 0.01f,
				muzzleWorldPosition.z * 0.01f
			};
			lastAimMuzzleWorldPosition = muzzleWorldPosition;
			lastLegacyCrosshairCompensationApplied = legacyCrosshairCompensationApplied;

			// Keep one validated mock-muzzle pair current while the weapon pose is
			// valid. The verified 0x13F89A0 player-fire boundary is the shot event and
			// consumes the latest stable pair; aim/shoot latches do not arm the hook.
			const bool nativeTraceWeapon = currentWeaponEquipped >= Pistol
				&& currentWeaponEquipped <= Sniper;
			const bool singlePrimaryWeapon = firstWeapon && !HasUsableWeapon(false);
			const glm::fvec3 nativeTraceOrigin = lastAimMuzzlePosition;
			const glm::fvec3 nativeTraceForward = NormalizeOrZero(lastRawBarrelAimForward);
			if (settingsManager->enableNativeShotOriginRedirects
				&& nativeTraceWeapon && singlePrimaryWeapon
				&& (!playerManager->isInVehicle || vehicleFreeAim)
				&& IsFiniteVector(nativeTraceOrigin)
				&& IsFiniteVector(nativeTraceForward)
				&& glm::length(nativeTraceForward) > 0.0001f)
			{
				constexpr float nativeTraceRange = 15000.0f;
				const glm::fvec3 nativeTraceTarget = nativeTraceOrigin
					+ nativeTraceForward * nativeTraceRange;
				if (IsFiniteVector(nativeTraceTarget))
				{
					const bool snapshotPublished = memoryManager->SetNativeShotTraceOverride(
						{ nativeTraceOrigin.x, nativeTraceOrigin.y, nativeTraceOrigin.z },
						{ nativeTraceTarget.x, nativeTraceTarget.y, nativeTraceTarget.z });
					if (snapshotPublished)
					{
						nativeShotPosePublishedThisUpdate = true;
						// Every native shot requested by held A may consume this frame's
						// coherent muzzle pair. Accelerator-only driving remains disarmed.
						if (vehicleFreeAim
							&& vehicleFaceButtonHeld.load(std::memory_order_acquire))
							memoryManager->SetVehicleShotTraceOverrideArmed(true);
					}
				}
			}
		}

		if (applyGameAim && !playerManager->isInVehicle && !meleeWeapon && IsAimStabilizedWeapon())
		{
			const glm::fvec3 currentAimNormal = NormalizeOrZero(calculatedAimForward);
			const glm::fvec3 stableAimNormal = NormalizeOrZero(lastStableGameAimForward);
			const bool shouldGuardAimJump = shotActiveForAimLatch || (silencedPistolAimStability && stableGameAimLatchFrames > 0);
			const float aimJumpDotThreshold = silencedPistolAimStability ? 0.75f : 0.35f;
			const bool suddenAimJump = hasStableGameAim
				&& shouldGuardAimJump
				&& glm::length(currentAimNormal) > 0.0f
				&& glm::length(stableAimNormal) > 0.0f
				&& glm::dot(currentAimNormal, stableAimNormal) < aimJumpDotThreshold;

			if (suddenAimJump)
			{
				if (settingsManager->debugMod && !aimLatchJumpLoggedThisShot)
				{
					uevr::API::get()->log_info("[AimLatch] blocked sudden aim-vector jump; mode=%i weapon=%i dotThreshold=%.2f current=(%.3f %.3f %.3f) stable=(%.3f %.3f %.3f)",
						static_cast<int>(cameraController->currentCameraMode),
						static_cast<int>(currentWeaponEquipped),
						aimJumpDotThreshold,
						calculatedAimForward.x,
						calculatedAimForward.y,
						calculatedAimForward.z,
						lastStableGameAimForward.x,
						lastStableGameAimForward.y,
						lastStableGameAimForward.z);
					aimLatchJumpLoggedThisShot = true;
				}

				calculatedAimForward = lastStableGameAimForward;
				calculatedAimPosition = lastStableGameAimPosition;
			}
			else
			{
				lastStableGameAimForward = calculatedAimForward;
				lastStableGameAimPosition = calculatedAimPosition;
				hasStableGameAim = true;
				stableGameAimLatchFrames = silencedPistolAimStability ? 90 : stableGameAimLatchFrameCount;
			}
		}

		// Ordinary VR firearms publish their own aim vector/ray. Rotating the
		// player actor here makes CJ enter the visible native shooting stance even
		// without LT. Keep actor alignment only for native special-weapon handling.
		const bool nativeSpecialWeapon = currentWeaponEquipped >= RocketLauncher;
		if (!playerManager->isInVehicle && !meleeWeapon && nativeSpecialWeapon)
		{
			if (applyGameAim)
				playerManager->AlignControllerToAimDirection(aimingDirection);
		}

		if (applyGameAim && !meleeWeapon)
		{
			memoryManager->SetAimCalibrationReference(
				calculatedAimForward.x,
				calculatedAimForward.y,
				calculatedAimForward.z,
				static_cast<int>(currentWeaponEquipped));
		}

		if (applyGameAim)
		{
			// If in vehicle, just use the forwardVector of the camera matrix
			if ((playerManager->isInVehicle
				&& cameraController->currentCameraMode != CameraController::AimWeaponFromCar
				&& !vehicleFreeAim) || meleeWeapon)
			{
				//uevr::API::get()->log_info("UpdateAimingVectors isInVehicle");
				//forward vector
				*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[0])) = cameraController->cameraMatrixValues[4];
				*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[1])) = cameraController->cameraMatrixValues[5];
				*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[2])) = cameraController->cameraMatrixValues[6];
			}
			else
			{
				//Apply new values to memory
				*(reinterpret_cast<float*>(memoryManager->cameraPositionAddresses[0])) = calculatedAimPosition.x;
				*(reinterpret_cast<float*>(memoryManager->cameraPositionAddresses[1])) = calculatedAimPosition.y;
				*(reinterpret_cast<float*>(memoryManager->cameraPositionAddresses[2])) = calculatedAimPosition.z;

				*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[0])) = calculatedAimForward.x;
				*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[1])) = calculatedAimForward.y;
				*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[2])) = calculatedAimForward.z;
			}
			memoryManager->RecordTriggerTimingAimVectorProxy(
				static_cast<int>(currentWeaponEquipped),
				firstWeapon,
				playerManager->isInVehicle,
				static_cast<int>(cameraController->currentCameraMode));
		}
		//Fix the up/down aiming for spray weapons
		if (applyGameAim && sprayWeapon)
		{
			*(reinterpret_cast<float*>(memoryManager->xAxisSpraysAimAddress)) = aimingDirection.z;
		}
	}
	else //if player unarmed
	{
		if (applyGameAim)
		{
			*(reinterpret_cast<float*>(memoryManager->cameraPositionAddresses[0])) = playerManager->actualPlayerPositionUE.x * 0.01f;
			*(reinterpret_cast<float*>(memoryManager->cameraPositionAddresses[1])) = -playerManager->actualPlayerPositionUE.y * 0.01f;
			*(reinterpret_cast<float*>(memoryManager->cameraPositionAddresses[2])) = playerManager->actualPlayerPositionUE.z * 0.01f;

			*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[0])) = cameraController->cameraMatrixValues[4];
			*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[1])) = cameraController->cameraMatrixValues[5];
			*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[2])) = cameraController->cameraMatrixValues[6];
		}
	}

	if (applyGameAim)
	{
		cameraController->forwardVectorUE = glm::fvec3(
			*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[0])),
			-*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[1])),
			*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[2]))
		);
		if (!nativeShotPosePublishedThisUpdate)
			clearNativeShotSnapshot();
	}
}

bool WeaponManager::IsWeaponShooting(bool firstWeapon) const
{
	if (firstWeapon)
		return firstWeaponIsShooting || memoryManager->FirstWeaponIsShooting;

	return secondWeaponIsShooting;
}

bool WeaponManager::IsAimStabilizedWeapon() const
{
	if (IsUtilityAimBypassWeapon())
		return false;

	switch (currentWeaponEquipped)
	{
	case Pistol:
	case PistolSilenced:
	case DesertEagle:
	case Shotgun:
	case Sawnoff:
	case Spas12:
	case MicroUzi:
	case Mp5:
	case Ak47:
	case M4:
	case Tec9:
	case Rifle:
	case Sniper:
	case RocketLauncher:
	case RocketLauncherHs:
	case Flamethrower:
	case Minigun:
	case SprayCan:
	case Extinguisher:
		return true;
	default:
		return false;
	}
}

bool WeaponManager::IsUtilityAimBypassWeapon() const
{
	if (!settingsManager->enableUtilityWeaponAimBypass)
		return false;

	return currentWeaponEquipped == SprayCan || currentWeaponEquipped == Extinguisher;
}

int WeaponManager::CurrentWeaponModelIdForStats() const
{
	switch (currentWeaponEquipped)
	{
	case Pistol: return 346;
	case PistolSilenced: return 347;
	case DesertEagle: return 348;
	case Shotgun: return 349;
	case Sawnoff: return 350;
	case Spas12: return 351;
	case MicroUzi: return 352;
	case Mp5: return 353;
	case Ak47: return 355;
	case M4: return 356;
	case Tec9: return 372;
	case Rifle: return 357;
	case Sniper: return 358;
	case RocketLauncher: return 359;
	case RocketLauncherHs: return 360;
	case Flamethrower: return 361;
	case Minigun: return 362;
	case SprayCan: return 365;
	case Extinguisher: return 366;
	case Camera: return 367;
	default: return -1;
	}
}

void WeaponManager::LogSpreadProbeIfShot(bool firstWeapon, bool isShooting)
{
	bool& latch = firstWeapon ? firstSpreadProbeShotActive : secondSpreadProbeShotActive;
	const bool vehicleTrace = IsVehicleFreeAimActive();

	MemoryManager::NativeShotTraceProbe nativeTraceProbe{};
	if (memoryManager->ReadLatestNativeShotTraceProbe(nativeTraceProbe))
	{
		const glm::fvec3 rawStart = {
			nativeTraceProbe.rawStart[0], nativeTraceProbe.rawStart[1], nativeTraceProbe.rawStart[2]
		};
		const glm::fvec3 rawTarget = {
			nativeTraceProbe.rawTarget[0], nativeTraceProbe.rawTarget[1], nativeTraceProbe.rawTarget[2]
		};
		const glm::fvec3 appliedStart = {
			nativeTraceProbe.appliedStart[0], nativeTraceProbe.appliedStart[1], nativeTraceProbe.appliedStart[2]
		};
		const glm::fvec3 appliedTarget = {
			nativeTraceProbe.appliedTarget[0], nativeTraceProbe.appliedTarget[1], nativeTraceProbe.appliedTarget[2]
		};
		if (vehicleTrace)
		{
			uevr::API::get()->log_info(
				"[VehicleFreeAim] damage trace seq=%u callerRva=0x%08X overridden=%s rawStart=(%.3f %.3f %.3f) rawTarget=(%.3f %.3f %.3f) appliedStart=(%.3f %.3f %.3f) appliedTarget=(%.3f %.3f %.3f)",
				nativeTraceProbe.sequence,
				nativeTraceProbe.callSiteRva,
				nativeTraceProbe.overridden ? "true" : "false",
				rawStart.x, rawStart.y, rawStart.z,
				rawTarget.x, rawTarget.y, rawTarget.z,
				appliedStart.x, appliedStart.y, appliedStart.z,
				appliedTarget.x, appliedTarget.y, appliedTarget.z);
		}
		if (settingsManager->debugSpreadProbe)
		{
			const glm::fvec3 rawDirection = NormalizeOrZero(rawTarget - rawStart);
			const glm::fvec3 appliedDirection = NormalizeOrZero(appliedTarget - appliedStart);
			uevr::API::get()->log_info(
				"[NativeTraceProbe] seq=%u overridden=%s rawStart=(%.3f %.3f %.3f) rawTarget=(%.3f %.3f %.3f) appliedStart=(%.3f %.3f %.3f) appliedTarget=(%.3f %.3f %.3f) rawOriginVsMock=%.3f appliedOriginVsMock=%.3f rawDirVsMock=%.3fdeg appliedDirVsMock=%.3fdeg",
				nativeTraceProbe.sequence,
				nativeTraceProbe.overridden ? "true" : "false",
				rawStart.x, rawStart.y, rawStart.z,
				rawTarget.x, rawTarget.y, rawTarget.z,
				appliedStart.x, appliedStart.y, appliedStart.z,
				appliedTarget.x, appliedTarget.y, appliedTarget.z,
				glm::length(rawStart - lastAimMuzzlePosition),
				glm::length(appliedStart - lastAimMuzzlePosition),
				AngleDegreesOrZero(rawDirection, lastRawBarrelAimForward),
				AngleDegreesOrZero(appliedDirection, lastRawBarrelAimForward));
		}
	}
	if (!settingsManager->debugSpreadProbe && !vehicleTrace)
	{
		latch = false;
		return;
	}

	MemoryManager::NativeShotEffectProbe nativeEffectProbe{};
	if (memoryManager->ReadLatestNativeShotEffectProbe(nativeEffectProbe))
	{
		const glm::fvec3 rawStart = {
			nativeEffectProbe.rawStart[0], nativeEffectProbe.rawStart[1], nativeEffectProbe.rawStart[2]
		};
		const glm::fvec3 rawTarget = {
			nativeEffectProbe.rawTarget[0], nativeEffectProbe.rawTarget[1], nativeEffectProbe.rawTarget[2]
		};
		const glm::fvec3 appliedStart = {
			nativeEffectProbe.appliedStart[0], nativeEffectProbe.appliedStart[1], nativeEffectProbe.appliedStart[2]
		};
		const glm::fvec3 appliedTarget = {
			nativeEffectProbe.appliedTarget[0], nativeEffectProbe.appliedTarget[1], nativeEffectProbe.appliedTarget[2]
		};
		if (vehicleTrace)
		{
			uevr::API::get()->log_info(
				"[VehicleFreeAim] tracer consume seq=%u callSite=0x%08X ownerLocal=%s overridden=%s rawStart=(%.3f %.3f %.3f) rawTarget=(%.3f %.3f %.3f) appliedStart=(%.3f %.3f %.3f) appliedTarget=(%.3f %.3f %.3f)",
				nativeEffectProbe.sequence,
				nativeEffectProbe.callSiteRva,
				nativeEffectProbe.ownerLocal ? "true" : "false",
				nativeEffectProbe.overridden ? "true" : "false",
				rawStart.x, rawStart.y, rawStart.z,
				rawTarget.x, rawTarget.y, rawTarget.z,
				appliedStart.x, appliedStart.y, appliedStart.z,
				appliedTarget.x, appliedTarget.y, appliedTarget.z);
		}
		const glm::fvec3 rawDirection = NormalizeOrZero(rawTarget - rawStart);
		const glm::fvec3 appliedDirection = NormalizeOrZero(appliedTarget - appliedStart);
		uevr::API::get()->log_info(
			"[NativeBulletTracerProbe] seq=%u callSite=0x%08X overridden=%s rawStart=(%.3f %.3f %.3f) rawTarget=(%.3f %.3f %.3f) appliedStart=(%.3f %.3f %.3f) appliedTarget=(%.3f %.3f %.3f) rawOriginVsMock=%.3f appliedOriginVsMock=%.3f rawDirVsMock=%.3fdeg appliedDirVsMock=%.3fdeg",
			nativeEffectProbe.sequence,
			nativeEffectProbe.callSiteRva,
			nativeEffectProbe.overridden ? "true" : "false",
			rawStart.x, rawStart.y, rawStart.z,
			rawTarget.x, rawTarget.y, rawTarget.z,
			appliedStart.x, appliedStart.y, appliedStart.z,
			appliedTarget.x, appliedTarget.y, appliedTarget.z,
			glm::length(rawStart - lastAimMuzzlePosition),
			glm::length(appliedStart - lastAimMuzzlePosition),
			AngleDegreesOrZero(rawDirection, lastRawBarrelAimForward),
			AngleDegreesOrZero(appliedDirection, lastRawBarrelAimForward));
	}
	if (!settingsManager->debugSpreadProbe)
	{
		latch = false;
		return;
	}

	if (!isShooting)
	{
		latch = false;
		return;
	}
	if (latch)
		return;
	latch = true;

	const int modelId = CurrentWeaponModelIdForStats();
	MemoryManager::WeaponInfoDebugSnapshot info{};
	const bool foundInfo = memoryManager->ReadWeaponInfoDebugSnapshot(modelId, info);
	const glm::fvec3 gameAim = {
		*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[0])),
		*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[1])),
		*(reinterpret_cast<float*>(memoryManager->aimForwardVectorAddresses[2]))
	};
	const glm::fvec3 gameOrigin = {
		*(reinterpret_cast<float*>(memoryManager->cameraPositionAddresses[0])),
		*(reinterpret_cast<float*>(memoryManager->cameraPositionAddresses[1])),
		*(reinterpret_cast<float*>(memoryManager->cameraPositionAddresses[2]))
	};
	const glm::fvec3 cameraAim = {
		cameraController->cameraMatrixValues[4],
		cameraController->cameraMatrixValues[5],
		cameraController->cameraMatrixValues[6]
	};
	const float gunVsGameDeg = AngleDegreesOrZero(calculatedAimForward, gameAim);
	const float cameraVsGameDeg = AngleDegreesOrZero(cameraAim, gameAim);
	const float rawVsAdjustedDeg = AngleDegreesOrZero(
		lastRawBarrelAimForward, lastPreLatchAimForward);
	const float adjustedVsFinalDeg = AngleDegreesOrZero(
		lastPreLatchAimForward, calculatedAimForward);
	const float muzzleVsGameOrigin = glm::length(gameOrigin - lastAimMuzzlePosition);
	uint32_t nativeBypassA = 0;
	uint32_t nativeBypassB = 0;
	memoryManager->GetNativeShotSpreadBypassCounts(nativeBypassA, nativeBypassB);

	uevr::API::get()->log_info(
		"[AimOriginProbe] shot hand=%s weapon=%d model=%d cameraMode=%d legacyCrosshair=%s muzzle=(%.4f %.4f %.4f) gameOrigin=(%.4f %.4f %.4f) originDelta=%.5f rawBarrel=(%.4f %.4f %.4f) adjusted=(%.4f %.4f %.4f) final=(%.4f %.4f %.4f) gameAim=(%.4f %.4f %.4f) rawVsAdjusted=%.3fdeg adjustedVsFinal=%.3fdeg finalVsGame=%.3fdeg",
		firstWeapon ? "first" : "second",
		static_cast<int>(currentWeaponEquipped), modelId,
		static_cast<int>(cameraController->currentCameraMode),
		lastLegacyCrosshairCompensationApplied ? "true" : "false",
		lastAimMuzzlePosition.x, lastAimMuzzlePosition.y, lastAimMuzzlePosition.z,
		gameOrigin.x, gameOrigin.y, gameOrigin.z, muzzleVsGameOrigin,
		lastRawBarrelAimForward.x, lastRawBarrelAimForward.y, lastRawBarrelAimForward.z,
		lastPreLatchAimForward.x, lastPreLatchAimForward.y, lastPreLatchAimForward.z,
		calculatedAimForward.x, calculatedAimForward.y, calculatedAimForward.z,
		gameAim.x, gameAim.y, gameAim.z,
		rawVsAdjustedDeg, adjustedVsFinalDeg, gunVsGameDeg);

	if (foundInfo)
	{
		uevr::API::get()->log_info("[SpreadProbe] shot hand=%s weapon=%d model=%d entries=%d fireType=%u spread=%.4f..%.4f accuracy=%.2f..%.2f damage=%u range=%.1f/%.1f nativeBypass=%u/%u gunVsGame=%.2fdeg cameraVsGame=%.2fdeg gunAim=(%.3f %.3f %.3f) gameAim=(%.3f %.3f %.3f)",
			firstWeapon ? "first" : "second",
			static_cast<int>(currentWeaponEquipped),
			modelId,
			info.matchingEntries,
			info.fireType,
			info.minSpread,
			info.maxSpread,
			info.minAccuracy,
			info.maxAccuracy,
			info.damage,
			info.targetRange,
			info.weaponRange,
			nativeBypassA,
			nativeBypassB,
			gunVsGameDeg,
			cameraVsGameDeg,
			calculatedAimForward.x,
			calculatedAimForward.y,
			calculatedAimForward.z,
			gameAim.x,
			gameAim.y,
			gameAim.z);
	}
	else
	{
		uevr::API::get()->log_info("[SpreadProbe] shot hand=%s weapon=%d model=%d entries=0 nativeBypass=%u/%u gunVsGame=%.2fdeg cameraVsGame=%.2fdeg gunAim=(%.3f %.3f %.3f) gameAim=(%.3f %.3f %.3f)",
			firstWeapon ? "first" : "second",
			static_cast<int>(currentWeaponEquipped),
			modelId,
			nativeBypassA,
			nativeBypassB,
			gunVsGameDeg,
			cameraVsGameDeg,
			calculatedAimForward.x,
			calculatedAimForward.y,
			calculatedAimForward.z,
			gameAim.x,
			gameAim.y,
			gameAim.z);
	}
}

void WeaponManager::ResetAimCalibration()
{
	aimCalibrationResiduals.clear();
	aimCalibrationGeneralSamples = 0;
	aimCalibrationScopedSamples = 0;
	memoryManager->DiscardPendingAimCalibrationSample();
	uevr::API::get()->dispatch_lua_event("aimCalibrationResult", "No accepted shots yet");
	if (settingsManager->enableAimCalibrationProbe)
		uevr::API::get()->log_info("%s", "[AimCalibration] samples reset");
}

void WeaponManager::ProcessAimCalibrationSample()
{
	if (!settingsManager->enableAimCalibrationProbe)
	{
		if (aimCalibrationWasEnabled)
			ResetAimCalibration();
		aimCalibrationWasEnabled = false;
		return;
	}

	if (!aimCalibrationWasEnabled)
	{
		aimCalibrationWasEnabled = true;
		ResetAimCalibration();
		uevr::API::get()->log_info("%s", "[AimCalibration] enabled; fire supported bullet weapons at a fixed surface");
		return;
	}

	MemoryManager::AimCalibrationSample sample{};
	if (!memoryManager->ReadAimCalibrationSample(sample))
		return;

	// Bullet weapons only. Rockets, sprays, flame, and scripted vehicle fire use
	// different collision paths and need separate calibration later.
	if (sample.weaponType < Pistol || sample.weaponType > Sniper || sample.hitEntity == 0)
		return;

	const glm::fvec3 intended = NormalizeOrZero({
		sample.intendedDirection[0],
		sample.intendedDirection[1],
		sample.intendedDirection[2]
	});
	const glm::fvec3 start = {
		sample.shotStart[0],
		sample.shotStart[1],
		sample.shotStart[2]
	};
	const glm::fvec3 target = {
		sample.shotTarget[0],
		sample.shotTarget[1],
		sample.shotTarget[2]
	};
	const glm::fvec3 hit = {
		sample.hitPoint[0],
		sample.hitPoint[1],
		sample.hitPoint[2]
	};
	const glm::fvec3 nativeDirection = NormalizeOrZero(target - start);
	const glm::fvec3 impactDirection = NormalizeOrZero(hit - start);
	const float impactDistance = glm::length(hit - start);
	if (glm::length(intended) <= 0.0f || glm::length(nativeDirection) <= 0.0f
		|| glm::length(impactDirection) <= 0.0f || !std::isfinite(impactDistance)
		|| impactDistance < 1.0f || impactDistance > 5000.0f)
		return;

	auto wrapDegrees = [](float value) {
		while (value > 180.0f) value -= 360.0f;
		while (value < -180.0f) value += 360.0f;
		return value;
	};
	auto yawDegrees = [](const glm::fvec3& value) {
		return std::atan2(value.y, value.x) * 57.2957795f;
	};
	auto pitchDegrees = [](const glm::fvec3& value) {
		return std::atan2(value.z, std::sqrt((value.x * value.x) + (value.y * value.y))) * 57.2957795f;
	};

	const float yawResidual = wrapDegrees(yawDegrees(impactDirection) - yawDegrees(intended));
	const float pitchResidual = pitchDegrees(impactDirection) - pitchDegrees(intended);
	const float totalResidual = AngleDegreesOrZero(intended, impactDirection);
	const float nativeResidual = AngleDegreesOrZero(intended, nativeDirection);
	if (!std::isfinite(yawResidual) || !std::isfinite(pitchResidual)
		|| !std::isfinite(totalResidual) || totalResidual > 20.0f)
		return;

	const bool scoped = sample.weaponType == Sniper;
	const size_t categoryLimit = 21;
	size_t categoryCount = 0;
	for (const auto& residual : aimCalibrationResiduals)
		categoryCount += residual.scoped == scoped ? 1 : 0;
	if (categoryCount >= categoryLimit)
	{
		const auto oldest = std::find_if(aimCalibrationResiduals.begin(), aimCalibrationResiduals.end(),
			[scoped](const AimCalibrationResidual& residual) { return residual.scoped == scoped; });
		if (oldest != aimCalibrationResiduals.end())
			aimCalibrationResiduals.erase(oldest);
	}

	aimCalibrationResiduals.push_back({
		yawResidual,
		pitchResidual,
		totalResidual,
		impactDistance,
		sample.weaponType,
		scoped
	});
	uint32_t& accepted = scoped ? aimCalibrationScopedSamples : aimCalibrationGeneralSamples;
	++accepted;
	uevr::API::get()->log_info("[AimCalibration] %s sample=%u weapon=%d distance=%.1f yaw=%+.3fdeg pitch=%+.3fdeg total=%.3fdeg nativeRay=%.3fdeg",
		scoped ? "scoped" : "general",
		accepted,
		sample.weaponType,
		impactDistance,
		yawResidual,
		pitchResidual,
		totalResidual,
		nativeResidual);

	if (accepted != 1 && accepted % 5 != 0)
		return;

	std::vector<float> yawValues;
	std::vector<float> pitchValues;
	for (const auto& residual : aimCalibrationResiduals)
	{
		if (residual.scoped == scoped)
		{
			yawValues.push_back(residual.yawDegrees);
			pitchValues.push_back(residual.pitchDegrees);
		}
	}
	auto median = [](std::vector<float> values) {
		if (values.empty())
			return 0.0f;
		std::sort(values.begin(), values.end());
		const size_t middle = values.size() / 2;
		return values.size() % 2 == 0
			? (values[middle - 1] + values[middle]) * 0.5f
			: values[middle];
	};

	const float medianYaw = median(yawValues);
	const float medianPitch = median(pitchValues);
	char result[192]{};
	std::snprintf(result, sizeof(result), "%s: %llu kept / %u total | yaw %+.3f deg | pitch %+.3f deg",
		scoped ? "Scoped" : "General",
		static_cast<unsigned long long>(yawValues.size()),
		accepted,
		medianYaw,
		medianPitch);
	uevr::API::get()->dispatch_lua_event("aimCalibrationResult", result);
	uevr::API::get()->log_info("[AimCalibration] median %s kept=%llu total=%u yaw=%+.3fdeg pitch=%+.3fdeg",
		scoped ? "scoped" : "general",
		static_cast<unsigned long long>(yawValues.size()),
		accepted,
		medianYaw,
		medianPitch);
}

void WeaponManager::ProcessWeaponVisibility()
{
	if (settingsManager->debugMod) uevr::API::get()->log_info("HandleWeaponVisibility()");

	if (firstWeaponMesh == nullptr)
		return;

	// During a custom grenade/Molotov flight the proxy owns the visual slot. The normal
	// weapon-visibility pass runs after ProcessPhysicalThrowableProbe and would
	// otherwise make the original held mesh visible again every engine tick,
	// hiding the fact that the released proxy is airborne.
	if ((currentWeaponEquipped == Grenade || currentWeaponEquipped == Molotov)
		&& (motionThrowableFlight.active || motionThrowableSourceHidden))
	{
		SetComponentVisibility(firstWeaponMesh, false);
		return;
	}
	// A newly accepted grip owns the source bottle until release. Keep this
	// explicit because weaponScaledVisible can still reflect the previous
	// detached flight and would otherwise hide the bottle in the hand.
	if ((currentWeaponEquipped == Grenade || currentWeaponEquipped == Molotov)
		&& throwableProbeActive
		&& !motionThrowableFlight.active && !motionThrowableSourceHidden)
	{
		SetComponentVisibility(firstWeaponMesh, true);
		auto heldClass = firstWeaponMesh->get_class();
		if (heldClass != nullptr)
		{
			if (heldClass->find_property(L"bHiddenInGame") != nullptr)
				firstWeaponMesh->set_bool_property(L"bHiddenInGame", false);
			if (heldClass->find_property(L"bOwnerNoSee") != nullptr)
				firstWeaponMesh->set_bool_property(L"bOwnerNoSee", false);
			if (heldClass->find_property(L"bOnlyOwnerSee") != nullptr)
				firstWeaponMesh->set_bool_property(L"bOnlyOwnerSee", false);
			if (heldClass->find_function(L"SetOwnerNoSee") != nullptr)
			{
				Utilities::ParameterSingleBool ownerNoSee{};
				ownerNoSee.boolValue = false;
				firstWeaponMesh->call_function(L"SetOwnerNoSee", &ownerNoSee);
			}
			if (heldClass->find_property(L"bVisible") != nullptr)
				firstWeaponMesh->set_bool_property(L"bVisible", true);
		}
		return;
	}

	bool hideWeapon = !weaponScaledVisible;
	switch (currentWeaponEquipped)
	{
	case NightVision:
		hideWeapon = true;
		break;
	case Infrared:
		hideWeapon = true;
		break;
	case Parachute:
		hideWeapon = true;
		break;
	default:
		break;
	}

	Utilities::ParameterSingleBool setOwnerNoSee_params;
	setOwnerNoSee_params.boolValue = playerManager->isInControl ? hideWeapon : false; //Enable visibility when in cutscenes
	firstWeaponMesh->call_function(L"SetOwnerNoSee", &setOwnerNoSee_params);
	firstWeaponMesh->set_bool_property(L"bVisible", !setOwnerNoSee_params.boolValue);
}

void WeaponManager::SetWeaponScaled(bool visible, bool force)
{
	if (!force && weaponScaledVisible == visible)
		return;

	weaponScaledVisible = visible;
	SetComponentVisibility(firstWeaponMesh, visible);
	const bool customSecondVisible = visible && secondWeaponMesh == customAkimboVisualMesh
		&& customAkimboActive;
	SetComponentVisibility(secondWeaponMesh,
		secondWeaponMesh == customAkimboVisualMesh ? customSecondVisible : visible);
}

void WeaponManager::SetMotionWeaponTrackingEnabled(bool enabled, bool force)
{
	if (!enabled && force)
		ResetRuntimeHandState("forced-tracking-disable", true, true);
	// Visibility events can request controller tracking before the game-thread
	// proximity test has accepted a grip. Keep the body slot authoritative.
	if (enabled && magneticIdleWeaponActive)
	{
		motionWeaponTrackingEnabled = false;
		visualWeaponTrackingEnabled = true;
		magneticTriggerBlockedSnapshot.store(true, std::memory_order_release);
		return;
	}
	const bool meleeWeapon = currentWeaponEquipped >= BrassKnuckles
		&& currentWeaponEquipped <= Cane;
	const bool preserveMeleePresentation = !enabled && !force && meleeWeapon
		&& magneticGripHand >= 0
		&& magneticGripWeaponId == static_cast<int>(currentWeaponEquipped)
		&& playerManager != nullptr
		&& playerManager->isInControl
		&& !playerManager->isInVehicle
		&& !playerManager->weaponWheelEnabled;
	if (preserveMeleePresentation)
	{
		// The Lua visibility state is shared by guns and melee. A default/idle
		// event must not turn a valid melee pickup into an immediate stash. The
		// normal grip-mask edge below still owns the real release.
		motionWeaponTrackingEnabled = true;
		visualWeaponTrackingEnabled = true;
		magneticReleaseRequested = false;
		return;
	}
	if (!enabled && magneticGripHand >= 0 && !force
		&& magneticGripWeaponId == static_cast<int>(currentWeaponEquipped))
	{
		// Capture the final controller-driven world pose on the game thread after
		// the corresponding grip edge is observed.
		magneticReleaseRequested = true;
		return;
	}
	if (!enabled && IsMagneticIdleSlotEligible())
	{
		EnterMagneticIdleSlot();
		return;
	}

	const bool controllerHeldMelee = meleeWeapon;
	const bool controllerHeldRanged = currentWeaponEquipped >= Pistol
		&& currentWeaponEquipped <= Minigun;
	const bool controllerHeldUtility = IsControllerHeldUtility();
	const bool keepVisualOnly = !enabled
		&& settingsManager->enableFreeAimWeaponHands
		&& playerManager->isInControl
		&& !playerManager->isInVehicle
		&& (controllerHeldMelee || controllerHeldRanged || controllerHeldUtility);
	const bool desiredVisualTracking = enabled || keepVisualOnly;
	if (!force && motionWeaponTrackingEnabled == enabled
		&& visualWeaponTrackingEnabled == desiredVisualTracking)
	{
		if (!desiredVisualTracking)
			return;

		const bool firstTrackingReady = firstWeaponMesh == nullptr ||
			uevr::API::UObjectHook::get_motion_controller_state(firstWeaponMesh) != nullptr;
		const bool secondTrackingReady = secondWeaponMesh == nullptr ||
			uevr::API::UObjectHook::get_motion_controller_state(secondWeaponMesh) != nullptr;
		if (firstTrackingReady && secondTrackingReady)
			return;
	}

	motionWeaponTrackingEnabled = enabled;
	visualWeaponTrackingEnabled = desiredVisualTracking;
	if (!desiredVisualTracking)
	{
		// The game and UEVR share this mesh. Removing UEVR's controller state
		// returns the visible gun to GTA's native hand attachment for idle poses.
		// Keep the independent hand clones alive; their presentation eligibility
		// is intentionally separate from the weapon's motion state.
		UnhookAndRepositionWeapon(false, force);
	}
	else
	{
		const bool firstTrackingReady = firstWeaponMesh != nullptr
			&& uevr::API::UObjectHook::get_motion_controller_state(firstWeaponMesh) != nullptr;
		if (force || !firstTrackingReady)
		{
			// Attach or repair the visual mesh. Gameplay aiming remains gated by
			// motionWeaponTrackingEnabled, so idle tracking is presentation-only.
			motionConfiguredFirstWeaponMesh = nullptr;
			motionConfiguredSecondWeaponMesh = nullptr;
			motionConfiguredFirstHand = -1;
			motionConfiguredSecondHand = -1;
			motionConfiguredFirstCalibrationRole = -1;
		}
	}

	if (settingsManager->debugInputLayerProbe)
		uevr::API::get()->log_info("[WeaponAttach] gameplay=%s visual=%s",
			enabled ? "motion" : "native", desiredVisualTracking ? "controller" : "native");
}

bool WeaponManager::IsMagneticIdleSlotEligible() const
{
	return settingsManager->enableFreeAimWeaponHands
		&& playerManager->isInControl
		&& !playerManager->isInVehicle
		&& !playerManager->weaponWheelEnabled
		&& cameraController->currentCameraMode != CameraController::Camera
		// Reuse the firearm pickup/stash state machine for melee meshes. GTA still
		// owns melee hit detection and animations; this only gives the visible
		// weapon and split hand presentation controller ownership while gripped.
		&& ((currentWeaponEquipped >= BrassKnuckles && currentWeaponEquipped <= Cane)
			|| (currentWeaponEquipped >= Pistol && currentWeaponEquipped <= Minigun)
			|| IsControllerHeldUtility())
		&& firstWeaponMesh != nullptr && secondWeaponMesh == nullptr
		&& uevr::API::UObjectHook::exists(firstWeaponMesh);
}

bool WeaponManager::IsControllerHeldUtility() const
{
	// Spray can retains GTA's native spray/mist/graffiti action. This flag only
	// opts its existing live mesh into the proven one-hand grip/stash and fake-
	// hand presentation paths. Do not broaden this to extinguisher or throwables
	// until their distinct native actions have been tested.
	return currentWeaponEquipped == SprayCan;
}

int WeaponManager::ResolveMagneticHolsterVerticalAxis() const
{
	// Firearms are authored along local X and retain the proven barrel-down path.
	// GTA's held melee meshes are not consistent with that convention; use their
	// measured longest local bounds axis so bats, clubs, cues, shovels and similar
	// weapons hang lengthwise instead of lying flat against the waist.
	if (currentWeaponEquipped < GolfClub || currentWeaponEquipped > Cane
		|| firstWeaponMesh == nullptr || !uevr::API::UObjectHook::exists(firstWeaponMesh))
		return 0;
	auto componentClass = firstWeaponMesh->get_class();
	if (componentClass == nullptr || componentClass->find_function(L"GetLocalBounds") == nullptr)
		return 0;
	ParameterGetLocalBounds bounds{};
	firstWeaponMesh->call_function(L"GetLocalBounds", &bounds);
	if (!IsFiniteVector(bounds.min) || !IsFiniteVector(bounds.max))
		return 0;
	const glm::fvec3 spans = glm::abs(bounds.max - bounds.min);
	if (!IsFiniteVector(spans) || (std::max)({ spans.x, spans.y, spans.z }) < 5.0f)
		return 0;
	if (spans.y > spans.x && spans.y >= spans.z)
		return 1;
	if (spans.z > spans.x && spans.z > spans.y)
		return 2;
	return 0;
}

bool WeaponManager::ReadMagneticBodyFrame(glm::fvec3& origin, glm::fquat& rotation)
{
	// A released weapon is lower-body inventory. Build the horizontal frame from
	// the animated left/right thigh positions so HMD, camera and aim yaw can never
	// redefine where CJ's hips are. The old actor-forward frame had a fixed model
	// heading offset: runtime drops showed anatomical left/right primarily in its
	// local X coordinate while normalization assumed local Y, producing the
	// persistent belt-buckle/opposite-side placement.
	auto actor = playerManager->playerActor != nullptr
		? playerManager->playerActor : playerManager->playerCharacter;
	origin = glm::fvec3(0.0f);
	if (actor != nullptr && uevr::API::UObjectHook::exists(actor)
		&& actor->get_class() != nullptr)
	{
		auto actorClass = actor->get_class();
		if (actorClass->find_function(L"K2_GetActorLocation") != nullptr)
		{
			Utilities::ParameterSingleVector3 locationParams{};
			actor->call_function(L"K2_GetActorLocation", &locationParams);
			origin = locationParams.vec3Value + playerManager->defaultPlayerHeadLocalPositionUE;
		}
	}
	if (!IsFiniteVector(origin) || glm::length(origin) < 1.0f)
		origin = playerManager->actualPlayerHeadPositionUE;
	if (!IsFiniteVector(origin) || glm::length(origin) < 1.0f)
		origin = playerManager->actualPlayerPositionUE;
	if (!IsFiniteVector(origin) || glm::length(origin) < 1.0f)
		return false;

	uevr::API::UObject* legComponent = nullptr;
	if (playerManager->playerCharacter != nullptr
		&& uevr::API::UObjectHook::exists(playerManager->playerCharacter)
		&& playerManager->playerCharacter->get_class() != nullptr
		&& playerManager->playerCharacter->get_class()->find_property(L"Trousers") != nullptr)
	{
		legComponent = playerManager->playerCharacter->get_property<uevr::API::UObject*>(L"Trousers");
	}
	if (legComponent == nullptr || !uevr::API::UObjectHook::exists(legComponent))
		legComponent = playerManager->lowerBodyVisibilityProperty;
	if (legComponent != magneticLegFrameComponent)
	{
		magneticLegFrameComponent = legComponent;
		magneticLeftThighBone = {};
		magneticRightThighBone = {};
		magneticLegFrameBonesResolved = false;
		magneticLegFrameResolutionAttempted = false;
		magneticLegFrameFallbackLogged = false;
	}

	const glm::fvec3 up(0.0f, 0.0f, 1.0f);
	glm::fvec3 forward(0.0f);
	glm::fvec3 right(0.0f);
	if (legComponent != nullptr && uevr::API::UObjectHook::exists(legComponent))
	{
		if (!magneticLegFrameResolutionAttempted)
		{
			magneticLegFrameResolutionAttempted = true;
			ResolvedBone leftThigh{};
			ResolvedBone rightThigh{};
			magneticLegFrameBonesResolved =
				ResolveFirstBone(legComponent,
					{ L"L_Thigh", L"L_UpperLeg", L"LeftUpLeg", L"thigh_l", L"Bip01 L Thigh" },
					leftThigh)
				&& ResolveFirstBone(legComponent,
					{ L"R_Thigh", L"R_UpperLeg", L"RightUpLeg", L"thigh_r", L"Bip01 R Thigh" },
					rightThigh);
			if (magneticLegFrameBonesResolved)
			{
				magneticLeftThighBone = leftThigh.name;
				magneticRightThighBone = rightThigh.name;
				uevr::API::get()->log_info(
					"[MagneticWeapon] leg frame resolved component=%ls left=%ls right=%ls",
					legComponent->get_full_name().c_str(), leftThigh.text.c_str(), rightThigh.text.c_str());
			}
			else
			{
				uevr::API::get()->log_warn(
					"[MagneticWeapon] leg frame unresolved component=%ls; using corrected actor fallback",
					legComponent->get_full_name().c_str());
			}
		}

		if (magneticLegFrameBonesResolved)
		{
			glm::fvec3 leftThighPosition{};
			glm::fvec3 rightThighPosition{};
			glm::fquat unusedRotation{};
			if (ReadBoneWorldTransform(legComponent, magneticLeftThighBone,
					leftThighPosition, unusedRotation)
				&& ReadBoneWorldTransform(legComponent, magneticRightThighBone,
					rightThighPosition, unusedRotation))
			{
				right = rightThighPosition - leftThighPosition;
				right.z = 0.0f;
				right = NormalizeOrZero(right);
				forward = NormalizeOrZero(glm::cross(right, up));
			}
		}
	}

	if (glm::length(forward) <= 0.0001f || glm::length(right) <= 0.0001f)
	{
		// GTA's actor forward is offset a quarter-turn from the visible lower-body
		// axes in this model. Correct that fixed offset, but never consult camera or
		// HMD yaw. This path is only a compatibility fallback for missing leg bones.
		glm::fvec3 actorForward(0.0f);
		if (actor != nullptr && uevr::API::UObjectHook::exists(actor)
			&& actor->get_class() != nullptr
			&& actor->get_class()->find_function(L"GetActorForwardVector") != nullptr)
		{
			Utilities::ParameterSingleVector3 forwardParams{};
			actor->call_function(L"GetActorForwardVector", &forwardParams);
			actorForward = forwardParams.vec3Value;
		}
		actorForward.z = 0.0f;
		actorForward = NormalizeOrZero(actorForward);
		if (glm::length(actorForward) <= 0.0001f)
			return false;
		right = actorForward;
		forward = NormalizeOrZero(glm::cross(right, up));
		if (!magneticLegFrameFallbackLogged)
		{
			magneticLegFrameFallbackLogged = true;
			uevr::API::get()->log_info(
				"[MagneticWeapon] leg frame source=actor-corrected camera=false hmd=false");
		}
	}

	rotation = RotationFromWeaponBasis(forward, right, up);
	return IsFiniteQuaternion(rotation) && glm::length(rotation) > 0.5f;
}

void WeaponManager::SetMagneticIdleAnchor(int hand)
{
	const int anchorHand = hand == 0 || hand == 1
		? hand
		: (settingsManager->leftHandedMode != SettingsManager::Disabled ? 0 : 1);
	const float side = anchorHand == 0 ? -1.0f : 1.0f;
	const bool longGun = IsTwoHandLongGun();
	const int verticalAxis = ResolveMagneticHolsterVerticalAxis();

	// Close body-local hip anchors. Weapon X is its barrel axis: sidearms point
	// down with a modest forward cant; long guns hang almost vertically.
	magneticIdleLocalPosition = longGun
		? glm::fvec3(8.0f, side * 22.0f, -68.0f)
		: glm::fvec3(10.0f, side * 19.0f, -62.0f);
	magneticIdleLocalForward = glm::fvec3(1.0f, 0.0f, 0.0f);
	magneticIdleLocalRight = glm::fvec3(0.0f, 1.0f, 0.0f);
	magneticIdleLocalUp = glm::fvec3(0.0f, 0.0f, 1.0f);
	NormalizeHolsterPose(longGun, verticalAxis, anchorHand, magneticIdleLocalPosition,
		magneticIdleLocalForward, magneticIdleLocalRight, magneticIdleLocalUp);
	magneticIdleAnchorHand = anchorHand;
	magneticIdleAnchorBucket = longGun ? 1 : 0;
	magneticCustomAnchorValid = false;
	magneticCustomAnchorWeaponId = -1;
	magneticBodyFrameRebaseAt = 0;
	magneticLastHeldPoseHand = -1;
	magneticLastHeldPoseGripGeneration = 0;
}

bool WeaponManager::ReadMagneticWaistAnchorsFile(const std::string& path,
	std::unordered_map<int, MagneticWaistAnchor>& result, int& loaded) const
{
	result.clear();
	loaded = 0;
	std::ifstream input(path);
	if (!input.is_open())
		return false;

	std::string line;
	while (std::getline(input, line))
	{
		if (line.empty() || line[0] == '#')
			continue;
		std::istringstream stream(line);
		std::string marker;
		int version = 0;
		int weaponId = -1;
		MagneticWaistAnchor anchor{};
		if (!(stream >> marker >> version >> weaponId >> anchor.hand
			>> anchor.position.x >> anchor.position.y >> anchor.position.z
			>> anchor.forward.x >> anchor.forward.y >> anchor.forward.z
			>> anchor.right.x >> anchor.right.y >> anchor.right.z
			>> anchor.up.x >> anchor.up.y >> anchor.up.z)
			|| marker != "entry" || version != 3
			|| weaponId < static_cast<int>(BrassKnuckles)
			|| weaponId > static_cast<int>(Parachute)
			|| (anchor.hand != 0 && anchor.hand != 1)
			|| !IsFiniteVector(anchor.position) || !IsFiniteVector(anchor.forward)
			|| !IsFiniteVector(anchor.right) || !IsFiniteVector(anchor.up)
			|| glm::length(anchor.forward) < 0.5f || glm::length(anchor.right) < 0.5f
			|| glm::length(anchor.up) < 0.5f)
			continue;
		anchor.forward = NormalizeOrZero(anchor.forward);
		anchor.right = NormalizeOrZero(anchor.right);
		anchor.up = NormalizeOrZero(anchor.up);
		anchor.gripGeneration = 0;
		result[weaponId] = anchor;
		++loaded;
	}
	return input.eof() || input.good();
}

bool WeaponManager::WriteMagneticWaistAnchorsFile(const std::string& path,
	const std::unordered_map<int, MagneticWaistAnchor>& values) const
{
	std::ofstream output(path, std::ios::trunc);
	if (!output.is_open())
		return false;
	output << "# UEVR GTA SA DE magnetic holster anchors v3\n";
	output << "# entry 3 weaponId hand posX posY posZ forwardX forwardY forwardZ rightX rightY rightZ upX upY upZ\n";
	output.setf(std::ios::fixed);
	output.precision(6);
	std::vector<int> weaponIds;
	weaponIds.reserve(values.size());
	for (const auto& [weaponId, unused] : values)
		weaponIds.push_back(weaponId);
	std::sort(weaponIds.begin(), weaponIds.end());
	for (const int weaponId : weaponIds)
	{
		const auto& anchor = values.at(weaponId);
		output << "entry 3 " << weaponId << ' ' << anchor.hand << ' '
			<< anchor.position.x << ' ' << anchor.position.y << ' ' << anchor.position.z << ' '
			<< anchor.forward.x << ' ' << anchor.forward.y << ' ' << anchor.forward.z << ' '
			<< anchor.right.x << ' ' << anchor.right.y << ' ' << anchor.right.z << ' '
			<< anchor.up.x << ' ' << anchor.up.y << ' ' << anchor.up.z << "\n";
	}
	output.flush();
	return output.good();
}

void WeaponManager::LoadMagneticWaistAnchors()
{
	if (magneticWaistAnchorsLoaded)
		return;
	magneticWaistAnchorsLoaded = true;
	const std::string path = settingsManager->GetHolsterAnchorsFilePath();
	if (path.empty())
		return;
	std::unordered_map<int, MagneticWaistAnchor> loadedValues;
	int loaded = 0;
	if (!ReadMagneticWaistAnchorsFile(path, loadedValues, loaded))
	{
		uevr::API::get()->log_info("[MagneticWeapon] anchor file=%s entries=0", path.c_str());
		return;
	}
	magneticWaistAnchors = std::move(loadedValues);
	uevr::API::get()->log_info("[MagneticWeapon] anchor file=%s schema=v3 entries=%d",
		path.c_str(), loaded);
}

bool WeaponManager::SaveMagneticWaistAnchors()
{
	const std::string path = settingsManager->GetHolsterAnchorsFilePath();
	if (path.empty())
		return false;
	const std::filesystem::path target(path);
	const std::filesystem::path temporary = target.wstring() + L".tmp";
	std::error_code error;
	std::filesystem::remove(temporary, error);
	if (!WriteMagneticWaistAnchorsFile(temporary.string(), magneticWaistAnchors))
	{
		uevr::API::get()->log_warn("[MagneticWeapon] anchor save failed stage=temp-write file=%s",
			path.c_str());
		std::filesystem::remove(temporary, error);
		return false;
	}
	std::unordered_map<int, MagneticWaistAnchor> roundTrip;
	int roundTripLoaded = 0;
	if (!ReadMagneticWaistAnchorsFile(temporary.string(), roundTrip, roundTripLoaded)
		|| roundTrip.size() != magneticWaistAnchors.size())
	{
		uevr::API::get()->log_warn("[MagneticWeapon] anchor save failed stage=temp-validation file=%s",
			path.c_str());
		std::filesystem::remove(temporary, error);
		return false;
	}
	if (!MoveFileExW(temporary.c_str(), target.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		const DWORD moveError = GetLastError();
		uevr::API::get()->log_warn(
			"[MagneticWeapon] anchor save failed stage=atomic-replace error=%lu file=%s",
			static_cast<unsigned long>(moveError), path.c_str());
		std::filesystem::remove(temporary, error);
		return false;
	}
	uevr::API::get()->log_info("[MagneticWeapon] anchor save committed schema=v3 entries=%d",
		roundTripLoaded);
	return true;
}

void WeaponManager::InitializeMagneticHolster()
{
	LoadMagneticWaistAnchors();
}

bool WeaponManager::CaptureMagneticReleaseAnchor(int hand, bool logCapture)
{
	glm::fvec3 bodyOrigin{};
	glm::fquat rawBodyRotation{};
	glm::fvec3 weaponPosition{};
	glm::fquat weaponRotation{};
	if (!ReadMagneticBodyFrame(bodyOrigin, rawBodyRotation)
		|| !ReadCurrentWeaponWorldTransform(weaponPosition, weaponRotation))
	{
		if (logCapture)
			uevr::API::get()->log_info(
				"[MagneticWeapon] release rejected weapon=%d reason=transform-read",
				static_cast<int>(currentWeaponEquipped));
		return false;
	}
	const glm::fquat bodyRotation = magneticStableBodyRotationValid
		&& IsFiniteQuaternion(magneticStableBodyRotation)
		&& glm::length(magneticStableBodyRotation) > 0.5f
		? magneticStableBodyRotation : rawBodyRotation;
	const glm::fquat inverseBody = glm::inverse(bodyRotation);
	const glm::fquat localRotation = glm::normalize(inverseBody * weaponRotation);
	const glm::fvec3 localPosition = inverseBody * (weaponPosition - bodyOrigin);
	const glm::fvec3 localForward = NormalizeOrZero(localRotation * glm::fvec3(1.0f, 0.0f, 0.0f));
	const glm::fvec3 localRight = NormalizeOrZero(localRotation * glm::fvec3(0.0f, 1.0f, 0.0f));
	const glm::fvec3 localUp = NormalizeOrZero(localRotation * glm::fvec3(0.0f, 0.0f, 1.0f));
	if (!IsFiniteVector(localPosition) || !IsFiniteVector(localForward)
		|| !IsFiniteVector(localRight) || !IsFiniteVector(localUp)
		|| glm::length(localForward) < 0.5f || glm::length(localRight) < 0.5f
		|| glm::length(localUp) < 0.5f)
	{
		if (logCapture)
			uevr::API::get()->log_info(
				"[MagneticWeapon] release rejected weapon=%d reason=invalid-basis",
				static_cast<int>(currentWeaponEquipped));
		return false;
	}
	const float horizontalRadius = glm::length(glm::fvec2(localPosition.x, localPosition.y));
	// Use the broad body volume from the last known-good free-placement path.
	// Actor heading can temporarily disagree with HMD heading after an aiming
	// stance, so front-of-player samples have legitimately appeared on either
	// side of local X. The final position is projected to the waist plane below.
	const bool waistPlacement = localPosition.z >= -165.0f && localPosition.z <= 40.0f
		&& localPosition.x >= -100.0f && localPosition.x <= 85.0f
		&& std::abs(localPosition.y) <= 105.0f
		&& std::isfinite(horizontalRadius)
		&& horizontalRadius >= 8.0f && horizontalRadius <= 110.0f
		&& glm::length(localPosition) <= 210.0f;
	if (!waistPlacement)
	{
		if (logCapture)
			uevr::API::get()->log_info(
				"[MagneticWeapon] release rejected weapon=%d generation=%u localP=(%.1f %.1f %.1f); same-generation pose required",
				static_cast<int>(currentWeaponEquipped),
				hand >= 0 && hand <= 1
					? gripPressGeneration[static_cast<size_t>(hand)].load(std::memory_order_acquire) : 0,
				localPosition.x, localPosition.y, localPosition.z);
		return false;
	}
	const uint32_t generation = hand >= 0 && hand <= 1
		? gripPressGeneration[static_cast<size_t>(hand)].load(std::memory_order_acquire) : 0;
	// Preserve where the user released the weapon around the horizontal waist
	// arc and preserve its exact orientation, but always lower it to the same
	// class-specific waist plane as the stable fallback. This replaces the old
	// presentation-only -15 cm offset, which left chest-height drops suspended
	// and was recaptured on the next pickup, accumulating another 15 cm each time.
	glm::fvec3 waistPosition = localPosition;
	glm::fvec3 waistForward = localForward;
	glm::fvec3 waistRight = localRight;
	glm::fvec3 waistUp = localUp;
	const int verticalAxis = ResolveMagneticHolsterVerticalAxis();
	const bool normalizedHolster = NormalizeHolsterPose(IsTwoHandLongGun(), verticalAxis, hand, waistPosition,
		waistForward, waistRight, waistUp);
	magneticIdleLocalPosition = waistPosition;
	magneticIdleLocalForward = waistForward;
	magneticIdleLocalRight = waistRight;
	magneticIdleLocalUp = waistUp;
	magneticIdleAnchorHand = hand;
	magneticIdleAnchorBucket = IsTwoHandLongGun() ? 1 : 0;
	magneticCustomAnchorValid = true;
	magneticCustomAnchorWeaponId = static_cast<int>(currentWeaponEquipped);
	magneticLastHeldPoseHand = hand;
	magneticLastHeldPoseGripGeneration = generation;
	magneticWaistAnchors[static_cast<int>(currentWeaponEquipped)] = {
		waistPosition, waistForward, waistRight, waistUp, hand, generation };
	if (logCapture)
	{
		if (!magneticStableBodyRotationValid)
		{
			magneticStableBodyRotation = bodyRotation;
			magneticStableBodyRotationValid = true;
		}
		magneticStableBodyRotationLastUpdate = GetTickCount64();
		magneticBodyFrameRebaseAt = magneticStableBodyRotationLastUpdate + 900;
	}
	if (logCapture)
		uevr::API::get()->log_info(
			"[MagneticWeapon] release pose captured hand=%s weapon=%d generation=%u axis=%c rawP=(%.1f %.1f %.1f) waistP=(%.1f %.1f %.1f) normalized=%s",
			hand == 0 ? "left" : "right", magneticCustomAnchorWeaponId, generation,
			verticalAxis == 2 ? 'Z' : verticalAxis == 1 ? 'Y' : 'X',
			localPosition.x, localPosition.y, localPosition.z,
			waistPosition.x, waistPosition.y, waistPosition.z,
			normalizedHolster ? "true" : "false");
	return true;
}

void WeaponManager::ConsumeCurrentGripPressGenerations()
{
	for (size_t hand = 0; hand < magneticConsumedGripPressGeneration.size(); ++hand)
		magneticConsumedGripPressGeneration[hand] = gripPressGeneration[hand].load(std::memory_order_acquire);
}

bool WeaponManager::BeginMagneticGrip(int hand, const char* reason)
{
	if (hand != 0 && hand != 1)
		return false;
	const uint32_t acceptedGeneration =
		gripPressGeneration[static_cast<size_t>(hand)].load(std::memory_order_acquire);
	magneticConsumedGripPressGeneration[static_cast<size_t>(hand)] = acceptedGeneration;
	magneticLoggedPendingGripPressGeneration[static_cast<size_t>(hand)] = acceptedGeneration;
	// A retained pose may only come from this exact accepted grip generation.
	// Keep the body anchor itself visible while held, but do not let an older hand
	// or press satisfy the short-release fallback.
	magneticLastHeldPoseHand = -1;
	magneticLastHeldPoseGripGeneration = 0;
	magneticCustomAnchorValid = false;
	magneticCustomAnchorWeaponId = -1;

	glm::fvec3 bodyOrigin{};
	glm::fquat bodyRotation{};
	if (ReadMagneticBodyFrame(bodyOrigin, bodyRotation))
	{
		magneticStableBodyRotation = bodyRotation;
		magneticStableBodyRotationValid = true;
		magneticStableBodyRotationLastUpdate = GetTickCount64();
	}
	magneticBodyFrameRebaseAt = 0;
	// Idle inventory is parented to CJ's root so both stereo eyes receive one
	// coherent transform. Detach with KeepWorld before UEVR takes controller
	// ownership; the original GTA parent remains recorded for later cleanup.
	DetachMagneticIdleWeaponFromBody();
	magneticIdleWeaponActive = false;
	magneticIdleWeaponId = -1;
	magneticGripHand = hand;
	magneticGripWeaponId = static_cast<int>(currentWeaponEquipped);
	magneticGripAttached = false;
	magneticReleaseRequested = false;
	magneticAnchoredWeaponMesh = nullptr;
	magneticIdleDetachFailureLogged = false;
	motionWeaponTrackingEnabled = true;
	visualWeaponTrackingEnabled = true;
	motionConfiguredFirstWeaponMesh = nullptr;
	motionConfiguredFirstHand = -1;
	motionConfiguredFirstCalibrationRole = -1;

	// ProcessMagneticIdleWeapon runs immediately after the normal mesh update.
	// Attach again now so a grip accepted on this frame is usable on this frame,
	// rather than remaining trigger-blocked until the next engine tick.
	UpdateActualWeaponMesh();
	uevr::API::get()->log_info("[MagneticWeapon] picked up hand=%s reason=%s generation=%u attached=%s weapon=%d",
		hand == 0 ? "left" : "right", reason != nullptr ? reason : "edge",
		acceptedGeneration, magneticGripAttached ? "true" : "false",
		static_cast<int>(currentWeaponEquipped));
	return true;
}

void WeaponManager::EnterMagneticIdleSlot(int anchorHand, bool allowSavedPose)
{
	const bool wasIdle = magneticIdleWeaponActive;
	const int currentWeaponId = static_cast<int>(currentWeaponEquipped);
	const bool sameWeaponIdle = wasIdle && magneticIdleWeaponId == currentWeaponId;
	const bool preserveCapturedPose = magneticCustomAnchorValid
		&& magneticCustomAnchorWeaponId == currentWeaponId;
	bool restoredSavedPose = false;
	uint32_t restoredSavedGeneration = 0;
	// Visibility/grace events can repeat the same idle request. Do not tear down
	// and re-anchor a mesh that is already stashed at the requested body-local
	// pose; doing so caused visible relocation and duplicate detach churn.
	if (sameWeaponIdle && allowSavedPose && magneticGripHand < 0
		&& (anchorHand < 0 || anchorHand == magneticIdleAnchorHand || preserveCapturedPose))
	{
		motionWeaponTrackingEnabled = false;
		visualWeaponTrackingEnabled = true;
		magneticTriggerBlockedSnapshot.store(true, std::memory_order_release);
		return;
	}
	if (!preserveCapturedPose)
	{
		const auto saved = magneticWaistAnchors.find(currentWeaponId);
		if (allowSavedPose && saved != magneticWaistAnchors.end())
		{
			magneticIdleLocalPosition = saved->second.position;
			magneticIdleLocalForward = saved->second.forward;
			magneticIdleLocalRight = saved->second.right;
			magneticIdleLocalUp = saved->second.up;
			NormalizeHolsterPose(IsTwoHandLongGun(), ResolveMagneticHolsterVerticalAxis(),
				saved->second.hand, magneticIdleLocalPosition,
				magneticIdleLocalForward, magneticIdleLocalRight, magneticIdleLocalUp);
			saved->second.position = magneticIdleLocalPosition;
			saved->second.forward = magneticIdleLocalForward;
			saved->second.right = magneticIdleLocalRight;
			saved->second.up = magneticIdleLocalUp;
			magneticIdleAnchorHand = saved->second.hand;
			magneticIdleAnchorBucket = IsTwoHandLongGun() ? 1 : 0;
			magneticCustomAnchorValid = true;
			magneticCustomAnchorWeaponId = currentWeaponId;
			magneticLastHeldPoseHand = -1;
			magneticLastHeldPoseGripGeneration = 0;
			restoredSavedGeneration = saved->second.gripGeneration;
			restoredSavedPose = true;
		}
		else
		{
			SetMagneticIdleAnchor(anchorHand);
		}
	}
	glm::fvec3 bodyOrigin{};
	glm::fquat bodyRotation{};
	if (ReadMagneticBodyFrame(bodyOrigin, bodyRotation)
		&& (!magneticStableBodyRotationValid || magneticBodyFrameRebaseAt == 0))
	{
		magneticStableBodyRotation = bodyRotation;
		magneticStableBodyRotationValid = true;
		magneticStableBodyRotationLastUpdate = GetTickCount64();
	}
	// Do not consume grip generations here. A press can arrive while the mesh or
	// body frame is still initializing; the engine thread consumes it only after
	// BeginMagneticGrip accepts that hand.
	magneticIdleWeaponActive = true;
	magneticIdleWeaponId = currentWeaponId;
	magneticGripHand = -1;
	magneticGripWeaponId = -1;
	magneticGripAttached = false;
	magneticReleaseRequested = false;
	motionWeaponTrackingEnabled = false;
	visualWeaponTrackingEnabled = true;
	if (firstWeaponMesh != nullptr && uevr::API::UObjectHook::exists(firstWeaponMesh))
		uevr::API::UObjectHook::remove_motion_controller_state(firstWeaponMesh);
	motionConfiguredFirstWeaponMesh = nullptr;
	motionConfiguredFirstHand = -1;
	motionConfiguredFirstCalibrationRole = -1;
	if (!sameWeaponIdle)
		magneticAnchoredWeaponMesh = nullptr;
	magneticTriggerBlockedSnapshot.store(true, std::memory_order_release);
	if (restoredSavedPose)
		uevr::API::get()->log_info(
			"[MagneticWeapon] restored saved body-local pose weapon=%d generation=%u localP=(%.1f %.1f %.1f)",
			currentWeaponId, restoredSavedGeneration, magneticIdleLocalPosition.x,
			magneticIdleLocalPosition.y, magneticIdleLocalPosition.z);
	if (!wasIdle || settingsManager->debugInputLayerProbe)
		uevr::API::get()->log_info(
			"[MagneticWeapon] stash requested source=%s class=%s hand=%s weapon=%d localP=(%.1f %.1f %.1f)",
			preserveCapturedPose ? "release-pose" : (restoredSavedPose ? "weapon-saved" : "class-fallback"),
			IsTwoHandLongGun() ? "long-gun" : "one-hand",
			magneticIdleAnchorHand == 0 ? "left" : "right",
			currentWeaponId,
			magneticIdleLocalPosition.x, magneticIdleLocalPosition.y, magneticIdleLocalPosition.z);
}

bool WeaponManager::DetachMagneticIdleWeapon()
{
	if (magneticIdleWeaponDetached)
		return magneticDetachedWeaponMesh == firstWeaponMesh
			&& magneticDetachedWeaponMesh != nullptr
			&& uevr::API::UObjectHook::exists(magneticDetachedWeaponMesh);
	if (firstWeaponMesh == nullptr || !uevr::API::UObjectHook::exists(firstWeaponMesh))
		return false;
	auto componentClass = firstWeaponMesh->get_class();
	if (componentClass == nullptr || componentClass->find_function(L"DetachFromParent") == nullptr)
		return false;
	uevr::API::UObject* nativeParent = nullptr;
	if (componentClass->find_property(L"AttachParent") != nullptr)
		nativeParent = firstWeaponMesh->get_property<uevr::API::UObject*>(L"AttachParent");
	if (nativeParent == nullptr || !uevr::API::UObjectHook::exists(nativeParent))
		nativeParent = firstWeaponContainer;
	if (nativeParent == nullptr || !uevr::API::UObjectHook::exists(nativeParent))
		return false;

	uevr::API::FName nativeSocket{};
	if (componentClass->find_property(L"AttachSocketName") != nullptr)
		nativeSocket = firstWeaponMesh->get_property<uevr::API::FName>(L"AttachSocketName");
	Utilities::ParameterDetachFromParent detachParams{};
	detachParams.maintainWorldPosition = true;
	detachParams.callModify = false;
	firstWeaponMesh->call_function(L"DetachFromParent", &detachParams);

	magneticDetachedWeaponMesh = firstWeaponMesh;
	magneticIdleNativeParent = nativeParent;
	magneticDetachedPlayerCharacter = playerManager != nullptr ? playerManager->playerCharacter : nullptr;
	magneticIdleNativeSocket = nativeSocket;
	magneticIdleWeaponDetached = true;
	uevr::API::get()->log_info(
		"[MagneticWeapon] detached for controller/body presentation mesh=%p parent=%p socket=%d",
		magneticDetachedWeaponMesh, magneticIdleNativeParent, magneticIdleNativeSocket.comparison_index);
	return true;
}

bool WeaponManager::AttachMagneticIdleWeaponToBody()
{
	if (!magneticIdleWeaponDetached || firstWeaponMesh == nullptr
		|| magneticDetachedWeaponMesh != firstWeaponMesh
		|| !uevr::API::UObjectHook::exists(firstWeaponMesh))
		return false;
	if (magneticIdleWeaponBodyAttached && magneticBodyAnchorParent != nullptr
		&& uevr::API::UObjectHook::exists(magneticBodyAnchorParent))
		return true;

	auto actor = playerManager != nullptr && playerManager->playerActor != nullptr
		? playerManager->playerActor
		: (playerManager != nullptr ? playerManager->playerCharacter : nullptr);
	if (actor == nullptr || !uevr::API::UObjectHook::exists(actor)
		|| actor->get_class() == nullptr
		|| actor->get_class()->find_property(L"RootComponent") == nullptr)
		return false;
	auto rootComponent = actor->get_property<uevr::API::UObject*>(L"RootComponent");
	if (rootComponent == nullptr || !uevr::API::UObjectHook::exists(rootComponent))
		return false;

	auto componentClass = firstWeaponMesh->get_class();
	if (componentClass == nullptr)
		return false;
	auto attachFunction = componentClass->find_function(L"AttachToComponent");
	if (attachFunction == nullptr)
		attachFunction = componentClass->find_function(L"K2_AttachToComponent");
	if (attachFunction == nullptr)
		return false;

	std::vector<uint8_t> params(attachFunction->get_properties_size());
	const uevr::API::FName noSocket{};
	// EAttachmentRule::KeepWorld is 1 in UE 4.26.
	const bool complete = SetReflectedObjectParameter(attachFunction, params, L"Parent", rootComponent)
		&& SetReflectedFNameParameter(attachFunction, params, L"SocketName", noSocket)
		&& SetReflectedByteParameter(attachFunction, params, L"LocationRule", 1)
		&& SetReflectedByteParameter(attachFunction, params, L"RotationRule", 1)
		&& SetReflectedByteParameter(attachFunction, params, L"ScaleRule", 1);
	SetReflectedBoolParameter(attachFunction, params, L"bWeldSimulatedBodies", false);
	SetReflectedBoolParameter(attachFunction, params, L"WeldSimulatedBodies", false);
	if (!complete)
		return false;
	attachFunction->call(firstWeaponMesh, params.data());
	magneticBodyAnchorParent = rootComponent;
	magneticIdleWeaponBodyAttached = true;
	uevr::API::get()->log_info(
		"[MagneticWeapon] body-root attached keep-world mesh=%p root=%p weapon=%d",
		firstWeaponMesh, rootComponent, static_cast<int>(currentWeaponEquipped));
	return true;
}

void WeaponManager::DetachMagneticIdleWeaponFromBody()
{
	if (!magneticIdleWeaponBodyAttached)
		return;
	auto weaponMesh = magneticDetachedWeaponMesh;
	if (weaponMesh != nullptr && uevr::API::UObjectHook::exists(weaponMesh)
		&& weaponMesh->get_class() != nullptr
		&& weaponMesh->get_class()->find_function(L"DetachFromParent") != nullptr)
	{
		Utilities::ParameterDetachFromParent detachParams{};
		detachParams.maintainWorldPosition = true;
		detachParams.callModify = false;
		weaponMesh->call_function(L"DetachFromParent", &detachParams);
	}
	magneticIdleWeaponBodyAttached = false;
	magneticBodyAnchorParent = nullptr;
}

void WeaponManager::RestoreMagneticIdleWeaponAttachment(const char* reason)
{
	if (!magneticIdleWeaponDetached)
		return;
	auto weaponMesh = magneticDetachedWeaponMesh;
	auto nativeParent = magneticIdleNativeParent;
	const auto nativeSocket = magneticIdleNativeSocket;
	const auto clearDetachedOwnership = [this]()
	{
		magneticIdleWeaponDetached = false;
		magneticDetachedWeaponMesh = nullptr;
		magneticIdleNativeParent = nullptr;
		magneticBodyAnchorParent = nullptr;
		magneticDetachedPlayerCharacter = nullptr;
		magneticIdleNativeSocket = {};
		magneticIdleWeaponBodyAttached = false;
	};
	if (weaponMesh == nullptr || nativeParent == nullptr
		|| !uevr::API::UObjectHook::exists(weaponMesh)
		|| !uevr::API::UObjectHook::exists(nativeParent))
	{
		clearDetachedOwnership();
		// GTA can destroy and replace its weapon component during a weapon/vehicle
		// transition. Expiring that generation is normal; never retry the stale
		// UObject or report it as a failed restore.
		uevr::API::get()->log_info("[MagneticWeapon] detached generation expired reason=%s; stale ownership discarded",
			reason != nullptr ? reason : "cleanup");
		return;
	}
	auto componentClass = weaponMesh->get_class();
	if (componentClass == nullptr)
		return;
	DetachMagneticIdleWeaponFromBody();
	auto attachFunction = componentClass->find_function(L"AttachToComponent");
	if (attachFunction == nullptr)
		attachFunction = componentClass->find_function(L"K2_AttachToComponent");
	if (attachFunction == nullptr)
	{
		uevr::API::get()->log_warn("[MagneticWeapon] native attachment restore failed reason=%s no-attach-function",
			reason != nullptr ? reason : "cleanup");
		return;
	}
	std::vector<uint8_t> params(attachFunction->get_properties_size());
	const bool complete = SetReflectedObjectParameter(attachFunction, params, L"Parent", nativeParent)
		&& SetReflectedFNameParameter(attachFunction, params, L"SocketName", nativeSocket)
		&& SetReflectedByteParameter(attachFunction, params, L"LocationRule", 0)
		&& SetReflectedByteParameter(attachFunction, params, L"RotationRule", 0)
		&& SetReflectedByteParameter(attachFunction, params, L"ScaleRule", 0);
	SetReflectedBoolParameter(attachFunction, params, L"bWeldSimulatedBodies", false);
	SetReflectedBoolParameter(attachFunction, params, L"WeldSimulatedBodies", false);
	if (!complete)
	{
		uevr::API::get()->log_warn("[MagneticWeapon] native attachment restore failed reason=%s parameters",
			reason != nullptr ? reason : "cleanup");
		return;
	}
	attachFunction->call(weaponMesh, params.data());
	Utilities::Parameter_K2_SetWorldOrRelativeLocation locationParams{};
	locationParams.newLocation = glm::fvec3(0.0f);
	locationParams.bSweep = false;
	locationParams.bTeleport = true;
	if (componentClass->find_function(L"K2_SetRelativeLocation") != nullptr)
		weaponMesh->call_function(L"K2_SetRelativeLocation", &locationParams);
	Utilities::Parameter_K2_SetWorldOrRelativeRotation rotationParams{};
	rotationParams.newRotation = { 0.0f, 0.0f, 0.0f };
	rotationParams.bSweep = false;
	rotationParams.bTeleport = true;
	if (componentClass->find_function(L"K2_SetRelativeRotation") != nullptr)
		weaponMesh->call_function(L"K2_SetRelativeRotation", &rotationParams);
	clearDetachedOwnership();
	uevr::API::get()->log_info("[MagneticWeapon] restored native attachment reason=%s mesh=%p parent=%p",
		reason != nullptr ? reason : "cleanup", weaponMesh, nativeParent);
}

void WeaponManager::SuspendMagneticIdleSlot()
{
	const bool ownedWeaponPresentation = magneticIdleWeaponActive || magneticGripHand >= 0;
	magneticIdleWeaponActive = false;
	magneticIdleWeaponId = -1;
	magneticGripHand = -1;
	magneticGripWeaponId = -1;
	magneticIdleAnchorHand = -1;
	magneticIdleAnchorBucket = -1;
	magneticGripAttached = false;
	magneticReleaseRequested = false;
	magneticAnchoredWeaponMesh = nullptr;
	magneticIdleDetachFailureLogged = false;
	magneticStableBodyRotationValid = false;
	magneticStableBodyRotationLastUpdate = 0;
	magneticBodyFrameRebaseAt = 0;
	magneticLastHeldPoseHand = -1;
	magneticLastHeldPoseGripGeneration = 0;
	magneticTriggerBlockedSnapshot.store(false, std::memory_order_release);
	if (ownedWeaponPresentation)
	{
		motionWeaponTrackingEnabled = false;
		visualWeaponTrackingEnabled = false;
		// An anchored mesh has no motion state, so the ordinary unhook helper
		// would return before undoing our direct world-transform writes.
		if (firstWeaponMesh != nullptr && uevr::API::UObjectHook::exists(firstWeaponMesh))
		{
			uevr::API::UObjectHook::remove_motion_controller_state(firstWeaponMesh);
			auto componentClass = firstWeaponMesh->get_class();
			if (componentClass != nullptr)
			{
				Utilities::Parameter_K2_SetWorldOrRelativeLocation locationParams{};
				locationParams.newLocation = glm::fvec3(0.0f);
				locationParams.bSweep = false;
				locationParams.bTeleport = true;
				if (componentClass->find_function(L"K2_SetRelativeLocation") != nullptr)
					firstWeaponMesh->call_function(L"K2_SetRelativeLocation", &locationParams);

				Utilities::Parameter_K2_SetWorldOrRelativeRotation rotationParams{};
				rotationParams.newRotation = { 0.0f, 0.0f, 0.0f };
				rotationParams.bSweep = false;
				rotationParams.bTeleport = true;
				if (componentClass->find_function(L"K2_SetRelativeRotation") != nullptr)
					firstWeaponMesh->call_function(L"K2_SetRelativeRotation", &rotationParams);
			}
		}
		motionConfiguredFirstWeaponMesh = nullptr;
		motionConfiguredFirstHand = -1;
		motionConfiguredFirstCalibrationRole = -1;
	}
	RestoreMagneticIdleWeaponAttachment("suspend");
}

void WeaponManager::ApplyMagneticIdlePose()
{
	if (!magneticIdleWeaponActive || firstWeaponMesh == nullptr
		|| !uevr::API::UObjectHook::exists(firstWeaponMesh))
		return;
	const bool bodyAnchorAttached = magneticIdleWeaponBodyAttached
		&& magneticDetachedWeaponMesh == firstWeaponMesh
		&& magneticBodyAnchorParent != nullptr
		&& uevr::API::UObjectHook::exists(magneticBodyAnchorParent);
	if (bodyAnchorAttached && magneticBodyFrameRebaseAt == 0)
		return;
	glm::fvec3 bodyOrigin{};
	glm::fquat rawBodyRotation{};
	if (!ReadMagneticBodyFrame(bodyOrigin, rawBodyRotation))
		return;
	glm::fquat bodyRotation = rawBodyRotation;
	const uint64_t now = GetTickCount64();
	if (!magneticStableBodyRotationValid
		|| !IsFiniteQuaternion(magneticStableBodyRotation)
		|| glm::length(magneticStableBodyRotation) <= 0.5f)
	{
		magneticStableBodyRotation = rawBodyRotation;
		magneticStableBodyRotationValid = true;
		magneticStableBodyRotationLastUpdate = now;
	}
	else if (magneticBodyFrameRebaseAt != 0 && now < magneticBodyFrameRebaseAt)
	{
		// Keep the exact release basis while GTA settles its native aim/stance.
		bodyRotation = magneticStableBodyRotation;
	}
	else if (magneticBodyFrameRebaseAt != 0)
	{
		// Rebase once to the settled actor frame without changing the current
		// world pose or any of the saved weapon orientation axes.
		const glm::fvec3 worldPosition = bodyOrigin
			+ magneticStableBodyRotation * magneticIdleLocalPosition;
		const glm::fvec3 worldForward = magneticStableBodyRotation * magneticIdleLocalForward;
		const glm::fvec3 worldRight = magneticStableBodyRotation * magneticIdleLocalRight;
		const glm::fvec3 worldUp = magneticStableBodyRotation * magneticIdleLocalUp;
		const glm::fquat inverseRawBodyRotation = glm::inverse(rawBodyRotation);
		const glm::fvec3 rebasedPosition = inverseRawBodyRotation
			* (worldPosition - bodyOrigin);
		const glm::fvec3 rebasedForward = NormalizeOrZero(inverseRawBodyRotation * worldForward);
		const glm::fvec3 rebasedRight = NormalizeOrZero(inverseRawBodyRotation * worldRight);
		const glm::fvec3 rebasedUp = NormalizeOrZero(inverseRawBodyRotation * worldUp);
		if (!IsFiniteVector(worldPosition) || !IsFiniteVector(worldForward)
			|| !IsFiniteVector(worldRight) || !IsFiniteVector(worldUp)
			|| !IsFiniteVector(rebasedPosition) || !IsFiniteVector(rebasedForward)
			|| !IsFiniteVector(rebasedRight) || !IsFiniteVector(rebasedUp)
			|| glm::length(rebasedForward) < 0.5f || glm::length(rebasedRight) < 0.5f
			|| glm::length(rebasedUp) < 0.5f)
			return;
		magneticIdleLocalPosition = rebasedPosition;
		magneticIdleLocalForward = rebasedForward;
		magneticIdleLocalRight = rebasedRight;
		magneticIdleLocalUp = rebasedUp;
		magneticStableBodyRotation = rawBodyRotation;
		magneticStableBodyRotationLastUpdate = now;
		magneticBodyFrameRebaseAt = 0;
		bodyRotation = rawBodyRotation;
		uevr::API::get()->log_info(
			"[MagneticWeapon] body-frame rebase weapon=%d source=%s",
			static_cast<int>(currentWeaponEquipped),
			magneticCustomAnchorValid ? "weapon-pose" : "class-fallback");
	}
	else
	{
		magneticStableBodyRotation = rawBodyRotation;
		magneticStableBodyRotationLastUpdate = now;
	}
	auto componentClass = firstWeaponMesh->get_class();
	if (componentClass == nullptr
		|| componentClass->find_function(L"K2_SetWorldLocation") == nullptr
		|| componentClass->find_function(L"K2_SetWorldRotation") == nullptr)
		return;
	if (Utilities::KismetMathLibrary == nullptr)
		Utilities::InitHelperClasses();
	if (Utilities::KismetMathLibrary == nullptr
		|| Utilities::KismetMathLibrary->get_class() == nullptr
		|| Utilities::KismetMathLibrary->get_class()->find_function(L"MakeRotationFromAxes") == nullptr)
		return;
	// If GTA replaced the component after a weapon/character transition, restore
	// or discard the old generation before adopting the newly discovered mesh.
	// Never let DetachMagneticIdleWeapon compare against and repeatedly reject a
	// stale component across frames.
	if (magneticIdleWeaponDetached && magneticDetachedWeaponMesh != firstWeaponMesh)
		RestoreMagneticIdleWeaponAttachment("component-replacement");
	const bool newlyAnchoring = magneticAnchoredWeaponMesh != firstWeaponMesh;
	if (newlyAnchoring)
	{
		uevr::API::UObjectHook::remove_motion_controller_state(firstWeaponMesh);
		motionConfiguredFirstWeaponMesh = nullptr;
		motionConfiguredFirstHand = -1;
		motionConfiguredFirstCalibrationRole = -1;
		if (!DetachMagneticIdleWeapon())
		{
			if (!magneticIdleDetachFailureLogged)
			{
				magneticIdleDetachFailureLogged = true;
				uevr::API::get()->log_warn("[MagneticWeapon] stash anchor rejected: native detach unavailable mesh=%p",
					firstWeaponMesh);
			}
			return;
		}
		magneticIdleDetachFailureLogged = false;
		magneticAnchoredWeaponMesh = firstWeaponMesh;
	}

	const glm::fvec3 worldPosition = bodyOrigin
		+ bodyRotation * magneticIdleLocalPosition;
	const glm::fvec3 worldForward = NormalizeOrZero(bodyRotation * magneticIdleLocalForward);
	const glm::fvec3 worldRight = NormalizeOrZero(bodyRotation * magneticIdleLocalRight);
	const glm::fvec3 worldUp = NormalizeOrZero(bodyRotation * magneticIdleLocalUp);
	if (!IsFiniteVector(worldPosition) || !IsFiniteVector(worldForward)
		|| !IsFiniteVector(worldRight) || !IsFiniteVector(worldUp))
		return;
	Utilities::Parameter_K2_SetWorldOrRelativeLocation locationParams{};
	locationParams.newLocation = worldPosition;
	locationParams.bSweep = false;
	locationParams.bTeleport = true;
	firstWeaponMesh->call_function(L"K2_SetWorldLocation", &locationParams);
	ParameterMakeRotationFromAxes makeRotationParams{};
	makeRotationParams.forward = worldForward;
	makeRotationParams.right = worldRight;
	makeRotationParams.up = worldUp;
	Utilities::KismetMathLibrary->call_function(L"MakeRotationFromAxes", &makeRotationParams);
	Utilities::Parameter_K2_SetWorldOrRelativeRotation rotationParams{};
	rotationParams.newRotation = makeRotationParams.returnValue;
	rotationParams.bSweep = false;
	rotationParams.bTeleport = true;
	firstWeaponMesh->call_function(L"K2_SetWorldRotation", &rotationParams);
	if (magneticBodyFrameRebaseAt != 0 && now < magneticBodyFrameRebaseAt)
	{
		if (newlyAnchoring)
			uevr::API::get()->log_info(
				"[MagneticWeapon] stash applied source=%s hand=%s weapon=%d phase=body-settle",
				magneticCustomAnchorValid ? "weapon-pose" : "class-fallback",
				magneticIdleAnchorHand == 0 ? "left" : "right",
				static_cast<int>(currentWeaponEquipped));
		return;
	}
	if (!AttachMagneticIdleWeaponToBody())
	{
		if (!magneticIdleDetachFailureLogged)
		{
			magneticIdleDetachFailureLogged = true;
			uevr::API::get()->log_warn(
				"[MagneticWeapon] body-root attachment unavailable; stash left at transition pose mesh=%p",
				firstWeaponMesh);
		}
		return;
	}
	if (newlyAnchoring)
		uevr::API::get()->log_info(
			"[MagneticWeapon] stash applied source=%s hand=%s weapon=%d phase=body-attached",
			magneticCustomAnchorValid ? "weapon-pose" : "class-fallback",
			magneticIdleAnchorHand == 0 ? "left" : "right",
			static_cast<int>(currentWeaponEquipped));

}

void WeaponManager::ProcessMagneticIdleWeapon()
{
	if (IsGripCalibrationActive())
	{
		RefreshRuntimeHandRoles("magnetic-calibration");
		return;
	}
	const uint8_t currentMask = gripStateMask.load(std::memory_order_acquire);
	if (explicitWeaponCyclePending)
	{
		const bool validReplacement = currentWeaponEquipped != explicitWeaponCycleSourceWeaponId
			&& firstWeaponMesh != nullptr && uevr::API::UObjectHook::exists(firstWeaponMesh)
			&& firstWeaponStaticMesh != nullptr && uevr::API::UObjectHook::exists(firstWeaponStaticMesh);
		if (validReplacement)
		{
			const int previousWeaponId = explicitWeaponCycleSourceWeaponId;
			const int restoreHand = explicitWeaponCycleRestoreAnchorHand;
			explicitWeaponCyclePending = false;
			explicitWeaponCycleSourceWeaponId = -1;
			explicitWeaponCycleDeadline = 0;
			// A native weapon replacement must never inherit or write the prior
			// weapon's release pose. EnterMagneticIdleSlot selects this weapon's
			// own saved entry or its class fallback below.
			magneticCustomAnchorValid = false;
			magneticCustomAnchorWeaponId = -1;
			magneticLastHeldPoseHand = -1;
			magneticLastHeldPoseGripGeneration = 0;
			magneticBodyFrameRebaseAt = 0;
			ConsumeCurrentGripPressGenerations();
			magneticProcessedGripMask = currentMask;
			if (IsMagneticIdleSlotEligible())
			{
				EnterMagneticIdleSlot(restoreHand);
			}
			else
				magneticTriggerBlockedSnapshot.store(false, std::memory_order_release);
			uevr::API::get()->log_info(
				"[MagneticWeapon] explicit cycle completed previousWeapon=%d currentWeapon=%d presentation=%s",
				previousWeaponId, static_cast<int>(currentWeaponEquipped),
				magneticIdleWeaponActive ? "stashed" : "native");
			RefreshRuntimeHandRoles("explicit-weapon-cycle-complete");
			return;
		}

		const bool invalidContext = playerManager == nullptr || !playerManager->isInControl
			|| playerManager->isInVehicle || playerManager->weaponWheelEnabled;
		const bool timedOut = GetTickCount64() >= explicitWeaponCycleDeadline;
		if (!invalidContext && !timedOut)
		{
			magneticTriggerBlockedSnapshot.store(true, std::memory_order_release);
			RefreshRuntimeHandRoles("explicit-weapon-cycle-wait");
			return;
		}

		const int restoreHand = explicitWeaponCycleRestoreAnchorHand;
		explicitWeaponCyclePending = false;
		explicitWeaponCycleSourceWeaponId = -1;
		explicitWeaponCycleDeadline = 0;
		magneticCustomAnchorValid = false;
		magneticCustomAnchorWeaponId = -1;
		magneticLastHeldPoseHand = -1;
		magneticLastHeldPoseGripGeneration = 0;
		magneticBodyFrameRebaseAt = 0;
		if (!invalidContext && IsMagneticIdleSlotEligible())
		{
			EnterMagneticIdleSlot(restoreHand);
			uevr::API::get()->log_info(
				"[MagneticWeapon] explicit cycle timed out; selected weapon-local anchor weapon=%d",
				static_cast<int>(currentWeaponEquipped));
		}
		else
		{
			magneticCustomAnchorValid = false;
			magneticCustomAnchorWeaponId = -1;
			magneticTriggerBlockedSnapshot.store(false, std::memory_order_release);
			uevr::API::get()->log_info("[MagneticWeapon] explicit cycle cancelled reason=%s",
				invalidContext ? "context-loss" : "ineligible-weapon");
		}
		RefreshRuntimeHandRoles("explicit-weapon-cycle-end");
		return;
	}
	if (playerManager->isInVehicle)
	{
		// Vehicle presentation is not the on-foot magnetic stash state. Do not
		// call SuspendMagneticIdleSlot here: it removes the controller state that
		// the bounded vehicle-pistol path just installed.
		RestoreMagneticIdleWeaponAttachment("vehicle-enter");
		magneticCustomAnchorValid = false;
		magneticCustomAnchorWeaponId = -1;
		magneticLastHeldPoseHand = -1;
		magneticLastHeldPoseGripGeneration = 0;
		magneticIdleWeaponActive = false;
		magneticIdleWeaponId = -1;
		magneticGripHand = -1;
		magneticGripWeaponId = -1;
		magneticGripAttached = false;
		magneticReleaseRequested = false;
		magneticAnchoredWeaponMesh = nullptr;
		magneticStableBodyRotationValid = false;
		magneticStableBodyRotationLastUpdate = 0;
		magneticBodyFrameRebaseAt = 0;
		magneticProcessedGripMask = currentMask;
		ConsumeCurrentGripPressGenerations();
		magneticTriggerBlockedSnapshot.store(false, std::memory_order_release);
		RefreshRuntimeHandRoles("magnetic-vehicle");
		return;
	}
	const uint8_t releasedMask = static_cast<uint8_t>(magneticProcessedGripMask & ~currentMask);
	magneticProcessedGripMask = currentMask;
	uint8_t pendingPressMask = 0;
	for (int hand = 0; hand < 2; ++hand)
	{
		const uint8_t bit = static_cast<uint8_t>(1U << hand);
		const uint32_t generation = gripPressGeneration[static_cast<size_t>(hand)].load(std::memory_order_acquire);
		if ((currentMask & bit) != 0
			&& generation != magneticConsumedGripPressGeneration[static_cast<size_t>(hand)])
			pendingPressMask = static_cast<uint8_t>(pendingPressMask | bit);
	}

	if (!IsMagneticIdleSlotEligible())
	{
		const bool hardExit = settingsManager == nullptr || !settingsManager->enableFreeAimWeaponHands
			|| !playerManager->isInControl || playerManager->isInVehicle
			|| playerManager->weaponWheelEnabled
			|| cameraController->currentCameraMode == CameraController::Camera
			|| secondWeaponMesh != nullptr
			|| !((currentWeaponEquipped >= BrassKnuckles && currentWeaponEquipped <= Cane)
				|| (currentWeaponEquipped >= Pistol && currentWeaponEquipped <= Minigun)
				|| IsControllerHeldUtility());
		if (hardExit)
		{
			ConsumeCurrentGripPressGenerations();
			magneticCustomAnchorValid = false;
			magneticCustomAnchorWeaponId = -1;
			magneticLastHeldPoseHand = -1;
			magneticLastHeldPoseGripGeneration = 0;
		}
		else
		{
			for (int hand = 0; hand < 2; ++hand)
			{
				const uint8_t bit = static_cast<uint8_t>(1U << hand);
				if ((pendingPressMask & bit) == 0)
					continue;
				const size_t handIndex = static_cast<size_t>(hand);
				const uint32_t generation = gripPressGeneration[handIndex].load(std::memory_order_acquire);
				if (magneticLoggedPendingGripPressGeneration[handIndex] == generation)
					continue;
				magneticLoggedPendingGripPressGeneration[handIndex] = generation;
				uevr::API::get()->log_info(
					"[MagneticWeapon] grip pending hand=%s generation=%u reason=weapon-component-not-ready",
					hand == 0 ? "left" : "right", generation);
			}
		}
		SuspendMagneticIdleSlot();
		RefreshRuntimeHandRoles("magnetic-ineligible");
		return;
	}
	if (magneticIdleWeaponActive
		&& magneticIdleWeaponId != static_cast<int>(currentWeaponEquipped))
		EnterMagneticIdleSlot(magneticIdleAnchorHand);
	if (magneticGripHand >= 0
		&& magneticGripWeaponId != static_cast<int>(currentWeaponEquipped))
	{
		const int previousHand = magneticGripHand;
		const int previousWeaponId = magneticGripWeaponId;
		// Never transfer controller ownership from one GTA weapon component to
		// another while the same physical grip remains held. Restore the old
		// generation, consume this edge, and require a clean release/re-press.
		SuspendMagneticIdleSlot();
		magneticCustomAnchorValid = false;
		magneticCustomAnchorWeaponId = -1;
		magneticLastHeldPoseHand = -1;
		magneticLastHeldPoseGripGeneration = 0;
		ConsumeCurrentGripPressGenerations();
		magneticProcessedGripMask = currentMask;
		uevr::API::get()->log_info(
			"[MagneticWeapon] reset reason=weapon-change-held previousWeapon=%d currentWeapon=%d previousHand=%s; fresh grip required",
			previousWeaponId, static_cast<int>(currentWeaponEquipped),
			previousHand == 0 ? "left" : "right");
		RefreshRuntimeHandRoles("magnetic-weapon-change");
		return;
	}

	// A weapon mesh can appear after the Lua visibility event that announced the
	// grip. Initialize from the authoritative grip mask for every eligible weapon,
	// so on-foot pickup works before any vehicle transition has occurred.
	if (magneticGripHand < 0 && !magneticIdleWeaponActive
		&& currentMask != 0 && pendingPressMask != 0)
	{
		int selectedHand = -1;
		if ((pendingPressMask & 1U) != 0 && (pendingPressMask & 2U) == 0)
			selectedHand = 0;
		else if ((pendingPressMask & 2U) != 0 && (pendingPressMask & 1U) == 0)
			selectedHand = 1;
		else
			selectedHand = twoHandFirstGripHandSnapshot.load(std::memory_order_acquire);

		if (selectedHand < 0)
			selectedHand = settingsManager->leftHandedMode != SettingsManager::Disabled ? 0 : 1;
		BeginMagneticGrip(selectedHand, "late-initialize");
	}
	if (magneticGripHand < 0 && !magneticIdleWeaponActive && currentMask == 0)
	{
		EnterMagneticIdleSlot();
	}
	if (magneticCustomAnchorValid
		&& magneticCustomAnchorWeaponId != static_cast<int>(currentWeaponEquipped))
		SetMagneticIdleAnchor(magneticIdleAnchorHand);
	if (magneticIdleWeaponActive && magneticIdleAnchorHand >= 0
		&& magneticIdleAnchorBucket != (IsTwoHandLongGun() ? 1 : 0))
		SetMagneticIdleAnchor(magneticIdleAnchorHand);
	const bool idleAtFrameStart = magneticIdleWeaponActive;
	if (idleAtFrameStart && magneticIdleWeaponActive && pendingPressMask != 0)
	{
		int selectedHand = -1;
		if (pendingPressMask == 1U)
			selectedHand = 0;
		else if (pendingPressMask == 2U)
			selectedHand = 1;
		else
		{
			const int firstGripHand = twoHandFirstGripHandSnapshot.load(std::memory_order_acquire);
			selectedHand = firstGripHand >= 0 ? firstGripHand
				: (settingsManager->leftHandedMode != SettingsManager::Disabled ? 0 : 1);
		}
		BeginMagneticGrip(selectedHand, "press-edge");
	}

	if (magneticGripHand >= 0)
	{
		const uint8_t handBit = static_cast<uint8_t>(1U << magneticGripHand);
		const bool released = (releasedMask & handBit) != 0
			|| (magneticReleaseRequested && (currentMask & handBit) == 0);
		if (released)
		{
			const int releaseHand = magneticGripHand;
			const uint32_t releaseGeneration = releaseHand >= 0 && releaseHand <= 1
				? gripPressGeneration[static_cast<size_t>(releaseHand)].load(std::memory_order_acquire) : 0;
			const bool capturedReleasePose = CaptureMagneticReleaseAnchor(releaseHand);
			if (!capturedReleasePose)
			{
				// A rejected final sample must not reuse a near-hand pose captured while
				// the weapon was still held. Fall back for this release; the persistent
				// per-weapon map remains available on a later, deliberate reacquire.
				magneticCustomAnchorValid = false;
				magneticCustomAnchorWeaponId = -1;
				magneticLastHeldPoseHand = -1;
				magneticLastHeldPoseGripGeneration = 0;
			}
			const bool hasGenerationMatchedPose = capturedReleasePose;
			if (hasGenerationMatchedPose && magneticBodyFrameRebaseAt == 0)
			{
				const bool stableBodyValid = magneticStableBodyRotationValid
					&& IsFiniteQuaternion(magneticStableBodyRotation)
					&& glm::length(magneticStableBodyRotation) > 0.5f;
				if (!stableBodyValid)
				{
					glm::fvec3 bodyOrigin{};
					glm::fquat bodyRotation{};
					if (ReadMagneticBodyFrame(bodyOrigin, bodyRotation))
					{
						magneticStableBodyRotation = bodyRotation;
						magneticStableBodyRotationValid = true;
						magneticStableBodyRotationLastUpdate = GetTickCount64();
					}
				}
				if (magneticStableBodyRotationValid
					&& IsFiniteQuaternion(magneticStableBodyRotation)
					&& glm::length(magneticStableBodyRotation) > 0.5f)
					magneticBodyFrameRebaseAt = GetTickCount64() + 900;
			}
			if (hasGenerationMatchedPose && !SaveMagneticWaistAnchors())
				uevr::API::get()->log_warn(
					"[MagneticWeapon] release anchor retained in memory but persistence failed weapon=%d",
					static_cast<int>(currentWeaponEquipped));
			EnterMagneticIdleSlot(releaseHand, hasGenerationMatchedPose);
			const char* releaseSource = capturedReleasePose ? "release-pose" : "class-fallback";
			uevr::API::get()->log_info("[MagneticWeapon] released hand=%s weapon=%d generation=%u source=%s",
				releaseHand == 0 ? "left" : "right",
				static_cast<int>(currentWeaponEquipped), releaseGeneration, releaseSource);
		}
	}

	if (magneticIdleWeaponActive)
		ApplyMagneticIdlePose();
	magneticTriggerBlockedSnapshot.store(
		magneticIdleWeaponActive || (magneticGripHand >= 0 && !magneticGripAttached),
		std::memory_order_release);
	RefreshRuntimeHandRoles("magnetic-update");
}

bool WeaponManager::ReadMotionMeleeControllerWorldPose(int controllerHand,
	const glm::fvec3& controllerPosition, const glm::fquat& controllerRotation,
	glm::fvec3& worldPosition, glm::fquat& worldRotation) const
{
	worldPosition = glm::fvec3(0.0f);
	worldRotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	if ((controllerHand != 0 && controllerHand != 1)
		|| !IsFiniteVector(controllerPosition) || !IsFiniteQuaternion(controllerRotation)
		|| glm::length(controllerRotation) <= 0.5f
		|| playerManager == nullptr || cameraController == nullptr
		|| !twoHandViewRotationValid.load(std::memory_order_acquire))
		return false;

	const auto hmdIndex = uevr::API::VR::get_hmd_index();
	if (hmdIndex < 0)
		return false;
	const auto hmdPose = uevr::API::VR::get_pose(hmdIndex);
	const glm::fvec3 hmdPosition(
		hmdPose.position.x, hmdPose.position.y, hmdPose.position.z);
	if (!IsFiniteVector(hmdPosition))
		return false;

	const UEVR_Rotatorf viewRotation = {
		twoHandViewPitch.load(std::memory_order_relaxed),
		twoHandViewYaw.load(std::memory_order_relaxed),
		twoHandViewRoll.load(std::memory_order_relaxed)
	};
	const glm::fquat viewInverse = FlattenViewInverse(viewRotation);
	const auto vrRotationOffsetUevr = uevr::API::VR::get_rotation_offset();
	const glm::fquat vrRotationOffset = glm::normalize(glm::fquat::wxyz(
		vrRotationOffsetUevr.w, vrRotationOffsetUevr.x,
		vrRotationOffsetUevr.y, vrRotationOffsetUevr.z));
	if (!IsFiniteQuaternion(viewInverse) || glm::length(viewInverse) <= 0.5f
		|| !IsFiniteQuaternion(vrRotationOffset) || glm::length(vrRotationOffset) <= 0.5f)
		return false;

	// Controller poses are in the same tracking space used by the existing
	// two-hand solver. Remove the HMD translation, then use that solver's exact
	// view/rotation-offset/converter basis to place the short contact segment in
	// the native UE world. This is a bounded fallback; it never scans objects.
	const glm::fvec3 relativeTrackingPosition = controllerPosition - hmdPosition;
	const glm::fvec3 relativeComponentPosition = VRSpaceToComponentSpace(
		viewInverse * (vrRotationOffset * relativeTrackingPosition));
	if (!IsFiniteVector(relativeComponentPosition))
		return false;

	glm::fvec3 worldOrigin = playerManager->actualPlayerHeadPositionUE;
	if (!IsFiniteVector(worldOrigin) || glm::length(worldOrigin) <= 0.001f)
		worldOrigin = playerManager->actualPlayerPositionUE;
	if ((!IsFiniteVector(worldOrigin) || glm::length(worldOrigin) <= 0.001f)
		&& IsFiniteVector(cameraController->cameraPositionUE))
		worldOrigin = cameraController->cameraPositionUE;
	if (!IsFiniteVector(worldOrigin))
		return false;

	worldPosition = worldOrigin + relativeComponentPosition * 100.0f;
	worldRotation = VRQuaternionToComponentSpace(glm::normalize(
		viewInverse * vrRotationOffset * glm::normalize(controllerRotation)));
	return IsFiniteVector(worldPosition) && IsFiniteQuaternion(worldRotation)
		&& glm::length(worldRotation) > 0.5f;
}

bool WeaponManager::ReadMotionMeleeHandWorldPose(int controllerHand,
	const glm::fvec3& controllerPosition, const glm::fquat& controllerRotation,
	glm::fvec3& worldPosition, glm::fquat& worldRotation,
	bool& usedVisibleBone) const
{
	worldPosition = glm::fvec3(0.0f);
	worldRotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	usedVisibleBone = false;
	if (controllerHand != 0 && controllerHand != 1
		|| !IsFiniteVector(controllerPosition) || !IsFiniteQuaternion(controllerRotation)
		|| glm::length(controllerRotation) <= 0.5f)
		return false;

	const auto handComponent = controllerHand == 0
		? freeAimFakeLeftHand : freeAimFakeRightHand;
	const auto& handBone = controllerHand == 0
		? freeAimFakeLeftHandBoneName : freeAimFakeRightHandBoneName;
	if (handComponent != nullptr && handBone.comparison_index != 0
		&& uevr::API::UObjectHook::exists(handComponent))
	{
		usedVisibleBone = ReadBoneWorldTransform(
			handComponent, handBone, worldPosition, worldRotation);
	}
	if (!usedVisibleBone)
	{
		usedVisibleBone = false;
		if (!ReadMotionMeleeControllerWorldPose(controllerHand,
			controllerPosition, controllerRotation, worldPosition, worldRotation))
			return false;
	}
	return IsFiniteVector(worldPosition) && IsFiniteQuaternion(worldRotation)
		&& glm::length(worldRotation) > 0.5f;
}

bool WeaponManager::ReadMotionMeleeHandGeometry(int controllerHand,
	const glm::fvec3& controllerPosition, const glm::fquat& controllerRotation,
	glm::fvec3& baseUE, glm::fvec3& tipUE, float& radiusUE) const
{
	baseUE = glm::fvec3(0.0f);
	tipUE = glm::fvec3(0.0f);
	radiusUE = 0.0f;
	if (controllerHand != 0 && controllerHand != 1)
		return false;

	glm::fvec3 handPosition{};
	glm::fquat handRotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
	bool usedVisibleBone = false;
	if (!ReadMotionMeleeHandWorldPose(controllerHand, controllerPosition,
		controllerRotation, handPosition, handRotation, usedVisibleBone))
		return false;

	const glm::fvec3 forward = NormalizeOrZero(handRotation * glm::fvec3(1.0f, 0.0f, 0.0f));
	if (glm::length(forward) <= 0.5f)
		return false;

	const bool unarmed = currentWeaponEquipped == Unarmed;
	const float baseOffset = unarmed ? 2.0f : 1.5f;
	// Restore the last proven geometry exactly: a 10 cm bare-fist segment and a
	// 13 cm brass-knuckle segment. Any temporal lead should be a separately
	// visible/testable sweep lane, not a silent permanent reach increase.
	const float contactLength = unarmed ? 10.0f : 13.0f;
	baseUE = handPosition + forward * baseOffset;
	tipUE = handPosition + forward * (baseOffset + contactLength);
	// A fist/knuckle contact is intentionally short and compact. Keep its radius
	// independent of the native mesh so an absent or decorative mesh cannot widen
	// the physical hit volume.
	// Slightly enlarge only the short hand volumes. The temporal lead remains
	// capped separately, so this improves near-edge fist contacts without making
	// bats or other held melee weapons feel broad.
	radiusUE = unarmed ? 6.5f : 6.0f;
	return IsFiniteVector(baseUE) && IsFiniteVector(tipUE)
		&& std::isfinite(radiusUE) && glm::length(tipUE - baseUE) >= 5.0f;
}

bool WeaponManager::ReadMotionMeleeContactGeometry(const glm::fvec3& meshPosition,
	const glm::fquat& meshRotation, glm::fvec3& baseUE, glm::fvec3& tipUE,
	float& radiusUE)
{
	baseUE = glm::fvec3(0.0f);
	tipUE = glm::fvec3(0.0f);
	radiusUE = 0.0f;
	if (firstWeaponMesh == nullptr || !uevr::API::UObjectHook::exists(firstWeaponMesh))
		return false;
	const int weaponId = static_cast<int>(currentWeaponEquipped);
	const bool geometryChanged = motionMeleeGeometryMesh != firstWeaponMesh
		|| motionMeleeGeometryWeapon != weaponId;
	if (geometryChanged)
	{
		motionMeleeGeometryMesh = firstWeaponMesh;
		motionMeleeGeometryWeapon = weaponId;
		motionMeleeGeometryValid = false;
		motionMeleeGeometryPrincipalAxis = -1;
		auto componentClass = firstWeaponMesh->get_class();
		auto getLocalBounds = componentClass != nullptr
			? componentClass->find_function(L"GetLocalBounds") : nullptr;
		if (getLocalBounds == nullptr)
		{
			uevr::API::get()->log_warn(
				"[MotionMelee] geometry unavailable weapon=%d reason=GetLocalBounds-missing",
				weaponId);
			return false;
		}
		ParameterGetLocalBounds bounds{};
		firstWeaponMesh->call_function(L"GetLocalBounds", &bounds);
		if (!IsFiniteVector(bounds.min) || !IsFiniteVector(bounds.max))
		{
			uevr::API::get()->log_warn(
				"[MotionMelee] geometry unavailable weapon=%d reason=non-finite-bounds",
				weaponId);
			return false;
		}
		const glm::fvec3 spans = glm::abs(bounds.max - bounds.min);
		int principalAxis = 0;
		if (spans.y > spans.x && spans.y >= spans.z)
			principalAxis = 1;
		else if (spans.z > spans.x && spans.z > spans.y)
			principalAxis = 2;
		if (spans[principalAxis] < 10.0f)
		{
			uevr::API::get()->log_warn(
				"[MotionMelee] geometry unavailable weapon=%d reason=degenerate-bounds spans=(%.1f %.1f %.1f)",
				weaponId, spans.x, spans.y, spans.z);
			return false;
		}

		const glm::fvec3 center = (bounds.min + bounds.max) * 0.5f;
		motionMeleeGeometryLocalBase = center;
		motionMeleeGeometryLocalTip = center;
		const float lengthPadding = (std::max)(8.0f, spans[principalAxis] * 0.10f);
		motionMeleeGeometryLocalBase[principalAxis] = bounds.min[principalAxis] - lengthPadding;
		motionMeleeGeometryLocalTip[principalAxis] = bounds.max[principalAxis] + lengthPadding;
		float perpendicularSpan = 0.0f;
		for (int axis = 0; axis < 3; ++axis)
		{
			if (axis != principalAxis)
				perpendicularSpan = (std::max)(perpendicularSpan, spans[axis]);
		}
		const bool batSizedWeapon = weaponId == GolfClub
			|| weaponId == BaseballBat || weaponId == Shovel
			|| weaponId == PoolCue || weaponId == Chainsaw;
		const float radiusFloor = batSizedWeapon ? 6.0f : 4.0f;
		const float radiusCeiling = batSizedWeapon ? 8.0f : 7.0f;
		const float radiusMargin = batSizedWeapon ? 0.75f : 0.5f;
		motionMeleeGeometryLocalRadius = (std::clamp)(
			(perpendicularSpan * 0.5f) + radiusMargin, radiusFloor, radiusCeiling);
		motionMeleeGeometryPrincipalAxis = principalAxis;
		motionMeleeGeometryValid = true;
		const char axisName = principalAxis == 0 ? 'X' : (principalAxis == 1 ? 'Y' : 'Z');
		uevr::API::get()->log_info(
			"[MotionMelee] geometry weapon=%d axis=%c boundsMin=(%.1f %.1f %.1f) boundsMax=(%.1f %.1f %.1f) expandedLength=%.1f localRadius=%.1f",
			weaponId, axisName, bounds.min.x, bounds.min.y, bounds.min.z,
			bounds.max.x, bounds.max.y, bounds.max.z,
			glm::length(motionMeleeGeometryLocalTip - motionMeleeGeometryLocalBase),
			motionMeleeGeometryLocalRadius);
	}
	if (!motionMeleeGeometryValid)
		return false;

	glm::fvec3 componentScale(1.0f);
	auto componentClass = firstWeaponMesh->get_class();
	if (componentClass != nullptr)
	{
		auto getScale = componentClass->find_function(L"GetComponentScale");
		if (getScale == nullptr)
			getScale = componentClass->find_function(L"K2_GetComponentScale");
		if (getScale != nullptr)
		{
			Utilities::ParameterSingleVector3 scaleParams{};
			getScale->call(firstWeaponMesh, &scaleParams);
			componentScale = glm::abs(scaleParams.vec3Value);
		}
		else if (componentClass->find_property(L"RelativeScale3D") != nullptr)
		{
			auto scaleData = firstWeaponMesh->get_property_data<glm::fvec3>(L"RelativeScale3D");
			if (scaleData != nullptr)
				componentScale = glm::abs(*scaleData);
		}
	}
	for (int axis = 0; axis < 3; ++axis)
	{
		if (!std::isfinite(componentScale[axis]) || componentScale[axis] < 0.001f
			|| componentScale[axis] > 100.0f)
			componentScale[axis] = 1.0f;
	}
	const glm::fvec3 scaledBase = motionMeleeGeometryLocalBase * componentScale;
	const glm::fvec3 scaledTip = motionMeleeGeometryLocalTip * componentScale;
	baseUE = meshPosition + (meshRotation * scaledBase);
	tipUE = meshPosition + (meshRotation * scaledTip);
	float perpendicularScale = 1.0f;
	for (int axis = 0; axis < 3; ++axis)
	{
		if (axis != motionMeleeGeometryPrincipalAxis)
			perpendicularScale = (std::max)(perpendicularScale, componentScale[axis]);
	}
	const bool batSizedWeapon = weaponId == GolfClub
		|| weaponId == BaseballBat || weaponId == Shovel
		|| weaponId == PoolCue || weaponId == Chainsaw;
	const float radiusFloor = batSizedWeapon ? 6.0f : 4.0f;
	const float radiusCeiling = batSizedWeapon ? 8.0f : 7.0f;
	// Keep the physical volume close to the authored cross-section. The old
	// eight-to-sixteen-centimetre clamp made a correctly sized bat feel like a
	// broad capsule and forced extra LOS lanes to compensate for it.
	radiusUE = (std::clamp)(motionMeleeGeometryLocalRadius * perpendicularScale,
		radiusFloor, radiusCeiling);
	return IsFiniteVector(baseUE) && IsFiniteVector(tipUE)
		&& std::isfinite(radiusUE) && glm::length(tipUE - baseUE) >= 10.0f;
}

bool WeaponManager::EnsureMotionMeleeDebugAxis()
{
	auto currentCharacter = playerManager != nullptr ? playerManager->playerCharacter : nullptr;
	if (motionMeleeDebugAxisCreationFailed
		&& motionMeleeDebugAxisCharacter == currentCharacter)
		return false;
	if (motionMeleeDebugAxisComponent != nullptr
		&& motionMeleeDebugAxisCharacter == currentCharacter
		&& uevr::API::UObjectHook::exists(motionMeleeDebugAxisComponent))
		return true;

	if (motionMeleeDebugAxisComponent != nullptr
		&& uevr::API::UObjectHook::exists(motionMeleeDebugAxisComponent))
	{
		auto destroy = motionMeleeDebugAxisComponent->get_class()->find_function(L"DestroyComponent");
		if (destroy != nullptr)
		{
			std::vector<uint8_t> params(destroy->get_properties_size());
			SetReflectedBoolParameter(destroy, params, L"bPromoteChildren", false);
			destroy->call(motionMeleeDebugAxisComponent, params.data());
		}
	}
	motionMeleeDebugAxisComponent = nullptr;
	motionMeleeDebugAxisCharacter = nullptr;
	motionMeleeDebugAxisCreationFailed = false;
	if (currentCharacter == nullptr || !uevr::API::UObjectHook::exists(currentCharacter))
		return false;

	auto proceduralMeshClass = uevr::API::get()->find_uobject<uevr::API::UClass>(
		L"Class /Script/ProceduralMeshComponent.ProceduralMeshComponent");
	if (proceduralMeshClass == nullptr)
	{
		motionMeleeDebugAxisCreationFailed = true;
		motionMeleeDebugAxisCharacter = currentCharacter;
		uevr::API::get()->log_warn("[MotionMelee] visible trace unavailable reason=procedural-mesh-class");
		return false;
	}

	auto component = uevr::API::get()->add_component_by_class(
		currentCharacter, proceduralMeshClass, false);
	if (component == nullptr || !uevr::API::UObjectHook::exists(component))
	{
		motionMeleeDebugAxisCreationFailed = true;
		motionMeleeDebugAxisCharacter = currentCharacter;
		uevr::API::get()->log_warn("[MotionMelee] visible trace unavailable reason=component-create");
		return false;
	}

	auto componentClass = component->get_class();
	auto createSection = componentClass != nullptr
		? componentClass->find_function(L"CreateMeshSection_LinearColor") : nullptr;
	if (createSection == nullptr && componentClass != nullptr)
		createSection = componentClass->find_function(L"CreateMeshSection");
	if (createSection == nullptr)
	{
		motionMeleeDebugAxisComponent = component;
		motionMeleeDebugAxisCharacter = currentCharacter;
		motionMeleeDebugAxisCreationFailed = true;
		SetComponentVisibility(component, false);
		uevr::API::get()->log_warn("[MotionMelee] visible trace unavailable reason=create-section-function");
		return false;
	}

	// A 100 cm long, 3 cm thick box aligned to local +X. Runtime scale and
	// rotation place its two ends on the exact native-LOS base and tip.
	std::array<glm::fvec3, 8> vertices{
		glm::fvec3(0.0f, -1.5f, -1.5f), glm::fvec3(0.0f, 1.5f, -1.5f),
		glm::fvec3(0.0f, 1.5f, 1.5f), glm::fvec3(0.0f, -1.5f, 1.5f),
		glm::fvec3(100.0f, -1.5f, -1.5f), glm::fvec3(100.0f, 1.5f, -1.5f),
		glm::fvec3(100.0f, 1.5f, 1.5f), glm::fvec3(100.0f, -1.5f, 1.5f)
	};
	std::array<int32_t, 36> triangles{
		0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
		0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2,
		0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5
	};
	std::vector<uint8_t> params(createSection->get_properties_size());
	const bool populated = SetReflectedIntParameter(createSection, params, L"SectionIndex", 0)
		&& SetReflectedArrayParameter(createSection, params, L"Vertices",
			vertices.data(), static_cast<int32_t>(vertices.size()))
		&& SetReflectedArrayParameter(createSection, params, L"Triangles",
			triangles.data(), static_cast<int32_t>(triangles.size()));
	SetReflectedBoolParameter(createSection, params, L"bCreateCollision", false);
	if (!populated)
	{
		motionMeleeDebugAxisComponent = component;
		motionMeleeDebugAxisCharacter = currentCharacter;
		motionMeleeDebugAxisCreationFailed = true;
		SetComponentVisibility(component, false);
		uevr::API::get()->log_warn("[MotionMelee] visible trace unavailable reason=create-section-parameters");
		return false;
	}
	createSection->call(component, params.data());
	motionMeleeDebugAxisComponent = component;
	motionMeleeDebugAxisCharacter = currentCharacter;
	SetComponentVisibility(component, false);
	uevr::API::get()->log_info("[MotionMelee] visible trace component created component=%p", component);
	return true;
}

void WeaponManager::UpdateMotionMeleeDebugAxis(const glm::fvec3& startUE,
	const glm::fvec3& endUE, float radiusUE)
{
	if (!EnsureMotionMeleeDebugAxis())
		return;
	const glm::fvec3 delta = endUE - startUE;
	const float length = glm::length(delta);
	if (!std::isfinite(length) || length <= 0.1f)
	{
		HideMotionMeleeDebugAxis();
		return;
	}
	auto componentClass = motionMeleeDebugAxisComponent->get_class();
	if (componentClass == nullptr || Utilities::KismetMathLibrary == nullptr
		|| componentClass->find_function(L"K2_SetWorldLocation") == nullptr
		|| componentClass->find_function(L"K2_SetWorldRotation") == nullptr)
	{
		HideMotionMeleeDebugAxis();
		return;
	}

	Utilities::ParameterFindLookAtRotation lookAt{};
	lookAt.start = startUE;
	lookAt.target = endUE;
	Utilities::KismetMathLibrary->call_function(L"FindLookAtRotation", &lookAt);
	Utilities::Parameter_K2_SetWorldOrRelativeLocation location{};
	location.newLocation = startUE;
	location.bTeleport = true;
	motionMeleeDebugAxisComponent->call_function(L"K2_SetWorldLocation", &location);
	Utilities::Parameter_K2_SetWorldOrRelativeRotation rotation{};
	rotation.newRotation = lookAt.outRotation;
	rotation.bTeleport = true;
	motionMeleeDebugAxisComponent->call_function(L"K2_SetWorldRotation", &rotation);
	ParameterSetScale3D scale{};
	const float widthScale = (std::max)(1.0f, (radiusUE * 2.0f) / 3.0f);
	scale.newScale3D = { length / 100.0f, widthScale, widthScale };
	if (componentClass->find_function(L"SetWorldScale3D") != nullptr)
		motionMeleeDebugAxisComponent->call_function(L"SetWorldScale3D", &scale);
	else if (componentClass->find_function(L"K2_SetWorldScale3D") != nullptr)
		motionMeleeDebugAxisComponent->call_function(L"K2_SetWorldScale3D", &scale);
	if (componentClass->find_property(L"bHiddenInGame") != nullptr)
		motionMeleeDebugAxisComponent->set_bool_property(L"bHiddenInGame", false);
	if (componentClass->find_property(L"bOwnerNoSee") != nullptr)
		motionMeleeDebugAxisComponent->set_bool_property(L"bOwnerNoSee", false);
	SetComponentVisibility(motionMeleeDebugAxisComponent, true);
}

void WeaponManager::HideMotionMeleeDebugAxis()
{
	SetComponentVisibility(motionMeleeDebugAxisComponent, false);
}

bool WeaponManager::PlayMotionMeleeImpactAudio(
	const MemoryManager::NativeMeleeContact& contact, int damageWeaponType)
{
	if (settingsManager == nullptr || !settingsManager->enableMotionMeleeImpactAudio
		|| playerManager == nullptr || playerManager->playerActor == nullptr
		|| !uevr::API::UObjectHook::exists(playerManager->playerActor)
		|| (contact.entityType != 2 && contact.entityType != 3))
		return false;

	if (!motionMeleeImpactSoundLoadAttempted)
	{
		motionMeleeImpactSoundLoadAttempted = true;
		// The DE package preserves the original GENRL collision-bank numbering:
		// S_029 is COLCARPED (a compact body impact) and S_020 is COLCAR01.
		motionMeleePedImpactSound = FindOrLoadSoundWave(
			L"/Game/SanAndreas/Audio/SFX/GENRL/COLLISIONS/S_029.S_029");
		// Prefer GTA's own weapon-bank impacts when those cooked assets are exposed
		// by this DE package. Each category falls back to the proven collision bank.
		motionMeleeFistImpactSound = FindOrLoadSoundWave(
			L"/Game/SanAndreas/Audio/SFX/GENRL/WEAPONS/S_037.S_037");
		motionMeleeBluntImpactSound = FindOrLoadSoundWave(
			L"/Game/SanAndreas/Audio/SFX/GENRL/WEAPONS/S_040.S_040");
		motionMeleeSharpImpactSound = FindOrLoadSoundWave(
			L"/Game/SanAndreas/Audio/SFX/GENRL/WEAPONS/S_082.S_082");
		motionMeleeVehicleImpactSound = FindOrLoadSoundWave(
			L"/Game/SanAndreas/Audio/SFX/GENRL/COLLISIONS/S_020.S_020");
		uevr::API::get()->log_info(
			"[MotionMeleeAudio] assets fist=%p blunt=%p sharp=%p pedFallback=%p vehicle=%p",
			motionMeleeFistImpactSound, motionMeleeBluntImpactSound,
			motionMeleeSharpImpactSound, motionMeleePedImpactSound,
			motionMeleeVehicleImpactSound);
	}

	const bool sharpWeapon = damageWeaponType == Knife || damageWeaponType == Katana
		|| damageWeaponType == Chainsaw;
	const bool fistWeapon = damageWeaponType == Unarmed
		|| damageWeaponType == BrassKnuckles;
	auto sound = motionMeleeVehicleImpactSound;
	float volume = 0.82f;
	float pitch = 1.0f;
	if (contact.entityType == 3)
	{
		sound = fistWeapon ? motionMeleeFistImpactSound
			: (sharpWeapon ? motionMeleeSharpImpactSound : motionMeleeBluntImpactSound);
		if (sound == nullptr || !uevr::API::UObjectHook::exists(sound))
			sound = motionMeleePedImpactSound;
		// The fallback body sample is wet at full volume. Fists use a shorter,
		// quieter, higher-pitched thud; blunt and sharp weapons retain more weight.
		volume = fistWeapon ? 0.48f : (sharpWeapon ? 0.68f : 0.62f);
		pitch = fistWeapon ? 1.22f : (sharpWeapon ? 1.08f : 0.94f);
	}
	if (sound == nullptr || !uevr::API::UObjectHook::exists(sound))
	{
		if (!motionMeleeImpactAudioFailureLogged)
		{
			motionMeleeImpactAudioFailureLogged = true;
			uevr::API::get()->log_warn(
				"[MotionMeleeAudio] unavailable reason=sound-asset entityType=%u",
				static_cast<unsigned int>(contact.entityType));
		}
		return false;
	}

	const glm::fvec3 point(contact.point[0], contact.point[1], contact.point[2]);
	const bool played = PlaySoundWaveAtLocation(playerManager->playerActor, sound,
		point, volume, pitch);
	if (!played && !motionMeleeImpactAudioFailureLogged)
	{
		motionMeleeImpactAudioFailureLogged = true;
		uevr::API::get()->log_warn(
			"[MotionMeleeAudio] unavailable reason=reflected-playback entityType=%u",
			static_cast<unsigned int>(contact.entityType));
	}
	return played;
}

bool WeaponManager::PlayMotionThrowableImpactEffect(
	int weaponType, const glm::fvec3& impactPoint,
	const MemoryManager::NativeMeleeContact* contact)
{
	if (settingsManager == nullptr || !settingsManager->enableMotionThrowables
		|| (weaponType == Molotov && settingsManager->enableNativeMolotovMode)
		|| playerManager == nullptr || playerManager->playerActor == nullptr
		|| !uevr::API::UObjectHook::exists(playerManager->playerActor)
		|| !IsFiniteVector(impactPoint))
		return false;

	// WeaponManager owns the physical bottle and measured collision in UE space.
	// At impact only, hand the converted point to DE's real Molotov explosion
	// lifecycle. Returning here prevents duplicate BP shells, synthetic damage,
	// sounds, or persistent unregistered fire actors from running as well.
	if (memoryManager != nullptr)
	{
		const std::array<float, 3> nativeImpact{
			impactPoint.x * 0.01f,
			-impactPoint.y * 0.01f,
			impactPoint.z * 0.01f };
		if (memoryManager->ApplyNativeThrowableExplosion(nativeImpact, weaponType))
			return true;
	}
	// Grenades have no Molotov Blueprint/manual fallback. If the validated native
	// endpoint is unavailable, fail closed instead of producing fire semantics.
	if (weaponType != Molotov)
		return false;

	if (!motionThrowableNativeExplosionLoadAttempted)
	{
		motionThrowableNativeExplosionLoadAttempted = true;
		motionThrowableNativeExplosionClass = FindOrLoadBlueprintGeneratedClass(
			L"/Game/Common/Effects/Explosions/BP_Explosion_Molotov.BP_Explosion_Molotov_C");
		uevr::API::get()->log_info(
			"[MotionThrowableImpact] native-molotov-class class=%p name=%ls",
			motionThrowableNativeExplosionClass,
			motionThrowableNativeExplosionClass != nullptr
				? motionThrowableNativeExplosionClass->get_full_name().c_str() : L"<null>");
		if (motionThrowableNativeExplosionClass != nullptr)
		{
			LogBlueprintDeclaredMembers(motionThrowableNativeExplosionClass,
				"MotionThrowableNativeMolotov");
			LogRelevantClassHierarchyMembers(motionThrowableNativeExplosionClass,
				"MotionThrowableNativeMolotov");
		}
		motionThrowableNativeFireClass = FindOrLoadBlueprintGeneratedClass(
			L"/Game/Common/Blueprints/BP_Fire.BP_Fire_C");
		uevr::API::get()->log_info(
			"[MotionThrowableNativeMolotov] fire-owner class=%p name=%ls",
			motionThrowableNativeFireClass,
			motionThrowableNativeFireClass != nullptr
				? motionThrowableNativeFireClass->get_full_name().c_str() : L"<null>");
		if (motionThrowableNativeFireClass != nullptr)
		{
			LogBlueprintDeclaredMembers(motionThrowableNativeFireClass,
				"MotionThrowableNativeFire");
			LogRelevantClassHierarchyMembers(motionThrowableNativeFireClass,
				"MotionThrowableNativeFire");
		}
		// The impact actor is a data-only shell. The package also exposes a
		// dedicated SpawnExplosion Blueprint, which is the likely native entry
		// point that configures fire, damage, and the chosen explosion subtype.
		motionThrowableNativeSpawnLibraryClass = FindOrLoadBlueprintGeneratedClass(
			L"/Game/Common/Effects/SpawnExplosion.SpawnExplosion_C");
		if (motionThrowableNativeSpawnLibraryClass != nullptr)
		{
			uevr::API::get()->log_info(
				"[MotionThrowableNativeMolotov] spawn-library class=%p name=%ls",
				motionThrowableNativeSpawnLibraryClass,
				motionThrowableNativeSpawnLibraryClass->get_full_name().c_str());
			LogBlueprintDeclaredMembers(motionThrowableNativeSpawnLibraryClass,
				"MotionThrowableSpawnExplosion");
			EnsureMotionThrowableNativeMolotovProbeHooks(
				motionThrowableNativeSpawnLibraryClass);
		}
		else
		{
			uevr::API::get()->log_warn(
				"[MotionThrowableNativeMolotov] spawn-library class=<null>");
		}
		if (motionThrowableNativeSpawnLibraryClass != nullptr)
		{
			uevr::API::UClass* selectedClass = nullptr;
			uevr::API::UObject* selectedDebris = nullptr;
			uint8_t selectedType = 0;
			motionThrowableNativeMolotovSelectionResolved =
				ResolveNativeMolotovExplosionSelection(playerManager->playerActor,
					motionThrowableNativeSpawnLibraryClass, selectedClass, selectedDebris,
					selectedType);
			if (motionThrowableNativeMolotovSelectionResolved)
			{
				motionThrowableNativeExplosionClass = selectedClass;
				motionThrowableNativeMolotovDebrisTemplate = selectedDebris;
				motionThrowableNativeMolotovTypeByte = selectedType;
				uevr::API::get()->log_info(
					"[MotionThrowableNativeMolotov] selector resolved type=%u class=%p name=%ls debris=%p debrisName=%ls",
					static_cast<unsigned int>(selectedType), selectedClass,
					selectedClass->get_full_name().c_str(), selectedDebris,
					selectedDebris != nullptr
						? selectedDebris->get_full_name().c_str() : L"<null>");
			}
			else
			{
				uevr::API::get()->log_warn(
					"[MotionThrowableNativeMolotov] selector found no Molotov class/debris pair; retaining direct class without debris");
			}
		}
	}
	bool nativeMolotovExplosionActivated = false;
	bool nativeMolotovFireActorSpawned = false;
	uevr::API::UObject* spawnedNativeExplosion = nullptr;
	uevr::API::UObject* spawnedNativeFire = nullptr;
	if (motionThrowableNativeExplosionClass != nullptr
		&& uevr::API::UObjectHook::exists(motionThrowableNativeExplosionClass))
	{
		// Keep the custom hand-driven bottle and measured collision. Only this
		// final handoff is native: the game's dedicated Molotov Blueprint is
		// created at the impact point so it owns fire spread, reactions, visuals,
		// and damage instead of a generic instant-hit dispatcher.
		if (auto* nativeExplosion = SpawnNativeActorAtLocation(playerManager->playerActor,
			motionThrowableNativeExplosionClass, impactPoint))
		{
			spawnedNativeExplosion = nativeExplosion;
			const bool setupApplied = motionThrowableNativeSpawnLibraryClass != nullptr
				&& uevr::API::UObjectHook::exists(motionThrowableNativeSpawnLibraryClass)
				&& SetupNativeSpawnedExplosion(playerManager->playerActor,
					motionThrowableNativeSpawnLibraryClass, nativeExplosion,
					motionThrowableNativeMolotovDebrisTemplate);
			uevr::API::get()->log_info(
				"[MotionThrowableImpact] native-molotov-spawned actor=%p class=%ls point=(%.2f %.2f %.2f) setup=%d",
				nativeExplosion,
				motionThrowableNativeExplosionClass->get_full_name().c_str(),
				impactPoint.x, impactPoint.y, impactPoint.z, setupApplied ? 1 : 0);
			// The cooked SetupSpawnedExplosion graph only activates ExplosionEffect
			// when ExplodingActor is valid. A world collision intentionally has no
			// Unreal actor, so explicitly activate the spawned native components here.
			// This is still impact-only: the custom hand flight never enters this path.
			if (nativeExplosion->get_class()->find_property(L"ExplosionEffect") != nullptr)
			{
				auto* explosionEffect = nativeExplosion->get_property<uevr::API::UObject*>(
					L"ExplosionEffect");
				if (explosionEffect != nullptr
					&& uevr::API::UObjectHook::exists(explosionEffect))
				{
					EnsureMotionThrowableVisualRenderable(explosionEffect);
					nativeMolotovExplosionActivated = true;
				}
			}
			if (nativeExplosion->get_class()->find_property(L"DebrisEffect") != nullptr)
			{
				auto* debrisEffect = nativeExplosion->get_property<uevr::API::UObject*>(
					L"DebrisEffect");
				if (debrisEffect != nullptr
					&& uevr::API::UObjectHook::exists(debrisEffect))
					EnsureMotionThrowableVisualRenderable(debrisEffect);
			}
			if (!setupApplied && !nativeMolotovExplosionActivated)
			{
				uevr::API::get()->log_warn(
					"[MotionThrowableImpact] native-molotov-setup failed actor=%p; using visual fallback",
					nativeExplosion);
			}
		}
		if (!nativeMolotovExplosionActivated)
		{
			uevr::API::get()->log_warn(
				"[MotionThrowableImpact] native-molotov-spawn/setup unavailable class=%ls; using visual fallback",
				motionThrowableNativeExplosionClass->get_full_name().c_str());
		}
	}
	// UGTAFire/BP_Fire is the game's persistent fire owner. Spawning it at the
	// measured impact point gives the native fire manager a real actor to tick;
	// the old particle-only fallback could never produce burn propagation or
	// vehicle/ped fire damage. Do not use this actor at release time.
	if (motionThrowableNativeFireClass != nullptr
		&& uevr::API::UObjectHook::exists(motionThrowableNativeFireClass))
	{
		if (auto* nativeFire = SpawnNativeActorAtLocation(playerManager->playerActor,
			motionThrowableNativeFireClass, impactPoint))
		{
			spawnedNativeFire = nativeFire;
			if (nativeFire->get_class()->find_property(L"FX") != nullptr)
			{
				auto* fireSystem = nativeFire->get_property<uevr::API::UObject*>(L"FX");
				if (fireSystem != nullptr
					&& uevr::API::UObjectHook::exists(fireSystem))
					EnsureMotionThrowableVisualRenderable(fireSystem);
			}
			// Force the BP's public system selector once after spawn. Native
			// BeginPlay normally does this, but the injected spawn can occur before
			// the first component tick on some frame paths.
			if (auto* getSystem = nativeFire->get_class()->find_function(L"GetSystem"))
			{
				std::vector<uint8_t> params(getSystem->get_properties_size());
				getSystem->call(nativeFire, params.data());
				uevr::API::UObject* selectedSystem = nullptr;
				if (ReadReflectedObjectParameter(getSystem, params, L"system", selectedSystem)
					&& selectedSystem != nullptr
					&& uevr::API::UObjectHook::exists(selectedSystem))
				{
					EnsureMotionThrowableVisualRenderable(selectedSystem);
				}
			}
			nativeMolotovFireActorSpawned = true;
			uevr::API::get()->log_info(
				"[MotionThrowableImpact] native-fire-owner spawned actor=%p class=%ls point=(%.2f %.2f %.2f) fireLifetimeMs=5000 registrationPath=bpFire-unregistered",
				nativeFire, motionThrowableNativeFireClass->get_full_name().c_str(),
				impactPoint.x, impactPoint.y, impactPoint.z);
		}
		else
		{
			uevr::API::get()->log_warn(
				"[MotionThrowableImpact] native-fire-owner spawn failed class=%ls",
				motionThrowableNativeFireClass->get_full_name().c_str());
		}
	}
	const bool nativeMolotovFireSetup = nativeMolotovExplosionActivated
		|| nativeMolotovFireActorSpawned;
	if (spawnedNativeExplosion != nullptr || spawnedNativeFire != nullptr)
	{
		if (motionThrowableNativeImpactActors.size() >= 8)
		{
			auto& oldest = motionThrowableNativeImpactActors.front();
			DestroyMotionThrowableNativeImpactActors(oldest, true, true);
			motionThrowableNativeImpactActors.erase(
				motionThrowableNativeImpactActors.begin());
		}
		MotionThrowableNativeImpactActors actors{};
		actors.explosion = spawnedNativeExplosion;
		actors.fire = spawnedNativeFire;
		const ULONGLONG now = GetTickCount64();
		// Classic's Molotov explosion object lives for roughly three seconds and
		// its planted fire patches for roughly two to five seconds. These bounds
		// prevent an injected BP_Fire from becoming an immortal orphan while the
		// real DE fire-registration entry remains under active verification.
		actors.explosionExpiresAt = spawnedNativeExplosion != nullptr ? now + 3500 : 0;
		actors.fireExpiresAt = spawnedNativeFire != nullptr ? now + 5000 : 0;
		motionThrowableNativeImpactActors.push_back(actors);
	}

	if (!motionThrowableImpactAssetsLoadAttempted)
	{
		motionThrowableImpactAssetsLoadAttempted = true;
		motionThrowableMolotovImpactEffect = FindOrLoadParticleSystem(
			L"/Game/Common/Effects/Explosions/FX_splode_molotov.FX_splode_molotov");
		motionThrowableVehicleImpactEffect = FindOrLoadParticleSystem(
			L"/Game/Common/Effects/Explosions/FX_splode_small.FX_splode_small");
		motionThrowableVehicleExplosionEffect = FindOrLoadParticleSystem(
			L"/Game/Common/Effects/Explosions/FX_splode_car.FX_splode_car");
		const auto loadFirstParticle = [](const std::array<const wchar_t*, 6>& paths)
			-> uevr::API::UObject* {
				for (const auto* path : paths)
				{
					auto* particle = FindOrLoadParticleSystem(path);
					if (particle != nullptr)
						return particle;
				}
				return nullptr;
			};
		motionThrowableMolotovFlameEffect = loadFirstParticle({
			L"/Game/Common/Effects/Cascade/FX_ground_fire.FX_ground_fire",
			L"/Game/Common/Effects/Cascade/FX_molotov_flame.FX_molotov_flame",
			L"/Game/Common/Effects/Cascade/FX_fire_med.FX_fire_med",
			L"/Game/Common/Effects/Cascade/FX_fire.FX_fire",
			L"/Game/Common/Effects/Cascade/FX_Flame.FX_Flame",
			L"/Game/Common/Effects/Explosions/FX_molotov_flame.FX_molotov_flame" });
		motionThrowableSmokeEffect = loadFirstParticle({
			L"/Game/Common/Effects/Cascade/FX_smoke_flare.FX_smoke_flare",
			L"/Game/Common/Effects/Cascade/FX_smoke50lit.FX_smoke50lit",
			L"/Game/Common/Effects/Cascade/FX_smoke30lit.FX_smoke30lit",
			L"/Game/Common/Effects/Cascade/FX_smoke30m.FX_smoke30m",
			L"/Game/Common/Effects/Cascade/FX_fireball_smoke_new.FX_fireball_smoke_new",
			L"/Game/Common/Effects/Cascade/FX_prt_collisionsmoke.FX_prt_collisionsmoke" });
		motionThrowableImpactSound = FindOrLoadSoundWave(
			L"/Game/SanAndreas/Audio/SFX/GENRL/EXPLOSIONS/S_001.S_001");
		uevr::API::get()->log_info(
			"[MotionThrowableImpact] assets effect=%p effectName=%ls vehicle=%p vehicleName=%ls vehicleExplosion=%p vehicleExplosionName=%ls flame=%p flameName=%ls smoke=%p smokeName=%ls sound=%p soundName=%ls",
			motionThrowableMolotovImpactEffect,
			motionThrowableMolotovImpactEffect != nullptr
				? motionThrowableMolotovImpactEffect->get_full_name().c_str() : L"<null>",
			motionThrowableVehicleImpactEffect,
			motionThrowableVehicleImpactEffect != nullptr
				? motionThrowableVehicleImpactEffect->get_full_name().c_str() : L"<null>",
			motionThrowableVehicleExplosionEffect,
			motionThrowableVehicleExplosionEffect != nullptr
				? motionThrowableVehicleExplosionEffect->get_full_name().c_str() : L"<null>",
			motionThrowableMolotovFlameEffect,
			motionThrowableMolotovFlameEffect != nullptr
				? motionThrowableMolotovFlameEffect->get_full_name().c_str() : L"<null>",
			motionThrowableSmokeEffect,
			motionThrowableSmokeEffect != nullptr
				? motionThrowableSmokeEffect->get_full_name().c_str() : L"<null>",
			motionThrowableImpactSound,
			motionThrowableImpactSound != nullptr
				? motionThrowableImpactSound->get_full_name().c_str() : L"<null>");
	}

	// Ground fallback is deliberately the center of the proxy's crossing plane.
	// Lift only the visual so it is not buried in the floor; sound remains at the
	// measured contact point.
	const glm::fvec3 effectPoint = impactPoint + glm::fvec3(0.0f, 0.0f, 8.0f);
	const bool useVisualFallback = !nativeMolotovFireSetup;
	auto spawnedEffect = useVisualFallback ? SpawnParticleSystemAtLocation(
		playerManager->playerActor, motionThrowableMolotovImpactEffect, effectPoint) : nullptr;
	// Keep the flame/smoke components alive for a short bounded burn. The
	// explosion burst remains auto-destroyed; the persistent components are
	// explicitly destroyed by ProcessMotionThrowableImpactVisuals.
	const glm::fvec3 flamePoint = impactPoint + glm::fvec3(0.0f, 0.0f, 3.0f);
	const glm::fvec3 smokePoint = impactPoint + glm::fvec3(0.0f, 0.0f, 12.0f);
	auto* flameTemplate = motionThrowableMolotovFlameEffect != nullptr
		? motionThrowableMolotovFlameEffect : motionThrowableMolotovImpactEffect;
	auto spawnedFlame = useVisualFallback ? SpawnParticleSystemAtLocation(
		playerManager->playerActor, flameTemplate, flamePoint,
			glm::fvec3(1.35f), false) : nullptr;
	auto spawnedSmoke = useVisualFallback ? SpawnParticleSystemAtLocation(
			playerManager->playerActor, motionThrowableSmokeEffect, smokePoint,
			glm::fvec3(1.15f), false) : nullptr;
	if (spawnedFlame != nullptr || spawnedSmoke != nullptr)
	{
		MotionThrowableImpactVisual persistentVisual{};
		persistentVisual.fire = spawnedFlame;
		persistentVisual.smoke = spawnedSmoke;
		persistentVisual.expiresAt = GetTickCount64() + 6500;
		motionThrowableImpactVisuals.push_back(persistentVisual);
	}
	const bool effectPlayed = spawnedEffect != nullptr
		|| spawnedFlame != nullptr || spawnedSmoke != nullptr;
	EnsureMotionThrowableVisualRenderable(spawnedEffect);
	EnsureMotionThrowableVisualRenderable(spawnedFlame);
	EnsureMotionThrowableVisualRenderable(spawnedSmoke);
	const bool soundPlayed = PlaySoundWaveAtLocation(
		playerManager->playerActor, motionThrowableImpactSound, impactPoint, 0.92f, 1.0f);
	int appliedDamage = 0;
	bool damageApplied = false;
	MemoryManager::NativeMeleeContact damageContact{};
	bool damageContactValid = false;
	if (contact != nullptr && memoryManager != nullptr
		&& (contact->entityType == 1 || contact->entityType == 2
			|| contact->entityType == 3))
	{
		damageContact = *contact;
		damageContactValid = true;
		// Deliberately do not call ApplyNativeThrowableImpactEvent here. That is a
		// generic CWeapon/CColPoint impact dispatcher requiring a live weapon entry;
		// classic Molotov damage comes from registered fire ticks, not impact HP.
		// The native contact is retained only for target classification/diagnostics.
	}
	uevr::API::UObject* spawnedVehicleBurst = nullptr;
	bool vehicleBurstPlayed = false;
	const bool vehicleDestroyed = damageContactValid && damageApplied
		&& damageContact.entityType == 2
		&& damageContact.targetHealthAfter >= 0.0f
		&& damageContact.targetHealthAfter <= 0.5f;
	if (damageContactValid && damageApplied && damageContact.entityType == 2)
	{
		auto* vehicleEffect = vehicleDestroyed
			? motionThrowableVehicleExplosionEffect
			: motionThrowableVehicleImpactEffect;
		const glm::fvec3 vehicleEffectPoint = impactPoint
			+ glm::fvec3(0.0f, 0.0f, vehicleDestroyed ? 14.0f : 8.0f);
		const glm::fvec3 vehicleEffectScale = vehicleDestroyed
			? glm::fvec3(1.25f) : glm::fvec3(0.85f);
		spawnedVehicleBurst = SpawnParticleSystemAtLocation(
			playerManager->playerActor, vehicleEffect, vehicleEffectPoint,
			vehicleEffectScale, true);
		vehicleBurstPlayed = spawnedVehicleBurst != nullptr;
		EnsureMotionThrowableVisualRenderable(spawnedVehicleBurst);
		// Do not simulate a Molotov by repeatedly applying the generic 75-damage
		// impact route. That was only a temporary vehicle test and looks unlike
		// GTA's actual fire system. The custom bottle now waits for a proven native
		// fire owner instead of presenting a misleading damage-over-time result.
	}
	uevr::API::get()->log_info(
		"[MotionThrowableImpact] playback molotovTypeByte=%u registrationPath=%s nativeFire=%s explosionActivated=%s fireOwner=%s dispatcherUsed=false nativeDamage=none-direct-writes-disabled burst=%s flame=%s smoke=%s vehicleBurst=%s vehicleDestroyed=%s component=%p sound=%s damage=%s amount=%d health=%.2f->%.2f point=(%.2f %.2f %.2f) visualPoint=(%.2f %.2f %.2f)",
		static_cast<unsigned int>(motionThrowableNativeMolotovTypeByte),
		nativeMolotovFireActorSpawned ? "bpFire-unregistered"
			: (nativeMolotovExplosionActivated ? "spawnLibrary-visual" : "fallback"),
		nativeMolotovFireSetup ? "setup" : "fallback",
		nativeMolotovExplosionActivated ? "true" : "false",
		nativeMolotovFireActorSpawned ? "spawned" : "missing",
		spawnedEffect != nullptr ? "spawned" : "not-spawned",
		spawnedFlame != nullptr ? "spawned" : "not-spawned",
		spawnedSmoke != nullptr ? "spawned" : "not-spawned",
		vehicleBurstPlayed ? "spawned" : "not-spawned",
		vehicleDestroyed ? "true" : "false", spawnedEffect,
		soundPlayed ? "played" : "not-played",
		damageApplied ? "accepted" : (contact != nullptr ? "rejected" : "not-target"),
		appliedDamage,
		damageContactValid ? damageContact.targetHealthBefore : -1.0f,
		damageContactValid ? damageContact.targetHealthAfter : -1.0f,
		impactPoint.x, impactPoint.y, impactPoint.z,
		effectPoint.x, effectPoint.y, effectPoint.z);
	const bool anyVisualPlayed = effectPlayed || vehicleBurstPlayed;
	if (!anyVisualPlayed && !soundPlayed && !damageApplied
		&& !motionThrowableImpactFailureLogged)
	{
		motionThrowableImpactFailureLogged = true;
		uevr::API::get()->log_warn(
			"[MotionThrowableImpact] unavailable effect=%p sound=%p point=(%.2f %.2f %.2f)",
			motionThrowableMolotovImpactEffect, motionThrowableImpactSound,
			impactPoint.x, impactPoint.y, impactPoint.z);
	}
	return nativeMolotovFireSetup || anyVisualPlayed || soundPlayed || damageApplied;
}

bool WeaponManager::SetMotionThrowableVisualTransform(uevr::API::UObject* component,
	const glm::fvec3& position, const glm::fquat& rotation, bool sweep) const
{
	if (component == nullptr || !uevr::API::UObjectHook::exists(component)
		|| !IsFiniteVector(position) || !IsFiniteQuaternion(rotation)
		|| glm::length(rotation) <= 0.5f)
		return false;
	auto componentClass = component->get_class();
	if (componentClass == nullptr
		|| componentClass->find_function(L"K2_SetWorldLocation") == nullptr
		|| componentClass->find_function(L"K2_SetWorldRotation") == nullptr)
		return false;
	if (Utilities::KismetMathLibrary == nullptr)
		Utilities::InitHelperClasses();
	if (Utilities::KismetMathLibrary == nullptr
		|| Utilities::KismetMathLibrary->get_class() == nullptr
		|| Utilities::KismetMathLibrary->get_class()->find_function(
			L"MakeRotationFromAxes") == nullptr)
		return false;

	Utilities::Parameter_K2_SetWorldOrRelativeLocation locationParams{};
	locationParams.newLocation = position;
	locationParams.bSweep = sweep;
	locationParams.bTeleport = !sweep;
	component->call_function(L"K2_SetWorldLocation", &locationParams);
	ParameterMakeRotationFromAxes makeRotationParams{};
	makeRotationParams.forward = NormalizeOrZero(rotation * glm::fvec3(1.0f, 0.0f, 0.0f));
	makeRotationParams.right = NormalizeOrZero(rotation * glm::fvec3(0.0f, 1.0f, 0.0f));
	makeRotationParams.up = NormalizeOrZero(rotation * glm::fvec3(0.0f, 0.0f, 1.0f));
	if (glm::length(makeRotationParams.forward) <= 0.5f
		|| glm::length(makeRotationParams.right) <= 0.5f
		|| glm::length(makeRotationParams.up) <= 0.5f)
		return false;
	Utilities::KismetMathLibrary->call_function(
		L"MakeRotationFromAxes", &makeRotationParams);
	Utilities::Parameter_K2_SetWorldOrRelativeRotation rotationParams{};
	rotationParams.newRotation = makeRotationParams.returnValue;
	rotationParams.bSweep = false;
	rotationParams.bTeleport = true;
	component->call_function(L"K2_SetWorldRotation", &rotationParams);
	return true;
}

void WeaponManager::DestroyMotionThrowableFlightVisual(MotionThrowableFlight& flight)
{
	if (flight.collisionProxy != nullptr
		&& uevr::API::UObjectHook::exists(flight.collisionProxy))
	{
		SetComponentVisibility(flight.collisionProxy, false);
		auto proxyClass = flight.collisionProxy->get_class();
		auto destroyProxy = proxyClass != nullptr
			? proxyClass->find_function(L"DestroyComponent") : nullptr;
		if (destroyProxy != nullptr)
		{
			std::vector<uint8_t> params(destroyProxy->get_properties_size());
			SetReflectedBoolParameter(destroyProxy, params, L"bPromoteChildren", false);
			destroyProxy->call(flight.collisionProxy, params.data());
		}
	}
	flight.collisionProxy = nullptr;
	auto visual = flight.visual;
	if (visual == nullptr || !uevr::API::UObjectHook::exists(visual))
	{
		flight.visual = nullptr;
		return;
	}
	SetComponentVisibility(visual, false);
	auto visualClass = visual->get_class();
	auto destroy = visualClass != nullptr
		? visualClass->find_function(L"DestroyComponent") : nullptr;
	if (destroy != nullptr)
	{
		std::vector<uint8_t> params(destroy->get_properties_size());
		SetReflectedBoolParameter(destroy, params, L"bPromoteChildren", false);
		destroy->call(visual, params.data());
	}
	flight.visual = nullptr;
}

void WeaponManager::DestroyMotionThrowableImpactVisual(MotionThrowableImpactVisual& visual)
{
	auto destroyComponent = [this](uevr::API::UObject*& component) {
		if (component == nullptr || !uevr::API::UObjectHook::exists(component))
		{
			component = nullptr;
			return;
		}
		SetComponentVisibility(component, false);
		auto componentClass = component->get_class();
		auto destroy = componentClass != nullptr
			? componentClass->find_function(L"DestroyComponent") : nullptr;
		if (destroy != nullptr)
		{
			std::vector<uint8_t> params(destroy->get_properties_size());
			SetReflectedBoolParameter(destroy, params, L"bPromoteChildren", false);
			destroy->call(component, params.data());
		}
		component = nullptr;
	};
	destroyComponent(visual.fire);
	destroyComponent(visual.smoke);
	visual.expiresAt = 0;
}

void WeaponManager::DestroyMotionThrowableNativeImpactActors(
	MotionThrowableNativeImpactActors& actors, bool destroyExplosion, bool destroyFire)
{
	const auto destroyActor = [](uevr::API::UObject*& actor, bool fireActor,
		bool& extinguishCalled)
	{
		if (actor == nullptr || !uevr::API::UObjectHook::exists(actor))
		{
			actor = nullptr;
			return;
		}
		auto* actorClass = actor->get_class();
		if (fireActor && actorClass != nullptr)
		{
			if (auto* extinguish = actorClass->find_function(L"Extinguish"))
			{
				std::vector<uint8_t> params(extinguish->get_properties_size());
				extinguish->call(actor, params.data());
				extinguishCalled = true;
			}
			if (uevr::API::UObjectHook::exists(actor))
			{
				if (auto* killAgain = actorClass->find_function(L"KillAgain"))
				{
					std::vector<uint8_t> params(killAgain->get_properties_size());
					killAgain->call(actor, params.data());
				}
			}
		}
		if (uevr::API::UObjectHook::exists(actor))
		{
			actorClass = actor->get_class();
			auto* destroy = actorClass != nullptr
				? actorClass->find_function(L"K2_DestroyActor") : nullptr;
			if (destroy == nullptr)
				destroy = actorClass->find_function(L"DestroyActor");
			if (destroy != nullptr)
			{
				std::vector<uint8_t> params(destroy->get_properties_size());
				destroy->call(actor, params.data());
			}
		}
		actor = nullptr;
	};

	if (destroyExplosion)
	{
		destroyActor(actors.explosion, false, actors.extinguishCalled);
		actors.explosionExpiresAt = 0;
	}
	if (destroyFire)
	{
		destroyActor(actors.fire, true, actors.extinguishCalled);
		actors.fireExpiresAt = 0;
	}
}

void WeaponManager::ProcessMotionThrowableImpactVisuals()
{
	const ULONGLONG now = GetTickCount64();
	const bool invalidContext = settingsManager == nullptr
		|| !settingsManager->enableMotionThrowables
		|| settingsManager->enableNativeMolotovMode
		|| playerManager == nullptr || playerManager->playerActor == nullptr
		|| !uevr::API::UObjectHook::exists(playerManager->playerActor);
	for (size_t i = 0; i < motionThrowableImpactVisuals.size();)
	{
		auto& visual = motionThrowableImpactVisuals[i];
		if (invalidContext || visual.expiresAt == 0 || now >= visual.expiresAt)
		{
			DestroyMotionThrowableImpactVisual(visual);
			motionThrowableImpactVisuals.erase(
				motionThrowableImpactVisuals.begin() + static_cast<ptrdiff_t>(i));
			continue;
		}
		++i;
	}
	for (size_t i = 0; i < motionThrowableNativeImpactActors.size();)
	{
		auto& actors = motionThrowableNativeImpactActors[i];
		const bool destroyExplosion = invalidContext || actors.explosion == nullptr
			|| actors.explosionExpiresAt == 0 || now >= actors.explosionExpiresAt;
		const bool destroyFire = invalidContext || actors.fire == nullptr
			|| actors.fireExpiresAt == 0 || now >= actors.fireExpiresAt;
		if (destroyExplosion || destroyFire)
			DestroyMotionThrowableNativeImpactActors(
				actors, destroyExplosion, destroyFire);
		if (actors.explosion == nullptr && actors.fire == nullptr)
		{
			uevr::API::get()->log_info(
				"[MotionThrowableImpact] native actors cleaned extinguishCalled=%s registrationPath=bpFire-unregistered",
				actors.extinguishCalled ? "true" : "false");
			motionThrowableNativeImpactActors.erase(
				motionThrowableNativeImpactActors.begin() + static_cast<ptrdiff_t>(i));
			continue;
		}
		++i;
	}
}

void WeaponManager::ResetMotionThrowableFlight(const char* reason, bool logResult)
{
	const bool wasActive = motionThrowableFlight.active;
	const uint32_t sequence = motionThrowableFlight.sequence;
	const int weaponType = motionThrowableFlight.weaponType;
	const float age = motionThrowableFlight.ageSeconds;
	auto sourceMesh = motionThrowableFlight.sourceMesh;
	DestroyMotionThrowableFlightVisual(motionThrowableFlight);
	const bool restoreSource = reason != nullptr
		&& (std::strcmp(reason, "new-grip") == 0
			|| std::strcmp(reason, "regrip-replace") == 0
			|| std::strcmp(reason, "replacement") == 0
			|| std::strcmp(reason, "context-change") == 0
			|| std::strcmp(reason, "disabled") == 0
			|| std::strcmp(reason, "presentation-restore") == 0
			|| std::strcmp(reason, "native-launch-owned") == 0
			|| std::strcmp(reason, "initial-transform-failed") == 0
			|| std::strcmp(reason, "visual-lost") == 0
			|| std::strcmp(reason, "transform-update-failed") == 0
			|| std::strcmp(reason, "non-finite-flight") == 0);
	if (restoreSource)
	{
		if (sourceMesh != nullptr && uevr::API::UObjectHook::exists(sourceMesh))
			SetComponentVisibility(sourceMesh, true);
		else if (motionThrowableSourceHidden && firstWeaponMesh != nullptr
			&& uevr::API::UObjectHook::exists(firstWeaponMesh))
			SetComponentVisibility(firstWeaponMesh, true);
		motionThrowableSourceHidden = false;
	}
	else if (wasActive)
	{
		// Impact/timeout means the custom bottle has been consumed by this
		// presentation path. Keep the body-attached source hidden until the next
		// grip; otherwise GTA's idle animation makes the bottle jog in the holster.
		motionThrowableSourceHidden = true;
	}
	if (reason != nullptr
		&& (std::strcmp(reason, "disabled") == 0
			|| std::strcmp(reason, "presentation-restore") == 0))
	{
		for (auto& detached : motionThrowableDetachedFlights)
			DestroyMotionThrowableFlightVisual(detached);
		motionThrowableDetachedFlights.clear();
		for (auto& visual : motionThrowableImpactVisuals)
			DestroyMotionThrowableImpactVisual(visual);
		motionThrowableImpactVisuals.clear();
		for (auto& actors : motionThrowableNativeImpactActors)
			DestroyMotionThrowableNativeImpactActors(actors, true, true);
		motionThrowableNativeImpactActors.clear();
	}
	motionThrowableFlight = MotionThrowableFlight{};
	if (wasActive && logResult)
	{
		const bool nativeProjectileOwnsExplosion = reason != nullptr
			&& std::strcmp(reason, "native-launch-owned") == 0;
		const bool visualImpactEffectWasAttempted = reason != nullptr
			&& (std::strcmp(reason, "impact-probe-complete") == 0
				|| std::strcmp(reason, "fuse-expired") == 0);
		uevr::API::get()->log_info(
			"[MotionThrowableFlight] end seq=%u weapon=%d age=%.3f reason=%s explosion=%s ammo=%s",
			sequence, weaponType, age, reason != nullptr ? reason : "unspecified",
			nativeProjectileOwnsExplosion ? "native-projectile-owned"
				: (visualImpactEffectWasAttempted ? "visual-audio-fallback" : "not-called-unproven"),
			nativeProjectileOwnsExplosion ? "native" : "unchanged");
	}
}

void WeaponManager::EnsureMotionThrowableVisualRenderable(uevr::API::UObject* component)
{
	if (component == nullptr || !uevr::API::UObjectHook::exists(component))
		return;
	SetComponentVisibility(component, true);
	auto componentClass = component->get_class();
	if (componentClass == nullptr)
		return;
	if (componentClass->find_property(L"bHiddenInGame") != nullptr)
		component->set_bool_property(L"bHiddenInGame", false);
	if (componentClass->find_property(L"bOwnerNoSee") != nullptr)
		component->set_bool_property(L"bOwnerNoSee", false);
	if (componentClass->find_property(L"bOnlyOwnerSee") != nullptr)
		component->set_bool_property(L"bOnlyOwnerSee", false);
	if (componentClass->find_property(L"bRenderInMainPass") != nullptr)
		component->set_bool_property(L"bRenderInMainPass", true);
	if (componentClass->find_function(L"SetOwnerNoSee") != nullptr)
	{
		Utilities::ParameterSingleBool ownerNoSee{};
		ownerNoSee.boolValue = false;
		component->call_function(L"SetOwnerNoSee", &ownerNoSee);
	}
	// SpawnEmitterAtLocation can return a valid component without activating it
	// when the reflected bAutoActivate parameter is absent or stale. Re-activate
	// only the returned particle component; this does not touch native gameplay.
	if (auto activateSystem = componentClass->find_function(L"ActivateSystem"))
	{
		std::vector<uint8_t> params(activateSystem->get_properties_size());
		if (FindFunctionParameter(activateSystem, L"bFlagAsJustAttached") != nullptr)
			SetReflectedBoolParameter(activateSystem, params,
				L"bFlagAsJustAttached", true);
		else if (FindFunctionParameter(activateSystem, L"bReset") != nullptr)
			SetReflectedBoolParameter(activateSystem, params, L"bReset", true);
		activateSystem->call(component, params.data());
	}
}

void WeaponManager::StartMotionThrowableVehicleBurn(
	const MemoryManager::NativeMeleeContact& contact)
{
	if (contact.entity == 0 || contact.entityType != 2
		|| memoryManager == nullptr || settingsManager == nullptr
		|| !settingsManager->enableMotionThrowables
		|| settingsManager->enableNativeMolotovMode)
		return;
	const ULONGLONG now = GetTickCount64();
	for (auto& burn : motionThrowableVehicleBurns)
	{
		if (burn.entity != contact.entity)
			continue;
		burn.nativePoint = contact.point;
		burn.expiresAt = now + 3000;
		burn.nextDamageAt = (std::min)(burn.nextDamageAt, now + 250);
		return;
	}
	if (motionThrowableVehicleBurns.size() >= 4)
		motionThrowableVehicleBurns.erase(motionThrowableVehicleBurns.begin());
	MotionThrowableVehicleBurn burn{};
	burn.entity = contact.entity;
	burn.nativePoint = contact.point;
	burn.expiresAt = now + 3000;
	burn.nextDamageAt = now + 250;
	motionThrowableVehicleBurns.push_back(burn);
	uevr::API::get()->log_info(
		"[MotionThrowableBurn] start entity=0x%llX duration_ms=3000 interval_ms=250 damage_source=native-75",
		static_cast<unsigned long long>(burn.entity));
}

void WeaponManager::ProcessMotionThrowableVehicleBurns()
{
	const ULONGLONG now = GetTickCount64();
	if (settingsManager == nullptr || !settingsManager->enableMotionThrowables
		|| settingsManager->enableNativeMolotovMode
		|| memoryManager == nullptr)
	{
		motionThrowableVehicleBurns.clear();
		return;
	}
	for (size_t i = 0; i < motionThrowableVehicleBurns.size();)
	{
		auto& burn = motionThrowableVehicleBurns[i];
		if (burn.entity == 0 || now >= burn.expiresAt)
		{
			uevr::API::get()->log_info(
				"[MotionThrowableBurn] end entity=0x%llX reason=%s ticks=%u",
				static_cast<unsigned long long>(burn.entity),
				now >= burn.expiresAt ? "timeout" : "invalid", burn.ticks);
			motionThrowableVehicleBurns.erase(
				motionThrowableVehicleBurns.begin() + static_cast<ptrdiff_t>(i));
			continue;
		}
		if (now < burn.nextDamageAt)
		{
			++i;
			continue;
		}
		MemoryManager::NativeMeleeContact contact{};
		contact.entity = burn.entity;
		contact.entityType = 2;
		contact.point = burn.nativePoint;
		int appliedDamage = 0;
		const bool accepted = memoryManager->ApplyNativeThrowableImpactDamage(
			contact, Molotov, appliedDamage);
		++burn.ticks;
		if (!accepted || contact.targetHealthAfter <= 0.5f
			|| burn.ticks >= 12)
		{
			const char* reason = !accepted ? "rejected"
				: (contact.targetHealthAfter <= 0.5f ? "destroyed" : "max-ticks");
			uevr::API::get()->log_info(
				"[MotionThrowableBurn] end entity=0x%llX reason=%s ticks=%u health=%.2f->%.2f",
				static_cast<unsigned long long>(burn.entity), reason, burn.ticks,
				contact.targetHealthBefore, contact.targetHealthAfter);
			motionThrowableVehicleBurns.erase(
				motionThrowableVehicleBurns.begin() + static_cast<ptrdiff_t>(i));
			continue;
		}
		burn.nextDamageAt = now + 250;
		++i;
	}
}

bool WeaponManager::StartMotionThrowableFlight(uint32_t sequence, int weaponType, int hand,
	const glm::fvec3& position, const glm::fquat& rotation,
	const glm::fvec3& linearVelocityMps, const glm::fvec3& angularVelocity)
{
	if (sequence == 0 || (weaponType != Grenade && weaponType != Molotov)
		|| (hand != 0 && hand != 1) || settingsManager == nullptr
		|| (weaponType == Molotov && settingsManager->enableNativeMolotovMode)
		|| playerManager == nullptr || playerManager->playerCharacter == nullptr
		|| !uevr::API::UObjectHook::exists(playerManager->playerCharacter)
		|| firstWeaponMesh == nullptr || firstWeaponStaticMesh == nullptr
		|| !uevr::API::UObjectHook::exists(firstWeaponMesh)
		|| !uevr::API::UObjectHook::exists(firstWeaponStaticMesh)
		|| !IsFiniteVector(position) || !IsFiniteQuaternion(rotation)
		|| !IsFiniteVector(linearVelocityMps) || !IsFiniteVector(angularVelocity))
		return false;

	ResetMotionThrowableFlight("replacement", true);
	auto staticMeshClass = uevr::API::get()->find_uobject<uevr::API::UClass>(
		L"Class /Script/Engine.StaticMeshComponent");
	auto visual = staticMeshClass != nullptr
		? AddSkeletalComponent(playerManager->playerCharacter, staticMeshClass) : nullptr;
	if (visual == nullptr || !CallSetStaticMesh(visual, firstWeaponStaticMesh))
	{
		if (visual != nullptr && uevr::API::UObjectHook::exists(visual))
			SetComponentVisibility(visual, false);
		return false;
	}
	// The visible static mesh is only a render proxy; weapon meshes in this
	// build do not provide a dependable collision body. Use a hidden sphere
	// primitive for authoritative swept movement and leave the visual free to
	// follow the exact simulated position.
	uevr::API::UObject* collisionProxy = nullptr;
	bool collisionProxyCreated = false;
	bool collisionProxyRegistered = false;
	bool collisionProxyRadiusConfigured = false;
	bool collisionProxyCollisionConfigured = false;
	bool collisionProxyResponsesConfigured = false;
	bool collisionProxyOwnerIgnored = false;
	if (auto sphereClass = uevr::API::get()->find_uobject<uevr::API::UClass>(
		L"Class /Script/Engine.SphereComponent"))
	{
		collisionProxy = AddSkeletalComponent(
			playerManager->playerCharacter, sphereClass);
		if (collisionProxy != nullptr && uevr::API::UObjectHook::exists(collisionProxy))
		{
			collisionProxyCreated = true;
			auto proxyClass = collisionProxy->get_class();
			if (proxyClass != nullptr)
			{
				auto setRadius = proxyClass->find_function(L"SetSphereRadius");
				if (setRadius != nullptr)
				{
					std::vector<uint8_t> params(setRadius->get_properties_size());
					collisionProxyRadiusConfigured = SetReflectedFloatParameter(
						setRadius, params, L"NewRadius", 6.0f);
					if (collisionProxyRadiusConfigured)
						setRadius->call(collisionProxy, params.data());
				}
				// Some shipped UE reflection profiles omit the SetSphereRadius
				// parameter metadata even though the component property is present.
				// Set the known float property as a guarded fallback so the proxy does
				// not silently remain a zero-radius shape.
				if (!collisionProxyRadiusConfigured
					&& proxyClass->find_property(L"SphereRadius") != nullptr)
				{
					auto radiusData = collisionProxy->get_property_data<float>(
						L"SphereRadius");
					if (radiusData != nullptr)
					{
						*radiusData = 6.0f;
						collisionProxyRadiusConfigured = true;
					}
				}
				if (collisionProxyRadiusConfigured)
				{
					auto updateBodySetup = proxyClass->find_function(L"UpdateBodySetup");
					if (updateBodySetup != nullptr)
					{
						std::vector<uint8_t> params(updateBodySetup->get_properties_size());
						updateBodySetup->call(collisionProxy, params.data());
					}
				}
				auto setCollisionEnabled = proxyClass->find_function(
					L"SetCollisionEnabled");
				if (setCollisionEnabled != nullptr)
				{
					std::vector<uint8_t> params(setCollisionEnabled->get_properties_size());
					collisionProxyCollisionConfigured = SetReflectedByteParameter(
						setCollisionEnabled, params,
						L"NewType", 3)
						|| SetReflectedByteParameter(setCollisionEnabled, params,
							L"Type", 3);
					if (collisionProxyCollisionConfigured)
						setCollisionEnabled->call(collisionProxy, params.data());
				}
				auto setAllResponses = proxyClass->find_function(
					L"SetCollisionResponseToAllChannels");
				if (setAllResponses != nullptr)
				{
					std::vector<uint8_t> params(setAllResponses->get_properties_size());
					collisionProxyResponsesConfigured = SetReflectedByteParameter(
						setAllResponses, params,
						L"NewResponse", 2)
						|| SetReflectedByteParameter(setAllResponses, params,
							L"Response", 2);
					if (collisionProxyResponsesConfigured)
						setAllResponses->call(collisionProxy, params.data());
				}
				auto ignoreOwner = proxyClass->find_function(L"MoveIgnoreActorAdd");
				if (ignoreOwner != nullptr)
				{
					std::vector<uint8_t> params(ignoreOwner->get_properties_size());
					collisionProxyOwnerIgnored = SetReflectedObjectParameter(
						ignoreOwner, params, L"Actor", playerManager->playerCharacter);
					if (collisionProxyOwnerIgnored)
						ignoreOwner->call(collisionProxy, params.data());
				}
				auto registerComponent = proxyClass->find_function(L"RegisterComponent");
				if (registerComponent != nullptr)
				{
					std::vector<uint8_t> params(registerComponent->get_properties_size());
					registerComponent->call(collisionProxy, params.data());
					collisionProxyRegistered = true;
				}
			}
			SetComponentVisibility(collisionProxy, false);
			if (collisionProxy->get_class()->find_property(L"bHiddenInGame") != nullptr)
				collisionProxy->set_bool_property(L"bHiddenInGame", true);
			if (collisionProxy->get_class()->find_property(L"bGenerateOverlapEvents") != nullptr)
				collisionProxy->set_bool_property(L"bGenerateOverlapEvents", false);
		}
	}
	if (!collisionProxyCreated)
		uevr::API::get()->log_warn(
			"[MotionThrowable] collision proxy unavailable reason=class-or-component-create");
	auto visualClass = visual->get_class();
	bool collisionConfigured = false;
	if (visualClass != nullptr)
	{
		auto setCollisionEnabled = visualClass->find_function(L"SetCollisionEnabled");
		if (setCollisionEnabled != nullptr)
		{
			std::vector<uint8_t> params(setCollisionEnabled->get_properties_size());
			collisionConfigured = SetReflectedByteParameter(
				setCollisionEnabled, params, L"NewType", 3)
				|| SetReflectedByteParameter(setCollisionEnabled, params, L"Type", 3);
			if (collisionConfigured)
				setCollisionEnabled->call(visual, params.data());
		}
		auto setAllResponses = visualClass->find_function(
			L"SetCollisionResponseToAllChannels");
		if (setAllResponses != nullptr)
		{
			std::vector<uint8_t> params(setAllResponses->get_properties_size());
			const bool responseSet = SetReflectedByteParameter(
				setAllResponses, params, L"NewResponse", 2)
				|| SetReflectedByteParameter(setAllResponses, params, L"Response", 2);
			if (responseSet)
				setAllResponses->call(visual, params.data());
			collisionConfigured = collisionConfigured || responseSet;
		}
	}
	for (int32_t element = 0; element < 4; ++element)
	{
		auto material = ReadComponentMaterial(firstWeaponMesh, element);
		if (material == nullptr)
			break;
		SetComponentMaterial(visual, element, material);
	}
	auto sourceClass = firstWeaponMesh->get_class();
	EnsureMotionThrowableVisualRenderable(visual);
	auto getScale = sourceClass != nullptr
		? sourceClass->find_function(L"GetComponentScale") : nullptr;
	if (getScale == nullptr && sourceClass != nullptr)
		getScale = sourceClass->find_function(L"K2_GetComponentScale");
	auto setScale = visualClass != nullptr
		? visualClass->find_function(L"SetWorldScale3D") : nullptr;
	if (setScale == nullptr && visualClass != nullptr)
		setScale = visualClass->find_function(L"K2_SetWorldScale3D");
	glm::fvec3 appliedScale(1.0f);
	bool appliedScaleValid = false;
	if (getScale != nullptr)
	{
		Utilities::ParameterSingleVector3 scaleParams{};
		getScale->call(firstWeaponMesh, &scaleParams);
		const glm::fvec3 candidate = glm::abs(scaleParams.vec3Value);
		if (IsFiniteVector(candidate)
			&& candidate.x > 0.001f && candidate.y > 0.001f && candidate.z > 0.001f
			&& candidate.x < 100.0f && candidate.y < 100.0f && candidate.z < 100.0f)
		{
			appliedScale = candidate;
			appliedScaleValid = true;
		}
	}
	if (!appliedScaleValid && sourceClass != nullptr
		&& sourceClass->find_property(L"RelativeScale3D") != nullptr)
	{
		const auto scaleData = firstWeaponMesh->get_property_data<glm::fvec3>(L"RelativeScale3D");
		if (scaleData != nullptr)
		{
			const glm::fvec3 candidate = glm::abs(*scaleData);
			if (IsFiniteVector(candidate)
				&& candidate.x > 0.001f && candidate.y > 0.001f && candidate.z > 0.001f
				&& candidate.x < 100.0f && candidate.y < 100.0f && candidate.z < 100.0f)
			{
				appliedScale = candidate;
				appliedScaleValid = true;
			}
		}
	}
	if (setScale != nullptr)
	{
		ParameterSetScale3D setScaleParams{};
		setScaleParams.newScale3D = appliedScale;
		setScale->call(visual, &setScaleParams);
	}

	motionThrowableFlight.active = true;
	motionThrowableFlight.sequence = sequence;
	motionThrowableFlight.generation = interactionEngineTickGeneration;
	motionThrowableFlight.weaponType = weaponType;
	motionThrowableFlight.hand = hand;
	motionThrowableFlight.ownerCharacter = playerManager->playerCharacter;
	motionThrowableFlight.sourceMesh = firstWeaponMesh;
	motionThrowableFlight.visual = visual;
	motionThrowableFlight.collisionProxy = collisionProxy;
	motionThrowableFlight.position = position;
	motionThrowableFlight.previousPosition = position;
	motionThrowableFlight.linearVelocityUE = linearVelocityMps * 100.0f;
	motionThrowableFlight.angularVelocity = angularVelocity;
	motionThrowableFlight.rotation = glm::normalize(rotation);
	motionThrowableFlight.earlyDiagnosticLogged = false;
	motionThrowableFlight.midDiagnosticLogged = false;
	if (!SetMotionThrowableVisualTransform(visual, position, rotation))
	{
		ResetMotionThrowableFlight("initial-transform-failed", true);
		return false;
	}
	if (collisionProxy != nullptr
		&& !SetMotionThrowableVisualTransform(collisionProxy, position,
			glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f), false))
	{
		DestroyMotionThrowableFlightVisual(motionThrowableFlight);
		motionThrowableFlight = MotionThrowableFlight{};
		return false;
	}
	EnsureMotionThrowableVisualRenderable(visual);
	SetComponentVisibility(firstWeaponMesh, false);
	motionThrowableSourceHidden = true;
	glm::fvec3 visualReadback{};
	if (visualClass != nullptr
		&& visualClass->find_function(L"K2_GetComponentLocation") != nullptr)
	{
		Utilities::ParameterSingleVector3 locationParams{};
		visual->call_function(L"K2_GetComponentLocation", &locationParams);
		visualReadback = locationParams.vec3Value;
	}
	auto visualMesh = visualClass != nullptr
		&& visualClass->find_property(L"StaticMesh") != nullptr
		? visual->get_property<uevr::API::UObject*>(L"StaticMesh") : nullptr;
	auto visualParent = visualClass != nullptr
		&& visualClass->find_property(L"AttachParent") != nullptr
		? visual->get_property<uevr::API::UObject*>(L"AttachParent") : nullptr;
	const int visible = visualClass != nullptr
		&& visualClass->find_property(L"bVisible") != nullptr
		&& visual->get_bool_property(L"bVisible") ? 1 : 0;
	const int hiddenInGame = visualClass != nullptr
		&& visualClass->find_property(L"bHiddenInGame") != nullptr
		&& visual->get_bool_property(L"bHiddenInGame") ? 1 : 0;
	const int ownerNoSee = visualClass != nullptr
		&& visualClass->find_property(L"bOwnerNoSee") != nullptr
		&& visual->get_bool_property(L"bOwnerNoSee") ? 1 : 0;
	auto proxyClass = collisionProxy != nullptr ? collisionProxy->get_class() : nullptr;
	uevr::API::get()->log_info(
		"[MotionThrowableFlight] visual-ready seq=%u component=%p class=%ls mesh=%p parent=%p collision=%d proxy=%p proxyClass=%ls proxyCreated=%d proxyRegistered=%d proxyRadius=%d proxyCollision=%d proxyResponses=%d proxyIgnoreOwner=%d scale=(%.3f %.3f %.3f) scaleSource=%s readback=(%.2f %.2f %.2f) bVisible=%d bHiddenInGame=%d bOwnerNoSee=%d",
		sequence, visual, visualClass != nullptr ? visualClass->get_full_name().c_str() : L"<null>",
		visualMesh, visualParent, collisionConfigured ? 1 : 0,
		collisionProxy,
		proxyClass != nullptr ? proxyClass->get_full_name().c_str() : L"<null>",
		collisionProxyCreated ? 1 : 0, collisionProxyRegistered ? 1 : 0,
		collisionProxyRadiusConfigured ? 1 : 0,
		collisionProxyCollisionConfigured ? 1 : 0,
		collisionProxyResponsesConfigured ? 1 : 0,
		collisionProxyOwnerIgnored ? 1 : 0,
		appliedScale.x, appliedScale.y, appliedScale.z,
		appliedScaleValid ? "component-or-relative" : "unit-fallback",
		visualReadback.x, visualReadback.y, visualReadback.z,
		visible, hiddenInGame, ownerNoSee);
	uevr::API::get()->log_info(
		"[MotionThrowableFlight] begin seq=%u weapon=%d hand=%s originUE=(%.2f %.2f %.2f) velocity_mps=(%.3f %.3f %.3f) speed=%.3f angular=(%.3f %.3f %.3f) explosion=blocked-unproven ammo=unchanged",
		sequence, weaponType, hand == 0 ? "left" : "right",
		position.x, position.y, position.z,
		linearVelocityMps.x, linearVelocityMps.y, linearVelocityMps.z,
		glm::length(linearVelocityMps), angularVelocity.x,
		angularVelocity.y, angularVelocity.z);
	return true;
}

void WeaponManager::ProcessMotionThrowableFlight(float delta)
{
	if (!processingDetachedThrowableFlights)
	{
		processingDetachedThrowableFlights = true;
		// Run the current flight through the existing state machine first, then
		// temporarily reuse that state slot for each retained older proxy. This
		// keeps the collision/effect implementation single-sourced and bounds
		// detached work to the small list populated on regrip.
		ProcessMotionThrowableFlight(delta);
		const MotionThrowableFlight currentFlightAfterProcess = motionThrowableFlight;
		auto detachedFlights = std::move(motionThrowableDetachedFlights);
		motionThrowableDetachedFlights.clear();
		for (auto& detached : detachedFlights)
		{
			motionThrowableFlight = detached;
			ProcessMotionThrowableFlight(delta);
			if (motionThrowableFlight.active)
				motionThrowableDetachedFlights.push_back(motionThrowableFlight);
		}
		motionThrowableFlight = currentFlightAfterProcess;
		processingDetachedThrowableFlights = false;
		return;
	}
	if (!motionThrowableFlight.active)
		return;
	const bool contextValid = settingsManager != nullptr
		&& settingsManager->enableMotionThrowables
		&& !(motionThrowableFlight.weaponType == Molotov
			&& settingsManager->enableNativeMolotovMode)
		&& playerManager != nullptr && playerManager->isInControl
		&& !playerManager->isInVehicle && !playerManager->weaponWheelEnabled
		&& playerManager->playerCharacter == motionThrowableFlight.ownerCharacter
		&& motionThrowableFlight.ownerCharacter != nullptr
		&& uevr::API::UObjectHook::exists(motionThrowableFlight.ownerCharacter);
	if (!contextValid)
	{
		ResetMotionThrowableFlight("context-change", true);
		return;
	}
	if (!std::isfinite(delta) || delta <= 0.0001f || delta > 0.1f)
		return;
	if (motionThrowableFlight.visual == nullptr
		|| !uevr::API::UObjectHook::exists(motionThrowableFlight.visual))
	{
		ResetMotionThrowableFlight("visual-lost", true);
		return;
	}
	if (motionThrowableFlight.sourceMesh != nullptr
		&& uevr::API::UObjectHook::exists(motionThrowableFlight.sourceMesh))
		SetComponentVisibility(motionThrowableFlight.sourceMesh, false);
	EnsureMotionThrowableVisualRenderable(motionThrowableFlight.visual);

	const float step = (std::min)(delta, 0.05f);
	constexpr float NativeGrenadeFuseSeconds = 2.0f;
	if (motionThrowableFlight.weaponType == Grenade && motionThrowableFlight.settled)
	{
		motionThrowableFlight.ageSeconds += step;
		if (motionThrowableFlight.ageSeconds >= NativeGrenadeFuseSeconds)
		{
			const bool exploded = PlayMotionThrowableImpactEffect(Grenade,
				motionThrowableFlight.position);
			uevr::API::get()->log_info(
				"[MotionThrowableFlight] grenade fuse seq=%u age=%.3f state=settled point=(%.3f %.3f %.3f) explosion=%s",
				motionThrowableFlight.sequence, motionThrowableFlight.ageSeconds,
				motionThrowableFlight.position.x, motionThrowableFlight.position.y,
				motionThrowableFlight.position.z, exploded ? "de-native" : "failed");
			ResetMotionThrowableFlight("fuse-expired", true);
		}
		return;
	}
	const glm::fvec3 previousPosition = motionThrowableFlight.position;
	const glm::fvec3 previousVelocity = motionThrowableFlight.linearVelocityUE;
	// Use a deliberately reduced VR throw gravity. Native GTA's projectile
	// gravity is not available to this visual proxy, and full 9.8 m/s^2 makes
	// a hand-thrown bottle drop almost vertically before it can be seen.
	constexpr float MotionThrowableGravityCmPerSecondSquared = -500.0f;
	glm::fvec3 nextVelocity = previousVelocity
		+ glm::fvec3(0.0f, 0.0f, MotionThrowableGravityCmPerSecondSquared) * step;
	nextVelocity *= std::exp(-0.08f * step);
	const glm::fvec3 nextPosition = previousPosition
		+ (previousVelocity + nextVelocity) * (0.5f * step);
	if (!IsFiniteVector(nextPosition) || !IsFiniteVector(nextVelocity))
	{
		ResetMotionThrowableFlight("non-finite-flight", true);
		return;
	}

	bool collided = false;
	bool worldSweep = false;
	bool collisionProxySweep = false;
	glm::fvec3 impactPoint = nextPosition;
	glm::fvec3 impactNormal(0.0f, 0.0f, 1.0f);
	MemoryManager::NativeMeleeContact contact{};
	const glm::fvec3 displacement = nextPosition - previousPosition;
	const float distance = glm::length(displacement);
	const auto toNativePoint = [](const glm::fvec3& uePoint) {
		return glm::fvec3(uePoint.x * 0.01f, -uePoint.y * 0.01f,
			uePoint.z * 0.01f);
	};
	const auto toUnrealPoint = [](const std::array<float, 3>& nativePoint) {
		return glm::fvec3(nativePoint[0] * 100.0f,
			-nativePoint[1] * 100.0f, nativePoint[2] * 100.0f);
	};
	MotionThrowableWorldSweepHit worldHit{};
	bool worldTraceAvailable = false;
	if (playerManager != nullptr && playerManager->playerActor != nullptr
		&& uevr::API::UObjectHook::exists(playerManager->playerActor))
	{
		constexpr float MotionThrowableSweepRadiusCm = 5.5f;
		// SphereTraceSingle takes ETraceTypeQuery, not a raw ECC label. The
		// project mapping is runtime-specific, so probe the standard query slots
		// once per movement step until one actually reports world geometry, then
		// retain that working slot for subsequent flights.
		static constexpr std::array<uint8_t, 8> MotionThrowableTraceChannels{
			3, 0, 1, 2, 4, 5, 6, 7 };
		const uint8_t traceChannel = motionThrowableWorldTraceChannel >= 0
			? static_cast<uint8_t>(motionThrowableWorldTraceChannel)
			: MotionThrowableTraceChannels[motionThrowableWorldTraceProbeCursor
				% MotionThrowableTraceChannels.size()];
		worldSweep = SweepMotionThrowableWorld(playerManager->playerActor,
			previousPosition, nextPosition, MotionThrowableSweepRadiusCm,
			traceChannel, worldTraceAvailable, worldHit);
		if (motionThrowableWorldTraceChannel < 0)
		{
			motionThrowableWorldTraceProbeCursor = static_cast<uint8_t>(
				(motionThrowableWorldTraceProbeCursor + 1)
				% MotionThrowableTraceChannels.size());
			if (worldSweep)
			{
				motionThrowableWorldTraceChannel = traceChannel;
				uevr::API::get()->log_info(
					"[MotionThrowable] world sphere sweep selected channel=%u",
					static_cast<unsigned int>(traceChannel));
			}
		}
		if (worldTraceAvailable && !motionThrowableWorldSweepReadyLogged)
		{
			motionThrowableWorldSweepReadyLogged = true;
			uevr::API::get()->log_info(
				"[MotionThrowable] world sphere sweep ready radius_cm=%.1f traceQuery=%u",
				MotionThrowableSweepRadiusCm,
				static_cast<unsigned int>(traceChannel));
		}
		if (!worldTraceAvailable && !motionThrowableWorldSweepFailureLogged)
		{
			motionThrowableWorldSweepFailureLogged = true;
			uevr::API::get()->log_warn(
				"[MotionThrowable] world sphere sweep unavailable reason=reflection-or-params");
		}
	}
	if (worldSweep)
	{
		collided = true;
		impactPoint = worldHit.point;
		impactNormal = worldHit.normal;
	}
	if (!collided && motionThrowableFlight.collisionProxy != nullptr)
	{
		MotionThrowableWorldSweepHit proxyHit{};
		if (MoveMotionThrowableCollisionProxy(
			motionThrowableFlight.collisionProxy, nextPosition, proxyHit))
		{
			collided = true;
			collisionProxySweep = true;
			impactPoint = proxyHit.point;
			impactNormal = proxyHit.normal;
		}
	}
	if (memoryManager != nullptr && std::isfinite(distance) && distance > 0.001f)
	{
		// QueryNativeLineOfSightEntity intentionally rejects segments longer than
		// 3 cm. Keep every flight segment below that bound even during a fast
		// release or a gravity step; the old 32-segment cap could produce a longer
		// final segment and silently discard the entire collision probe.
		constexpr float NativeThrowableTraceSegmentCm = 2.0f;
		constexpr int NativeThrowableTraceMaxSegments = 128;
		const int segmentCount = (std::clamp)(
			static_cast<int>(std::ceil(distance / NativeThrowableTraceSegmentCm)),
			1, NativeThrowableTraceMaxSegments);
		for (int segment = 0; segment < segmentCount && !collided; ++segment)
		{
			const float startT = static_cast<float>(segment)
				/ static_cast<float>(segmentCount);
			const float endT = static_cast<float>(segment + 1)
				/ static_cast<float>(segmentCount);
			const glm::fvec3 segmentStart = glm::mix(previousPosition, nextPosition, startT);
			const glm::fvec3 segmentEnd = glm::mix(previousPosition, nextPosition, endT);
			const glm::fvec3 nativeStart = toNativePoint(segmentStart);
			const glm::fvec3 nativeEnd = toNativePoint(segmentEnd);
			collided = memoryManager->QueryNativeLineOfSightEntity(
				{ nativeStart.x, nativeStart.y, nativeStart.z },
				{ nativeEnd.x, nativeEnd.y, nativeEnd.z }, contact, false);
		}
	}
	if (collided)
	{
		if (!worldSweep && !collisionProxySweep)
		{
			impactPoint = toUnrealPoint(contact.point);
			impactNormal = glm::fvec3(contact.normal[0],
				-contact.normal[1], contact.normal[2]);
		}
		if (IsFiniteVector(impactPoint))
		{
			motionThrowableFlight.position = impactPoint;
			SetMotionThrowableVisualTransform(motionThrowableFlight.visual,
				impactPoint, motionThrowableFlight.rotation);
		}
		if (motionThrowableFlight.weaponType == Grenade)
		{
			glm::fvec3 normal = NormalizeOrZero(impactNormal);
			if (glm::length(normal) < 0.5f)
				normal = glm::fvec3(0.0f, 0.0f, 1.0f);
			glm::fvec3 bouncedVelocity = nextVelocity;
			const float intoSurface = glm::dot(bouncedVelocity, normal);
			if (intoSurface < 0.0f)
				bouncedVelocity -= normal * (1.35f * intoSurface);
			bouncedVelocity *= 0.62f;
			motionThrowableFlight.settled = !IsFiniteVector(bouncedVelocity)
				|| glm::length(bouncedVelocity) < 65.0f
				|| motionThrowableFlight.bounceCount >= 5;
			if (motionThrowableFlight.settled)
				bouncedVelocity = glm::fvec3(0.0f);
			motionThrowableFlight.position = impactPoint + normal * 6.0f;
			motionThrowableFlight.previousPosition = motionThrowableFlight.position;
			motionThrowableFlight.linearVelocityUE = bouncedVelocity;
			motionThrowableFlight.angularVelocity *= 0.65f;
			motionThrowableFlight.ageSeconds += step;
			++motionThrowableFlight.bounceCount;
			SetMotionThrowableVisualTransform(motionThrowableFlight.visual,
				motionThrowableFlight.position, motionThrowableFlight.rotation, false);
			uevr::API::get()->log_info(
				"[MotionThrowableFlight] grenade contact seq=%u bounce=%u age=%.3f source=%s point=(%.3f %.3f %.3f) normal=(%.3f %.3f %.3f) speed_mps=%.3f settled=%s",
				motionThrowableFlight.sequence, motionThrowableFlight.bounceCount,
				motionThrowableFlight.ageSeconds,
				worldSweep ? "world-sphere-sweep"
					: (collisionProxySweep ? "sphere-component-sweep" : "native-los"),
				impactPoint.x, impactPoint.y, impactPoint.z,
				normal.x, normal.y, normal.z,
				glm::length(bouncedVelocity) * 0.01f,
				motionThrowableFlight.settled ? "true" : "false");
			if (motionThrowableFlight.ageSeconds >= NativeGrenadeFuseSeconds)
			{
				const bool exploded = PlayMotionThrowableImpactEffect(Grenade,
					motionThrowableFlight.position, &contact);
				uevr::API::get()->log_info(
					"[MotionThrowableFlight] grenade fuse seq=%u age=%.3f state=contact explosion=%s",
					motionThrowableFlight.sequence, motionThrowableFlight.ageSeconds,
					exploded ? "de-native" : "failed");
				ResetMotionThrowableFlight("fuse-expired", true);
			}
			return;
		}
		const bool impactEffectPlayed = IsFiniteVector(impactPoint)
			&& PlayMotionThrowableImpactEffect(motionThrowableFlight.weaponType,
				impactPoint, &contact);
		uevr::API::get()->log_info(
			"[MotionThrowableFlight] impact seq=%u weapon=%d hand=%s source=%s point=(%.3f %.3f %.3f) entity=0x%llX type=%u speed_mps=%.3f explosion=%s ammo=unchanged",
			motionThrowableFlight.sequence, motionThrowableFlight.weaponType,
			motionThrowableFlight.hand == 0 ? "left" : "right",
			worldSweep ? "world-sphere-sweep"
				: (collisionProxySweep ? "sphere-component-sweep" : "native-los"),
			impactPoint.x, impactPoint.y, impactPoint.z,
			static_cast<unsigned long long>(contact.entity),
			static_cast<unsigned int>(contact.entityType),
			glm::length(nextVelocity) * 0.01f,
			impactEffectPlayed ? "visual-audio" : "unavailable-native-damage-unproven");
		ResetMotionThrowableFlight("impact-probe-complete", true);
		return;
	}

	motionThrowableFlight.previousPosition = previousPosition;
	motionThrowableFlight.position = nextPosition;
	motionThrowableFlight.linearVelocityUE = nextVelocity;
	const float angularSpeed = glm::length(motionThrowableFlight.angularVelocity);
	if (std::isfinite(angularSpeed) && angularSpeed > 0.001f)
	{
		const glm::fvec3 axis = motionThrowableFlight.angularVelocity / angularSpeed;
		motionThrowableFlight.rotation = glm::normalize(glm::angleAxis(
			angularSpeed * step, axis) * motionThrowableFlight.rotation);
	}
	motionThrowableFlight.ageSeconds += step;
	if (!SetMotionThrowableVisualTransform(motionThrowableFlight.visual,
		motionThrowableFlight.position, motionThrowableFlight.rotation, false))
	{
		ResetMotionThrowableFlight("transform-update-failed", true);
		return;
	}
	if (motionThrowableFlight.weaponType == Grenade
		&& motionThrowableFlight.ageSeconds >= NativeGrenadeFuseSeconds)
	{
		const bool exploded = PlayMotionThrowableImpactEffect(Grenade,
			motionThrowableFlight.position);
		uevr::API::get()->log_info(
			"[MotionThrowableFlight] grenade fuse seq=%u age=%.3f state=airborne point=(%.3f %.3f %.3f) explosion=%s",
			motionThrowableFlight.sequence, motionThrowableFlight.ageSeconds,
			motionThrowableFlight.position.x, motionThrowableFlight.position.y,
			motionThrowableFlight.position.z, exploded ? "de-native" : "failed");
		ResetMotionThrowableFlight("fuse-expired", true);
		return;
	}
	// A static-mesh proxy can provide a second, engine-side collision signal even
	// when the native DE LOS helper returns no entity for world geometry. A sweep
	// that stops short of its requested point is treated as an impact; the
	// native LOS/entity path above remains authoritative for actors.
	glm::fvec3 sweptPosition{};
	bool sweepBlocked = false;
	if (motionThrowableFlight.visual != nullptr
		&& uevr::API::UObjectHook::exists(motionThrowableFlight.visual))
	{
		auto visualClass = motionThrowableFlight.visual->get_class();
		auto getLocation = visualClass != nullptr
			? visualClass->find_function(L"K2_GetComponentLocation") : nullptr;
		if (getLocation != nullptr)
		{
			Utilities::ParameterSingleVector3 locationParams{};
			getLocation->call(motionThrowableFlight.visual, &locationParams);
			sweptPosition = locationParams.vec3Value;
			const float requestedDistance = glm::length(
				nextPosition - previousPosition);
			const float blockedDistance = glm::length(sweptPosition - nextPosition);
			sweepBlocked = IsFiniteVector(sweptPosition)
				&& std::isfinite(requestedDistance)
				&& std::isfinite(blockedDistance)
				&& requestedDistance > 1.0f
				&& blockedDistance > (std::max)(1.0f, requestedDistance * 0.25f);
		}
	}
	if (sweepBlocked)
	{
		motionThrowableFlight.position = sweptPosition;
		if (motionThrowableFlight.weaponType == Grenade)
		{
			motionThrowableFlight.linearVelocityUE = glm::fvec3(0.0f);
			motionThrowableFlight.angularVelocity *= 0.5f;
			motionThrowableFlight.settled = true;
			++motionThrowableFlight.bounceCount;
			uevr::API::get()->log_info(
				"[MotionThrowableFlight] grenade contact seq=%u bounce=%u age=%.3f source=component-sweep point=(%.3f %.3f %.3f) settled=true",
				motionThrowableFlight.sequence, motionThrowableFlight.bounceCount,
				motionThrowableFlight.ageSeconds, sweptPosition.x,
				sweptPosition.y, sweptPosition.z);
			return;
		}
		const bool impactEffectPlayed = PlayMotionThrowableImpactEffect(
			motionThrowableFlight.weaponType, sweptPosition);
		uevr::API::get()->log_info(
			"[MotionThrowableFlight] impact seq=%u weapon=%d hand=%s source=component-sweep point=(%.3f %.3f %.3f) entity=0x0 type=0 speed_mps=%.3f explosion=%s ammo=unchanged",
			motionThrowableFlight.sequence, motionThrowableFlight.weaponType,
			motionThrowableFlight.hand == 0 ? "left" : "right",
			sweptPosition.x, sweptPosition.y, sweptPosition.z,
			glm::length(nextVelocity) * 0.01f,
			impactEffectPlayed ? "visual-and-audio" : "unavailable");
		ResetMotionThrowableFlight("impact-probe-complete", true);
		return;
	}
	const auto logFlightDiagnostic = [&](const char* phase) {
		if (motionThrowableFlight.visual == nullptr
			|| !uevr::API::UObjectHook::exists(motionThrowableFlight.visual))
			return;
		auto visualClass = motionThrowableFlight.visual->get_class();
		glm::fvec3 readback{};
		if (visualClass != nullptr
			&& visualClass->find_function(L"K2_GetComponentLocation") != nullptr)
		{
			Utilities::ParameterSingleVector3 locationParams{};
			motionThrowableFlight.visual->call_function(
				L"K2_GetComponentLocation", &locationParams);
			readback = locationParams.vec3Value;
		}
		const int visible = visualClass != nullptr
			&& visualClass->find_property(L"bVisible") != nullptr
			&& motionThrowableFlight.visual->get_bool_property(L"bVisible") ? 1 : 0;
		const int hiddenInGame = visualClass != nullptr
			&& visualClass->find_property(L"bHiddenInGame") != nullptr
			&& motionThrowableFlight.visual->get_bool_property(L"bHiddenInGame") ? 1 : 0;
		uevr::API::get()->log_info(
			"[MotionThrowableFlight] diagnostic seq=%u phase=%s age=%.3f state=(%.2f %.2f %.2f) readback=(%.2f %.2f %.2f) velocity_mps=(%.3f %.3f %.3f) visible=%d hiddenInGame=%d",
			motionThrowableFlight.sequence, phase, motionThrowableFlight.ageSeconds,
			motionThrowableFlight.position.x, motionThrowableFlight.position.y,
			motionThrowableFlight.position.z, readback.x, readback.y, readback.z,
			motionThrowableFlight.linearVelocityUE.x * 0.01f,
			motionThrowableFlight.linearVelocityUE.y * 0.01f,
			motionThrowableFlight.linearVelocityUE.z * 0.01f,
			visible, hiddenInGame);
	};
	if (!motionThrowableFlight.earlyDiagnosticLogged
		&& motionThrowableFlight.ageSeconds >= 0.08f)
	{
		motionThrowableFlight.earlyDiagnosticLogged = true;
		logFlightDiagnostic("early");
	}
	if (!motionThrowableFlight.midDiagnosticLogged
		&& motionThrowableFlight.ageSeconds >= 0.50f)
	{
		motionThrowableFlight.midDiagnosticLogged = true;
		logFlightDiagnostic("mid");
	}
	// This is only a fail-safe for an unreachable/missed collision probe. Normal
	// flights end on native LOS or the bounded ground crossing, and native GTA
	// projectile ownership ends them earlier once the release pulse is accepted.
	if (motionThrowableFlight.ageSeconds >= 6.0f)
		ResetMotionThrowableFlight("timeout", true);
}

void WeaponManager::ResetPhysicalThrowableProbe(const char* reason, bool logCancellation)
{
	if (throwableProbeActive && logCancellation)
	{
		uevr::API::get()->log_info(
			"[ThrowableProbe] cancel seq=%u weapon=%d reason=%s native=unchanged",
			throwableProbeSequence, static_cast<int>(currentWeaponEquipped),
			reason != nullptr ? reason : "unspecified");
	}
	throwableProbeActive = false;
	throwableProbeSequence = 0;
	throwableProbeHandMask = 0;
	throwableProbeGripMask = 0;
	throwableProbePoseValid = { false, false };
	throwableProbeLastPoseFromVisibleBone = { false, false };
	throwableProbePreviousWorldPositions = {};
	throwableProbeLastWorldPositions = {};
	throwableProbeLastDirections = {};
	throwableProbePreviousWorldRotations = {
		glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f),
		glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f) };
	throwableProbeLastWorldRotations = throwableProbePreviousWorldRotations;
	throwableProbeSmoothedVelocityMps = {};
	throwableProbeSmoothedAngularVelocity = {};
	throwableProbeSampleCounts = { 0, 0 };
	throwableProbeVelocityHistoryMps = {};
	throwableProbeVelocityHistoryTimes = {};
	throwableProbePositionHistoryM = {};
	throwableProbePositionHistoryTimes = {};
	throwableProbeVelocityHistoryCursor = {};
	motionThrowableVehicleBurns.clear();
}

void WeaponManager::ProcessPhysicalThrowableProbe(float delta)
{
	if (memoryManager != nullptr)
	{
		MemoryManager::NativeThrowableLaunchProbe nativeLaunchProbe{};
		if (memoryManager->ReadLatestNativeThrowableLaunchProbe(nativeLaunchProbe))
		{
			const bool nativeMode = settingsManager != nullptr
				&& settingsManager->enableNativeMolotovMode;
			uevr::API::get()->log_info(
				"[NativeThrowableProbe] launch seq=%u weapon=%d mode=%s overridden=%s identity=local-player origin=(%.3f %.3f %.3f) direction=%s(%.4f %.4f %.4f) force=%.4f target=0x%llX",
				nativeLaunchProbe.sequence, nativeLaunchProbe.weaponType,
				nativeMode ? "native" : "custom-or-native",
				nativeLaunchProbe.overridden ? "true" : "false",
				nativeLaunchProbe.rawOrigin[0], nativeLaunchProbe.rawOrigin[1],
				nativeLaunchProbe.rawOrigin[2],
				nativeLaunchProbe.directionSupplied ? "provided" : "none",
				nativeLaunchProbe.rawDirection[0], nativeLaunchProbe.rawDirection[1],
				nativeLaunchProbe.rawDirection[2], nativeLaunchProbe.force,
				static_cast<unsigned long long>(nativeLaunchProbe.target));
		}
	}
	const bool nativeMolotovOnly = currentWeaponEquipped == Molotov
		&& settingsManager != nullptr && settingsManager->enableNativeMolotovMode;
	const bool motionEnabled = settingsManager != nullptr
		&& settingsManager->enableMotionThrowables && !nativeMolotovOnly;
	const bool probeEnabled = settingsManager != nullptr
		&& !nativeMolotovOnly
		&& (motionEnabled || settingsManager->enableThrowableMotionProbe);
	const ULONGLONG now = GetTickCount64();
	ProcessMotionThrowableImpactVisuals();
	ProcessMotionThrowableVehicleBurns();
	ProcessMotionThrowableFlight(delta);
	if (throwableMotionHiddenMesh != nullptr
		&& throwableMotionVisualRestoreAt != 0
		&& now >= throwableMotionVisualRestoreAt)
	{
		if (throwableMotionHiddenMesh == firstWeaponMesh
			&& uevr::API::UObjectHook::exists(throwableMotionHiddenMesh)
			&& currentWeaponEquipped >= Grenade && currentWeaponEquipped <= Molotov)
			SetComponentVisibility(throwableMotionHiddenMesh, true);
		throwableMotionHiddenMesh = nullptr;
		throwableMotionVisualRestoreAt = 0;
	}
	uint32_t consumedSequence = 0;
	if (memoryManager != nullptr
		&& memoryManager->ConsumeNativeThrowableMotionApplied(consumedSequence))
	{
		uevr::API::get()->log_info(
			"[MotionThrowable] native launch consumed seq=%u weapon=%d",
			consumedSequence, throwableMotionOverrideWeapon);
		if (consumedSequence == throwableMotionOverrideSequence)
		{
			uevr::API::UObject* proxySourceMesh = nullptr;
			if (motionThrowableFlight.active
				&& motionThrowableFlight.sequence == consumedSequence)
				proxySourceMesh = motionThrowableFlight.sourceMesh;
			if (proxySourceMesh != nullptr)
				ResetMotionThrowableFlight("native-launch-owned", true);
			throwableMotionOverrideSequence = 0;
			throwableMotionOverrideExpiresAt = 0;
			throwableMotionOverrideWeapon = -1;
			// The native projectile now owns the visible object. Permit GTA's next
			// inventory bottle to reappear after a short handoff interval.
			if (proxySourceMesh != nullptr
				&& uevr::API::UObjectHook::exists(proxySourceMesh))
			{
				throwableMotionHiddenMesh = proxySourceMesh;
				SetComponentVisibility(throwableMotionHiddenMesh, false);
				throwableMotionVisualRestoreAt = now + 220;
			}
			else if (throwableMotionHiddenMesh != nullptr)
				throwableMotionVisualRestoreAt = now + 220;
		}
	}
	if (throwableMotionOverrideSequence != 0
		&& throwableMotionOverrideExpiresAt != 0
		&& now >= throwableMotionOverrideExpiresAt)
	{
		if (memoryManager != nullptr)
			memoryManager->ClearNativeThrowableMotionOverride();
		uevr::API::get()->log_warn(
			"[MotionThrowable] native launch expired seq=%u weapon=%d",
			throwableMotionOverrideSequence, throwableMotionOverrideWeapon);
		throwableMotionOverrideSequence = 0;
		throwableMotionOverrideExpiresAt = 0;
		throwableMotionOverrideWeapon = -1;
		if (throwableMotionHiddenMesh != nullptr)
			throwableMotionVisualRestoreAt = now;
	}
	if (!probeEnabled)
	{
		throwableProbeEdgePending.store(false, std::memory_order_release);
		throwableProbeReleasePending.store(false, std::memory_order_release);
		throwableProbeCancelPending.store(false, std::memory_order_release);
		if (memoryManager != nullptr)
			memoryManager->ClearNativeThrowableMotionOverride();
		throwableMotionOverrideSequence = 0;
		throwableMotionOverrideExpiresAt = 0;
		throwableMotionOverrideWeapon = -1;
		if (throwableMotionHiddenMesh != nullptr)
			throwableMotionVisualRestoreAt = now;
		ResetMotionThrowableFlight("disabled", true);
		ResetPhysicalThrowableProbe("disabled", false);
		return;
	}

	if (throwableProbeCancelPending.exchange(false, std::memory_order_acq_rel))
		ResetPhysicalThrowableProbe("lua-transition", true);

	const bool contextValid = playerManager != nullptr
		&& playerManager->isInControl
		&& !playerManager->isInVehicle
		&& !playerManager->weaponWheelEnabled
		&& currentWeaponEquipped >= Grenade
		&& currentWeaponEquipped <= Molotov;

	if (throwableProbeEdgePending.exchange(false, std::memory_order_acq_rel))
	{
		const uint32_t sequence = throwableProbeEdgeSequence.load(std::memory_order_relaxed);
		const uint8_t handMask = throwableProbeEdgeHandMask.load(std::memory_order_relaxed) & 3U;
		const uint8_t gripMask = throwableProbeEdgeGripMask.load(std::memory_order_relaxed) & 3U;
		const int luaWeaponId = throwableProbeEdgeLuaWeapon.load(std::memory_order_relaxed);
		ResetPhysicalThrowableProbe("new-edge", false);
		const uint8_t heldHandMask = static_cast<uint8_t>(handMask & gripMask);
		if (contextValid && sequence != 0 && heldHandMask != 0)
		{
			const bool customFlightOwnsSlot =
				(currentWeaponEquipped == Grenade || currentWeaponEquipped == Molotov)
				&& motionThrowableFlight.active;
			if (customFlightOwnsSlot)
			{
				// A new grip is a new held bottle even if an older proxy is still
				// airborne. Keep the old proxy in the bounded detached list instead
				// of rejecting the grip and leaving the source bottle hidden.
				uevr::API::get()->log_info(
					"[MotionThrowableFlight] grip retains active seq=%u oldSeq=%u age=%.3f detached=%zu",
					sequence, motionThrowableFlight.sequence,
					motionThrowableFlight.ageSeconds,
					motionThrowableDetachedFlights.size());
				constexpr size_t MotionThrowableMaximumDetachedFlights = 3;
				if (motionThrowableDetachedFlights.size()
					>= MotionThrowableMaximumDetachedFlights)
				{
					DestroyMotionThrowableFlightVisual(
						motionThrowableDetachedFlights.front());
					motionThrowableDetachedFlights.erase(
						motionThrowableDetachedFlights.begin());
				}
				MotionThrowableFlight detachedFlight = motionThrowableFlight;
				// The native source remains hidden for the new handoff and must
				// never be restored by cleanup of an older detached proxy.
				detachedFlight.sourceMesh = nullptr;
				motionThrowableDetachedFlights.push_back(detachedFlight);
				motionThrowableFlight = MotionThrowableFlight{};
				if (memoryManager != nullptr)
					memoryManager->ClearNativeThrowableMotionOverride();
				throwableMotionOverrideSequence = 0;
				throwableMotionOverrideExpiresAt = 0;
				throwableMotionOverrideWeapon = -1;
				if (firstWeaponMesh != nullptr
					&& uevr::API::UObjectHook::exists(firstWeaponMesh))
					SetComponentVisibility(firstWeaponMesh, true);
				motionThrowableSourceHidden = false;
				throwableProbeActive = true;
				throwableProbeSequence = sequence;
				throwableProbeHandMask = heldHandMask;
				throwableProbeGripMask = gripMask;
			}
			else
			{
				if (motionThrowableSourceHidden && firstWeaponMesh != nullptr
					&& uevr::API::UObjectHook::exists(firstWeaponMesh))
					SetComponentVisibility(firstWeaponMesh, true);
				motionThrowableSourceHidden = false;
				ResetMotionThrowableFlight("new-grip", true);
				if (memoryManager != nullptr)
					memoryManager->ClearNativeThrowableMotionOverride();
				throwableMotionOverrideSequence = 0;
				throwableMotionOverrideExpiresAt = 0;
				throwableMotionOverrideWeapon = -1;
				throwableProbeActive = true;
				throwableProbeSequence = sequence;
				throwableProbeHandMask = heldHandMask;
				throwableProbeGripMask = gripMask;
				uevr::API::get()->log_info(
					"[ThrowableProbe] edge seq=%u weapon=%d luaWeapon=%d hands=%u grips=%u source=grip custom-flight=armed",
					sequence, static_cast<int>(currentWeaponEquipped), luaWeaponId,
					static_cast<unsigned int>(heldHandMask),
					static_cast<unsigned int>(gripMask));
			}
		}
		else if (settingsManager->debugInputLayerProbe
			|| (luaWeaponId >= Grenade && luaWeaponId <= Molotov))
		{
			uevr::API::get()->log_info(
				"[ThrowableProbe] edge rejected seq=%u nativeWeapon=%d luaWeapon=%d hands=%u reason=context",
				sequence, static_cast<int>(currentWeaponEquipped), luaWeaponId,
				static_cast<unsigned int>(handMask));
		}
	}

	if (throwableProbeActive && !contextValid)
	{
		if (memoryManager != nullptr)
			memoryManager->ClearNativeThrowableMotionOverride();
		if (throwableMotionHiddenMesh != nullptr)
			throwableMotionVisualRestoreAt = now;
		ResetPhysicalThrowableProbe("authoritative-context-change", true);
	}

	if (throwableProbeActive && delta > 0.001f && delta <= 0.1f)
	{
		for (int hand = 0; hand < 2; ++hand)
		{
			const uint8_t bit = static_cast<uint8_t>(1U << hand);
			if ((throwableProbeHandMask & bit) == 0)
				continue;

			glm::fvec3 controllerPosition{};
			glm::fquat controllerRotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
			glm::fvec3 worldPosition{};
			glm::fquat worldRotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
			bool usedVisibleBone = false;
			if (!ReadControllerCalibrationPose(hand, controllerPosition, controllerRotation)
				|| !ReadMotionMeleeHandWorldPose(hand, controllerPosition,
					controllerRotation, worldPosition, worldRotation, usedVisibleBone))
				continue;

			const glm::fvec3 direction = NormalizeOrZero(
				worldRotation * glm::fvec3(1.0f, 0.0f, 0.0f));
			if (glm::length(direction) <= 0.5f)
				continue;

			glm::fvec3 bodyOrigin = playerManager->actualPlayerHeadPositionUE;
			if (!IsFiniteVector(bodyOrigin) || glm::length(bodyOrigin) <= 0.001f)
				bodyOrigin = playerManager->actualPlayerPositionUE;
			const glm::fvec3 handRelativePositionM =
				(worldPosition - bodyOrigin) * 0.01f;
			glm::fvec3 instantaneousVelocityMps(0.0f);
			bool velocitySampleValid = false;
			if (throwableProbePoseValid[hand])
			{
				// Throw momentum comes from the same visible hand bone used by melee.
				// Keep it body-relative so locomotion is not mistaken for a throw.
				const glm::fvec3 handDeltaM = handRelativePositionM
					- throwableProbePreviousWorldPositions[hand];
				instantaneousVelocityMps = handDeltaM / delta;
				if (IsFiniteVector(instantaneousVelocityMps)
					&& glm::length(instantaneousVelocityMps) <= 20.0f)
				{
					velocitySampleValid = true;
					throwableProbeSmoothedVelocityMps[hand] =
						throwableProbeSmoothedVelocityMps[hand] * 0.25f
						+ instantaneousVelocityMps * 0.75f;
				}
				glm::fquat deltaRotation = glm::normalize(worldRotation
					* glm::inverse(throwableProbePreviousWorldRotations[hand]));
				if (deltaRotation.w < 0.0f)
					deltaRotation = glm::fquat::wxyz(-deltaRotation.w,
						-deltaRotation.x, -deltaRotation.y, -deltaRotation.z);
				const float clampedW = (std::clamp)(deltaRotation.w, -1.0f, 1.0f);
				const float angle = 2.0f * std::acos(clampedW);
				const float sinHalf = std::sqrt((std::max)(0.0f, 1.0f - clampedW * clampedW));
				if (std::isfinite(angle) && sinHalf > 0.0001f)
				{
					const glm::fvec3 axis(deltaRotation.x / sinHalf,
						deltaRotation.y / sinHalf, deltaRotation.z / sinHalf);
					const glm::fvec3 instantaneousAngularVelocity = axis * (angle / delta);
					if (IsFiniteVector(instantaneousAngularVelocity)
						&& glm::length(instantaneousAngularVelocity) <= 50.0f)
					{
						throwableProbeSmoothedAngularVelocity[hand] =
							throwableProbeSmoothedAngularVelocity[hand] * 0.25f
							+ instantaneousAngularVelocity * 0.75f;
					}
				}
			}
			else
			{
				throwableProbeSmoothedVelocityMps[hand] = glm::fvec3(0.0f);
				throwableProbeSmoothedAngularVelocity[hand] = glm::fvec3(0.0f);
				throwableProbePoseValid[hand] = true;
			}
			if (IsFiniteVector(handRelativePositionM))
			{
				const size_t sampleIndex = throwableProbeVelocityHistoryCursor[hand]
					% ThrowableVelocityHistorySize;
				throwableProbePositionHistoryM[hand][sampleIndex] = handRelativePositionM;
				throwableProbePositionHistoryTimes[hand][sampleIndex] = now;
				throwableProbeVelocityHistoryMps[hand][sampleIndex] =
					velocitySampleValid ? instantaneousVelocityMps : glm::fvec3(0.0f);
				throwableProbeVelocityHistoryTimes[hand][sampleIndex] = now;
				++throwableProbeVelocityHistoryCursor[hand];
			}
			throwableProbeLastPoseFromVisibleBone[hand] = usedVisibleBone;
			throwableProbePreviousWorldPositions[hand] = handRelativePositionM;
			throwableProbeLastWorldPositions[hand] = worldPosition;
			throwableProbeLastDirections[hand] = direction;
			throwableProbePreviousWorldRotations[hand] = worldRotation;
			throwableProbeLastWorldRotations[hand] = worldRotation;
			++throwableProbeSampleCounts[hand];
		}
	}

	if (!throwableProbeReleasePending.exchange(false, std::memory_order_acq_rel))
		return;

	const uint32_t sequence = throwableProbeReleaseSequence.load(std::memory_order_relaxed);
	const uint8_t releaseHandMask = throwableProbeReleaseHandMask.load(std::memory_order_relaxed) & 3U;
	const uint8_t releaseGripMask = throwableProbeReleaseGripMask.load(std::memory_order_relaxed) & 3U;
	const uint32_t holdMilliseconds = throwableProbeReleaseHoldMilliseconds.load(std::memory_order_relaxed);
	const int luaWeaponId = throwableProbeReleaseLuaWeapon.load(std::memory_order_relaxed);
	if (!throwableProbeActive || sequence != throwableProbeSequence)
		return;

	throwableProbeHandMask |= static_cast<uint8_t>(releaseHandMask & releaseGripMask);
	throwableProbeGripMask |= releaseGripMask;
	const uint8_t releasedHandMask = static_cast<uint8_t>(releaseHandMask & releaseGripMask);
	int selectedHand = -1;
	// Prefer the hand whose grip actually released. The configured primary hand
	// is only a fallback; choosing it first can discard the moving hand when both
	// hands were held during the same throwable interaction.
	if (releasedHandMask != 0)
	{
		if ((releasedHandMask & 1U) != 0 && (releasedHandMask & 2U) == 0)
			selectedHand = 0;
		else if ((releasedHandMask & 2U) != 0 && (releasedHandMask & 1U) == 0)
			selectedHand = 1;
		else if (motionConfiguredFirstHand >= 0 && motionConfiguredFirstHand <= 1
			&& (releasedHandMask & (1U << motionConfiguredFirstHand)) != 0)
			selectedHand = motionConfiguredFirstHand;
	}
	if (selectedHand < 0)
		selectedHand = motionConfiguredFirstHand;
	if (selectedHand < 0 || selectedHand > 1
		|| (throwableProbeHandMask & (1U << selectedHand)) == 0)
	{
		selectedHand = (throwableProbeHandMask & 1U) != 0 ? 0
			: ((throwableProbeHandMask & 2U) != 0 ? 1 : -1);
	}
	if (selectedHand >= 0)
	{
		glm::fvec3 oldestPositionM(0.0f);
		glm::fvec3 newestPositionM(0.0f);
		ULONGLONG oldestSampleTime = (std::numeric_limits<ULONGLONG>::max)();
		ULONGLONG newestSampleTime = 0;
		for (size_t sample = 0; sample < ThrowableVelocityHistorySize; ++sample)
		{
			const ULONGLONG sampleTime = throwableProbePositionHistoryTimes[
				static_cast<size_t>(selectedHand)][sample];
			if (sampleTime == 0 || now - sampleTime > 120)
				continue;
			const glm::fvec3 samplePosition = throwableProbePositionHistoryM[
				static_cast<size_t>(selectedHand)][sample];
			if (!IsFiniteVector(samplePosition))
				continue;
			if (sampleTime < oldestSampleTime)
			{
				oldestSampleTime = sampleTime;
				oldestPositionM = samplePosition;
			}
			if (sampleTime > newestSampleTime)
			{
				newestSampleTime = sampleTime;
				newestPositionM = samplePosition;
			}
		}
		const ULONGLONG windowMilliseconds = newestSampleTime > oldestSampleTime
			? newestSampleTime - oldestSampleTime : 0;
		if (windowMilliseconds >= 25 && windowMilliseconds <= 120)
		{
			const float windowSeconds = static_cast<float>(windowMilliseconds) * 0.001f;
			const glm::fvec3 windowVelocity =
				(newestPositionM - oldestPositionM) / windowSeconds;
			if (IsFiniteVector(windowVelocity)
				&& glm::length(windowVelocity) <= 20.0f)
			{
				// Use actual displacement across the release window instead of the
				// single fastest sample. This preserves throw direction while rejecting
				// one-frame controller/bone noise.
				throwableProbeSmoothedVelocityMps[static_cast<size_t>(selectedHand)] =
					windowVelocity;
			}
		}
	}
	for (int hand = 0; hand < 2; ++hand)
	{
		const uint8_t bit = static_cast<uint8_t>(1U << hand);
		if ((throwableProbeHandMask & bit) == 0)
			continue;
		if (!throwableProbePoseValid[hand])
		{
			uevr::API::get()->log_info(
				"[ThrowableProbe] release seq=%u weapon=%d luaWeapon=%d hand=%s hold_ms=%u grips=%u pose=unavailable native=unchanged correlation=pending",
				sequence, static_cast<int>(currentWeaponEquipped), luaWeaponId,
				hand == 0 ? "left" : "right",
				holdMilliseconds, static_cast<unsigned int>(throwableProbeGripMask));
			continue;
		}
		const auto& position = throwableProbeLastWorldPositions[hand];
		const auto& direction = throwableProbeLastDirections[hand];
		const auto& velocity = throwableProbeSmoothedVelocityMps[hand];
		uevr::API::get()->log_info(
			"[ThrowableProbe] release seq=%u weapon=%d luaWeapon=%d hand=%s hold_ms=%u grips=%u samples=%u poseSource=%s anchorUE=(%.2f %.2f %.2f) direction=(%.4f %.4f %.4f) velocity_mps=(%.3f %.3f %.3f) speed=%.3f native=%s correlation=pending",
			sequence, static_cast<int>(currentWeaponEquipped), luaWeaponId,
			hand == 0 ? "left" : "right",
			holdMilliseconds, static_cast<unsigned int>(throwableProbeGripMask),
			throwableProbeSampleCounts[hand],
			throwableProbeLastPoseFromVisibleBone[hand] ? "visible-bone" : "controller-fallback",
			position.x, position.y, position.z,
			direction.x, direction.y, direction.z,
			velocity.x, velocity.y, velocity.z, glm::length(velocity),
			motionEnabled && hand == selectedHand
				? ((currentWeaponEquipped == Grenade || currentWeaponEquipped == Molotov)
					? "custom-flight-candidate" : "override-candidate")
				: "unchanged");
	}

	if (motionEnabled && selectedHand >= 0
		&& throwableProbePoseValid[static_cast<size_t>(selectedHand)]
		&& memoryManager != nullptr)
	{
		const glm::fvec3 position = throwableProbeLastWorldPositions[static_cast<size_t>(selectedHand)];
		const glm::fvec3 forward = NormalizeOrZero(
			throwableProbeLastDirections[static_cast<size_t>(selectedHand)]);
		const glm::fvec3 handVelocity =
			throwableProbeSmoothedVelocityMps[static_cast<size_t>(selectedHand)];
		if (currentWeaponEquipped == Grenade || currentWeaponEquipped == Molotov)
		{
			const float measuredSpeed = glm::length(handVelocity);
			const bool drop = !std::isfinite(measuredSpeed) || measuredSpeed < 0.35f;
			// The visible hand displacement is intentionally conservative because it
			// is measured over a short bone window. Apply a bounded VR assist so a
			// normal arm throw produces a readable flight without inventing a fixed
			// forward direction.
			// Both physical throwables should carry normal arm motion farther than the
			// conservative visible-bone sample. Molotovs remain only slightly slower
			// than grenades; gravity and drag stay identical so they do not feel heavy.
			const float velocityScale = currentWeaponEquipped == Grenade ? 4.0f : 3.6f;
			const float maximumLaunchSpeed = currentWeaponEquipped == Grenade ? 42.0f : 38.0f;
			glm::fvec3 launchVelocity = drop
				? glm::fvec3(0.0f) : handVelocity * velocityScale;
			const char* launchMode = drop ? "drop" : "hand-impulse";
			if (IsFiniteVector(launchVelocity))
			{
				const float launchSpeed = glm::length(launchVelocity);
				if (std::isfinite(launchSpeed) && launchSpeed > maximumLaunchSpeed)
					launchVelocity *= maximumLaunchSpeed / launchSpeed;
			}
			else
			{
				launchVelocity = glm::fvec3(0.0f);
				launchMode = "drop";
			}
			const glm::fvec3 originOffset = forward * 8.0f;
			const glm::fvec3 originUE = position + originOffset;
			const glm::fquat rotation =
				throwableProbeLastWorldRotations[static_cast<size_t>(selectedHand)];
			const glm::fvec3 angularVelocity =
				throwableProbeSmoothedAngularVelocity[static_cast<size_t>(selectedHand)];
			uevr::API::get()->log_info(
				"[MotionThrowableFlight] launch-adjust seq=%u weapon=%d hand=%s raw_speed=%.3f raw_velocity=(%.3f %.3f %.3f) scale=%.2f cap=%.1f launch_mode=%s launch_velocity=(%.3f %.3f %.3f) origin_offset=(%.1f %.1f %.1f)",
				sequence, static_cast<int>(currentWeaponEquipped),
				selectedHand == 0 ? "left" : "right", measuredSpeed,
				handVelocity.x, handVelocity.y, handVelocity.z,
				velocityScale, maximumLaunchSpeed, launchMode,
				launchVelocity.x, launchVelocity.y, launchVelocity.z,
				originOffset.x, originOffset.y, originOffset.z);
			const bool started = StartMotionThrowableFlight(sequence,
				static_cast<int>(currentWeaponEquipped), selectedHand, originUE,
				rotation, launchVelocity, angularVelocity);
			if (!started)
			{
				uevr::API::get()->log_warn(
					"[MotionThrowableFlight] begin rejected seq=%u weapon=%d hand=%s reason=visual-or-pose",
					sequence, static_cast<int>(currentWeaponEquipped),
					selectedHand == 0 ? "left" : "right");
			}
			else if (!drop)
			{
				// The custom hand-velocity flight owns the entire throw. Native GTA is
				// intentionally entered only by the impact adapter after collision;
				// never queue a second native projectile on release.
				uevr::API::get()->log_info(
					"[MotionThrowable] custom flight owns release seq=%u weapon=%d; native launch disabled",
					sequence, static_cast<int>(currentWeaponEquipped));
			}
		}
		else
		{
		const float speed = std::clamp(glm::length(handVelocity), 0.0f, 20.0f);
		const glm::fvec3 motionDirection = speed > 0.15f
			? NormalizeOrZero(handVelocity) : forward;
		const float motionWeight = std::clamp((speed - 0.15f) / 0.50f, 0.0f, 0.95f);
		glm::fvec3 launchDirection = NormalizeOrZero(
			forward * (1.0f - motionWeight) + motionDirection * motionWeight);
		if (glm::length(launchDirection) <= 0.5f)
			launchDirection = forward;

		const float strength = std::clamp(speed / 4.0f, 0.05f, 1.0f);
		const float horizontalScale = 0.22f * strength + 0.15f;
		const float verticalAssist = 0.04f + 0.10f * strength;
		const glm::fvec3 originUE = position + forward * 8.0f;
		const glm::fvec3 originNative(
			originUE.x * 0.01f, -originUE.y * 0.01f, originUE.z * 0.01f);
		const glm::fvec3 directionNative = NormalizeOrZero(glm::fvec3(
			launchDirection.x, -launchDirection.y, launchDirection.z));
		const glm::fvec3 velocityNative = directionNative * horizontalScale
			+ glm::fvec3(0.0f, 0.0f, verticalAssist);
		const bool armed = IsFiniteVector(originNative)
			&& IsFiniteVector(velocityNative)
			&& memoryManager->SetNativeThrowableMotionOverride(
				{ originNative.x, originNative.y, originNative.z },
				{ velocityNative.x, velocityNative.y, velocityNative.z },
				static_cast<int>(currentWeaponEquipped), sequence);
		if (armed)
		{
			throwableMotionOverrideSequence = sequence;
			throwableMotionOverrideWeapon = static_cast<int>(currentWeaponEquipped);
			throwableMotionOverrideExpiresAt = now + 800;
			if (firstWeaponMesh != nullptr && uevr::API::UObjectHook::exists(firstWeaponMesh))
			{
				throwableMotionHiddenMesh = firstWeaponMesh;
				SetComponentVisibility(throwableMotionHiddenMesh, false);
				throwableMotionVisualRestoreAt = now + 900;
			}
			uevr::API::get()->log_info(
				"[MotionThrowable] override armed seq=%u weapon=%d hand=%s speed=%.3f strength=%.3f origin=(%.3f %.3f %.3f) velocity=(%.3f %.3f %.3f)",
				sequence, throwableMotionOverrideWeapon,
				selectedHand == 0 ? "left" : "right", speed, strength,
				originNative.x, originNative.y, originNative.z,
				velocityNative.x, velocityNative.y, velocityNative.z);
		}
		else
		{
			memoryManager->ClearNativeThrowableMotionOverride();
			uevr::API::get()->log_warn(
				"[MotionThrowable] override rejected seq=%u weapon=%d reason=invalid-payload",
				sequence, static_cast<int>(currentWeaponEquipped));
		}
		}
	}
	ResetPhysicalThrowableProbe("release-complete", false);
}

void WeaponManager::ProcessMotionMelee(float delta)
{
	// Contact is deliberately narrower than the old synthetic-trigger path:
	// only the final held mesh pose can arm it, and only native LOS/entity
	// identity is proved here. Health, reactions, missions, and damage events
	// remain entirely native and are not guessed from this plugin.
	constexpr float SwingTriggerSpeed = 2.00f;
	constexpr float SwingSustainSpeed = 0.85f;
	constexpr float SwingResetSpeed = 0.65f;
	constexpr float SwingLowSpeedDwell = 0.09f;
	constexpr float SwingMinimumInterSwingInterval = 0.16f;
	constexpr float SwingInitialContactWindow = 0.14f;
	constexpr float SwingSustainedContactTail = 0.05f;
	constexpr float SwingMinimumDisplacement = 0.025f;
	constexpr float MaximumPlausibleSwingSpeed = 20.0f;
	constexpr float MinimumContactQueryDisplacement = 0.008f;
	// Native-world geometry also moves when CJ walks into a target. Require real
	// tracking-space hand motion for fists/knuckles so a stationary clenched hand
	// cannot damage a pedestrian merely because either body is moving.
	constexpr float FistTriggerControllerSpeed = 0.90f;
	constexpr float FistTriggerControllerDisplacement = 0.008f;
	constexpr float FistContactControllerSpeed = 0.30f;
	constexpr float FistContactControllerDisplacement = 0.003f;
	const bool unarmed = currentWeaponEquipped == Unarmed;
	const bool nativeMeleeWeapon = static_cast<int>(currentWeaponEquipped) >= static_cast<int>(Unarmed)
		&& static_cast<int>(currentWeaponEquipped) <= static_cast<int>(Cane);
	const bool handGeometryWeapon = unarmed || currentWeaponEquipped == BrassKnuckles;
	const bool localPlayerReady = playerManager != nullptr
		&& playerManager->isInControl
		&& playerManager->playerActor != nullptr
		&& uevr::API::UObjectHook::exists(playerManager->playerActor);
	const bool meleeEligible = localPlayerReady
		&& !playerManager->isInVehicle
		&& !playerManager->weaponWheelEnabled
		&& !IsGripCalibrationActive()
		&& nativeMeleeWeapon
		&& motionWeaponTrackingEnabled
		&& memoryManager != nullptr;
	const bool blockNativeMeleeTriggers = localPlayerReady
		&& !playerManager->isInVehicle
		&& !playerManager->weaponWheelEnabled
		&& nativeMeleeWeapon
		&& motionWeaponTrackingEnabled;
	motionMeleeNativeTriggerBlockSnapshot.store(
		blockNativeMeleeTriggers, std::memory_order_release);
	const uint8_t clenchMask = motionMeleeClenchMask.load(std::memory_order_acquire);
	if (clenchMask != motionMeleeLastLoggedClenchMask)
	{
		motionMeleeLastLoggedClenchMask = clenchMask;
		if (blockNativeMeleeTriggers)
			uevr::API::get()->log_info(
				"[MotionMelee] clench mask=%u left=%s right=%s weapon=%d nativeTriggers=blocked",
				static_cast<unsigned int>(clenchMask),
				(clenchMask & 1U) != 0 ? "held" : "released",
				(clenchMask & 2U) != 0 ? "held" : "released",
				static_cast<int>(currentWeaponEquipped));
	}
	const uint8_t grips = gripStateMask.load(std::memory_order_acquire);
	const auto resetMeleeHandState = [this](size_t handIndex) {
		motionMeleeControllerPositionValid[handIndex] = false;
		motionMeleeContactPoseValid[handIndex] = false;
		motionMeleeSwingArmed[handIndex] = true;
		motionMeleeLowSpeedDwellRemaining[handIndex] = 0.0f;
		motionMeleeInterSwingCooldownRemaining[handIndex] = 0.0f;
		motionMeleeContactWindowRemaining[handIndex] = 0.0f;
		motionMeleeContactSwingGeneration[handIndex] = 0;
		motionMeleeContactEntities[handIndex].clear();
	};

	const bool stateTransition = motionMeleeLastWeapon != static_cast<int>(currentWeaponEquipped)
		|| motionMeleeLastGripMask != grips
		|| motionMeleeLastEligible != meleeEligible;
	if (stateTransition)
	{
		++motionMeleeTransitionGeneration;
		motionMeleeControllerPositionValid = { false, false };
		motionMeleeContactPoseValid = { false, false };
		motionMeleeSwingArmed = { true, true };
		motionMeleeLowSpeedDwellRemaining = { 0.0f, 0.0f };
		motionMeleeInterSwingCooldownRemaining = { 0.0f, 0.0f };
		motionMeleeContactWindowRemaining = { 0.0f, 0.0f };
		motionMeleeHandGeometryMode = { -1, -1 };
		for (auto& entities : motionMeleeContactEntities)
			entities.clear();
		motionMeleeRejectedContactDiagnostics.clear();
		motionMeleeContactSwingGeneration = { 0, 0 };
	}
	motionMeleeLastWeapon = static_cast<int>(currentWeaponEquipped);
	motionMeleeLastGripMask = grips;
	motionMeleeLastEligible = meleeEligible;

	// This is also the cleanup path for entering a vehicle, losing control,
	// opening the weapon wheel, changing weapon/grip, or a bad frame interval.
	if (!meleeEligible || delta <= 0.001f || delta > 0.1f)
	{
		HideMotionMeleeDebugAxis();
		motionMeleeControllerPositionValid = { false, false };
		motionMeleeContactPoseValid = { false, false };
		motionMeleeSwingArmed = { true, true };
		motionMeleeLowSpeedDwellRemaining = { 0.0f, 0.0f };
		motionMeleeInterSwingCooldownRemaining = { 0.0f, 0.0f };
		motionMeleeContactWindowRemaining = { 0.0f, 0.0f };
		motionMeleeHandActiveState = { false, false };
		motionMeleeHandGeometryMode = { -1, -1 };
		motionMeleeContactSwingGeneration = { 0, 0 };
		for (auto& entities : motionMeleeContactEntities)
			entities.clear();
		motionMeleeRejectedContactDiagnostics.clear();
		return;
	}

	const auto toNativePoint = [](const glm::fvec3& uePoint) {
		// Same UE-centimetres -> native-metres conversion used by ProcessAiming;
		// native Y is the reflected UE Y axis.
		return glm::fvec3(uePoint.x * 0.01f, -uePoint.y * 0.01f, uePoint.z * 0.01f);
	};
	const auto pulseMeleeContactHaptic = [](int strikingHand) {
		if (strikingHand < 0 || strikingHand > 1)
			return;
		const auto* pluginParam = uevr::API::get()->param();
		const auto* vr = pluginParam != nullptr ? pluginParam->vr : nullptr;
		// Use the raw VR data function pointer. The API.hpp convenience wrapper has
		// historically been unsafe here because its source/parameter ordering is
		// not reliable across the supported UEVR headers.
		if (vr == nullptr || vr->is_runtime_ready == nullptr
			|| !vr->is_runtime_ready() || vr->is_hmd_active == nullptr
			|| !vr->is_hmd_active() || vr->trigger_haptic_vibration == nullptr)
			return;
		UEVR_InputSourceHandle source = nullptr;
		if (strikingHand == 0)
		{
			if (vr->get_left_joystick_source == nullptr)
				return;
			source = vr->get_left_joystick_source();
		}
		else
		{
			if (vr->get_right_joystick_source == nullptr)
				return;
			source = vr->get_right_joystick_source();
		}
		if (source == nullptr)
			return;
		// One brief, low-amplitude pulse on the actual striking controller.
		vr->trigger_haptic_vibration(0.0f, 0.08f, 120.0f, 0.35f, source);
	};
	const auto processMeleeContactSweep = [this, &pulseMeleeContactHaptic, delta](size_t handIndex, int hand,
		const glm::fvec3& previousBase, const glm::fvec3& previousTip,
		const glm::fvec3& base, const glm::fvec3& tip, float radius,
		bool shortHandGeometry, int damageWeaponType, uint32_t sequence) -> bool {
		// A swing is intentionally single-contact. Once native LOS confirms an
		// entity, stop both this sweep and all later sustained-window queries for
		// the same swing. This keeps the native damage pipeline authoritative while
		// preventing a broad multi-frame volume from producing repeated hits.
		if (!motionMeleeContactEntities[handIndex].empty())
			return false;

		// Build a small time/rotation lattice instead of relying on only the two
		// endpoint rods. Intermediate rods follow the shortest rotation between the
		// observed poses, while the quarter points preserve temporal coverage along
		// the full bat/club length. The lattice stays bounded and single-contact.
		constexpr size_t SweepSampleCount = 5;
		constexpr size_t BaseSweepSegmentCount = 10;
		constexpr size_t MaximumSweepSegmentCount = 13;
		const glm::fvec3 previousCenter = (previousBase + previousTip) * 0.5f;
		const glm::fvec3 currentCenter = (base + tip) * 0.5f;
		const float previousLength = glm::length(previousTip - previousBase);
		const float currentLength = glm::length(tip - base);
		const glm::fvec3 previousRodDirection = NormalizeOrZero(previousTip - previousBase);
		const glm::fvec3 currentRodDirection = NormalizeOrZero(tip - base);
		// Fists/knuckles intentionally use a compact hand segment. Keep the
		// non-zero direction/finite checks below, but do not apply the long-mesh
		// minimum to that valid short geometry. Long mesh weapons retain the
		// degenerate-rod guard that prevents meaningless LOS sweeps.
		const bool rodLengthsValid = shortHandGeometry
			? previousLength > 0.0001f && currentLength > 0.0001f
			: previousLength >= 0.5f && currentLength >= 0.5f;
		if (!IsFiniteVector(previousCenter) || !IsFiniteVector(currentCenter)
			|| !std::isfinite(previousLength) || !std::isfinite(currentLength)
			|| !rodLengthsValid
			|| glm::length(previousRodDirection) < 0.5f
			|| glm::length(currentRodDirection) < 0.5f)
			return false;

		const glm::fquat identityRotation = glm::fquat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
		const glm::fquat rotationDelta = RotationBetweenDirections(
			previousRodDirection, currentRodDirection);
		const auto interpolateRodDirection = [&](float t) {
			return NormalizeOrZero(glm::slerp(identityRotation, rotationDelta, t)
				* previousRodDirection);
		};
		std::array<glm::fvec3, SweepSampleCount> sampleBases{};
		std::array<glm::fvec3, SweepSampleCount> sampleTips{};
		std::array<glm::fvec3, SweepSampleCount> sampleDirections{};
		for (size_t sample = 0; sample < SweepSampleCount; ++sample)
		{
			const float t = static_cast<float>(sample)
				/ static_cast<float>(SweepSampleCount - 1);
			const glm::fvec3 center = glm::mix(previousCenter, currentCenter, t);
			const glm::fvec3 direction = interpolateRodDirection(t);
			const float halfLength = glm::mix(previousLength, currentLength, t) * 0.5f;
			if (!IsFiniteVector(center) || !IsFiniteVector(direction)
				|| !std::isfinite(halfLength) || glm::length(direction) < 0.5f)
				return false;
			sampleDirections[sample] = direction;
			sampleBases[sample] = center - direction * halfLength;
			sampleTips[sample] = center + direction * halfLength;
		}

		const std::array<float, 5> sectionFractions{ 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
		std::array<glm::fvec3, MaximumSweepSegmentCount> starts{};
		std::array<glm::fvec3, MaximumSweepSegmentCount> ends{};
		std::array<glm::fvec3, MaximumSweepSegmentCount> segmentDirections{};
		size_t segmentCount = 0;
		const auto appendSegment = [&](const glm::fvec3& start, const glm::fvec3& end,
			const glm::fvec3& direction) {
			starts[segmentCount] = start;
			ends[segmentCount] = end;
			segmentDirections[segmentCount] = direction;
			++segmentCount;
		};
		// Preserve the original query order first: base, tip, midpoint, old rod,
		// current rod. Added paths follow only after those established lanes.
		appendSegment(previousBase, base, interpolateRodDirection(0.5f));
		appendSegment(previousTip, tip, interpolateRodDirection(0.5f));
		appendSegment((previousBase + previousTip) * 0.5f,
			(base + tip) * 0.5f, interpolateRodDirection(0.5f));
		appendSegment(previousBase, previousTip, sampleDirections[0]);
		appendSegment(base, tip, sampleDirections[SweepSampleCount - 1]);
		// Quarter-section paths fill the temporal gaps between the endpoint rays.
		for (size_t fractionIndex : { size_t{ 1 }, size_t{ 3 } })
		{
			const float fraction = sectionFractions[fractionIndex];
			appendSegment(glm::mix(previousBase, previousTip, fraction),
				glm::mix(base, tip, fraction), interpolateRodDirection(0.5f));
		}
		// Three interpolated rods cover rotation that is invisible to endpoint-only
		// LOS lines. They also keep a long bat from tunnelling through a target when
		// its direction changes materially between frames.
		for (size_t sample = 1; sample + 1 < SweepSampleCount; ++sample)
			appendSegment(sampleBases[sample], sampleTips[sample], sampleDirections[sample]);

		// Engine-tick hand poses can trail the rendered fist. For short fist/knuckle
		// geometry only, add a 10 ms translational prediction capped at 4 cm. These
		// three lanes extend the sweep in the measured direction of travel without
		// enlarging the fist radius, shifting its normal geometry, or affecting bats.
		bool predictedShortHandPose = false;
		if (shortHandGeometry && delta > 0.001f && delta <= 0.1f)
		{
			glm::fvec3 prediction = (currentCenter - previousCenter) * (0.010f / delta);
			const float predictionLength = glm::length(prediction);
			if (IsFiniteVector(prediction) && std::isfinite(predictionLength)
				&& predictionLength > 0.0005f)
			{
				if (predictionLength > 0.04f)
					prediction *= 0.04f / predictionLength;
				const glm::fvec3 predictedBase = base + prediction;
				const glm::fvec3 predictedTip = tip + prediction;
				appendSegment(base, predictedBase, currentRodDirection);
				appendSegment(tip, predictedTip, currentRodDirection);
				appendSegment(predictedBase, predictedTip, currentRodDirection);
				predictedShortHandPose = true;
			}
		}

		const size_t expectedSegmentCount = BaseSweepSegmentCount
			+ (predictedShortHandPose ? 3U : 0U);
		if (segmentCount != expectedSegmentCount)
			return false;
		constexpr size_t SweepLaneCount = 5;
		const float laneRadius = (std::max)(0.0f, radius * 0.70f);
		const auto laneOffset = [&](const glm::fvec3& rodDirection, size_t lane) {
			if (lane == 0 || glm::length(rodDirection) < 0.5f)
				return glm::fvec3(0.0f);
			const glm::fvec3 reference = std::abs(rodDirection.z) < 0.85f
				? glm::fvec3(0.0f, 0.0f, 1.0f) : glm::fvec3(0.0f, 1.0f, 0.0f);
			const glm::fvec3 side = NormalizeOrZero(glm::cross(rodDirection, reference));
			const glm::fvec3 normal = NormalizeOrZero(glm::cross(side, rodDirection));
			switch (lane)
			{
			case 1: return side * laneRadius;
			case 2: return side * -laneRadius;
			case 3: return normal * laneRadius;
			case 4: return normal * -laneRadius;
			default: return glm::fvec3(0.0f);
			}
		};

		const auto processLane = [&](size_t lane) -> bool {
			for (size_t segment = 0; segment < segmentCount; ++segment)
			{
				const glm::fvec3 offset = laneOffset(segmentDirections[segment], lane);
				const glm::fvec3 segmentStart = starts[segment] + offset;
				const glm::fvec3 segmentEnd = ends[segment] + offset;
				if (glm::length(segmentEnd - segmentStart) <= 0.0001f)
					continue;
				const std::array<float, 3> nativeStart{
					segmentStart.x, segmentStart.y, segmentStart.z };
				const std::array<float, 3> nativeEnd{
					segmentEnd.x, segmentEnd.y, segmentEnd.z };
				MemoryManager::NativeMeleeContact contact{};
				if (!memoryManager->QueryNativeLineOfSightEntity(nativeStart, nativeEnd, contact))
					continue;
				if (contact.entity == 0)
					continue;
				motionMeleeContactEntities[handIndex].insert(contact.entity);

				int appliedDamage = 0;
				const bool applied = memoryManager->ApplyNativeMeleeContactDamage(
					contact, damageWeaponType, appliedDamage);
				const bool impactAudioPlayed = PlayMotionMeleeImpactAudio(
					contact, damageWeaponType);
				if (applied
					&& contact.damageResult == MemoryManager::NativeMeleeDamageResult::Accepted
					&& (contact.entityType == 2 || contact.entityType == 3))
					pulseMeleeContactHaptic(hand);
				const bool logRejected = contact.damageResult
					== MemoryManager::NativeMeleeDamageResult::Rejected
					&& motionMeleeRejectedContactDiagnostics.insert(contact.entity).second;
				if (applied || logRejected || contact.damageResult
					== MemoryManager::NativeMeleeDamageResult::UnverifiedVehicleDispatch)
				{
					const char* damageResult = "rejected";
					switch (contact.damageResult)
					{
					case MemoryManager::NativeMeleeDamageResult::Attempted:
						damageResult = "attempted";
						break;
					case MemoryManager::NativeMeleeDamageResult::Accepted:
						damageResult = "accepted";
						break;
					case MemoryManager::NativeMeleeDamageResult::UnverifiedVehicleDispatch:
						damageResult = "unverified_vehicle";
						break;
					case MemoryManager::NativeMeleeDamageResult::Rejected:
					default:
						break;
					}
					uevr::API::get()->log_info(
						"[MotionMelee] contact seq=%u generation=%u hand=%s weapon=%d damageWeapon=%d lane=%zu segment=%zu entity=0x%llX type=%u piece=%u point=(%.3f %.3f %.3f) native_damage=%s damage=%d fx_source=%s",
						sequence, motionMeleeTransitionGeneration,
						hand == 0 ? "left" : "right", static_cast<int>(currentWeaponEquipped),
						damageWeaponType, lane, segment,
						static_cast<unsigned long long>(contact.entity),
						static_cast<unsigned int>(contact.entityType),
						static_cast<unsigned int>(contact.piece),
						contact.point[0], contact.point[1], contact.point[2],
						damageResult, appliedDamage,
						impactAudioPlayed ? (contact.entityType == 3
							? "basic-ped" : "basic-vehicle") : "none");
				}
				return true;
			}
			return false;
		};

		// The central lane is the normal physical path. Only if it misses all
		// segments do the four narrow outer lanes compensate for controller/mesh
		// uncertainty, and the first confirmed contact terminates the sweep.
		if (processLane(0))
			return true;
		for (size_t lane = 1; lane < SweepLaneCount; ++lane)
			if (processLane(lane))
				return true;
		return false;
	};

	// The swept contact visualizer was useful while establishing collision, but
	// normal gameplay deliberately runs without creating debug geometry.
	for (int hand = 0; hand < 2; ++hand)
	{
		const size_t handIndex = static_cast<size_t>(hand);
		const bool gripHeld = (grips & static_cast<uint8_t>(1U << hand)) != 0;
		const bool magneticGripOwnsWeapon = magneticGripHand >= 0
			&& magneticGripHand <= 1
			&& magneticGripWeaponId == static_cast<int>(currentWeaponEquipped)
			&& magneticGripAttached;
		const bool weaponHand = magneticGripOwnsWeapon && magneticGripHand == hand;
		const bool offHandFist = magneticGripOwnsWeapon && magneticGripHand != hand;
		// Shoulder/grip state is the only melee arming input. Unarmed and knuckle
		// fighting may use both hands. For other melee weapons, the magnetic owner
		// keeps the weapon geometry while the opposite held hand is an independent
		// bare fist, so bat+fist and knife+fist combinations remain possible.
		const bool handActive = motionWeaponTrackingEnabled && gripHeld
			&& (handGeometryWeapon || weaponHand || offHandFist);
		const bool useHandGeometry = handGeometryWeapon || offHandFist;
		const int8_t geometryMode = handActive
			? static_cast<int8_t>(useHandGeometry ? 0 : 1)
			: static_cast<int8_t>(-1);
		// The visible brass-knuckle mesh remains owned by the magnetic primary
		// hand. Its opposite controller is an independently damaging bare fist.
		const int damageWeaponType = weaponHand
			? static_cast<int>(currentWeaponEquipped)
			: (unarmed ? static_cast<int>(Unarmed)
				: (currentWeaponEquipped == BrassKnuckles && !magneticGripOwnsWeapon
					? static_cast<int>(Unarmed)
					: (offHandFist ? static_cast<int>(Unarmed)
						: static_cast<int>(currentWeaponEquipped))));
		if (handActive != motionMeleeHandActiveState[handIndex]
			|| geometryMode != motionMeleeHandGeometryMode[handIndex])
		{
			motionMeleeHandActiveState[handIndex] = handActive;
			motionMeleeHandGeometryMode[handIndex] = geometryMode;
			resetMeleeHandState(handIndex);
			uevr::API::get()->log_info("[MotionMelee] %s hand=%s weapon=%d role=%s damageWeapon=%d generation=%u",
				handActive ? "armed" : "disarmed",
				hand == 0 ? "left" : "right",
				static_cast<int>(currentWeaponEquipped),
				geometryMode == 1 ? "weapon" : (geometryMode == 0 ? "fist" : "none"),
				damageWeaponType,
				motionMeleeTransitionGeneration);
		}
		if (!handActive)
		{
			resetMeleeHandState(handIndex);
			continue;
		}

		// A controller pose is still required as a calibration/validity gate. Mesh
		// coordinates remain authoritative; unarmed/knuckles use the short known
		// hand geometry, and other melee uses controller geometry only when mesh
		// bounds are missing or degenerate. No world scan is used as a fallback.
		glm::fvec3 controllerPosition{};
		glm::fquat controllerRotation{};
		if (!ReadControllerCalibrationPose(hand, controllerPosition, controllerRotation))
		{
			resetMeleeHandState(handIndex);
			continue;
		}
		const bool previousControllerPositionValid =
			motionMeleeControllerPositionValid[handIndex];
		const float controllerLocalDisplacement = previousControllerPositionValid
			? glm::length(controllerPosition
				- motionMeleePreviousControllerPositions[handIndex])
			: 0.0f;
		const float controllerLocalSpeed = previousControllerPositionValid
			? controllerLocalDisplacement / delta : 0.0f;
		motionMeleePreviousControllerPositions[handIndex] = controllerPosition;
		motionMeleeControllerPositionValid[handIndex] = true;
		if (!std::isfinite(controllerLocalDisplacement)
			|| !std::isfinite(controllerLocalSpeed))
		{
			resetMeleeHandState(handIndex);
			continue;
		}

		// Prefer the live mesh for long melee weapons. Unarmed and brass knuckles
		// deliberately use the known controller/hand pose so they do not depend on
		// a decorative firstWeaponMesh. Other melee classes use that same bounded
		// hand fallback only when their mesh bounds are unavailable/degenerate.
		const bool meshAvailable = firstWeaponMesh != nullptr
			&& uevr::API::UObjectHook::exists(firstWeaponMesh);
		glm::fvec3 meshPosition{};
		glm::fquat meshRotation{};
		glm::fvec3 meshBase{};
		glm::fvec3 meshTip{};
		float meshRadius = 0.0f;
		bool meshGeometryValid = false;
		bool meshBoundsFallbackAllowed = !meshAvailable;
		if (meshAvailable && !useHandGeometry)
		{
			if (ReadCurrentWeaponWorldTransform(meshPosition, meshRotation))
			{
				meshGeometryValid = ReadMotionMeleeContactGeometry(
					meshPosition, meshRotation, meshBase, meshTip, meshRadius);
				meshBoundsFallbackAllowed = !meshGeometryValid;
			}
			else if (!useHandGeometry)
			{
				resetMeleeHandState(handIndex);
				continue;
			}
		}

		glm::fvec3 baseUE{};
		glm::fvec3 tipUE{};
		float radiusUE = 0.0f;
		bool geometryValid = false;
		if (meshGeometryValid && !useHandGeometry)
		{
			baseUE = meshBase;
			tipUE = meshTip;
			radiusUE = meshRadius;
			geometryValid = true;
		}
		else if (useHandGeometry || meshBoundsFallbackAllowed)
		{
			geometryValid = ReadMotionMeleeHandGeometry(
				hand, controllerPosition, controllerRotation, baseUE, tipUE, radiusUE);
			if (!geometryValid && useHandGeometry && meshGeometryValid)
			{
				baseUE = meshBase;
				tipUE = meshTip;
				radiusUE = meshRadius;
				geometryValid = true;
			}
		}
		if (!geometryValid)
		{
			resetMeleeHandState(handIndex);
			continue;
		}
		const glm::fvec3 base = toNativePoint(baseUE);
		const glm::fvec3 tip = toNativePoint(tipUE);
		const float contactRadius = radiusUE * 0.01f;
		if (!IsFiniteVector(base) || !IsFiniteVector(tip)
			|| !std::isfinite(contactRadius) || contactRadius <= 0.0f)
		{
			resetMeleeHandState(handIndex);
			continue;
		}
		if (motionMeleeContactPoseValid[handIndex])
		{
			const glm::fvec3 previousBase = motionMeleePreviousBasePoints[handIndex];
			const glm::fvec3 previousTip = motionMeleePreviousTipPoints[handIndex];
			const float baseDisplacement = glm::length(base - previousBase);
			const float tipDisplacement = glm::length(tip - previousTip);
			const float midpointDisplacement = glm::length(
				((base + tip) - (previousBase + previousTip)) * 0.5f);
			const float displacement = (std::max)(
				(std::max)(baseDisplacement, tipDisplacement), midpointDisplacement);
			const float speed = displacement / delta;
			if (!std::isfinite(speed) || !std::isfinite(displacement))
			{
				resetMeleeHandState(handIndex);
				continue;
			}
			// Attachment initialization can move a newly-created component by many
			// metres in one frame. Treat that as a discontinuity, not a strike.
			if (speed > MaximumPlausibleSwingSpeed)
			{
				motionMeleeSwingArmed[handIndex] = true;
				motionMeleeContactWindowRemaining[handIndex] = 0.0f;
				motionMeleeContactEntities[handIndex].clear();
				motionMeleePreviousBasePoints[handIndex] = base;
				motionMeleePreviousTipPoints[handIndex] = tip;
				motionMeleeContactPoseValid[handIndex] = true;
				continue;
			}

			// A single low-speed sample is not a release. Require a stable dwell
			// after the active contact window and a bounded cooldown before rearming.
			motionMeleeInterSwingCooldownRemaining[handIndex] = (std::max)(0.0f,
				motionMeleeInterSwingCooldownRemaining[handIndex] - delta);
			if (!motionMeleeSwingArmed[handIndex] && speed >= SwingSustainSpeed)
				motionMeleeContactWindowRemaining[handIndex] = (std::max)(
					motionMeleeContactWindowRemaining[handIndex], SwingSustainedContactTail);
			const bool contactWindowActive =
				motionMeleeContactWindowRemaining[handIndex] > 0.0f;
			if (contactWindowActive)
			{
				const bool fistContactMotionMeaningful = !useHandGeometry
					|| (controllerLocalSpeed >= FistContactControllerSpeed
						&& controllerLocalDisplacement
							>= FistContactControllerDisplacement);
				const bool contactMotionMeaningful = displacement
					>= MinimumContactQueryDisplacement
					&& fistContactMotionMeaningful
					&& motionMeleeContactEntities[handIndex].empty();
				if (contactMotionMeaningful && processMeleeContactSweep(handIndex, hand,
					previousBase, previousTip, base, tip, contactRadius,
					useHandGeometry, damageWeaponType,
					motionMeleeContactSwingGeneration[handIndex]))
				{
					// The sweep has already recorded the first confirmed entity. Do not
					// spend the remaining window on another LOS query.
					motionMeleeContactWindowRemaining[handIndex] = 0.0f;
				}
				else
				{
					motionMeleeContactWindowRemaining[handIndex] = (std::max)(0.0f,
						motionMeleeContactWindowRemaining[handIndex] - delta);
				}
				motionMeleeLowSpeedDwellRemaining[handIndex] = 0.0f;
			}
			else if (!motionMeleeSwingArmed[handIndex])
			{
				if (speed <= SwingResetSpeed)
				{
					motionMeleeLowSpeedDwellRemaining[handIndex] = (std::min)(
						SwingLowSpeedDwell,
						motionMeleeLowSpeedDwellRemaining[handIndex] + delta);
				}
				else
				{
					motionMeleeLowSpeedDwellRemaining[handIndex] = 0.0f;
				}

				if (motionMeleeLowSpeedDwellRemaining[handIndex] >= SwingLowSpeedDwell
					&& motionMeleeInterSwingCooldownRemaining[handIndex] <= 0.0f)
				{
					motionMeleeSwingArmed[handIndex] = true;
					motionMeleeLowSpeedDwellRemaining[handIndex] = 0.0f;
				}
			}
			else
			{
				motionMeleeLowSpeedDwellRemaining[handIndex] = 0.0f;
			}

			if (motionMeleeSwingArmed[handIndex]
				&& motionMeleeInterSwingCooldownRemaining[handIndex] <= 0.0f
				&& speed >= SwingTriggerSpeed
				&& displacement >= SwingMinimumDisplacement
				&& (!useHandGeometry
					|| (controllerLocalSpeed >= FistTriggerControllerSpeed
						&& controllerLocalDisplacement
							>= FistTriggerControllerDisplacement)))
			{
				motionMeleeSwingArmed[handIndex] = false;
				motionMeleeLowSpeedDwellRemaining[handIndex] = 0.0f;
				motionMeleeInterSwingCooldownRemaining[handIndex] =
					SwingMinimumInterSwingInterval;
				motionMeleeContactWindowRemaining[handIndex] = SwingInitialContactWindow;
				motionMeleeContactEntities[handIndex].clear();
				const uint32_t sequence = motionMeleeContactSequence.fetch_add(1,
					std::memory_order_acq_rel) + 1;
				motionMeleeContactSwingGeneration[handIndex] = sequence;
				uevr::API::get()->log_info(
					"[MotionMelee] contact swing seq=%u generation=%u hand=%s weapon=%d damageWeapon=%d speed=%.2f displacement=%.3f controllerSpeed=%.2f controllerMove=%.3f baseMove=%.3f tipMove=%.3f radius=%.3f base=(%.3f %.3f %.3f) tip=(%.3f %.3f %.3f)",
					sequence, motionMeleeTransitionGeneration,
					hand == 0 ? "left" : "right", static_cast<int>(currentWeaponEquipped),
					damageWeaponType, speed, displacement,
					controllerLocalSpeed, controllerLocalDisplacement, baseDisplacement,
					tipDisplacement, contactRadius,
					base.x, base.y, base.z, tip.x, tip.y, tip.z);
				const bool contactConfirmed = processMeleeContactSweep(handIndex, hand,
					previousBase, previousTip, base, tip, contactRadius,
					useHandGeometry, damageWeaponType, sequence);
				motionMeleeContactWindowRemaining[handIndex] = contactConfirmed
					? 0.0f
					: (std::max)(0.0f,
						motionMeleeContactWindowRemaining[handIndex] - delta);
			}
		}

		motionMeleePreviousBasePoints[handIndex] = base;
		motionMeleePreviousTipPoints[handIndex] = tip;
		motionMeleeContactPoseValid[handIndex] = true;
	}
}

void WeaponManager::SetFreeAimWeaponHandsPresentationActive(bool active)
{
	if (freeAimWeaponHandsPresentationActive == active)
		return;

	if (active && settingsManager->enableFreeAimWeaponHands)
		playerManager->SetHandsScaled(true, true);
	freeAimWeaponHandsPresentationActive = active;
	ProcessFreeAimWeaponHands(true);
}

bool WeaponManager::IsFreeAimHandsEligibleWeapon() const
{
	// Controller-held melee, physical throwables, firearms, and the tested
	// spray-can utility share the split-hand presentation.
	return (currentWeaponEquipped >= BrassKnuckles && currentWeaponEquipped <= Cane)
		|| (currentWeaponEquipped >= Grenade && currentWeaponEquipped <= Molotov)
		|| (currentWeaponEquipped >= Pistol && currentWeaponEquipped <= Minigun)
		|| IsControllerHeldUtility();
}

bool WeaponManager::CreateFreeAimFakeHands()
{
	auto currentCharacter = playerManager->playerCharacter;
	if (freeAimHandsBlockedCharacter != currentCharacter)
	{
		freeAimHandsCreationBlocked = false;
		freeAimHandsBlockedCharacter = nullptr;
	}
	if (freeAimHandsCreationBlocked && freeAimHandsBlockedCharacter == currentCharacter)
		return false;

	if (freeAimFakeHandsReady && freeAimFakeHandsCharacter == playerManager->playerCharacter
		&& uevr::API::UObjectHook::exists(freeAimFakeLeftHand)
		&& uevr::API::UObjectHook::exists(freeAimFakeRightHand))
		return true;

	RemoveFreeAimFakeHands(true);
	const auto failCreation = [&](const char* stage) -> bool
	{
		freeAimHandsCreationBlocked = true;
		freeAimHandsBlockedCharacter = currentCharacter;
		uevr::API::get()->log_warn("[FreeAimHands] creation blocked stage=%s character=%p; retry waits for character change or restore", stage, currentCharacter);
		RemoveFreeAimFakeHands(true);
		return false;
	};
	if (playerManager->playerCharacter == nullptr || playerManager->handScaleHands == nullptr
		|| !uevr::API::UObjectHook::exists(playerManager->playerCharacter)
		|| !uevr::API::UObjectHook::exists(playerManager->handScaleHands))
		return false; // Normal transient state while the player presentation initializes.

	auto skeletalMeshClass = uevr::API::get()->find_uobject<uevr::API::UClass>(L"Class /Script/Engine.SkeletalMeshComponent");
	if (skeletalMeshClass == nullptr)
		return failCreation("skeletal-component-class");
	auto staticMeshClass = uevr::API::get()->find_uobject<uevr::API::UClass>(
		L"Class /Script/Engine.StaticMeshComponent");
	const auto destroyTransientComponent = [this](uevr::API::UObject*& component)
	{
		if (component == nullptr)
			return;
		if (uevr::API::UObjectHook::exists(component))
		{
			uevr::API::UObjectHook::remove_motion_controller_state(component);
			SetComponentVisibility(component, false);
			auto* componentClass = component->get_class();
			auto* destroy = componentClass != nullptr
				? componentClass->find_function(L"DestroyComponent") : nullptr;
			if (destroy != nullptr)
			{
				std::vector<uint8_t> params(destroy->get_properties_size());
				SetReflectedBoolParameter(destroy, params, L"bPromoteChildren", false);
				destroy->call(component, params.data());
			}
		}
		component = nullptr;
	};
	auto skeletalMesh = playerManager->handScaleHands->get_property<uevr::API::UObject*>(L"SkeletalMesh");
	if (skeletalMesh == nullptr)
		return failCreation("skeletal-mesh-property");

	// Keep the proven split skeletal clones authoritative for tracking,
	// calibration, weapon-local attachment, the forearms and watch.
	freeAimFakeLeftHand = AddSkeletalComponent(playerManager->playerCharacter, skeletalMeshClass);
	freeAimFakeRightHand = AddSkeletalComponent(playerManager->playerCharacter, skeletalMeshClass);
	if (freeAimFakeLeftHand == nullptr || freeAimFakeRightHand == nullptr)
		return failCreation("add-components");
	if (!CallSetSkeletalMesh(freeAimFakeLeftHand, skeletalMesh)
		|| !CallSetSkeletalMesh(freeAimFakeRightHand, skeletalMesh))
		return failCreation("set-skeletal-mesh");

	// The baked assets contain one hand each, so clenching one side can never
	// reveal a second hand driven by the same controller. Asset failure is a
	// presentation-only fallback: the proven ordinary hands remain usable.
	freeAimClenchedLeftMesh = LoadStaticMeshAssetBlocking(
		L"/Game/SAVRHands/SAVR_HandGrip_L.SAVR_HandGrip_L");
	freeAimClenchedRightMesh = LoadStaticMeshAssetBlocking(
		L"/Game/SAVRHands/SAVR_HandGrip_R.SAVR_HandGrip_R");
	if (staticMeshClass != nullptr && freeAimClenchedLeftMesh != nullptr
		&& freeAimClenchedRightMesh != nullptr)
	{
		auto nativeHandMaterial = ReadComponentMaterial(playerManager->handScaleHands, 0);
		freeAimClenchedLeftHand = AddSkeletalComponent(
			playerManager->playerCharacter, staticMeshClass);
		freeAimClenchedRightHand = AddSkeletalComponent(
			playerManager->playerCharacter, staticMeshClass);
		// AddComponent returns visible components. Hide both before any asset or
		// material setup so a partial respawn-time failure can never flash or strand
		// the baked Vice-derived hand at its registration transform.
		SetComponentVisibility(freeAimClenchedLeftHand, false);
		SetComponentVisibility(freeAimClenchedRightHand, false);
		if (freeAimClenchedLeftHand == nullptr || freeAimClenchedRightHand == nullptr
			|| !CallSetStaticMesh(freeAimClenchedLeftHand, freeAimClenchedLeftMesh)
			|| !CallSetStaticMesh(freeAimClenchedRightHand, freeAimClenchedRightMesh)
			|| nativeHandMaterial == nullptr
			|| !SetComponentMaterial(freeAimClenchedLeftHand, 0, nativeHandMaterial)
			|| !SetComponentMaterial(freeAimClenchedLeftHand, 1, nativeHandMaterial)
			|| !SetComponentMaterial(freeAimClenchedRightHand, 0, nativeHandMaterial)
			|| !SetComponentMaterial(freeAimClenchedRightHand, 1, nativeHandMaterial))
		{
			destroyTransientComponent(freeAimClenchedLeftHand);
			destroyTransientComponent(freeAimClenchedRightHand);
		}
		else
		{
			uevr::API::get()->log_info(
				"[ClenchedHands] native material applied object=%p name=%ls",
				nativeHandMaterial, nativeHandMaterial->get_full_name().c_str());
		}
	}
	if ((freeAimClenchedLeftHand == nullptr || freeAimClenchedRightHand == nullptr)
		&& !freeAimClenchedAssetFailureLogged)
	{
		uevr::API::get()->log_warn(
			"[ClenchedHands] baked assets unavailable class=%p meshes=%p/%p; ordinary hands retained",
			staticMeshClass, freeAimClenchedLeftMesh, freeAimClenchedRightMesh);
		freeAimClenchedAssetFailureLogged = true;
	}

	// Capability probe only: determine whether the existing native/fake hand
	// classes expose reflected poseable-bone setters. Do not invoke any setter;
	// this is intentionally one-shot and does not alter hand tracking or pose.
	if (!freeAimBareHandCapabilityProbeLogged)
	{
		const auto probeClass = [](const char* role, uevr::API::UObject* sample)
		{
			const auto classObject = sample != nullptr ? sample->get_class() : nullptr;
			if (classObject == nullptr)
			{
				uevr::API::get()->log_info(
					"[BareHandCapability] role=%s class=<unavailable> transform=0 rotation=0 translation=0 scale=0",
					role);
				return;
			}
			const auto className = classObject->get_full_name();
			uevr::API::get()->log_info(
				"[BareHandCapability] role=%s class=%ls transform=%d rotation=%d translation=%d scale=%d",
				role, className.c_str(),
				classObject->find_function(L"SetBoneTransformByName") != nullptr ? 1 : 0,
				classObject->find_function(L"SetBoneRotationByName") != nullptr ? 1 : 0,
				classObject->find_function(L"SetBoneTranslationByName") != nullptr ? 1 : 0,
				classObject->find_function(L"SetBoneScaleByName") != nullptr ? 1 : 0);
		};
		probeClass("native", playerManager->handScaleHands);
		probeClass("fake", freeAimFakeLeftHand != nullptr ? freeAimFakeLeftHand : freeAimFakeRightHand);
		freeAimBareHandCapabilityProbeLogged = true;
	}

	// The native watch is animated by GTA even when its component follows the
	// controller. A fresh component has the same asset but no walking animation,
	// making it a rigid accessory representation rather than a second wrist rig.
	freeAimFakeWatch = nullptr;
	if (playerManager->handScaleWatch != nullptr
		&& uevr::API::UObjectHook::exists(playerManager->handScaleWatch)
		&& playerManager->handScaleWatch->get_class() != nullptr
		&& playerManager->handScaleWatch->get_class()->find_property(L"SkeletalMesh") != nullptr)
	{
		auto watchMesh = playerManager->handScaleWatch->get_property<uevr::API::UObject*>(L"SkeletalMesh");
		if (watchMesh != nullptr)
		{
			freeAimFakeWatch = AddSkeletalComponent(playerManager->playerCharacter, skeletalMeshClass);
			if (freeAimFakeWatch == nullptr || !CallSetSkeletalMesh(freeAimFakeWatch, watchMesh))
				destroyTransientComponent(freeAimFakeWatch);
		}
	}

	// Keep clones hidden until split verification and controller alignment both
	// succeed, preventing four-hand flashes during deferred initialization.
	SetComponentVisibility(freeAimFakeLeftHand, false);
	SetComponentVisibility(freeAimFakeRightHand, false);
	SetComponentVisibility(freeAimClenchedLeftHand, false);
	SetComponentVisibility(freeAimClenchedRightHand, false);
	SetComponentVisibility(freeAimFakeWatch, false);
	freeAimFakeHandsCharacter = playerManager->playerCharacter;
	freeAimFakeHandsReady = true;
	freeAimFakeHandsInitialized = false;
	freeAimFakeHandsInitializing = false;
	freeAimFakeHandsWarmupFrames = 1;
	freeAimFakeHandsAlignmentFramesRemaining = 30;
	freeAimFakeHandsActive = false;
	freeAimClenchedHandsReady = false;
	freeAimClenchedVisibleMask = 0;
	freeAimHandsCreationBlocked = false;
	freeAimHandsBlockedCharacter = nullptr;
	freeAimHandsOffsetsLogged = false;
	freeAimHandsFailureLogged = false;
	uevr::API::get()->log_info(
		"[FreeAimHands] fake copies created character=%p asset=%p left=%p right=%p clenchedStatic=%p/%p pose=none init=deferred",
		playerManager->playerCharacter, skeletalMesh, freeAimFakeLeftHand,
		freeAimFakeRightHand, freeAimClenchedLeftHand, freeAimClenchedRightHand);
	return true;
}

bool WeaponManager::ApplyFreeAimFakeHandPresentation()
{
	if (!freeAimFakeHandsReady || freeAimFakeLeftHand == nullptr || freeAimFakeRightHand == nullptr)
		return false;
	const bool vehicleRightOnly = IsVehicleFreeAimActive();
	if (!freeAimFakeHandsInitialized)
	{
		freeAimFakeHandsInitializing = true;
		if (freeAimFakeHandsWarmupFrames > 0)
		{
			--freeAimFakeHandsWarmupFrames;
			if (!freeAimHandsFailureLogged)
				uevr::API::get()->log_info("[FreeAimHands] initialization deferred one frame after component registration");
			return false;
		}
		ResolvedBone leftOpposite;
		ResolvedBone rightOpposite;
		ResolvedBone leftHand;
		ResolvedBone rightHand;
		ResolvedBone nativeLeftOpposite;
		ResolvedBone nativeRightOpposite;
		ResolvedBone nativeLeftHand;
		ResolvedBone nativeRightHand;
		ResolvedBone watchLeftHand;
		const bool nativeBonesResolved = ResolveBone(playerManager->handScaleHands, L"R_UpperArm", nativeLeftOpposite)
			&& ResolveBone(playerManager->handScaleHands, L"L_UpperArm", nativeRightOpposite)
			&& ResolveBone(playerManager->handScaleHands, L"L_Hand", nativeLeftHand)
			&& ResolveBone(playerManager->handScaleHands, L"R_Hand", nativeRightHand);
		const bool cloneBonesResolved = ResolveBone(freeAimFakeLeftHand, L"R_UpperArm", leftOpposite)
			&& ResolveBone(freeAimFakeRightHand, L"L_UpperArm", rightOpposite)
			&& ResolveBone(freeAimFakeLeftHand, L"L_Hand", leftHand)
			&& ResolveBone(freeAimFakeRightHand, L"R_Hand", rightHand);
		const bool nativeIndicesValid = nativeBonesResolved
			&& nativeLeftOpposite.index >= 0 && nativeRightOpposite.index >= 0
			&& nativeLeftHand.index >= 0 && nativeRightHand.index >= 0;
		const bool cloneIndicesValid = cloneBonesResolved
			&& leftOpposite.index >= 0 && rightOpposite.index >= 0
			&& leftHand.index >= 0 && rightHand.index >= 0;
		if (!nativeIndicesValid || !cloneIndicesValid)
		{
			uevr::API::get()->log_warn("[FreeAimHands] initialization blocked stage=required-bone-index nativeOpposite=%d/%d nativeHands=%d/%d cloneOpposite=%d/%d cloneHands=%d/%d",
				nativeLeftOpposite.index, nativeRightOpposite.index, nativeLeftHand.index, nativeRightHand.index,
				leftOpposite.index, rightOpposite.index, leftHand.index, rightHand.index);
			LogHandSkeletalInventory("native", playerManager->handScaleHands);
			LogHandSkeletalInventory("clone-left", freeAimFakeLeftHand);
			freeAimHandsCreationBlocked = true;
			freeAimHandsBlockedCharacter = freeAimFakeHandsCharacter;
			freeAimFakeHandsInitializing = false;
			return false;
		}
		if (!HideBone(freeAimFakeLeftHand, leftOpposite.name)
			|| !HideBone(freeAimFakeRightHand, rightOpposite.name))
		{
			uevr::API::get()->log_warn("[FreeAimHands] initialization blocked stage=hide-opposite-arm");
			freeAimHandsCreationBlocked = true;
			freeAimHandsBlockedCharacter = freeAimFakeHandsCharacter;
			freeAimFakeHandsInitializing = false;
			return false;
		}
		bool leftOppositeHidden = false;
		bool rightOppositeHidden = false;
		const bool leftHideVerified = TryReadBoneHiddenByName(
			freeAimFakeLeftHand, leftOpposite.name, leftOppositeHidden);
		const bool rightHideVerified = TryReadBoneHiddenByName(
			freeAimFakeRightHand, rightOpposite.name, rightOppositeHidden);
		if (!leftHideVerified || !rightHideVerified || !leftOppositeHidden || !rightOppositeHidden)
		{
			uevr::API::get()->log_warn("[FreeAimHands] initialization blocked stage=hide-verification result=%d/%d hidden=%d/%d",
				leftHideVerified, rightHideVerified, leftOppositeHidden, rightOppositeHidden);
			freeAimHandsCreationBlocked = true;
			freeAimHandsBlockedCharacter = freeAimFakeHandsCharacter;
			freeAimFakeHandsInitializing = false;
			return false;
		}
		freeAimFakeLeftHandBoneName = leftHand.name;
		freeAimFakeRightHandBoneName = rightHand.name;
		freeAimClenchedHandsReady = freeAimClenchedLeftHand != nullptr
			&& freeAimClenchedRightHand != nullptr
			&& uevr::API::UObjectHook::exists(freeAimClenchedLeftHand)
			&& uevr::API::UObjectHook::exists(freeAimClenchedRightHand)
			&& AttachStaticHandToDriver(freeAimClenchedLeftHand,
				freeAimFakeLeftHand, freeAimFakeLeftHandBoneName)
			&& AttachStaticHandToDriver(freeAimClenchedRightHand,
				freeAimFakeRightHand, freeAimFakeRightHandBoneName);
		if (!freeAimClenchedHandsReady)
		{
			SetComponentVisibility(freeAimClenchedLeftHand, false);
			SetComponentVisibility(freeAimClenchedRightHand, false);
			uevr::API::get()->log_warn(
				"[ClenchedHands] static attachment unavailable; ordinary hands retained");
		}
		else
		{
			uevr::API::get()->log_info(
				"[ClenchedHands] independent static hands ready left=%p right=%p drivers=%p/%p",
				freeAimClenchedLeftHand, freeAimClenchedRightHand,
				freeAimFakeLeftHand, freeAimFakeRightHand);
		}
		freeAimFakeWatchHandBoneName = uevr::API::FName{};
		if (freeAimFakeWatch != nullptr && ResolveBone(freeAimFakeWatch, L"L_Hand", watchLeftHand)
			&& watchLeftHand.index >= 0)
			freeAimFakeWatchHandBoneName = watchLeftHand.name;
		freeAimFakeHandsInitialized = true;
		freeAimFakeHandsInitializing = false;
		uevr::API::get()->log_info("[FreeAimHands] required bones resolved nativeOpposite=%d/%d nativeHands=%d/%d cloneOpposite=%d/%d cloneHands=%d/%d hideResult=%d/%d verified=%d/%d",
			nativeLeftOpposite.index, nativeRightOpposite.index, nativeLeftHand.index, nativeRightHand.index,
			leftOpposite.index, rightOpposite.index, leftHand.index, rightHand.index,
			leftOppositeHidden, rightOppositeHidden, leftHideVerified, rightHideVerified);
	}
	if (vehicleRightOnly)
	{
		if (!ApplyVehicleNativeRightArmPresentation(true))
		{
			SetComponentVisibility(freeAimFakeLeftHand, false);
			SetComponentVisibility(freeAimFakeRightHand, false);
			SetComponentVisibility(freeAimClenchedLeftHand, false);
			SetComponentVisibility(freeAimClenchedRightHand, false);
			SetComponentVisibility(freeAimFakeWatch, false);
			return false;
		}
	}
	else if (vehicleNativeRightArmHidden)
	{
		ApplyVehicleNativeRightArmPresentation(false);
	}
	// The left clone's bind pose has the opposite local roll regardless of its
	// current weapon role. Keep this anatomical correction active for free,
	// primary, and watch presentation; role-specific calibration remains separate.
	constexpr bool leftAnatomicalCorrection = true;
	const uint8_t heldGripMask = gripStateMask.load(std::memory_order_acquire);
	const bool meleeClenchContext = !playerManager->isInVehicle
		&& currentWeaponEquipped >= Unarmed && currentWeaponEquipped <= Cane;
	const bool throwableClenchContext = !playerManager->isInVehicle
		&& currentWeaponEquipped >= Grenade && currentWeaponEquipped <= Molotov;
	const uint8_t desiredClenchedMask = freeAimClenchedHandsReady
		&& (meleeClenchContext || throwableClenchContext)
		? static_cast<uint8_t>(heldGripMask & 0x03U) : 0;
	const int primaryHandForSupport = twoHandPrimaryHand.load(std::memory_order_acquire);
	const int supportHand = twoHandSupportActive && (primaryHandForSupport == 0 || primaryHandForSupport == 1)
		? 1 - primaryHandForSupport : -1;
	GripCalibrationTransform supportContact;
	const bool supportContactCandidate = settingsManager->enableGripCalibration
		&& !IsGripCalibrationActive() && supportHand >= 0;
	const bool supportContactDesired = supportContactCandidate
		&& GetSupportContactForHand(supportHand, supportContact);
	GripCalibrationTransform primaryGrip;
	const bool primaryHandCandidate = settingsManager->enableGripCalibration
		&& !IsGripCalibrationActive()
		&& twoHandSupportActive && twoHandOffsetApplied
		&& twoHandAppliedWeaponMesh == firstWeaponMesh
		&& (primaryHandForSupport == 0 || primaryHandForSupport == 1);
	// The canonical primary record is only an availability/hand-role gate here.
	// Attachment derives a fresh bone contact from the visible hand and final
	// weapon pose; the controller-to-weapon O is never passed as that contact.
	const bool primaryHandAttachmentDesired = primaryHandCandidate
		&& GetCanonicalPrimaryGripForHand(primaryHandForSupport, primaryGrip);
	if (supportContactCandidate && !freeAimSupportContactDiagnosticLogged)
	{
		freeAimSupportContactDiagnosticLogged = true;
		uevr::API::get()->log_info(
			"[GripCalibration] support-contact request desired=%d hand=%d weapon=%d contactValid=%d contact=(%.2f %.2f %.2f)",
			supportContactDesired ? 1 : 0, supportHand, static_cast<int>(currentWeaponEquipped),
			supportContact.valid ? 1 : 0, supportContact.position.x, supportContact.position.y,
			supportContact.position.z);
	}
	else if (!supportContactCandidate)
		freeAimSupportContactDiagnosticLogged = false;
	if (freeAimSupportHandAttached
		&& (!supportContactDesired || freeAimSupportAttachedHand != supportHand
			|| freeAimSupportAttachedWeapon != firstWeaponMesh))
		RestoreSupportFakeHandAttachment();
	if (freeAimPrimaryHandAttached
		&& (!primaryHandAttachmentDesired
			|| freeAimPrimaryAttachedHand != primaryHandForSupport
			|| freeAimPrimaryAttachedWeapon != firstWeaponMesh))
	{
		RestorePrimaryFakeHandAttachment();
	}
	else if (!primaryHandAttachmentDesired && !freeAimPrimaryHandAttached)
		freeAimPrimaryAttachFailureDiagnosticLogged = false;
	if (!primaryHandAttachmentDesired)
	{
		freeAimPrimaryAttachPrimeFramesRemaining = 0;
		freeAimPrimaryAttachPrimeHand = -1;
		freeAimPrimaryAttachPrimeWeapon = nullptr;
		freeAimPrimaryAttachPrimeGeneration = 0;
		freeAimPrimaryAttachPrimeEngineTick = 0;
	}
	else if (!freeAimPrimaryHandAttached)
	{
		const bool primeTargetChanged = freeAimPrimaryAttachPrimeHand != primaryHandForSupport
			|| freeAimPrimaryAttachPrimeWeapon != firstWeaponMesh
			|| freeAimPrimaryAttachPrimeGeneration != runtimeHandGeneration;
		if (primeTargetChanged)
		{
			freeAimPrimaryAttachPrimeFramesRemaining = 1;
			freeAimPrimaryAttachPrimeHand = static_cast<int8_t>(primaryHandForSupport);
			freeAimPrimaryAttachPrimeWeapon = firstWeaponMesh;
			freeAimPrimaryAttachPrimeGeneration = runtimeHandGeneration;
			freeAimPrimaryAttachPrimeEngineTick = interactionEngineTickGeneration;
			const int supportRoleHand = 1 - primaryHandForSupport;
			uevr::API::get()->log_info(
				"[GripCalibration] primary-grip prime hand=%d support=%d weapon=%d generation=%u primaryRole=%s supportRole=%s frames=1",
				primaryHandForSupport, supportRoleHand,
				static_cast<int>(currentWeaponEquipped), runtimeHandGeneration,
				RuntimeHandRoleName(runtimeHandStates[static_cast<size_t>(primaryHandForSupport)].role),
				RuntimeHandRoleName(runtimeHandStates[static_cast<size_t>(supportRoleHand)].role));
		}
	}
	// Once attached in weapon-local space, the support and primary fake hands
	// follow the weapon. Re-deriving either relation from an already-attached
	// skeletal component each frame compounds its rotation and caused the
	// palm/knuckle flip after regrip.
	if (freeAimFakeHandsActive && freeAimLeftPalmOffsetApplied == leftAnatomicalCorrection
		&& freeAimAppliedSupportContactActive == supportContactDesired
		&& freeAimAppliedCalibrationWeaponId == static_cast<int>(currentWeaponEquipped)
		&& freeAimAppliedVehicleRightOnly == vehicleRightOnly
		&& freeAimAppliedTwoHandWristOverrideActive == twoHandWristOverrideActive
		&& freeAimAppliedPrimaryGripAttachmentActive == primaryHandAttachmentDesired
		&& freeAimClenchedVisibleMask == desiredClenchedMask
		&& desiredClenchedMask == 0
		&& !twoHandWristOverrideActive
		&& (!supportContactDesired || freeAimSupportHandAttached)
		&& (!primaryHandAttachmentDesired || freeAimPrimaryHandAttached))
		return true;

	const auto applyComponent = [&](uevr::API::UObject* component, const uevr::API::FName& boneName,
		int hand, bool applyPalmOffset) -> bool
	{
		if (!uevr::API::UObjectHook::exists(component))
			return false;
		if (primaryHandAttachmentDesired && hand == primaryHandForSupport
			&& freeAimPrimaryAttachPrimeHand == hand
			&& freeAimPrimaryAttachPrimeWeapon == firstWeaponMesh
			&& freeAimPrimaryAttachPrimeGeneration == runtimeHandGeneration
			&& freeAimPrimaryAttachPrimeFramesRemaining == 0
			&& freeAimPrimaryAttachPrimeEngineTick != interactionEngineTickGeneration)
		{
			if (AttachPrimaryFakeHandToWeapon(hand))
				return true;
			// A failed capture falls back to controller tracking and earns another
			// complete priming frame before the next bounded attempt.
			freeAimPrimaryAttachPrimeFramesRemaining = 1;
			freeAimPrimaryAttachPrimeEngineTick = interactionEngineTickGeneration;
		}
		if (supportContactDesired && hand == supportHand
			&& AttachSupportFakeHandToWeapon(hand, supportContact))
			return true;
		glm::fvec3 boneTranslation{};
		glm::fquat boneRotation{};
		if (!ReadBoneTransform(component, boneName, boneTranslation, boneRotation))
			return false;
		const ControllerPalmAdapter palmAdapter = BuildControllerPalmAdapter(
			hand, boneTranslation, boneRotation, applyPalmOffset);
		if (!palmAdapter.valid)
			return false;
		auto motionState = uevr::API::UObjectHook::get_or_add_motion_controller_state(component);
		if (motionState == nullptr)
		{
			if (!freeAimHandsFailureLogged)
				uevr::API::get()->log_warn("[FreeAimHands] apply failed stage=motion-state hand=%d", hand);
			return false;
		}
		// UEVR consumes rotation_offset inversely. Express both values in its
		// controller basis so controller * inverse(bone) places the selected
		// hand bone at the physical controller without applying bone rotation twice.
		glm::fquat boneRotationVR = palmAdapter.rotation;
		if (twoHandWristOverrideActive && hand == twoHandWristPrimaryHand
			&& IsFiniteQuaternion(twoHandWristPrimaryPoseRotation)
			&& glm::length(twoHandWristPrimaryPoseRotation) > 0.0001f
			&& IsFiniteQuaternion(twoHandWristRotationDelta)
			&& glm::length(twoHandWristRotationDelta) > 0.0001f)
		{
			// The hand's normal component-space pose is H * inverse(O), where H
			// is the current primary controller pose and O is the controller-relative
			// bone offset. Rotate that visible pose by the exact same component-space
			// delta as the final two-hand weapon solve, then solve back for O. This
			// changes only wrist orientation; the controller-relative bone location
			// remains untouched below.
			const glm::fquat baseOffsetComponent = VRQuaternionToComponentSpace(boneRotationVR);
			const glm::fquat neutralHandComponent = glm::normalize(
				twoHandWristPrimaryPoseRotation * glm::inverse(baseOffsetComponent));
			const glm::fquat targetHandComponent = glm::normalize(
				twoHandWristRotationDelta * neutralHandComponent);
			const glm::fquat targetOffsetComponent = glm::normalize(
				glm::inverse(targetHandComponent) * twoHandWristPrimaryPoseRotation);
			if (IsFiniteQuaternion(baseOffsetComponent)
				&& IsFiniteQuaternion(neutralHandComponent)
				&& IsFiniteQuaternion(targetHandComponent)
				&& IsFiniteQuaternion(targetOffsetComponent)
				&& glm::length(targetOffsetComponent) > 0.0001f)
			{
				boneRotationVR = ComponentQuaternionToVRSpace(targetOffsetComponent);
			}
		}
		const glm::fvec3 location = palmAdapter.position;
		const UEVR_Vector3f locationOffset{ location.x, location.y, location.z };
		const UEVR_Quaternionf rotationOffset{ boneRotationVR.w, boneRotationVR.x, boneRotationVR.y, boneRotationVR.z };
		motionState->set_location_offset(&locationOffset);
		motionState->set_rotation_offset(&rotationOffset);
		motionState->set_hand(static_cast<uint32_t>(hand));
		motionState->set_permanent(true);
		if (!freeAimHandsOffsetsLogged)
			uevr::API::get()->log_info("[FreeAimHands] controller=%d bone=%d offset=(%.2f %.2f %.2f)",
				hand, boneName.comparison_index, location.x, location.y, location.z);
		return true;
	};
	// Hand copies always follow their matching physical controllers. Weapon-hand
	// selection may change during two-hand stabilization and must not move both
	// visible hands onto the same controller.
	const bool applied = applyComponent(freeAimFakeLeftHand, freeAimFakeLeftHandBoneName, 0, leftAnatomicalCorrection)
		&& applyComponent(freeAimFakeRightHand, freeAimFakeRightHandBoneName, 1, false);
	if (applied)
	{
		uint8_t visibleClenchedMask = freeAimClenchedVisibleMask;
		const bool staticHandsValid = freeAimClenchedHandsReady
			&& freeAimClenchedLeftHand != nullptr && freeAimClenchedRightHand != nullptr
			&& uevr::API::UObjectHook::exists(freeAimClenchedLeftHand)
			&& uevr::API::UObjectHook::exists(freeAimClenchedRightHand);
		const uint8_t requestedMask = staticHandsValid ? desiredClenchedMask : 0;
		if (requestedMask != freeAimClenchedVisibleMask)
		{
			const bool leftClosed = (requestedMask & 0x01U) != 0;
			const bool rightClosed = (requestedMask & 0x02U) != 0;
			// Hiding the hand bone deforms vertices blended between Hand and ForeArm
			// into a long wrist spike. Keep the driver pose intact and hide the
			// entire driver locally instead; its attached static fist remains visible.
			UnhideBone(freeAimFakeLeftHand, freeAimFakeLeftHandBoneName);
			UnhideBone(freeAimFakeRightHand, freeAimFakeRightHandBoneName);
			visibleClenchedMask = static_cast<uint8_t>(
				(leftClosed ? 0x01U : 0U) | (rightClosed ? 0x02U : 0U));
		}
		if (primaryHandAttachmentDesired && !freeAimPrimaryHandAttached
			&& freeAimPrimaryAttachPrimeHand == primaryHandForSupport
			&& freeAimPrimaryAttachPrimeWeapon == firstWeaponMesh
			&& freeAimPrimaryAttachPrimeGeneration == runtimeHandGeneration
			&& freeAimPrimaryAttachPrimeFramesRemaining > 0)
			--freeAimPrimaryAttachPrimeFramesRemaining;
		// When the left support hand is attached to the weapon, its watch is
		// attached by AttachSupportFakeHandToWeapon as well. Do not immediately
		// replace that gun-local attachment with a controller motion state below.
		const bool watchAttachedToSupport = supportContactDesired && supportHand == 0
			&& freeAimSupportHandAttached && freeAimSupportWatchAttached;
		freeAimWatchActive = watchAttachedToSupport;
		if (!watchAttachedToSupport && freeAimFakeWatch != nullptr
			&& freeAimFakeWatchHandBoneName.comparison_index != 0
			&& uevr::API::UObjectHook::exists(freeAimFakeWatch))
		{
			freeAimWatchActive = applyComponent(freeAimFakeWatch,
				freeAimFakeWatchHandBoneName, 0, leftAnatomicalCorrection);
			if (!freeAimWatchActive)
				uevr::API::UObjectHook::remove_motion_controller_state(freeAimFakeWatch);
			if (!freeAimWatchFailureLogged)
			{
				uevr::API::get()->log_info("[FreeAimHands] watch controller attachment=%s",
					freeAimWatchActive ? "active" : "unavailable; hidden");
				freeAimWatchFailureLogged = true;
			}
		}
		SetComponentVisibility(freeAimFakeLeftHand,
			!vehicleRightOnly && (visibleClenchedMask & 0x01U) == 0, false);
		SetComponentVisibility(freeAimFakeRightHand,
			(visibleClenchedMask & 0x02U) == 0, false);
		SetComponentVisibility(freeAimClenchedLeftHand,
			!vehicleRightOnly && (visibleClenchedMask & 0x01U) != 0);
		SetComponentVisibility(freeAimClenchedRightHand,
			(visibleClenchedMask & 0x02U) != 0);
		SetComponentVisibility(freeAimFakeWatch, vehicleRightOnly ? false : freeAimWatchActive);
		if (freeAimClenchedVisibleMask != visibleClenchedMask)
			uevr::API::get()->log_info(
				"[ClenchedHands] static visibility mask=%u weapon=%d grips=%u",
				static_cast<unsigned int>(visibleClenchedMask),
				static_cast<int>(currentWeaponEquipped),
				static_cast<unsigned int>(heldGripMask));
		freeAimClenchedVisibleMask = visibleClenchedMask;
		freeAimHandsOffsetsLogged = true;
		freeAimLeftPalmOffsetApplied = leftAnatomicalCorrection;
		freeAimAppliedSupportContactActive = supportContactDesired && freeAimSupportHandAttached;
		freeAimAppliedPrimaryGripAttachmentActive = primaryHandAttachmentDesired
			&& freeAimPrimaryHandAttached;
		freeAimAppliedVehicleRightOnly = vehicleRightOnly;
		freeAimAppliedTwoHandWristOverrideActive = twoHandWristOverrideActive;
		freeAimAppliedCalibrationWeaponId = static_cast<int>(currentWeaponEquipped);
		freeAimFakeHandsActive = true;
		freeAimFakeHandsInitializing = false;
		return true;
	}

	// A newly registered skeletal component can need several engine frames before
	// reference-pose socket transforms become nonzero. Retry the existing hidden
	// components in place; never destroy/recreate them each frame.
	uevr::API::UObjectHook::remove_motion_controller_state(freeAimFakeLeftHand);
	uevr::API::UObjectHook::remove_motion_controller_state(freeAimFakeRightHand);
	SetComponentVisibility(freeAimFakeLeftHand, false);
	SetComponentVisibility(freeAimFakeRightHand, false);
	SetComponentVisibility(freeAimClenchedLeftHand, false);
	SetComponentVisibility(freeAimClenchedRightHand, false);
	if (freeAimFakeHandsAlignmentFramesRemaining > 0)
	{
		--freeAimFakeHandsAlignmentFramesRemaining;
		freeAimFakeHandsInitializing = true;
		return false;
	}

	uevr::API::get()->log_warn("[FreeAimHands] initialization blocked stage=nonzero-hand-transform timeout=30");
	freeAimHandsCreationBlocked = true;
	freeAimHandsBlockedCharacter = freeAimFakeHandsCharacter;
	freeAimFakeHandsInitializing = false;
	return false;
}

void WeaponManager::RemoveFreeAimFakeHands(bool destroyComponents)
{
	const bool hadComponents = freeAimFakeLeftHand != nullptr || freeAimFakeRightHand != nullptr
		|| freeAimClenchedLeftHand != nullptr || freeAimClenchedRightHand != nullptr
		|| freeAimFakeWatch != nullptr;
	int destroyedComponentCount = 0;
	int hiddenOnlyComponentCount = 0;
	RestoreSupportFakeHandAttachment();
	RestorePrimaryFakeHandAttachment();
	if (freeAimFakeLeftHand != nullptr && freeAimFakeLeftHandBoneName.comparison_index != 0)
		UnhideBone(freeAimFakeLeftHand, freeAimFakeLeftHandBoneName);
	if (freeAimFakeRightHand != nullptr && freeAimFakeRightHandBoneName.comparison_index != 0)
		UnhideBone(freeAimFakeRightHand, freeAimFakeRightHandBoneName);

	const auto removeComponent = [&](uevr::API::UObject*& component)
	{
		if (component == nullptr)
			return;
		if (uevr::API::UObjectHook::exists(component))
		{
			uevr::API::UObjectHook::remove_motion_controller_state(component);
			SetComponentVisibility(component, false);
			if (destroyComponents)
			{
				auto componentClass = component->get_class();
				auto destroy = componentClass != nullptr
					? componentClass->find_function(L"DestroyComponent") : nullptr;
				if (destroy != nullptr)
				{
					std::vector<uint8_t> params(destroy->get_properties_size());
					SetReflectedBoolParameter(destroy, params, L"bPromoteChildren", false);
					destroy->call(component, params.data());
					++destroyedComponentCount;
				}
				else
				{
					// Hiding remains the fail-closed fallback, but retain a diagnostic:
					// clearing a live pointer without destroying the component is what
					// allowed an old clenched hand to survive player respawn.
					++hiddenOnlyComponentCount;
				}
			}
		}
		if (destroyComponents)
			component = nullptr;
	};
	removeComponent(freeAimFakeLeftHand);
	removeComponent(freeAimFakeRightHand);
	removeComponent(freeAimClenchedLeftHand);
	removeComponent(freeAimClenchedRightHand);
	removeComponent(freeAimFakeWatch);
	if (playerManager->handScaleWatch != nullptr
		&& uevr::API::UObjectHook::exists(playerManager->handScaleWatch))
		uevr::API::UObjectHook::remove_motion_controller_state(playerManager->handScaleWatch);
	freeAimWatchActive = false;
	freeAimFakeHandsActive = false;
	freeAimAppliedSupportContactActive = false;
	freeAimAppliedPrimaryGripAttachmentActive = false;
	freeAimClenchedVisibleMask = 0;
	ApplyVehicleNativeRightArmPresentation(false);
	if (destroyComponents)
	{
		cachedControllerPalmAdapterComponents = {};
		cachedControllerPalmAdapterBones = {};
		cachedControllerPalmAdapters = {};
		freeAimFakeHandsCharacter = nullptr;
		freeAimFakeHandsReady = false;
		freeAimFakeHandsInitialized = false;
		freeAimFakeHandsInitializing = false;
		freeAimFakeHandsWarmupFrames = 0;
		freeAimFakeHandsAlignmentFramesRemaining = 0;
		freeAimFakeLeftHandBoneName = uevr::API::FName{};
		freeAimFakeRightHandBoneName = uevr::API::FName{};
		freeAimFakeWatchHandBoneName = uevr::API::FName{};
		freeAimClenchedHandsReady = false;
		freeAimClenchedLeftMesh = nullptr;
		freeAimClenchedRightMesh = nullptr;
		freeAimClenchedAssetFailureLogged = false;
		freeAimLeftPalmOffsetApplied = false;
		freeAimAppliedVehicleRightOnly = false;
		freeAimAppliedTwoHandWristOverrideActive = false;
		freeAimAppliedCalibrationWeaponId = -1;
		freeAimPrimaryAttachFailureDiagnosticLogged = false;
		freeAimBareHandCapabilityProbeLogged = false;
		if (hadComponents)
		{
			uevr::API::get()->log_info(
				"[FreeAimHands] transient copies destroyed=%d hiddenOnly=%d motionStatesRemoved=true",
				destroyedComponentCount, hiddenOnlyComponentCount);
		}
	}
}

void WeaponManager::ProcessNativeVehicleHandPoseTrace()
{
	const bool inVehicle = playerManager != nullptr && playerManager->isInVehicle;
	if (!inVehicle)
	{
		nativeVehicleHandPoseTraceWasInVehicle = false;
		nativeVehicleHandPoseTraceAt = 0;
		return;
	}

	const uint64_t now = GetTickCount64();
	if (!nativeVehicleHandPoseTraceWasInVehicle)
	{
		nativeVehicleHandPoseTraceWasInVehicle = true;
		if (!nativeVehicleHandPoseTraceCaptured)
		{
			nativeVehicleHandPoseTraceAt = now + 1500;
			uevr::API::get()->log_info(
				"[NativeHandPose] armed source=vehicle-entry settle_ms=1500 weapon=%d",
				static_cast<int>(currentWeaponEquipped));
		}
	}
	if (nativeVehicleHandPoseTraceCaptured || nativeVehicleHandPoseTraceAt == 0
		|| now < nativeVehicleHandPoseTraceAt)
		return;

	auto nativeHands = playerManager->handScaleHands;
	if (nativeHands == nullptr || !uevr::API::UObjectHook::exists(nativeHands)
		|| nativeHands->get_class() == nullptr)
	{
		// Player presentation can initialize a frame later than vehicle state.
		nativeVehicleHandPoseTraceAt = now + 250;
		return;
	}

	int32_t boneCount = -1;
	if (!ReadBoneCount(nativeHands, boneCount) || boneCount <= 0)
	{
		uevr::API::get()->log_warn(
			"[NativeHandPose] capture postponed reason=bone-count-unavailable class=%ls",
			nativeHands->get_class()->get_full_name().c_str());
		nativeVehicleHandPoseTraceAt = now + 500;
		return;
	}

	const int32_t cappedCount = (std::min)(boneCount, 256);
	int32_t loggedCount = 0;
	uevr::API::get()->log_info(
		"[NativeHandPose] capture begin class=%ls object=%ls bones=%d weapon=%d vehicleFreeAim=%d space=component",
		nativeHands->get_class()->get_full_name().c_str(), nativeHands->get_full_name().c_str(),
		cappedCount, static_cast<int>(currentWeaponEquipped), IsVehicleFreeAimActive() ? 1 : 0);
	for (int32_t index = 0; index < cappedCount; ++index)
	{
		std::wstring boneText;
		uevr::API::FName boneName{};
		if (!ReadBoneNameAtIndex(nativeHands, index, boneText, &boneName))
			continue;
		glm::fvec3 position{};
		glm::fquat rotation{};
		if (!ReadBoneTransform(nativeHands, boneName, position, rotation, 2))
		{
			uevr::API::get()->log_info(
				"[NativeHandPose] bone index=%d name=%ls readable=0", index, boneText.c_str());
			continue;
		}
		uevr::API::get()->log_info(
			"[NativeHandPose] bone index=%d name=%ls p=(%.6f %.6f %.6f) q=(%.7f %.7f %.7f %.7f)",
			index, boneText.c_str(), position.x, position.y, position.z,
			rotation.x, rotation.y, rotation.z, rotation.w);
		++loggedCount;
	}

	if (loggedCount <= 0)
	{
		uevr::API::get()->log_warn("[NativeHandPose] capture postponed reason=no-readable-transforms");
		nativeVehicleHandPoseTraceAt = now + 500;
		return;
	}
	nativeVehicleHandPoseTraceCaptured = true;
	nativeVehicleHandPoseTraceAt = 0;
	uevr::API::get()->log_info(
		"[NativeHandPose] capture complete logged=%d total=%d space=component",
		loggedCount, cappedCount);
}

void WeaponManager::ProcessFreeAimWeaponHands(bool force)
{
	ProcessNativeVehicleHandPoseTrace();
	const bool fakeHandsWereActive = freeAimFakeHandsActive;
	const bool firstWeaponReady = HasUsableWeapon(true);
	const bool unarmedFreeTracking = currentWeaponEquipped == Unarmed;
	const bool vehicleFreeAimPresentation = IsVehicleFreeAimActive();
	// Hand clones follow their own controller states. They remain eligible while
	// a ranged weapon is equipped even when the weapon itself is intentionally
	// detached for GTA's native idle/preview pose.
	const bool supportedWeaponTracking = firstWeaponReady && IsFreeAimHandsEligibleWeapon();
	const bool shouldShowHands = settingsManager->enableFreeAimWeaponHands
		&& playerManager->isInControl
		&& (!playerManager->isInVehicle || vehicleFreeAimPresentation)
		&& !playerManager->weaponWheelEnabled
		&& (unarmedFreeTracking || supportedWeaponTracking);

	bool showHandsOnFreeAimWeapon = false;
	if (shouldShowHands)
		showHandsOnFreeAimWeapon = CreateFreeAimFakeHands() && ApplyFreeAimFakeHandPresentation();
	if (vehicleFreeAimPresentation && showHandsOnFreeAimWeapon && !fakeHandsWereActive)
	{
		// Vehicle attachment can run one frame before its controller-palm clone is
		// available. Reconfigure once so rotation uses the same calibrated palm
		// relation as on foot instead of retaining the temporary default rotation.
		motionConfiguredFirstWeaponMesh = nullptr;
		motionConfiguredFirstHand = -1;
		motionConfiguredFirstCalibrationRole = -1;
	}
	const bool initializationPending = freeAimFakeHandsInitializing;
	if (!showHandsOnFreeAimWeapon && !initializationPending)
	{
		if (shouldShowHands && !freeAimHandsFailureLogged)
		{
			uevr::API::get()->log_warn("[FreeAimHands] split presentation unavailable; native hands restored");
			freeAimHandsFailureLogged = true;
		}
		// Preserve initialized components and terminal failure evidence. They are
		// hidden and reused or remain fail-closed until player replacement/restore.
		RemoveFreeAimFakeHands(false);
	}

	const bool vehicleRightOnly = vehicleFreeAimPresentation && showHandsOnFreeAimWeapon;
	const bool desiredVisible = vehicleFreeAimPresentation
		? true
		: !settingsManager->enableFreeAimWeaponHands || !showHandsOnFreeAimWeapon;
	const bool changed = !freeAimWeaponHandsVisibilityInitialized || freeAimWeaponHandsVisible != desiredVisible;
	playerManager->SetHandsScaled(desiredVisible, force || changed);
	if (showHandsOnFreeAimWeapon)
		playerManager->SetWatchScaled(vehicleRightOnly, force || changed);
	else if (vehicleFreeAimPresentation)
		playerManager->SetWatchScaled(true, force || changed);
	freeAimWeaponHandsVisible = desiredVisible;
	freeAimWeaponHandsVisibilityInitialized = true;
	RefreshRuntimeHandRoles("hand-presentation");
}

void WeaponManager::RestoreFreeAimWeaponHands()
{
	RemoveCustomAkimboVisual("presentation-restore");
	ResetMotionThrowableFlight("presentation-restore", true);
	ResetPhysicalThrowableProbe("presentation-restore", false);
	ResetRuntimeHandState("hand-presentation-restore", true, true);
	SuspendMagneticIdleSlot();
	RemoveFreeAimFakeHands(true);
	freeAimHandsCreationBlocked = false;
	freeAimHandsBlockedCharacter = nullptr;
	freeAimHandsFailureLogged = false;
	freeAimWatchFailureLogged = false;
	freeAimWeaponHandsPresentationActive = false;
	ApplyVehicleNativeRightArmPresentation(false);
	playerManager->SetHandsScaled(true, true);
	playerManager->SetWatchScaled(true, true);
}

void WeaponManager::ProcessWeaponHandling(float delta)
{
	if (settingsManager->debugMod) uevr::API::get()->log_info("ProcessWeaponHandling()");
	const bool vehicleFreeAim = IsVehicleFreeAimActive();
	if (memoryManager != nullptr)
	{
		memoryManager->SetVehicleShotTraceModeActive(vehicleFreeAim
			&& !playerManager->weaponWheelEnabled && motionWeaponTrackingEnabled);
		if (playerManager->isInVehicle
			&& (!vehicleFreeAim || playerManager->weaponWheelEnabled || !motionWeaponTrackingEnabled))
			memoryManager->ClearNativeShotTraceOverride();
		else if (!playerManager->isInVehicle && playerManager->wasInVehicle)
			memoryManager->SetVehicleShotTraceOverrideArmed(false);
	}
	if (vehicleFaceButtonNoShotCheckPending
		&& GetTickCount64() >= vehicleFaceButtonNoShotCheckTime)
	{
		vehicleFaceButtonNoShotCheckPending = false;
		if (memoryManager != nullptr
			&& memoryManager->GetNativeShotTraceSequenceSnapshot() == vehicleFaceButtonTraceSequenceAtPress)
		{
			uevr::API::get()->log_info(
				"[VehicleFreeAim] request seq=%u ended without native damage trace reason=short-press-empty-clip-or-native-rejection",
				vehicleFaceButtonSequence);
		}
	}
	if (vehicleFaceButtonFirePending.exchange(false, std::memory_order_acq_rel))
	{
		if (vehicleFreeAim)
		{
			vehicleFaceButtonLastLoggedSequence = vehicleFaceButtonSequence;
			uevr::API::get()->log_info(
				"[VehicleFreeAim] face-fire request reached engine seq=%u; awaiting native cadence/damage trace",
				vehicleFaceButtonLastLoggedSequence);
		}
		else
			uevr::API::get()->log_info("[VehicleFreeAim] face-fire request rejected before engine dispatch");
	}
	// The vehicle presentation remains ordinary driving; only the validated local
	// player trace/tracer snapshots are redirected while the mode is eligible.
	if (playerManager->isInVehicle && !playerManager->wasInVehicle && !vehicleFreeAim)
	{
		UpdateActualWeaponMesh();
		UnhookAndRepositionWeapon();
	}

	if (!playerManager->isInVehicle && playerManager->wasInVehicle)
	{
		UpdateActualWeaponMesh();
	}

	if (!motionWeaponTrackingEnabled)
	{
		memoryManager->FirstWeaponIsShooting = false;
		return;
	}

	if (firstWeaponMesh == nullptr || (playerManager->isInVehicle
		&& cameraController->currentCameraMode != CameraController::AimWeaponFromCar
		&& !vehicleFreeAim)) // check unsupported vehicle weapons before returning
		return;

	// If weapon equipped doesn't have a muzzle flash, we need to detect the shoot from memory. HelicannonFirstPerson cam mod never has muzzle flash.
	bool shootDetectionFromMemory = cameraController->currentCameraMode == CameraController::HelicannonFirstPerson ? true : false;

	glm::fvec3 positionRecoilForce = { 0.0f, 0.0f, 0.0f };
	glm::fvec3 rotationRecoilForceEuler = { 0.0f, 0.0f, 0.0f };
	switch (currentWeaponEquipped)
	{
	case Pistol:
		positionRecoilForce = { 0.0f, -0.005f, -0.5f };
		rotationRecoilForceEuler = { -0.08f, 0.0f, 0.0f };
		break;
	case PistolSilenced:
		positionRecoilForce = { 0.0f, -0.005f, -0.5f };
		rotationRecoilForceEuler = { -0.05f, 0.0f, 0.0f };
		break;
	case DesertEagle:
		positionRecoilForce = { 0.0f, -0.005f, -2.5f };
		rotationRecoilForceEuler = { -0.15f, 0.0f, 0.0f };
		break;
	case Shotgun:
		positionRecoilForce = { 0.0f, -0.005f, -5.0f };
		rotationRecoilForceEuler = { -0.3f, 0.0f, 0.0f };
		break;
	case Sawnoff:
		positionRecoilForce = { 0.0f, -0.005f, -5.0f };
		rotationRecoilForceEuler = { -0.3f, 0.0f, 0.0f };
		break;
	case Spas12:
		positionRecoilForce = { 0.0f, -0.005f, -5.0f };
		rotationRecoilForceEuler = { -0.3f, 0.0f, 0.0f };
		break;
	case MicroUzi:
		positionRecoilForce = { 0.0f, -0.005f, -1.0f };
		rotationRecoilForceEuler = { -0.01f, 0.0f, 0.0f };
		break;
	case Mp5:
		positionRecoilForce = { 0.0f, -0.005f, -1.0f };
		rotationRecoilForceEuler = { -0.01f, 0.0f, 0.0f };
		break;
	case Ak47:
		positionRecoilForce = { 0.0f, -0.005f, -1.0f };
		rotationRecoilForceEuler = { -0.01f, 0.0f, 0.0f };
		break;
	case M4:
		positionRecoilForce = { 0.0f, -0.005f, -1.0f };
		rotationRecoilForceEuler = { -0.01f, 0.0f, 0.0f };
		break;
	case Tec9:
		positionRecoilForce = { 0.0f, -0.005f, -1.0f };
		rotationRecoilForceEuler = { -0.01f, 0.0f, 0.0f };
		break;
	case Rifle: // "cuntgun"
		positionRecoilForce = { 0.0f, -0.005f, -2.0f };
		rotationRecoilForceEuler = { -0.02f, 0.0f, 0.0f };
		shootDetectionFromMemory = true;

		break;
	case Sniper:
		positionRecoilForce = { 0.0f, -0.005f, -2.0f };
		rotationRecoilForceEuler = { -0.02f, 0.0f, 0.0f };
		shootDetectionFromMemory = true;
		break;
	case RocketLauncher:
		positionRecoilForce = { 0.0f, -0.005f, -3.0f };
		rotationRecoilForceEuler = { -0.02f, 0.0f, 0.0f };
		shootDetectionFromMemory = true;
		break;
	case RocketLauncherHs : // RocketLauncherHeatSeek
		positionRecoilForce = { 0.0f, -0.005f, -3.0f };
		rotationRecoilForceEuler = { -0.02f, 0.0f, 0.0f };
		shootDetectionFromMemory = true;
		break;
	case Flamethrower:
		positionRecoilForce = { -0.0f, 0.5f, -0.5f };
		rotationRecoilForceEuler = { -0.0f, 0.0f, 0.0f };
		break;
	case Minigun:	
		//positionRecoilForce = { 0.0f, -0.005f, -5.0f };
		//rotationRecoilForceEuler = { -0.3f, 0.0f, 0.0f };
		positionRecoilForce = { 0.0f, -0.005f, -1.5f };
		rotationRecoilForceEuler = { -0.01f, 0.0f, 0.0f };
		break;
	case Grenade:
	case Teargas:
	case Molotov:
		// The motion-controller transform is already owned by the normal tracked
		// weapon path. Do not run firearm recoil or the default native unhook.
		return;
	case Detonator:
		return;
	case SprayCan:
		return;
	case Extinguisher:
		return;
	case Camera:
		HandleCameraWeaponAiming();
		return;

	default:

		UnhookAndRepositionWeapon();
		return;
	}

	if (shootDetectionFromMemory)
	{
		firstWeaponIsShooting = currentWeaponEquipped == previousWeaponEquipped ? memoryManager->FirstWeaponIsShooting : false;
	}

	LogSpreadProbeIfShot(true, firstWeaponIsShooting);
	if (secondWeaponMesh)
		LogSpreadProbeIfShot(false, secondWeaponIsShooting);
	RedirectWorldShotEffects(true);
	
	// Recoil disabled means the plugin must leave UEVR's controller transform
	// alone. Calling ApplyRecoil(false) still writes offsets every frame and can
	// fight the left-hand attachment, making the weapon alternate between poses.
	if (settingsManager->enableMotionWeaponRecoil)
	{
		ApplyRecoil(firstWeaponMesh, firstWeaponIsShooting, positionRecoilForce, rotationRecoilForceEuler, delta);
		if (secondWeaponMesh)
			ApplyRecoil(secondWeaponMesh, secondWeaponIsShooting, positionRecoilForce, rotationRecoilForceEuler, delta);
	}

	memoryManager->FirstWeaponIsShooting = false;
}

void WeaponManager::ApplyRecoil(uevr::API::UObject* weaponMesh, bool isShooting, const glm::fvec3& positionRecoilForce, const glm::fvec3& rotationRecoilForceEuler, float delta)
{
	auto motionState = uevr::API::UObjectHook::get_motion_controller_state(weaponMesh);
	if (motionState == nullptr)
		return;
	auto [recoilPosPtr, recoilRotPtr] = GetRecoilState(weaponMesh);
	auto& recoilPos = *recoilPosPtr;
	auto& recoilRot = *recoilRotPtr;
	const glm::fvec3 gripPosition = GetWeaponGripPositionOffset(weaponMesh);
	const int recoilHand = weaponMesh == firstWeaponMesh ? motionConfiguredFirstHand : motionConfiguredSecondHand;

	if (isShooting)
	{
		recoilPos += positionRecoilForce;
		recoilRot += rotationRecoilForceEuler;

		glm::fquat rotQuat = glm::fquat(recoilRot);
		if (settingsManager->enableGripCalibration)
			rotQuat = GetWeaponGripRotationOffset(recoilHand) * rotQuat;
		rotQuat = ComposeTwoHandRotationOffset(rotQuat, weaponMesh);
		const glm::fvec3 composedPosition = gripPosition + recoilPos;
		UEVR_Vector3f pos = { composedPosition.x, composedPosition.y, composedPosition.z };
		UEVR_Quaternionf rot = { rotQuat.w, rotQuat.x, rotQuat.y, rotQuat.z };

		motionState->set_location_offset(&pos);
		motionState->set_rotation_offset(&rot);
		motionState->set_permanent(true);
	}
	else
	{
		// Smoothly return position to base
		recoilPos = glm::mix(recoilPos, defaultWeaponPosition, delta * recoilPositionRecoverySpeed);

		glm::fquat currentQuat = glm::fquat(recoilRot);
		glm::fquat targetQuat = settingsManager->enableGripCalibration
			? GetWeaponGripRotationOffset(recoilHand)
			: glm::fquat(defaultWeaponRotationEuler);
		glm::fquat smoothedQuat = glm::slerp(currentQuat, targetQuat, delta * recoilRotationRecoverySpeed);
		recoilRot = glm::eulerAngles(smoothedQuat);
		smoothedQuat = ComposeTwoHandRotationOffset(smoothedQuat, weaponMesh);

		const glm::fvec3 composedPosition = gripPosition + recoilPos;
		UEVR_Vector3f pos = { composedPosition.x, composedPosition.y, composedPosition.z };
		UEVR_Quaternionf rot = { smoothedQuat.w, smoothedQuat.x, smoothedQuat.y, smoothedQuat.z };

		motionState->set_location_offset(&pos);
		motionState->set_rotation_offset(&rot);
	}
}

WeaponManager::WeaponRecoilState WeaponManager::GetRecoilState(uevr::API::UObject* weaponMesh) {
	if (weaponMesh == firstWeaponMesh)
		return { &currentFirstWeaponRecoilPosition, &currentFirstWeaponRecoilRotationEuler };
	else
		return { &currentSecondWeaponRecoilPosition, &currentSecondWeaponRecoilRotationEuler };
}

glm::fvec3 WeaponManager::GetWeaponGripPositionOffset(int hand) const
{
	// The correction is for the solo-left-primary pose only. Two-hand weapon
	// stabilization owns the weapon basis and must retain its existing origin.
	const bool twoHandPathActive = gripStateMask.load(std::memory_order_acquire) == 3
		|| twoHandSupportActive;
	const glm::fvec3 defaultPosition = hand == 0 && !twoHandPathActive
		? leftControllerWeaponGripOffset : defaultWeaponPosition;
	if (!settingsManager->enableGripCalibration)
		return defaultPosition;
	// Position is stored as a compact controller-relative offset and does not
	// require the visible native palm adapter. Vehicle presentation hides the
	// native right arm, so coupling position lookup to that adapter incorrectly
	// reduced a valid calibrated offset to (0,0,0).
	GripCalibrationTransform stored;
	if (ReadGripCalibrationTransform(static_cast<int>(currentWeaponEquipped),
		GripCalibrationRecord::PrimaryGrip, hand, stored, nullptr)
		&& stored.valid && !stored.palmFramed && IsFiniteVector(stored.position))
		return glm::inverse(UEVRQuatConverter()) * stored.position;
	GripCalibrationTransform saved;
	if (!GetCanonicalPrimaryGripForHand(hand, saved))
		return defaultPosition;
	return glm::inverse(UEVRQuatConverter()) * saved.position;
}

glm::fquat WeaponManager::GetWeaponGripRotationOffset(int hand) const
{
	const glm::fquat defaultRotation = glm::normalize(glm::fquat(defaultWeaponRotationEuler));
	if (!settingsManager->enableGripCalibration)
		return defaultRotation;
	GripCalibrationTransform saved;
	if (!GetCanonicalPrimaryGripForHand(hand, saved))
		return defaultRotation;
	return ComponentQuaternionToVRSpace(saved.rotation);
}

glm::fvec3 WeaponManager::GetWeaponGripPositionOffset(uevr::API::UObject* weaponMesh) const
{
	if (weaponMesh == firstWeaponMesh)
		return GetWeaponGripPositionOffset(motionConfiguredFirstHand);
	if (weaponMesh == secondWeaponMesh)
		return GetWeaponGripPositionOffset(motionConfiguredSecondHand);
	return defaultWeaponPosition;
}

void WeaponManager::HandleCameraWeaponAiming()
{
	//The camera aiming vector doesn't works like every weapons. Instead it's based on the camera matrix (the original game's one, not the UEVR).
	//So the aiming has to be manually controlled with joystick.

	if (settingsManager->debugMod) uevr::API::get()->log_info("HandleCameraWeaponAiming()");
	if (Utilities::KismetMathLibrary == nullptr)
		Utilities::InitHelperClasses();
	if (Utilities::KismetMathLibrary == nullptr)
		return;

	// detaching the weaponMesh from motion controls
	if (cameraController->currentCameraMode == CameraController::Camera  && cameraController->previousCameraMode != CameraController::Camera )
	{
		uevr::API::UObjectHook::remove_motion_controller_state(firstWeaponMesh);

		Utilities::ParameterDetachFromParent detachFromParent_params;
		detachFromParent_params.maintainWorldPosition = true;
		detachFromParent_params.callModify = false;
		firstWeaponMesh->call_function(L"DetachFromParent", &detachFromParent_params);
	}

	if (cameraController->currentCameraMode != CameraController::Camera  && cameraController->previousCameraMode == CameraController::Camera )
	{
		uevr::API::UObjectHook::get_or_add_motion_controller_state(firstWeaponMesh);
	}

	// Manually sets the camera Mesh to a position in front of the player.
	if (cameraController->currentCameraMode == CameraController::Camera)
	{
		glm::fvec3 cameraOffsetsPoint1 = { 13.8476, -11.6162, -1.72577 };
		glm::fvec3 cameraOffsetsPoint2 = { 27.6432, -11.6162, -2.84382 };

		// Convert local offsets into world positions
		glm::fvec3 worldOffset1 = (cameraController->forwardVectorUE * cameraOffsetsPoint1.x) + ( cameraController->rightVectorUE * cameraOffsetsPoint1.y) + (cameraController->upVectorUE* cameraOffsetsPoint1.z);
		glm::fvec3 worldOffset2 = (cameraController->forwardVectorUE * cameraOffsetsPoint2.x) + ( cameraController->rightVectorUE * cameraOffsetsPoint2.y) + (cameraController->upVectorUE * cameraOffsetsPoint2.z);

		glm::fvec3 weaponPoint1 = cameraController->cameraPositionUE + (cameraController->forwardVectorUE * 35.0f) + worldOffset1;
		glm::fvec3 weaponPoint2 = cameraController->cameraPositionUE + (cameraController->forwardVectorUE * 35.0f) + worldOffset2;

		// Apply position to weaponMesh
		Utilities::Parameter_K2_SetWorldOrRelativeLocation setWorldLocation_params{};
		setWorldLocation_params.bSweep = false;
		setWorldLocation_params.bTeleport = true;
		setWorldLocation_params.newLocation = weaponPoint1;
		firstWeaponMesh->call_function(L"K2_SetWorldLocation", &setWorldLocation_params);

		// FindLookAtRotation from Point1 to Point2
		Utilities::ParameterFindLookAtRotation lookAtRotationParams;
		lookAtRotationParams.start = weaponPoint1;
		lookAtRotationParams.target = weaponPoint2;
		Utilities::KismetMathLibrary->call_function(L"FindLookAtRotation", &lookAtRotationParams);

		// Apply rotation to weaponMesh
		Utilities::Parameter_K2_SetWorldOrRelativeRotation setWorldRotation_params{};
		setWorldRotation_params.bSweep = false;
		setWorldRotation_params.bTeleport = true;
		setWorldRotation_params.newRotation = lookAtRotationParams.outRotation;
		firstWeaponMesh->call_function(L"K2_SetWorldRotation", &setWorldRotation_params);
	}
}

void WeaponManager::UnhookAndRepositionWeapon(bool restoreHands, bool force)
{
	const bool meleeGripActive = currentWeaponEquipped >= BrassKnuckles
		&& currentWeaponEquipped <= Cane
		&& magneticGripHand >= 0
		&& magneticGripWeaponId == static_cast<int>(currentWeaponEquipped)
		&& playerManager != nullptr
		&& playerManager->isInControl
		&& !playerManager->isInVehicle
		&& !playerManager->weaponWheelEnabled
		&& !force;
	if (meleeGripActive)
	{
		// Camera/visibility transitions call this shared helper for guns. Do not
		// remove the controller state or zero the relative transform of a melee
		// mesh while its magnetic grip is still valid.
		motionWeaponTrackingEnabled = true;
		visualWeaponTrackingEnabled = true;
		magneticReleaseRequested = false;
		return;
	}
	if (settingsManager->debugMod) uevr::API::get()->log_info("UnhookAndRepositionWeapon()");
	if (restoreHands)
	{
		RemoveFreeAimFakeHands();
		freeAimWeaponHandsPresentationActive = false;
		playerManager->SetHandsScaled(true, true);
		freeAimWeaponHandsVisible = true;
		freeAimWeaponHandsVisibilityInitialized = true;
	}

	if (firstWeaponMesh == nullptr)
		return;
	if (!uevr::API::UObjectHook::get_motion_controller_state(firstWeaponMesh))
		return;

	uevr::API::UObjectHook::remove_motion_controller_state(firstWeaponMesh);

	//Reset weapon position and rotation for melee weapons
	Utilities::Parameter_K2_SetWorldOrRelativeLocation setRelativeLocation_params{};
	setRelativeLocation_params.bSweep = false;
	setRelativeLocation_params.bTeleport = true;
	setRelativeLocation_params.newLocation = glm::fvec3(0.0f, 0.0f, 0.0f);
	firstWeaponMesh->call_function(L"K2_SetRelativeLocation", &setRelativeLocation_params);

	Utilities::Parameter_K2_SetWorldOrRelativeRotation setRelativeRotation_params{};
	setRelativeRotation_params.bSweep = false;
	setRelativeRotation_params.bTeleport = true;
	setRelativeRotation_params.newRotation = { 0.0f, 0.0f, 0.0f };
	firstWeaponMesh->call_function(L"K2_SetRelativeRotation", &setRelativeRotation_params);

	if (secondWeaponMesh == nullptr)
		return;

	uevr::API::UObjectHook::remove_motion_controller_state(secondWeaponMesh);
	secondWeaponMesh->call_function(L"K2_SetRelativeLocation", &setRelativeLocation_params);
	secondWeaponMesh->call_function(L"K2_SetRelativeRotation", &setRelativeRotation_params);
}
