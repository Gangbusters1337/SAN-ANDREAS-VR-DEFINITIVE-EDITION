#include "uevr/Plugin.hpp"
#include "uevr/API.hpp"
#include "MemoryManager.h"
#include "SettingsManager.h"
#include "CameraController.h"
#include "ControlGuideOverlay.h"
#include "PlayerManager.h"
#include "WeaponManager.h"
#include "Utilities.h"
#include <string>
#include <exception>
#include <chrono>
#include <cstdint>
#include <atomic>
#include <windows.h>

using namespace uevr;

#define PLUGIN_LOG_ONCE(...) {\
    static bool _logged_ = false; \
    if (!_logged_) { \
        _logged_ = true; \
        API::get()->log_info(__VA_ARGS__); \
    }}

class GTASADE_Plugin : public uevr::Plugin {
private:
	MemoryManager memoryManager;
	SettingsManager settingsManager;
	CameraController cameraController;
	PlayerManager playerManager;
	WeaponManager weaponManager;
	ControlGuideOverlay controlGuideOverlay;
	UEVR_ActionHandle thumbRestDpadAction = nullptr;
	UEVR_ActionHandle rightThumbRestCalibrationAction = nullptr;
	bool thumbRestModifierActive = false;
	std::atomic<bool> thumbRestTouchActive{ false };
	std::atomic<bool> thumbRestDpadUsed{ false };
	std::atomic<bool> thumbRestActionRebindRequested{ false };
	ULONGLONG thumbRestActionRefreshAt = 0;
	enum class ThumbRestHapticRequest : uint8_t { None = 0, PinOn, AutoHide };
	std::atomic<uint8_t> pendingThumbRestHaptic{ static_cast<uint8_t>(ThumbRestHapticRequest::None) };
	bool thumbRestTouchWasActiveInput = false;
	bool dualThumbRestRawActive = false;
	bool dualThumbRestCalibrationActive = false;
	std::chrono::steady_clock::time_point dualThumbRestStartedAt{};
	WORD thumbRestDpadLastDirection = 0;
	WORD thumbRestDpadPulseDirection = 0;
	uint8_t thumbRestDpadPulseSamplesRemaining = 0;
	int thumbRestDpadProbeState = -1;
	const char* thumbRestDpadResolvedPath = nullptr;
	const char* rightThumbRestCalibrationResolvedPath = nullptr;
	std::atomic<uint8_t> aircraftCameraRevealRequestMask{ 0 };
	bool aircraftLeftStickPressHeld = false;
	bool aircraftCameraRevealActive = false;
	bool aircraftCameraRevealRestoring = false;
	ULONGLONG aircraftCameraRevealExpiresAt = 0;
	CameraController::VehicleCameraMode aircraftCameraRevealRestoreMode = CameraController::VehicleCameraMode::Close;

	static bool IsRetractableLandingGearPlane(int modelId)
	{
		return modelId == 519  // Shamal
			|| modelId == 520   // Hydra
			|| modelId == 577;  // AT-400
	}

	static void PulseThumbRestDpadHaptic()
	{
		const auto* pluginParam = API::get()->param();
		const auto* vr = pluginParam != nullptr ? pluginParam->vr : nullptr;
		if (vr == nullptr || vr->is_runtime_ready == nullptr || !vr->is_runtime_ready()
			|| vr->is_hmd_active == nullptr || !vr->is_hmd_active()
			|| vr->get_right_joystick_source == nullptr
			|| vr->trigger_haptic_vibration == nullptr)
			return;
		const UEVR_InputSourceHandle source = vr->get_right_joystick_source();
		if (source != nullptr)
			vr->trigger_haptic_vibration(0.0f, 0.04f, 110.0f, 0.20f, source);
	}

	static bool PulseLeftThumbRestHaptic(float duration, float frequency, float amplitude,
		float delayedSeconds = 0.0f)
	{
		const auto* pluginParam = API::get()->param();
		const auto* vr = pluginParam != nullptr ? pluginParam->vr : nullptr;
		if (vr == nullptr || vr->is_runtime_ready == nullptr || !vr->is_runtime_ready()
			|| vr->is_hmd_active == nullptr || !vr->is_hmd_active()
			|| vr->trigger_haptic_vibration == nullptr)
			return false;
		UEVR_InputSourceHandle source = vr->get_left_joystick_source != nullptr
			? vr->get_left_joystick_source() : nullptr;
		if (source == nullptr && vr->get_right_joystick_source != nullptr)
		{
			source = vr->get_right_joystick_source();
			static bool fallbackLogged = false;
			if (source != nullptr && !fallbackLogged)
			{
				fallbackLogged = true;
				API::get()->log_info("%s",
					"[ThumbRestHud] left haptic source unavailable; using proven right-controller fallback");
			}
		}
		if (source == nullptr)
			return false;
		vr->trigger_haptic_vibration(delayedSeconds, duration, frequency, amplitude, source);
		return true;
	}

	void DispatchPendingThumbRestHaptic()
	{
		const auto request = static_cast<ThumbRestHapticRequest>(
			pendingThumbRestHaptic.exchange(
				static_cast<uint8_t>(ThumbRestHapticRequest::None), std::memory_order_acq_rel));
		if (request == ThumbRestHapticRequest::None)
			return;

		bool dispatched = false;
		const char* pattern = "unknown";
		switch (request)
		{
		case ThumbRestHapticRequest::PinOn:
			pattern = "pin-long";
			dispatched = PulseLeftThumbRestHaptic(0.24f, 105.0f, 0.65f);
			break;
		case ThumbRestHapticRequest::AutoHide:
			pattern = "auto-hide-triple";
			dispatched = PulseLeftThumbRestHaptic(0.03f, 145.0f, 0.58f);
			dispatched = PulseLeftThumbRestHaptic(0.03f, 145.0f, 0.58f, 0.06f) || dispatched;
			dispatched = PulseLeftThumbRestHaptic(0.03f, 145.0f, 0.58f, 0.12f) || dispatched;
			break;
		default:
			break;
		}
		API::get()->log_info("[ThumbRestHud] haptic pattern=%s dispatched=%s layer=xinput",
			pattern, dispatched ? "true" : "false");
	}

	void PublishThumbRestTouchState(bool active, bool consumed = false)
	{
		if (active && !thumbRestTouchWasActiveInput)
		{
			thumbRestDpadUsed.store(false, std::memory_order_release);
			const bool dispatched = PulseLeftThumbRestHaptic(0.035f, 110.0f, 0.30f);
			API::get()->log_info("[ThumbRestHud] haptic pattern=touch-edge dispatched=%s layer=xinput",
				dispatched ? "true" : "false");
		}
		if (consumed)
			thumbRestDpadUsed.store(true, std::memory_order_release);
		thumbRestTouchActive.store(active, std::memory_order_release);
		thumbRestTouchWasActiveInput = active;
	}

	using InteractionPerfClock = std::chrono::steady_clock;
	struct InteractionPerfTotals {
		uint64_t frames = 0;
		double elapsedSeconds = 0.0;
		double totalMs = 0.0;
		double lifecycleMs = 0.0;
		double poseContactMs = 0.0;
		double aimShotMs = 0.0;
		double handlingPresentationMs = 0.0;
		double postTraceMs = 0.0;
	};
	InteractionPerfTotals interactionPerfTotals;

	bool UpdateDualThumbRestCalibration() {
		const auto* pluginParam = API::get()->param();
		const auto* vr = pluginParam != nullptr ? pluginParam->vr : nullptr;
		if (vr == nullptr || vr->get_action_handle == nullptr
			|| vr->is_action_active_any_joystick == nullptr) {
			dualThumbRestRawActive = false;
			dualThumbRestCalibrationActive = false;
			return false;
		}

		const ULONGLONG nowTick = GetTickCount64();
		const bool forceRebind = thumbRestActionRebindRequested.exchange(
			false, std::memory_order_acq_rel);
		if (forceRebind)
		{
			thumbRestDpadAction = nullptr;
			rightThumbRestCalibrationAction = nullptr;
			thumbRestDpadResolvedPath = nullptr;
			rightThumbRestCalibrationResolvedPath = nullptr;
			thumbRestDpadProbeState = -1;
			thumbRestModifierActive = false;
			thumbRestTouchActive.store(false, std::memory_order_release);
			thumbRestDpadUsed.store(false, std::memory_order_release);
			thumbRestTouchWasActiveInput = false;
			dualThumbRestRawActive = false;
			dualThumbRestCalibrationActive = false;
			thumbRestDpadLastDirection = 0;
			thumbRestDpadPulseDirection = 0;
			thumbRestDpadPulseSamplesRemaining = 0;
			API::get()->log_info("%s", "[ThumbRestHud] action bindings invalidated reason=device-reset");
		}

		const bool refreshBindings = forceRebind || nowTick >= thumbRestActionRefreshAt;
		if (refreshBindings)
			thumbRestActionRefreshAt = nowTick + 1000;

		if (thumbRestDpadAction == nullptr || refreshBindings) {
			static constexpr const char* leftPaths[] = {
				"/actions/default/in/ThumbRestTouchLeft",
				"/actions/default/in/ThumbrestTouchLeft",
				"/actions/default/in/thumbresttouchleft",
			};
			for (const char* path : leftPaths) {
				if (auto candidate = vr->get_action_handle(path); candidate != nullptr) {
					if (thumbRestDpadAction != nullptr && candidate != thumbRestDpadAction)
						API::get()->log_info("%s", "[ThumbRestHud] left action handle rebound");
					thumbRestDpadAction = candidate;
					thumbRestDpadResolvedPath = path;
					break;
				}
			}
		}
		if (rightThumbRestCalibrationAction == nullptr || refreshBindings) {
			static constexpr const char* rightPaths[] = {
				"/actions/default/in/ThumbRestTouchRight",
				"/actions/default/in/ThumbrestTouchRight",
				"/actions/default/in/thumbresttouchright",
			};
			for (const char* path : rightPaths) {
				if (auto candidate = vr->get_action_handle(path); candidate != nullptr) {
					if (rightThumbRestCalibrationAction != nullptr
						&& candidate != rightThumbRestCalibrationAction)
						API::get()->log_info("%s", "[ThumbRestHud] right action handle rebound");
					rightThumbRestCalibrationAction = candidate;
					rightThumbRestCalibrationResolvedPath = path;
					break;
				}
			}
		}

		const bool bothTouched = thumbRestDpadAction != nullptr
			&& rightThumbRestCalibrationAction != nullptr
			&& vr->is_action_active_any_joystick(thumbRestDpadAction)
			&& vr->is_action_active_any_joystick(rightThumbRestCalibrationAction);
		const auto now = std::chrono::steady_clock::now();
		if (bothTouched && !dualThumbRestRawActive) {
			dualThumbRestStartedAt = now;
			dualThumbRestRawActive = true;
		}
		if (bothTouched && !dualThumbRestCalibrationActive
			&& now - dualThumbRestStartedAt >= std::chrono::milliseconds(500)) {
			dualThumbRestCalibrationActive = true;
			API::get()->log_info("[GripCalibration] dual-thumb hold active left=%s right=%s",
				thumbRestDpadResolvedPath != nullptr ? thumbRestDpadResolvedPath : "missing",
				rightThumbRestCalibrationResolvedPath != nullptr ? rightThumbRestCalibrationResolvedPath : "missing");
		}
		if (!bothTouched) {
			if (dualThumbRestCalibrationActive)
				API::get()->log_info("%s", "[GripCalibration] dual-thumb hold released");
			dualThumbRestRawActive = false;
			dualThumbRestCalibrationActive = false;
		}
		return dualThumbRestCalibrationActive;
	}

	static double InteractionPerfElapsedMs(InteractionPerfClock::time_point start,
		InteractionPerfClock::time_point end) {
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	void RecordInteractionPerf(float delta, double totalMs, double lifecycleMs,
		double poseContactMs, double aimShotMs, double handlingPresentationMs) {
		if (!settingsManager.debugMod) {
			interactionPerfTotals = {};
			return;
		}

		auto& totals = interactionPerfTotals;
		++totals.frames;
		totals.elapsedSeconds += delta > 0.0f ? static_cast<double>(delta) : 0.0;
		totals.totalMs += totalMs;
		totals.lifecycleMs += lifecycleMs;
		totals.poseContactMs += poseContactMs;
		totals.aimShotMs += aimShotMs;
		totals.handlingPresentationMs += handlingPresentationMs;
		if (totals.elapsedSeconds < 5.0 || totals.frames == 0)
			return;

		const double divisor = static_cast<double>(totals.frames);
		API::get()->log_info(
			"[InteractionPerf] frames=%llu avgTotalMs=%.3f lifecycle=%.3f poseContact=%.3f aimShot=%.3f handlingPresentation=%.3f postTrace=%.3f",
			static_cast<unsigned long long>(totals.frames), totals.totalMs / divisor,
			totals.lifecycleMs / divisor, totals.poseContactMs / divisor,
			totals.aimShotMs / divisor, totals.handlingPresentationMs / divisor,
			totals.postTraceMs / divisor);
		totals = {};
	}

	void ApplyThumbRestDpadFallback(XINPUT_STATE* state) {
		if (state == nullptr) {
			thumbRestModifierActive = false;
			PublishThumbRestTouchState(false);
			cameraController.SetRightJoystickCameraSuppressed(false);
			return;
		}
		DispatchPendingThumbRestHaptic();

		// The Lua layer cannot currently expose is_action_active_any_joystick,
		// while get_left_joystick_source() is unavailable on this OpenXR setup.
		// Use the native UEVR VR API here as a compatibility fallback. This mirrors
		// DUALGRIP.lua's existing always-enabled thumb-rest D-pad feature.
		if (dualThumbRestRawActive) {
			thumbRestModifierActive = false;
			PublishThumbRestTouchState(true, true);
			cameraController.SetRightJoystickCameraSuppressed(false);
			thumbRestDpadLastDirection = 0;
			thumbRestDpadPulseDirection = 0;
			thumbRestDpadPulseSamplesRemaining = 0;
			return;
		}

		const bool gameplayInput = playerManager.isInControl && !playerManager.weaponWheelEnabled;
		const bool aircraft = playerManager.vehicleType == PlayerManager::Helicopter
			|| playerManager.vehicleType == PlayerManager::Plane;
		if (!gameplayInput) {
			thumbRestModifierActive = false;
			PublishThumbRestTouchState(false);
			cameraController.SetRightJoystickCameraSuppressed(false);
			thumbRestDpadLastDirection = 0;
			thumbRestDpadPulseDirection = 0;
			thumbRestDpadPulseSamplesRemaining = 0;
			return;
		}

		const auto* pluginParam = API::get()->param();
		const auto* vr = pluginParam != nullptr
			? pluginParam->vr
			: nullptr;
		if (vr == nullptr || vr->get_action_handle == nullptr
			|| vr->is_action_active_any_joystick == nullptr) {
			thumbRestModifierActive = false;
			PublishThumbRestTouchState(false);
			cameraController.SetRightJoystickCameraSuppressed(false);
			if (settingsManager.debugInputLayerProbe && thumbRestDpadProbeState != 0) {
				API::get()->log_info("[DUALGRIP] thumb-rest fallback unavailable: any-joystick API missing");
				thumbRestDpadProbeState = 0;
			}
			thumbRestDpadLastDirection = 0;
			thumbRestDpadPulseDirection = 0;
			thumbRestDpadPulseSamplesRemaining = 0;
			return;
		}

		if (thumbRestDpadAction == nullptr) {
			static constexpr const char* actionPaths[] = {
				"/actions/default/in/ThumbRestTouchLeft",
				"/actions/default/in/ThumbrestTouchLeft",
				"/actions/default/in/thumbresttouchleft",
			};
			for (const char* path : actionPaths) {
				UEVR_ActionHandle candidate = vr->get_action_handle(path);
				if (candidate != nullptr) {
					thumbRestDpadAction = candidate;
					thumbRestDpadResolvedPath = path;
					break;
				}
			}
			if (thumbRestDpadAction == nullptr) {
				thumbRestModifierActive = false;
				PublishThumbRestTouchState(false);
				cameraController.SetRightJoystickCameraSuppressed(false);
				if (settingsManager.debugInputLayerProbe && thumbRestDpadProbeState != 1) {
					API::get()->log_info("[DUALGRIP] thumb-rest fallback unavailable: action handle missing");
					thumbRestDpadProbeState = 1;
				}
				thumbRestDpadLastDirection = 0;
				thumbRestDpadPulseDirection = 0;
				thumbRestDpadPulseSamplesRemaining = 0;
				return;
			}
			if (settingsManager.debugInputLayerProbe && thumbRestDpadProbeState != 2) {
				API::get()->log_info("[DUALGRIP] thumb-rest fallback bound: %s", thumbRestDpadResolvedPath);
				thumbRestDpadProbeState = 2;
			}
		}

		const bool modifierActive = vr->is_action_active_any_joystick(thumbRestDpadAction);
		thumbRestModifierActive = modifierActive;
		PublishThumbRestTouchState(modifierActive);
		cameraController.SetRightJoystickCameraSuppressed(!aircraft && modifierActive);
		if (settingsManager.debugInputLayerProbe) {
			const int state = modifierActive ? 3 : 4;
			if (thumbRestDpadProbeState != state) {
				API::get()->log_info("[DUALGRIP] thumb-rest fallback %s",
					modifierActive ? "active" : "inactive");
				thumbRestDpadProbeState = state;
			}
		}

		if (!modifierActive) {
			thumbRestDpadLastDirection = 0;
			thumbRestDpadPulseDirection = 0;
			thumbRestDpadPulseSamplesRemaining = 0;
			return;
		}

		const int x = static_cast<int>(state->Gamepad.sThumbRX);
		const int y = static_cast<int>(state->Gamepad.sThumbRY);
		const int absX = x < 0 ? -x : x;
		const int absY = y < 0 ? -y : y;
		constexpr int stickThreshold = 16000;
		constexpr int stickReleaseThreshold = 8000;
		constexpr WORD dpadMask = XINPUT_GAMEPAD_DPAD_UP
			| XINPUT_GAMEPAD_DPAD_DOWN
			| XINPUT_GAMEPAD_DPAD_LEFT
			| XINPUT_GAMEPAD_DPAD_RIGHT;

		if (aircraft) {
			const bool upDeflected = y >= stickThreshold && absY >= absX;
			const bool finishingUpPulse = thumbRestDpadPulseSamplesRemaining > 0
				&& thumbRestDpadPulseDirection == XINPUT_GAMEPAD_DPAD_UP;

			// Aircraft need their native right-stick axes. Consume them only for the
			// deliberate thumb-rest + upward deflection used by Hydra Auto-Hover.
			if (!upDeflected && !finishingUpPulse) {
				cameraController.SetRightJoystickCameraSuppressed(false);
				if ((absX > absY ? absX : absY) <= stickReleaseThreshold) {
					thumbRestDpadLastDirection = 0;
					thumbRestDpadPulseDirection = 0;
					thumbRestDpadPulseSamplesRemaining = 0;
				}
				return;
			}

			cameraController.SetRightJoystickCameraSuppressed(true);
			state->Gamepad.sThumbRX = 0;
			state->Gamepad.sThumbRY = 0;
			state->Gamepad.wButtons = static_cast<WORD>(state->Gamepad.wButtons
				& ~XINPUT_GAMEPAD_DPAD_UP);

			if (finishingUpPulse) {
				state->Gamepad.wButtons = static_cast<WORD>(state->Gamepad.wButtons
					| XINPUT_GAMEPAD_DPAD_UP);
				--thumbRestDpadPulseSamplesRemaining;
			}

			if (upDeflected && thumbRestDpadLastDirection == 0) {
				thumbRestDpadUsed.store(true, std::memory_order_release);
				thumbRestDpadLastDirection = XINPUT_GAMEPAD_DPAD_UP;
				thumbRestDpadPulseDirection = XINPUT_GAMEPAD_DPAD_UP;
				thumbRestDpadPulseSamplesRemaining = 3;
				state->Gamepad.wButtons = static_cast<WORD>(state->Gamepad.wButtons
					| XINPUT_GAMEPAD_DPAD_UP);
				--thumbRestDpadPulseSamplesRemaining;
				aircraftCameraRevealRequestMask.fetch_or(2U, std::memory_order_release);
				PulseThumbRestDpadHaptic();
				if (settingsManager.debugInputLayerProbe)
					API::get()->log_info("[DUALGRIP] aircraft thumb-rest D-pad Up pulse action=AutoHover stick=(%d,%d) samples=3",
						x, y);
			}
			return;
		}

		state->Gamepad.sThumbRX = 0;
		state->Gamepad.sThumbRY = 0;
		state->Gamepad.wButtons = static_cast<WORD>(state->Gamepad.wButtons & ~dpadMask);

		WORD direction = 0;
		if (absX >= stickThreshold || absY >= stickThreshold) {
			if (absX >= absY)
				direction = x < 0 ? XINPUT_GAMEPAD_DPAD_LEFT : XINPUT_GAMEPAD_DPAD_RIGHT;
			else
				direction = y < 0 ? XINPUT_GAMEPAD_DPAD_DOWN : XINPUT_GAMEPAD_DPAD_UP;
		}

		const auto actionForDirection = [](WORD value) -> const char* {
			return value == XINPUT_GAMEPAD_DPAD_LEFT
				? "ViewStats(hold)"
				: value == XINPUT_GAMEPAD_DPAD_DOWN
				? "MapZoom/secondary"
				: value == XINPUT_GAMEPAD_DPAD_UP
				? "GangActive/vehicleSubMission"
				: "PositiveResponse/radio";
		};

		// Finish a pulse even if the stick is already moving back toward center;
		// the latch below prevents another pulse until the release threshold is met.
		if (thumbRestDpadPulseSamplesRemaining > 0) {
			state->Gamepad.wButtons = static_cast<WORD>(state->Gamepad.wButtons
				| thumbRestDpadPulseDirection);
			--thumbRestDpadPulseSamplesRemaining;
		}

		if (direction == 0) {
			if ((absX > absY ? absX : absY) <= stickReleaseThreshold) {
				thumbRestDpadLastDirection = 0;
				thumbRestDpadPulseDirection = 0;
				thumbRestDpadPulseSamplesRemaining = 0;
			}
		} else if (thumbRestDpadLastDirection == 0) {
			thumbRestDpadUsed.store(true, std::memory_order_release);
			thumbRestDpadLastDirection = direction;
			PulseThumbRestDpadHaptic();
			if (direction == XINPUT_GAMEPAD_DPAD_LEFT) {
				// View Stats is a native hold action, unlike the directional
				// weapon/radio/response actions below.
				state->Gamepad.wButtons = static_cast<WORD>(state->Gamepad.wButtons | direction);
				if (settingsManager.debugInputLayerProbe)
					API::get()->log_info("[DUALGRIP] thumb-rest fallback D-pad hold=0x%04X action=%s stick=(%d,%d)",
						direction, actionForDirection(direction), x, y);
			} else {
				thumbRestDpadPulseDirection = direction;
				thumbRestDpadPulseSamplesRemaining = 3;
				state->Gamepad.wButtons = static_cast<WORD>(state->Gamepad.wButtons | direction);
				--thumbRestDpadPulseSamplesRemaining;
				if (settingsManager.debugInputLayerProbe)
					API::get()->log_info("[DUALGRIP] thumb-rest fallback D-pad pulse=0x%04X action=%s stick=(%d,%d) samples=3",
						direction, actionForDirection(direction), x, y);
			}
		} else if (thumbRestDpadLastDirection == XINPUT_GAMEPAD_DPAD_LEFT
			&& direction == XINPUT_GAMEPAD_DPAD_LEFT) {
			// Keep View Stats asserted for the full left-stick deflection.
			state->Gamepad.wButtons = static_cast<WORD>(state->Gamepad.wButtons | direction);
		}
	}


public:
	GTASADE_Plugin() : cameraController(&memoryManager, &settingsManager, &playerManager),
        weaponManager(&playerManager, &cameraController, &memoryManager, &settingsManager),
		playerManager(&settingsManager),
		memoryManager(&settingsManager){}

	void on_dllmain() override {}

	void on_device_reset() override {
		controlGuideOverlay.OnDeviceReset();
		// UEVR can recreate its OpenXR action set without unloading this plugin.
		// Re-resolve cached action handles from the XInput callback before the next
		// query instead of retaining a non-null handle that can no longer report.
		thumbRestActionRebindRequested.store(true, std::memory_order_release);
	}

	void on_post_render_vr_framework_dx12(ID3D12GraphicsCommandList* commandList,
		ID3D12Resource* renderTarget, D3D12_CPU_DESCRIPTOR_HANDLE* renderTargetView) override {
		controlGuideOverlay.RenderDx12(commandList, renderTarget, renderTargetView);
	}

	void on_post_render_vr_framework_dx11(ID3D11DeviceContext* context,
		ID3D11Texture2D* renderTarget, ID3D11RenderTargetView* renderTargetView) override {
		controlGuideOverlay.RenderDx11(context, renderTarget, renderTargetView);
	}

	bool on_message(HWND hwnd, UINT, WPARAM, LPARAM) override {
		if (hwnd != nullptr && IsWindow(hwnd))
		{
			const HWND rootWindow = GetAncestor(hwnd, GA_ROOT);
			gameWindowHandle = rootWindow != nullptr ? rootWindow : hwnd;
		}
		return true;
	}

	void on_pre_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle device, int viewIndex,
		float worldToMeters, UEVR_Vector3f* position, UEVR_Rotatorf* rotation, bool isDouble) override {
		(void)device;
		(void)viewIndex;
		(void)worldToMeters;
		(void)position;
		(void)isDouble;
		if (rotation != nullptr)
			weaponManager.SetTwoHandViewRotation(*rotation);
	}

	void on_xinput_get_state(uint32_t* retval, uint32_t userIndex, XINPUT_STATE* state) override {
		(void)retval;
		if (userIndex != 0 || state == nullptr)
		{
			if (userIndex == 0) {
				thumbRestModifierActive = false;
				PublishThumbRestTouchState(false);
				cameraController.SetRightJoystickCameraSuppressed(false);
				weaponManager.SetGripState(false, false);
				weaponManager.SetMeleeClenchState(false, false);
			}
			return;
		}

		const WORD buttons = state->Gamepad.wButtons;
		const bool aircraftLeftStickPressed = (buttons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
		const bool fixedWingAircraft = playerManager.isInVehicle
			&& playerManager.vehicleType == PlayerManager::Plane
			&& IsRetractableLandingGearPlane(activeDrivingVehicleModelId);
		if (fixedWingAircraft && aircraftLeftStickPressed && !aircraftLeftStickPressHeld)
			aircraftCameraRevealRequestMask.fetch_or(1U, std::memory_order_release);
		// Keep the physical edge latch independent of the game-state snapshot.
		// Camera transitions can briefly flicker vehicle/control state while L3 is
		// still held; clearing the latch there would turn one press into many.
		aircraftLeftStickPressHeld = aircraftLeftStickPressed;
		const bool rawLeftTriggerHeld = state->Gamepad.bLeftTrigger > 30;
		const bool rawRightTriggerHeld = state->Gamepad.bRightTrigger > 30;
		// This native callback already supplies the raw shoulder/grip state used by
		// the working two-hand latch, so capture both raw trigger channels at the
		// same boundary before either is cleared below.
		weaponManager.SetMeleeClenchState(rawLeftTriggerHeld, rawRightTriggerHeld);
		weaponManager.SetGripState(
			(buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0,
			(buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
		const bool dualThumbCalibration = UpdateDualThumbRestCalibration();
		const uint8_t dualThumbHandMask = dualThumbCalibration
			? weaponManager.GetDualThumbCalibrationHandMask() : 0U;
		weaponManager.SetCalibrationButtonState(
			(dualThumbHandMask & 1U) != 0,
			(dualThumbHandMask & 2U) != 0);
		state->Gamepad.wButtons = buttons;
		// A body-anchored gun is visible inventory, not an active weapon. Keep
		// native trigger input blocked until a proximity-qualified grip has attached it.
		if (!playerManager.isInVehicle && weaponManager.ShouldBlockWeaponTriggerInput())
		{
			state->Gamepad.bLeftTrigger = 0;
			state->Gamepad.bRightTrigger = 0;
		}
		// Physical melee owns controller motion, while GTA's native trigger attack
		// would add an unrelated animation/hit. This authoritative engine-thread
		// snapshot is evaluated after every other plugin input mutation.
		if (weaponManager.ShouldBlockNativeMeleeTriggerInput())
		{
			state->Gamepad.bLeftTrigger = 0;
			state->Gamepad.bRightTrigger = 0;
		}
		// Apply this after the plugin's other XInput mutations. The Lua remap is
		// disabled for this feature, leaving this as the single D-pad owner.
		ApplyThumbRestDpadFallback(state);

	}

	void on_dllmain_detach() override {
		if (controlGuideNativeHudSuppressed)
			API::get()->execute_command(L"gta.hud.setvis 1");
		if (controlGuideHudForcedVisible && !controlGuideHudWasVisible)
			SetHudVisibility(false, "restore hidden HUD after control guide detach");
		if (pauseUi2dScreenAutoEnabled || resultScreen2dAutoEnabled)
			settingsManager.SetPause2dScreenMode(false);
		controlGuideHudForcedVisible = false;
		controlGuideHudWasVisible = false;
		controlGuideNativeHudSuppressed = false;
		controlGuideOverlay.SetVisible(false);
		controlGuideOverlay.OnDeviceReset();
		thumbRestModifierActive = false;
		PublishThumbRestTouchState(false, true);
		aircraftLeftStickPressHeld = false;
		aircraftCameraRevealRequestMask.store(0U, std::memory_order_release);
		aircraftCameraRevealActive = false;
		aircraftCameraRevealRestoring = false;
		dualThumbRestRawActive = false;
		dualThumbRestCalibrationActive = false;
		resultScreenPresentationActive = false;
		resultScreenHudAutoRevealed = false;
		resultScreen2dAutoEnabled = false;
		RestoreThumbRestHudContext("detach");
		cameraController.SetRightJoystickCameraSuppressed(false);
		weaponManager.CancelGripCalibration();
		weaponManager.SetMeleeClenchState(false, false);
		weaponManager.RestoreFreeAimWeaponHands();
		ManagePluginState(false);
		memoryManager.ResetTriggerTimingProbe();
		memoryManager.RestoreManualReloadState();
		memoryManager.RestoreCombatAssistPatches();
	}

	void on_initialize() override {
		API::get()->log_info("%s", "VR cpp mod initializing - Codex combat assist build");
		settingsManager.InitSettingsManager();
		pause2dStartupRecoveryPending = !settingsManager.RecoverPluginOwnedPause2dScreenMode();
		if (pause2dStartupRecoveryPending)
			API::get()->log_info("%s", "[PauseUI] interrupted-session 2D recovery queued until runtime ready");
		API::get()->log_info("[ManualReload] startup saved=%s active=%s",
			settingsManager.enableManualReloadMode ? "true" : "false",
			settingsManager.activeManualReloadMode ? "true" : "false");
		settingsManager.ApplyHudVisibilityDefault();
		hudUiVisible = settingsManager.enableShowUiAtStartup;
		hudUiStateInitialized = true;
		startupHudVisibilityRetryPending = settingsManager.enableShowUiAtStartup &&
			(API::get()->param()->vr == nullptr || !API::VR::is_runtime_ready());
		if (startupHudVisibilityRetryPending)
			API::get()->log_info("[PauseUI] startup UI request queued until runtime ready");
		lastHudAutoHideEnabled = settingsManager.enableHudAutoHide;
		hudUiPinned = false;
		if (hudUiVisible && settingsManager.enableHudAutoHide)
			ResetHudAutoHideTimer();
		memoryManager.InitMemoryManager();
		if (settingsManager.enableCombatAssist)
			memoryManager.ApplyCombatAssistPatches();
		if (settingsManager.enableDualGripAimFire)
			memoryManager.ApplyPlayerSemiAutoFireGatePatch();
		if (settingsManager.activeManualReloadMode)
			memoryManager.ApplyManualReloadCapturePatch();
		Utilities::InitHelperClasses();
		weaponManager.InitializeGripCalibration();
		weaponManager.InitializeMagneticHolster();
		if (settingsManager.enableBulletTraceHidden)
			weaponManager.HideBulletTrace();
	}

	void on_custom_event(const char* event_name, const char* event_data) override {
		if (event_name == nullptr || event_data == nullptr)
			return;

		const std::string eventName(event_name);
		if (eventName == "DUALGRIP.ControlGuideNav")
		{
			if (!controlGuideOverlay.IsVisible())
				return;
			const std::string action(event_data);
			if (action == "up")
				controlGuideSelectedOption = (controlGuideSelectedOption + 1) % 2;
			else if (action == "down")
				controlGuideSelectedOption = (controlGuideSelectedOption + 1) % 2;
			else if (action == "toggle")
				ToggleControlGuideOption(controlGuideSelectedOption);
			else
				return;
			RefreshControlGuideOptions();
			API::get()->log_info("[ControlGuide] navigation action=%s selected=%d",
				action.c_str(), controlGuideSelectedOption);
			return;
		}
		if (eventName == "DUALGRIP.ControlGuide")
		{
			const bool show = std::string(event_data) == "show";
			if (show && !controlGuideOverlay.IsVisible())
			{
				controlGuideHudWasVisible = hudUiVisible;
				controlGuideHudForcedVisible = !controlGuideHudWasVisible
					&& SetHudVisibility(true, "enable UEVR UI composition for control guide");
			}
			if (show)
			{
				RefreshControlGuideOptions();
				// UEVR's UI composition must remain enabled for the custom guide texture,
				// but GTA's own HUD otherwise renders over it. This title exposes a
				// dedicated HUD visibility command (documented in its live cvar dump), so
				// suppress only the native HUD and leave the UEVR compositor active.
				controlGuideNativeHudRequest.store(1, std::memory_order_release);
			}
			controlGuideOverlay.SetVisible(show);
			// The old floating ImGui window depended on the main UEVR menu and could
			// be moved/collapsed outside its clipped UI target. The guide now owns a
			// fixed controller-operated panel, so keep that legacy window closed.
			API::get()->dispatch_lua_event("controlGuideOptionsVisible", "false");
			if (!show)
			{
				controlGuideNativeHudRequest.store(-1, std::memory_order_release);
				if (controlGuideHudForcedVisible && !controlGuideHudWasVisible)
					SetHudVisibility(false, "restore hidden HUD after control guide");
				controlGuideHudForcedVisible = false;
				controlGuideHudWasVisible = false;
			}
			API::get()->log_info("[ControlGuide] visible=%s source=A+X",
				show ? "true" : "false");
			return;
		}
		if (eventName == "DUALGRIP.TriggerTiming")
		{
			if (weaponManager.ShouldBlockNativeMeleeTriggerInput())
			{
				memoryManager.ResetTriggerTimingProbe();
				return;
			}
			const std::string payload(event_data);
			auto readField = [&](const char* key) -> std::string {
				const std::string prefix = std::string(key) + "=";
				const size_t start = payload.find(prefix);
				if (start == std::string::npos)
					return {};
				const size_t valueStart = start + prefix.size();
				const size_t valueEnd = payload.find(';', valueStart);
				return payload.substr(valueStart, valueEnd == std::string::npos ? std::string::npos : valueEnd - valueStart);
			};
			auto readBool = [&](const char* key) {
				const std::string value = readField(key);
				return value == "true" || value == "1" || value == "on";
			};

			try
			{
				const std::string type = readField("type");
				const uint32_t sequence = static_cast<uint32_t>(std::stoul(readField("seq")));
				const std::string side = readField("side");
				const double luaClockSeconds = std::stod(readField("lua"));
				if (type == "edge")
				{
					memoryManager.RecordTriggerTimingEdge(
						sequence,
						std::stoi(readField("weapon")),
						readBool("aim"),
						readBool("vehicle"),
						readBool("input"),
						readBool("eligible"),
						readBool("gate"),
						side.c_str(),
						luaClockSeconds);
				}
				else if (type == "release")
				{
					memoryManager.RecordTriggerTimingRelease(sequence, side.c_str(), luaClockSeconds);
				}
			}
			catch (const std::exception&)
			{
				API::get()->log_warn("[TriggerTiming] rejected malformed Lua event payload");
			}
			return;
		}

		if (eventName == "DUALGRIP.HandSide")
		{
			const std::string handSide(event_data);
			// Once the support grip joins, keep the already-selected primary hand.
			// DUALGRIP otherwise gives the left grip priority while both are held.
			if (weaponManager.ShouldPreservePrimaryHandForTwoHand())
				return;

			if (handSide == "left")
				settingsManager.leftHandedMode = SettingsManager::TriggerSwap;
			else if (handSide == "right")
				settingsManager.leftHandedMode = SettingsManager::Disabled;
			else
				return;

			weaponManager.UpdateActualWeaponMesh();
			if (settingsManager.debugInputLayerProbe)
				API::get()->log_info("[DUALGRIP] weapon hand side = %s", handSide.c_str());
			return;
		}

		if (eventName == "DUALGRIP.VehicleFaceFire")
		{
			weaponManager.SetVehicleFaceButtonState(
				std::string(event_data) == "pressed" || std::string(event_data) == "true");
			return;
		}

		if (eventName == "DUALGRIP.VisibilityState")
		{
			const std::string visibilityState(event_data);
			auto setGraceEnforcement = [&](bool enabled) {
				if (heldVisualGraceEnforcementActive != enabled)
					API::get()->log_info("[WeaponGrace] per-frame enforcement %s (state=%s)",
						enabled ? "started" : "stopped", visibilityState.c_str());
				heldVisualGraceEnforcementActive = enabled;
			};
			if (!settingsManager.enableAlternateWeaponHandsVisibility)
			{
				setGraceEnforcement(false);
				weaponManager.SetFreeAimWeaponHandsPresentationActive(false);
				weaponManager.SetWeaponScaled(true);
				weaponManager.SetMotionWeaponTrackingEnabled(true);
				return;
			}

			// The controller-pistol vehicle proof intentionally bypasses the old
			// vehicle_firing/idle visibility split. Keep its right-hand motion state
			// alive while Lua continues to pass ordinary driving controls through.
			if (playerManager.isInVehicle && weaponManager.IsVehicleFreeAimActive())
			{
				setGraceEnforcement(false);
				weaponManager.SetFreeAimWeaponHandsPresentationActive(false);
				weaponManager.SetWeaponScaled(true);
				weaponManager.SetMotionWeaponTrackingEnabled(true);
				return;
			}

			if (visibilityState == "default")
			{
				setGraceEnforcement(false);
				weaponManager.SetFreeAimWeaponHandsPresentationActive(false);
				weaponManager.SetWeaponScaled(true);
				weaponManager.SetMotionWeaponTrackingEnabled(false);
				return;
			}

			if (visibilityState == "active" || visibilityState == "active_grace")
			{
				setGraceEnforcement(visibilityState == "active_grace");
				weaponManager.SetFreeAimWeaponHandsPresentationActive(true);
				weaponManager.SetWeaponScaled(true);
				weaponManager.SetMotionWeaponTrackingEnabled(true);
			}

			else if (visibilityState == "vehicle_firing")
			{
				setGraceEnforcement(false);
				// Let GTA enter its native AimWeaponFromCar path, then drive the
				// visible weapon and aim vector from the configured VR hand. The
				// native drive-by task still owns weapon eligibility and firing.
				weaponManager.SetFreeAimWeaponHandsPresentationActive(false);
				weaponManager.SetWeaponScaled(true);
				weaponManager.SetMotionWeaponTrackingEnabled(true);
			}
			else if (visibilityState == "weapon_preview")
			{
				setGraceEnforcement(false);
				weaponManager.SetFreeAimWeaponHandsPresentationActive(false);
				weaponManager.SetWeaponScaled(true);
				weaponManager.SetMotionWeaponTrackingEnabled(false);
			}
			else if (visibilityState == "idle" || visibilityState == "empty_idle")
			{
				setGraceEnforcement(false);
				weaponManager.SetFreeAimWeaponHandsPresentationActive(false);
				weaponManager.SetWeaponScaled(true);
				weaponManager.SetMotionWeaponTrackingEnabled(false);
			}
			else
			{
				return;
			}

			if (settingsManager.debugInputLayerProbe)
				API::get()->log_info("[DUALGRIP] visibility state = %s", visibilityState.c_str());
			return;
		}

		if (eventName == "DUALGRIP.CycleWeapon")
		{
			// Lua owns the single proven native shoulder pulse. C++ only opens a
			// bounded native-attachment window so the resulting replacement component
			// can be adopted without adding a second cycle input.
			weaponManager.PrepareForExplicitWeaponCycle();
			return;
		}

		if (eventName == "DUALGRIP.ToggleHudUi")
		{
			if (!settingsManager.enableChordHudToggle)
				return;

			ShowHudForAutoHide("legacy manual chord");
			return;
		}

		if (eventName == "DUALGRIP.PauseUiReveal")
		{
			// Mark the upcoming loss of control as an intentional pause. Shops also
			// remove player control, so that signal alone must not force flat mode.
			pauseUiExplicitRequestExpiresAt = GetTickCount64() + PauseUiExplicitRequestWindowMs;
			if (settingsManager.enablePauseUiAutoShow && !hudUiVisible && SetHudVisibility(true, "pause chord"))
				hudUiAutoRevealedForPause = true;
			return;
		}

		if (eventName == "DUALGRIP.CycleCameraView")
		{
			if (!settingsManager.enableShortPressCameraSwitch)
				return;

			PostGameKey('V', "camera cycle");
			return;
		}

		if (eventName == "DUALGRIP.ToggleHudUiPinned")
		{
			if (!settingsManager.enableChordHudToggle)
				return;

			ToggleHudPin("legacy double chord");
			return;
		}

		if (eventName == "GTASADE.CheatAction")
		{
			const std::string action(event_data);
			if (action == "ReloadCurrentWeapon")
			{
				const int currentWeapon = static_cast<int>(weaponManager.currentWeaponEquipped);
				const int expectedWeapon = currentWeapon >= 22 && currentWeapon <= 33 ? currentWeapon : -1;
				const bool dualWield = expectedWeapon >= 0 && weaponManager.HasUsableWeapon(false);
				memoryManager.ReloadCurrentWeaponOneMagazine(expectedWeapon, dualWield);
			}
			else if (action == "WeaponSet1")
				QueueGameCheat("LXGIWYL", "weapon set 1");
			else if (action == "WeaponSet2")
				QueueGameCheat("KJKSZPJ", "weapon set 2");
			else if (action == "WeaponSet3")
				QueueGameCheat("UZUMYMW", "weapon set 3");
			else if (action == "ClearWantedLevel")
				QueueGameCheat("TURNDOWNTHEHEAT", "clear wanted level");
			else if (action == "ToggleNeverWanted")
				QueueGameCheat("AEZAKMI", "toggle never wanted");
			else
				API::get()->log_warn("[CheatActions] unknown action: %s", action.c_str());
			return;
		}

		if (eventName == "GTASADE.ResetAimCalibration")
		{
			weaponManager.ResetAimCalibration();
			return;
		}

		if (eventName == "GTASADE.ResetGripCalibration")
		{
			weaponManager.ResetGripCalibration();
			return;
		}

		if (eventName != "GTASADE.SetFeatureFlag")
			return;

		std::string payload(event_data);
		const size_t separator = payload.find('=');
		if (separator == std::string::npos)
			return;

		const std::string flagName = payload.substr(0, separator);
		const std::string flagValue = payload.substr(separator + 1);
		const bool enabled = flagValue == "true" || flagValue == "1" || flagValue == "on";

		bool liveApply = false;
		if (!settingsManager.SetFeatureFlagFromUi(flagName, enabled, liveApply))
		{
			API::get()->log_warn("Unknown feature flag from UI: %s", flagName.c_str());
			return;
		}
		API::get()->log_info("[FeatureFlags] applied %s=%s live=%s", flagName.c_str(),
			enabled ? "true" : "false", liveApply ? "true" : "false");

		if (!liveApply)
			return;

		if (flagName == "DebugInputLayerProbe")
		{
			memoryManager.ResetTriggerTimingProbe();
			return;
		}
		if (flagName == "EnableDualGripAimFire" && enabled)
			memoryManager.ApplyPlayerSemiAutoFireGatePatch();
		if (flagName == "EnableHudAutoHide")
		{
			lastHudAutoHideEnabled = enabled;
			if (enabled && hudUiVisible && !hudUiAutoRevealedForPause && !hudUiPinned)
				ResetHudAutoHideTimer();
			else
				CancelHudAutoHideTimer();
		}

		if (flagName == "EnableCombatAssist")
		{
			if (enabled)
			{
				memoryManager.ApplyCombatAssistPatches();
				if (settingsManager.enableDualGripAimFire)
					memoryManager.ApplyPlayerSemiAutoFireGatePatch();
				if (settingsManager.activeManualReloadMode)
					memoryManager.ApplyManualReloadCapturePatch();
			}
			else
			{
				memoryManager.RestoreCombatAssistPatches();
				if (settingsManager.enableDualGripAimFire)
					memoryManager.ApplyPlayerSemiAutoFireGatePatch();
				if (settingsManager.activeManualReloadMode)
					memoryManager.ApplyManualReloadCapturePatch();
			}
		}
		else if (flagName == "EnableCameraProfiles")
		{
			if (pluginStateApplied == Driving)
				ApplyDrivingCameraSettings();
			else if (pluginStateApplied == OnFoot)
				settingsManager.ApplyCameraSettings(SettingsManager::OnFoot);
		}
		else if (flagName == "EnableCombatAssistDamage" || flagName == "EnableCombatAssistAnimationTiming" || flagName == "EnableWeaponNoSpread" || flagName == "EnableSilencedPistolNoSpread")
		{
			memoryManager.RefreshCombatAssistWeaponInfoValues();
		}
		else if (flagName == "EnableFreeAimWeaponHands")
		{
			weaponManager.ProcessFreeAimWeaponHands(true);
		}
		else if (flagName == "EnableGripCalibration" && !enabled)
		{
			weaponManager.CancelGripCalibration();
		}
	}

	void on_pre_engine_tick(API::UGameEngine* engine, float delta) override {
		PLUGIN_LOG_ONCE("Pre Engine Tick: %f", delta);
		weaponManager.BeginInteractionEngineTick();
		const bool collectInteractionPerf = settingsManager.debugMod;
		if (!collectInteractionPerf)
			interactionPerfTotals = {};
		const auto interactionPerfTickStart = collectInteractionPerf
			? InteractionPerfClock::now()
			: InteractionPerfClock::time_point{};

		FetchRequiredValuesFromMemory();
		ProcessControlGuideNativeHudRequest();
		UpdateAircraftCameraReveal();
		RetryStartupHudVisibility();
		UpdatePhoneRingingState();
		ProcessQueuedCheat();
		EnforceFirstPersonCameraLock();
		memoryManager.UpdateHealthRecovery(playerManager.isInControl);
		if (settingsManager.activeManualReloadMode)
			memoryManager.MaintainManualReloadMode();
		if (settingsManager.enableCombatAssist)
			memoryManager.MaintainCombatAssistValues();
		weaponManager.ProcessAimCalibrationSample();
		playerManager.FetchPlayerUObjects();
		weaponManager.ProcessGripCalibration();

		const bool playerObjectsReady = playerManager.playerController != nullptr && playerManager.playerHead != nullptr;
		if (settingsManager.debugInputLayerProbe && (!playerManager.isInControl || playerManager.isInVehicle))
			memoryManager.ResetTriggerTimingProbe();
		if (!playerObjectsReady)
		{
			ProcessThumbRestHudGesture(false);
			ProcessThumbRestHudContext(false);
			weaponManager.CancelGripCalibration();
			if (pluginStateApplied != VRdisabled)
				ApplyVRdisabledState();
			SendStatesToLua();
			settingsManager.UpdateSettingsIfModifiedByPlayer();
			UpdatePreviousStates();
			if (collectInteractionPerf) {
				const auto now = InteractionPerfClock::now();
				const double totalMs = InteractionPerfElapsedMs(interactionPerfTickStart, now);
				RecordInteractionPerf(delta, totalMs, totalMs, 0.0, 0.0, 0.0);
			}
			return;
		}

		if (!cameraController.underwaterViewFixed && playerManager.isInControl)
			cameraController.FixUnderwaterView(true);

		ManagePluginState(true, delta);
		ProcessThumbRestHudGesture(true);
		ProcessThumbRestHudContext(true);
		// Apply pause/failure UI after ManagePluginState, because camera profile
		// reloads occur during the state transition and would otherwise overwrite it.
		UpdatePauseHudVisibility();
		UpdateHudAutoHide(delta);
		const auto interactionPerfLifecycleEnd = collectInteractionPerf
			? InteractionPerfClock::now()
			: InteractionPerfClock::time_point{};
		double poseContactMs = 0.0;
		double aimShotMs = 0.0;
		double handlingPresentationMs = 0.0;

		// Main VR functions :
		if (pluginStateApplied != VRdisabled)
		{
			const auto poseContactStart = collectInteractionPerf
				? InteractionPerfClock::now()
				: InteractionPerfClock::time_point{};
			if (settingsManager.enableBodyVisibility)
				playerManager.ProcessBodyVisibility(delta);
			if (heldVisualGraceEnforcementActive && playerManager.isInControl && !playerManager.isInVehicle)
			{
				// GTA reapplies its native idle presentation after grip release. During
				// the short grace state, override that native update every engine tick.
				weaponManager.ProcessFreeAimWeaponHands(true);
				weaponManager.SetWeaponScaled(true, true);
				// Preserve an existing motion attachment. The non-forced path still
				// detects and repairs a genuinely lost controller state without clearing
				// the attachment cache and rebuilding the same mesh every frame.
				weaponManager.SetMotionWeaponTrackingEnabled(true);
			}
			weaponManager.UpdateActualWeaponMesh();
			weaponManager.ProcessMagneticIdleWeapon();
			weaponManager.ProcessMotionMelee(delta);
			weaponManager.ProcessTwoHandStabilization(delta);
			const auto poseContactEnd = collectInteractionPerf
				? InteractionPerfClock::now()
				: InteractionPerfClock::time_point{};
			if (collectInteractionPerf)
				poseContactMs = InteractionPerfElapsedMs(poseContactStart, poseContactEnd);
			
			const auto aimShotStart = collectInteractionPerf
				? InteractionPerfClock::now()
				: InteractionPerfClock::time_point{};
			if (!playerManager.weaponWheelEnabled)
			{
				cameraController.ProcessCameraMatrix(delta);
				cameraController.ProcessHookedHeadPosition(delta);
				const bool hasSecondWeapon = weaponManager.HasUsableWeapon(false);
				const bool updateFirstWeapon = !weaponManager.firstWeaponShotDone || !hasSecondWeapon;
				weaponManager.UpdateShootingState(updateFirstWeapon);
				memoryManager.FlushTriggerTimingNativeShot(static_cast<int>(weaponManager.currentWeaponEquipped), playerManager.isInVehicle);
				const bool primaryFirstWeapon = !(weaponManager.firstWeaponShotDone && weaponManager.HasUsableWeapon(false));
				weaponManager.ProcessAiming(primaryFirstWeapon,
					settingsManager.enableAimAlignment && weaponManager.IsGameplayWeaponTrackingActive()
					&& !weaponManager.IsGripCalibrationActive());
				if (weaponManager.HasUsableWeapon(false))
					weaponManager.ProcessAiming(!primaryFirstWeapon, false);
			}
			const auto aimShotEnd = collectInteractionPerf
				? InteractionPerfClock::now()
				: InteractionPerfClock::time_point{};
			if (collectInteractionPerf)
				aimShotMs = InteractionPerfElapsedMs(aimShotStart, aimShotEnd);

			const auto handlingPresentationStart = collectInteractionPerf
				? InteractionPerfClock::now()
				: InteractionPerfClock::time_point{};
			weaponManager.ProcessWeaponHandling(delta);
			weaponManager.ProcessFreeAimWeaponHands();
			weaponManager.ProcessWeaponVisibility();
			const auto handlingPresentationEnd = collectInteractionPerf
				? InteractionPerfClock::now()
				: InteractionPerfClock::time_point{};
			if (collectInteractionPerf) {
				handlingPresentationMs = InteractionPerfElapsedMs(
					handlingPresentationStart, handlingPresentationEnd);
			}
		}
		SendStatesToLua();
		settingsManager.UpdateSettingsIfModifiedByPlayer();
		UpdatePreviousStates();
		if (collectInteractionPerf) {
			const auto interactionPerfTickEnd = InteractionPerfClock::now();
			RecordInteractionPerf(delta,
				InteractionPerfElapsedMs(interactionPerfTickStart, interactionPerfTickEnd),
				InteractionPerfElapsedMs(interactionPerfTickStart, interactionPerfLifecycleEnd),
				poseContactMs, aimShotMs, handlingPresentationMs);
		}
	}


	void on_post_engine_tick(API::UGameEngine* engine, float delta) override {
		PLUGIN_LOG_ONCE("Post Engine Tick: %f", delta);
		const bool collectInteractionPerf = settingsManager.debugMod;
		const auto start = collectInteractionPerf
			? InteractionPerfClock::now()
			: InteractionPerfClock::time_point{};
		weaponManager.ProcessBulletTracePostTick();
		if (collectInteractionPerf) {
			interactionPerfTotals.postTraceMs += InteractionPerfElapsedMs(
				start, InteractionPerfClock::now());
		}
	}

	void on_pre_slate_draw_window(UEVR_FSlateRHIRendererHandle renderer, UEVR_FViewportInfoHandle viewport_info) override {
		PLUGIN_LOG_ONCE("Pre Slate Draw Window");
	}

	void on_post_slate_draw_window(UEVR_FSlateRHIRendererHandle renderer, UEVR_FViewportInfoHandle viewport_info) override {
		PLUGIN_LOG_ONCE("Post Slate Draw Window");
	}

	void ManagePluginState(bool enableVR, float delta = 0.0f)
	{
		if (settingsManager.debugMod) API::get()->log_info("ManagePluginState");

		if (playerManager.isInVehicle && cameraController.currentCameraMode == CameraController::AimWeaponFromCar)
		{
			if (!carAimingVectorRestoredForVehicleAim)
			{
				memoryManager.RestoreCarAimingVectorInstructions();
				carAimingVectorRestoredForVehicleAim = true;
			}
		}
		else
		{
			carAimingVectorRestoredForVehicleAim = false;
		}

		// A visible exterior camera requires the same presentation state used by
		// GTA's normal non-FPS camera modes. Enter it once for the reveal, then
		// hold it until UpdateAircraftCameraReveal restores the saved FPS mode.
		// Reapplying this state every frame caused the earlier camera oscillation.
		if (aircraftCameraRevealActive && !aircraftCameraRevealRestoring)
		{
			if (pluginStateApplied != VRdisabled)
				ApplyVRdisabledState();
			return;
		}

		// We need to fetch the weapon one last time after player lost control so the plugin can correctly reset the weapon position for cutscenes.
		if (!playerManager.isInControl && playerManager.wasInControl && !aircraftCameraRevealActive)
			weaponManager.UpdateActualWeaponMesh();

		bool viewRequiresDisabledVR = !aircraftCameraRevealActive && playerManager.isInControl &&
			(!playerManager.isInVehicle && cameraController.currentOnFootCameraMode != CameraController::OnFootCameraMode::Close) ||
			(!aircraftCameraRevealActive && playerManager.isInVehicle
				&& cameraController.currentVehicleCameraMode != CameraController::VehicleCameraMode::Close);

		if (pluginStateApplied != VRdisabled
			&& ((!playerManager.isInControl && !aircraftCameraRevealActive) || viewRequiresDisabledVR || !enableVR))
		{
			ApplyVRdisabledState();
			return;
		}

		if (viewRequiresDisabledVR || !enableVR)
			return;
		if (aircraftCameraRevealActive && pluginStateApplied == Driving)
			return;

		if (pluginStateApplied != OnFoot && playerManager.isInControl && !playerManager.isInVehicle &&
			cameraController.currentCameraMode != CameraController::Camera)
			ApplyBaseState();

		// Toggles the game's original instructions when going in or out of a vehicle if there's no scripted event with AimWeaponFromCar camera.
		// Then sets UEVR settings according to the vehicle type
		if (pluginStateApplied != Driving && playerManager.isInControl && playerManager.isInVehicle
			&& cameraController.currentCameraMode != CameraController::Camera)
		{
			ApplyBaseState(false);
			ApplyDrivingState();
		}
		else if (pluginStateApplied == Driving && playerManager.isInControl && playerManager.isInVehicle
			&& cameraController.currentCameraMode != CameraController::Camera)
		{
			RefreshDrivingCameraProfile(delta);
		}
		
		// Toggles the game's original instructions for the camera weapon controls
		if (pluginStateApplied != CameraWeapon && cameraController.currentCameraMode == CameraController::Camera)
			ApplyCameraWeaponState();
	}

	void FetchRequiredValuesFromMemory()
	{
		if (settingsManager.debugMod) API::get()->log_info("FetchRequiredValuesFromMemory");
		playerManager.isInControl = *(reinterpret_cast<uint8_t*>(memoryManager.playerIsInControlAddress)) == 0;
		playerManager.isInVehicle = *(reinterpret_cast<uint8_t*>(memoryManager.playerIsInVehicleAddress)) > 0;
		playerManager.vehicleType = *(reinterpret_cast<PlayerManager::VehicleType*>(memoryManager.vehicleTypeAddress));
		const int rawShootFromCarInput = *(reinterpret_cast<int*>(memoryManager.playerShootFromCarInputAddress));
		const bool shootFromCarInput = rawShootFromCarInput >= 2;
		static bool shootFromCarInputSampleInitialized = false;
		static int lastRawShootFromCarInput = 0;
		if (!shootFromCarInputSampleInitialized || rawShootFromCarInput != lastRawShootFromCarInput)
		{
			API::get()->log_info("[VehicleFreeAim] shoot-from-car raw=%d active=%s (threshold>=2)",
				rawShootFromCarInput, shootFromCarInput ? "true" : "false");
			shootFromCarInputSampleInitialized = true;
			lastRawShootFromCarInput = rawShootFromCarInput;
		}
		playerManager.shootFromCarInput = shootFromCarInput;
		playerManager.weaponWheelEnabled = *(reinterpret_cast<int*>(memoryManager.weaponWheelDisplayedAddress)) > 30;
		cameraController.currentCameraMode = *(reinterpret_cast<CameraController::CameraMode*>(memoryManager.cameraModeAddress));
		cameraController.currentOnFootCameraMode = *(reinterpret_cast<CameraController::OnFootCameraMode*>(memoryManager.onFootCameraModeAddress));
		cameraController.currentVehicleCameraMode = *(reinterpret_cast<CameraController::VehicleCameraMode*>(memoryManager.vehicleCameraModeAddress));
		//cameraController.isCutscenePlaying = *(reinterpret_cast<uint8_t*>(memoryManager.cutscenePlayingAddress)) > 0;
	}

	void UpdatePreviousStates()
	{
		if (settingsManager.debugMod) API::get()->log_info("UpdatePreviousStates");

		playerManager.wasInControl = playerManager.isInControl;
		playerManager.wasInVehicle = playerManager.isInVehicle;
		playerManager.previousVehicleType = playerManager.vehicleType;
		cameraController.previousCameraMode = cameraController.currentCameraMode;
		cameraController.previousOnFootCameraMode = cameraController.currentOnFootCameraMode;
		cameraController.previousVehicleCameraMode = cameraController.currentVehicleCameraMode;
		//cameraController.wasCutscenePlaying = cameraController.isCutscenePlaying;
		weaponManager.previousWeaponEquipped = weaponManager.currentWeaponEquipped;
	}

	enum PluginState {
		Uninitialized = 0,
		VRdisabled = 1,
		OnFoot = 2,
		Driving = 3,
		CameraWeapon = 4
	};

	PluginState pluginStateApplied = Uninitialized;
	bool carAimingVectorRestoredForVehicleAim = false;
	bool hudUiVisible = false;
	bool hudUiStateInitialized = false;
	bool startupHudVisibilityRetryPending = false;
	bool pause2dStartupRecoveryPending = true;
	bool hudUiAutoRevealedForPause = false;
	bool thumbRestHudRevealOwned = false;
	bool thumbRestHudContextWasEligible = false;
	bool thumbRestHudRevealAttempted = false;
	bool thumbRestHudWasVisible = false;
	bool thumbRestHudWasPinned = false;
	bool thumbRestHudWasPauseOwned = false;
	bool thumbRestHudWasAutoHideTimerActive = false;
	float thumbRestHudWasAutoHideRemaining = 0.0f;
	bool thumbRestGestureWasActive = false;
	bool thumbRestGestureConsumed = false;
	bool thumbRestSingleTapPending = false;
	ULONGLONG thumbRestGestureStartedAt = 0;
	ULONGLONG thumbRestSingleTapDeadline = 0;
	bool previousControlStateForHud = false;
	bool hudAutoHideTimerActive = false;
	bool lastHudAutoHideEnabled = true;
	float hudAutoHideRemaining = 0.0f;
	bool hudUiPinned = false;
	bool controlGuideHudWasVisible = false;
	bool controlGuideHudForcedVisible = false;
	bool controlGuideNativeHudSuppressed = false;
	std::atomic<int> controlGuideNativeHudRequest{ 0 };
	int controlGuideSelectedOption = 0;
	ULONGLONG hudPinnedRuntimeCheckAt = 0;
	int hudPinnedObservedPluginState = -1;
	int hudPinnedObservedCameraMode = -1;
	bool pauseUi2dScreenAutoEnabled = false;
	bool resultScreenPresentationActive = false;
	bool resultScreenHudAutoRevealed = false;
	bool resultScreen2dAutoEnabled = false;
	ULONGLONG pauseUiExplicitRequestExpiresAt = 0;
	bool heldVisualGraceEnforcementActive = false;
	ULONGLONG pauseUiNoControlTraceNextAt = 0;
	int pauseUiNoControlTraceSamples = 0;
	bool phoneRinging = false;
	bool phoneRingingStateInitialized = false;
	HWND gameWindowHandle = nullptr;
	SettingsManager::CameraModeSettings appliedDrivingCameraSettings = SettingsManager::Cutscene;
	std::string appliedDrivingCameraProfilePrefix;
	float drivingCameraProfileRefreshTimer = 0.0f;
	int candidateDrivingVehicleModelId = -1;
	int candidateDrivingVehicleModelSamples = 0;
	int activeDrivingVehicleModelId = -1;
	int lastDrivingVehicleModelIdSentToLua = -1;
	bool meleeNativeTriggerBlockStateInitialized = false;
	bool lastMeleeNativeTriggerBlockSentToLua = false;
	std::string queuedCheat;
	size_t queuedCheatIndex = 0;
	ULONGLONG nextQueuedCheatTime = 0;
	static constexpr float HudAutoHideDelaySeconds = 20.0f;
	static constexpr float DrivingCameraProfileSampleSeconds = 0.25f;
	static constexpr float DrivingCameraProfileActiveSampleSeconds = 1.0f;
	static constexpr int DrivingCameraProfileRequiredSamples = 2;
	static constexpr int PauseUiNoControlTraceSampleLimit = 6;
	static constexpr ULONGLONG PauseUiNoControlTraceIntervalMs = 1000;
	static constexpr ULONGLONG PauseUiExplicitRequestWindowMs = 2000;
	static constexpr ULONGLONG ThumbRestTapMaximumMs = 250;
	static constexpr ULONGLONG ThumbRestDoubleTapWindowMs = 320;
	static constexpr ULONGLONG HudPinnedRuntimeCheckIntervalMs = 1000;

	std::string ReadHudRuntimeValue()
	{
		if (API::get()->param()->vr == nullptr || !API::VR::is_runtime_ready())
			return "not-ready";
		try
		{
			const std::string value = API::VR::get_mod_value<std::string>("VR_EnableGUI");
			return value.empty() ? "empty" : value;
		}
		catch (...)
		{
			return "read-error";
		}
	}

	std::string ReadPause2dRuntimeValue()
	{
		bool enabled = false;
		if (!settingsManager.GetPause2dScreenMode(enabled))
			return "not-ready";
		return enabled ? "true" : "false";
	}

	bool ReadHudRuntimeVisible(bool& visible)
	{
		visible = false;
		if (API::get()->param()->vr == nullptr || !API::VR::is_runtime_ready())
			return false;
		try
		{
			visible = API::VR::get_mod_value<bool>("VR_EnableGUI");
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	bool SetHudVisibility(bool visible, const char* reason)
	{
		if (!settingsManager.SetHudUiVisible(visible))
		{
			API::get()->log_warn("[PauseUI] runtime not ready: %s", reason);
			return false;
		}
		hudUiVisible = visible;
		if (!visible)
			CancelHudAutoHideTimer();
		API::get()->log_info("[PauseUI] VR_EnableGUI=%s runtime=%s: %s",
			visible ? "true" : "false", ReadHudRuntimeValue().c_str(), reason);
		return true;
	}

	int ReadControlGuideMovementOrientation() const
	{
		if (API::get()->param()->vr == nullptr || !API::VR::is_runtime_ready())
			return 0;
		try
		{
			return API::VR::get_mod_value<int>("VR_MovementOrientation");
		}
		catch (...)
		{
			API::get()->log_warn("%s", "[ControlGuide] movement orientation was unreadable");
		}
		return 0;
	}

	void RefreshControlGuideOptions()
	{
		controlGuideOverlay.SetOptionsState(ReadControlGuideMovementOrientation(),
			settingsManager.enableHudAutoHide,
			static_cast<uint32_t>(controlGuideSelectedOption));
	}

	void ProcessControlGuideNativeHudRequest()
	{
		const int request = controlGuideNativeHudRequest.exchange(0, std::memory_order_acq_rel);
		if (request > 0 && !controlGuideNativeHudSuppressed && playerManager.isInControl)
		{
			API::get()->execute_command(L"gta.hud.setvis 0");
			controlGuideNativeHudSuppressed = true;
			API::get()->log_info("%s", "[ControlGuide] native GTA HUD suppressed");
		}
		else if (request < 0 && controlGuideNativeHudSuppressed)
		{
			API::get()->execute_command(L"gta.hud.setvis 1");
			controlGuideNativeHudSuppressed = false;
			API::get()->log_info("%s", "[ControlGuide] native GTA HUD restored");
		}
	}

	void ToggleControlGuideOption(int option)
	{
		if (option < 0 || option > 1)
			return;
		if (option == 0)
		{
			if (API::get()->param()->vr == nullptr || !API::VR::is_runtime_ready())
				return;
			try
			{
				const int current = ReadControlGuideMovementOrientation();
				API::VR::set_mod_value("VR_MovementOrientation", current == 1 ? 0 : 1);
				API::VR::save_config();
			}
			catch (...)
			{
				API::get()->log_warn("[ControlGuide] quick option toggle failed option=%d", option);
			}
			return;
		}

		const bool enabled = !settingsManager.enableHudAutoHide;
		bool liveApply = false;
		if (!settingsManager.SetFeatureFlagFromUi("EnableHudAutoHide", enabled, liveApply))
			return;
		lastHudAutoHideEnabled = enabled;
		if (enabled && hudUiVisible && !hudUiAutoRevealedForPause && !hudUiPinned)
			ResetHudAutoHideTimer();
		else
			CancelHudAutoHideTimer();
		settingsManager.DispatchFeatureFlagsToLua();
	}

	void RetryStartupHudVisibility()
	{
		if (pause2dStartupRecoveryPending)
			pause2dStartupRecoveryPending = !settingsManager.RecoverPluginOwnedPause2dScreenMode();

		if (!startupHudVisibilityRetryPending)
			return;

		if (!settingsManager.enableShowUiAtStartup)
		{
			startupHudVisibilityRetryPending = false;
			return;
		}

		if (API::get()->param()->vr == nullptr || !API::VR::is_runtime_ready())
			return;

		if (SetHudVisibility(true, "startup retry after runtime ready"))
		{
			startupHudVisibilityRetryPending = false;
			if (settingsManager.enableHudAutoHide)
				ResetHudAutoHideTimer();
			API::get()->log_info("[PauseUI] startup UI restored after runtime became ready");
		}
	}

	void ResetHudAutoHideTimer()
	{
		if (!settingsManager.enableHudAutoHide || hudUiPinned)
		{
			CancelHudAutoHideTimer();
			return;
		}

		hudAutoHideRemaining = HudAutoHideDelaySeconds;
		hudAutoHideTimerActive = true;
	}

	void CancelHudAutoHideTimer()
	{
		hudAutoHideTimerActive = false;
		hudAutoHideRemaining = 0.0f;
	}

	void ShowHudForAutoHide(const char* reason)
	{
		if (!hudUiVisible && !SetHudVisibility(true, reason))
			return;
		hudUiAutoRevealedForPause = false;
		if (!hudUiPinned)
			ResetHudAutoHideTimer();
		API::get()->log_info("[ThumbRestHud] timed reveal reason=%s seconds=%.0f",
			reason, HudAutoHideDelaySeconds);
	}

	void ToggleHudPin(const char* reason)
	{
		hudUiPinned = !hudUiPinned;
		hudPinnedRuntimeCheckAt = 0;
		hudPinnedObservedPluginState = -1;
		hudPinnedObservedCameraMode = -1;
		if (!hudUiVisible)
			SetHudVisibility(true, hudUiPinned ? "thumb-rest HUD pin" : "thumb-rest HUD unpin");
		hudUiAutoRevealedForPause = false;
		if (hudUiPinned)
			CancelHudAutoHideTimer();
		else
		{
			SetHudVisibility(false, "thumb-rest HUD unpin hide");
			CancelHudAutoHideTimer();
		}
		if (hudUiPinned)
		{
			// Dispatch from the proven XInput haptic path on its next sample.
			pendingThumbRestHaptic.store(
				static_cast<uint8_t>(ThumbRestHapticRequest::PinOn), std::memory_order_release);
		}
		else
		{
			pendingThumbRestHaptic.store(
				static_cast<uint8_t>(ThumbRestHapticRequest::AutoHide), std::memory_order_release);
		}
		API::get()->log_info("[ThumbRestHud] pin=%s reason=%s",
			hudUiPinned ? "on" : "off", reason);
	}

	void RelinquishThumbRestHudContextWithoutRestore(const char* reason)
	{
		if (!thumbRestHudRevealOwned)
			return;

		// A completed tap is taking ownership of the already-visible HUD. Do not
		// hide it between the temporary hold reveal and the timed/pinned state.
		thumbRestHudRevealOwned = false;
		thumbRestHudRevealAttempted = false;
		thumbRestHudWasVisible = false;
		thumbRestHudWasPinned = false;
		thumbRestHudWasPauseOwned = false;
		thumbRestHudWasAutoHideTimerActive = false;
		thumbRestHudWasAutoHideRemaining = 0.0f;
		API::get()->log_info("[ThumbRestHud] contextual reveal adopted reason=%s", reason);
	}

	void ProcessThumbRestHudGesture(bool playerObjectsReady)
	{
		const ULONGLONG now = GetTickCount64();
		const bool touchActive = thumbRestTouchActive.load(std::memory_order_acquire);
		const bool gestureAllowed = playerObjectsReady && playerManager.isInControl
			&& !playerManager.weaponWheelEnabled
			&& (pluginStateApplied == OnFoot || pluginStateApplied == Driving)
			&& !hudUiAutoRevealedForPause && !pauseUi2dScreenAutoEnabled
			&& pauseUiExplicitRequestExpiresAt == 0;

		if (!gestureAllowed)
		{
			thumbRestGestureWasActive = touchActive;
			thumbRestGestureConsumed = true;
			thumbRestSingleTapPending = false;
			thumbRestSingleTapDeadline = 0;
			return;
		}

		if (touchActive)
		{
			if (!thumbRestGestureWasActive)
			{
				thumbRestGestureStartedAt = now;
				thumbRestGestureConsumed = false;
			}
			if (dualThumbRestRawActive || thumbRestDpadUsed.load(std::memory_order_acquire))
				thumbRestGestureConsumed = true;
		}
		else if (thumbRestGestureWasActive)
		{
			const ULONGLONG heldMs = now >= thumbRestGestureStartedAt
				? now - thumbRestGestureStartedAt : ThumbRestTapMaximumMs + 1;
			if (!thumbRestGestureConsumed && heldMs <= ThumbRestTapMaximumMs)
			{
				if (thumbRestSingleTapPending && now <= thumbRestSingleTapDeadline)
				{
					thumbRestSingleTapPending = false;
					thumbRestSingleTapDeadline = 0;
					RelinquishThumbRestHudContextWithoutRestore("double tap");
					ToggleHudPin("left thumb-rest double tap");
				}
				else
				{
					thumbRestSingleTapPending = true;
					thumbRestSingleTapDeadline = now + ThumbRestDoubleTapWindowMs;
				}
			}
		}

		thumbRestGestureWasActive = touchActive;
		if (!touchActive && thumbRestSingleTapPending && now >= thumbRestSingleTapDeadline)
		{
			thumbRestSingleTapPending = false;
			thumbRestSingleTapDeadline = 0;

			// Touch-down temporarily reveals a hidden HUD so D-pad interactions are
			// visible immediately. A completed tap adopts that reveal and starts the
			// normal timer. If the HUD was already visible, the same tap hides it
			// without changing the user's pin/auto-hide mode. The touch-edge haptic
			// already acknowledged this gesture, so do not queue a second vibration.
			const bool contextualRevealFromHidden = thumbRestHudRevealOwned
				&& !thumbRestHudWasVisible;
			const bool pauseOwned = hudUiAutoRevealedForPause || pauseUi2dScreenAutoEnabled
				|| pauseUiExplicitRequestExpiresAt != 0;
			if (contextualRevealFromHidden || !hudUiVisible)
			{
				RelinquishThumbRestHudContextWithoutRestore("single tap show");
				ShowHudForAutoHide("left thumb-rest tap show");
				API::get()->log_info("%s", "[ThumbRestHud] single tap action=show-timed");
			}
			else if (!hudUiPinned && !pauseOwned)
			{
				RestoreThumbRestHudContext("single tap hide cleanup");
				SetHudVisibility(false, "left thumb-rest tap hide");
				CancelHudAutoHideTimer();
				API::get()->log_info("%s", "[ThumbRestHud] single tap action=hide");
			}
			else
			{
				RestoreThumbRestHudContext("single tap protected owner");
				API::get()->log_info(
					"[ThumbRestHud] single tap action=unchanged pinned=%s pauseOwned=%s",
					hudUiPinned ? "true" : "false", pauseOwned ? "true" : "false");
			}
		}
	}

	void RestoreThumbRestHudContext(const char* reason)
	{
		if (!thumbRestHudRevealOwned)
			return;

		// Restore only while the HUD still looks like the state this context
		// created. A pause/UI chord, pin change, or external hide wins ownership.
		const bool stillContextOwner = hudUiVisible && !hudUiPinned
			&& !hudUiAutoRevealedForPause && !pauseUi2dScreenAutoEnabled;
		if (stillContextOwner)
		{
			SetHudVisibility(thumbRestHudWasVisible, reason);
			if (thumbRestHudWasAutoHideTimerActive && settingsManager.enableHudAutoHide
				&& !hudUiPinned)
			{
				hudAutoHideTimerActive = true;
				hudAutoHideRemaining = thumbRestHudWasAutoHideRemaining;
			}
			else
			{
				CancelHudAutoHideTimer();
			}
			API::get()->log_info(
				"[ThumbRestHud] restored owned context visible=%s pinned=%s timer=%s remaining=%.2f reason=%s",
				thumbRestHudWasVisible ? "true" : "false", thumbRestHudWasPinned ? "true" : "false",
				thumbRestHudWasAutoHideTimerActive ? "true" : "false", thumbRestHudWasAutoHideRemaining,
				reason);
		}
		else
		{
			API::get()->log_info(
				"[ThumbRestHud] restore skipped external owner visible=%s pinned=%s pause=%s reason=%s",
				hudUiVisible ? "true" : "false", hudUiPinned ? "true" : "false",
				(hudUiAutoRevealedForPause || pauseUi2dScreenAutoEnabled) ? "true" : "false", reason);
		}

		thumbRestHudRevealOwned = false;
		thumbRestHudRevealAttempted = false;
		thumbRestHudWasVisible = false;
		thumbRestHudWasPinned = false;
		thumbRestHudWasPauseOwned = false;
		thumbRestHudWasAutoHideTimerActive = false;
		thumbRestHudWasAutoHideRemaining = 0.0f;
	}

	void ProcessThumbRestHudContext(bool playerObjectsReady)
	{
		const bool flybyWithoutControl = !playerManager.isInControl
			&& cameraController.currentCameraMode == CameraController::Flyby;
		const bool resultScreenCamera = flybyWithoutControl ||
			cameraController.currentCameraMode == CameraController::PlayerFallenWater ||
			cameraController.currentCameraMode == CameraController::PedDeadBaby ||
			cameraController.currentCameraMode == CameraController::ArrestCamOne ||
			cameraController.currentCameraMode == CameraController::ArrestCamTwo;
		const bool disallowedCamera = cameraController.currentCameraMode == CameraController::Camera
			|| (!playerManager.isInVehicle
				&& cameraController.currentCameraMode == CameraController::AimWeaponFromCar);
		const bool supportedGameplayState = pluginStateApplied == OnFoot
			|| (pluginStateApplied == Driving && playerManager.isInVehicle);
		const bool contextAllowed = playerObjectsReady && thumbRestModifierActive
			&& playerManager.isInControl
			&& !playerManager.weaponWheelEnabled && !resultScreenCamera
			&& !disallowedCamera && supportedGameplayState
			&& !hudUiAutoRevealedForPause && !pauseUi2dScreenAutoEnabled
			&& pauseUiExplicitRequestExpiresAt == 0;

		if (!contextAllowed)
		{
			// Keep a reveal made on touch-down visible while the gesture waits to
			// distinguish a single tap from a double tap. The completed gesture then
			// adopts this visible state directly, eliminating the hide/show flicker.
			if (thumbRestSingleTapPending && thumbRestHudRevealOwned)
			{
				thumbRestHudContextWasEligible = false;
				return;
			}
			RestoreThumbRestHudContext("thumb-rest context ended");
			thumbRestHudRevealAttempted = false;
			if (thumbRestHudContextWasEligible)
				API::get()->log_info("[ThumbRestHud] context inactive; no HUD ownership");
			thumbRestHudContextWasEligible = false;
			return;
		}

		if (!thumbRestHudContextWasEligible)
		{
			thumbRestHudContextWasEligible = true;
			API::get()->log_info("[ThumbRestHud] context eligible state=%s modifier=true",
				playerManager.isInVehicle ? "vehicle" : "on-foot");
		}

		if (thumbRestHudRevealOwned)
			return;
		if (thumbRestHudRevealAttempted)
			return;

		thumbRestHudWasVisible = hudUiVisible;
		thumbRestHudWasPinned = hudUiPinned;
		thumbRestHudWasPauseOwned = hudUiAutoRevealedForPause || pauseUi2dScreenAutoEnabled
			|| pauseUiExplicitRequestExpiresAt != 0;
		thumbRestHudWasAutoHideTimerActive = hudAutoHideTimerActive;
		thumbRestHudWasAutoHideRemaining = hudAutoHideRemaining;

		// Never steal an already-visible/pinned/pause-owned HUD. This feature only
		// owns a reveal when the contextual modifier starts with HUD hidden.
		if (thumbRestHudWasVisible || thumbRestHudWasPinned || thumbRestHudWasPauseOwned)
		{
			thumbRestHudRevealAttempted = true;
			API::get()->log_info(
				"[ThumbRestHud] reveal skipped visible=%s pinned=%s pauseOwned=%s",
				thumbRestHudWasVisible ? "true" : "false", thumbRestHudWasPinned ? "true" : "false",
				thumbRestHudWasPauseOwned ? "true" : "false");
			return;
		}

		// Do not retry a failed runtime write every engine tick. A fresh thumb-rest
		// edge (or a later reload) can retry once the UEVR runtime is ready.
		thumbRestHudRevealAttempted = true;
		if (SetHudVisibility(true, "thumb-rest contextual HUD reveal"))
		{
			thumbRestHudRevealOwned = true;
			CancelHudAutoHideTimer();
			API::get()->log_info(
				"[ThumbRestHud] reveal owned visibleBefore=false pinnedBefore=%s timerBefore=%s remaining=%.2f",
				thumbRestHudWasPinned ? "true" : "false",
				thumbRestHudWasAutoHideTimerActive ? "true" : "false", thumbRestHudWasAutoHideRemaining);
		}
	}

	void UpdateHudAutoHide(float delta)
	{
		if (lastHudAutoHideEnabled != settingsManager.enableHudAutoHide)
		{
			lastHudAutoHideEnabled = settingsManager.enableHudAutoHide;
			if (lastHudAutoHideEnabled && hudUiVisible && !hudUiAutoRevealedForPause && !hudUiPinned)
				ResetHudAutoHideTimer();
			else
				CancelHudAutoHideTimer();
		}

		if (thumbRestHudRevealOwned)
			return;

		if (!settingsManager.enableHudAutoHide || hudUiPinned || !hudAutoHideTimerActive || !hudUiVisible)
			return;
		if (!playerManager.isInControl || hudUiAutoRevealedForPause)
			return;

		hudAutoHideRemaining -= delta;
		if (hudAutoHideRemaining <= 0.0f)
			SetHudVisibility(false, "20-second auto-hide");
	}

	bool PostGameKey(WORD virtualKey, const char* reason)
	{
		if (gameWindowHandle == nullptr || !IsWindow(gameWindowHandle))
		{
			API::get()->log_warn("[DUALGRIP] no game window for %s", reason);
			return false;
		}

		const LPARAM keyDown = 1;
		const LPARAM keyUp = 1 | (1L << 30) | (1L << 31);
		const bool posted = PostMessage(gameWindowHandle, WM_KEYDOWN, virtualKey, keyDown) != FALSE &&
			PostMessage(gameWindowHandle, WM_KEYUP, virtualKey, keyUp) != FALSE;
		if (posted)
			API::get()->log_info("[DUALGRIP] %s key posted", reason);
		else
			API::get()->log_warn("[DUALGRIP] failed to post %s key", reason);
		return posted;
	}

	bool QueueGameCheat(const char* cheat, const char* reason)
	{
		if (cheat == nullptr || cheat[0] == '\0')
			return false;
		if (gameWindowHandle == nullptr || !IsWindow(gameWindowHandle))
		{
			API::get()->log_warn("[CheatActions] no game window for %s", reason);
			return false;
		}
		if (queuedCheatIndex < queuedCheat.size())
		{
			API::get()->log_warn("[CheatActions] ignored %s because another cheat is still being entered", reason);
			return false;
		}

		queuedCheat = cheat;
		queuedCheatIndex = 0;
		nextQueuedCheatTime = 0;
		API::get()->log_info("[CheatActions] queued %s (%s)", cheat, reason);
		return true;
	}

	bool SendCheatKey(WORD virtualKey)
	{
		const HWND foregroundWindow = GetForegroundWindow();
		const HWND foregroundRoot = foregroundWindow != nullptr ? GetAncestor(foregroundWindow, GA_ROOT) : nullptr;
		if (foregroundWindow != gameWindowHandle && foregroundRoot != gameWindowHandle)
		{
			API::get()->log_warn("[CheatActions] GTA is not the foreground window; cheat entry cancelled");
			return false;
		}

		const WORD scanCode = static_cast<WORD>(MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC));
		if (scanCode == 0)
		{
			API::get()->log_warn("[CheatActions] no scan code for virtual key %u", virtualKey);
			return false;
		}

		INPUT input[2]{};
		input[0].type = INPUT_KEYBOARD;
		input[0].ki.wScan = scanCode;
		input[0].ki.dwFlags = KEYEVENTF_SCANCODE;
		input[1] = input[0];
		input[1].ki.dwFlags |= KEYEVENTF_KEYUP;

		SetLastError(ERROR_SUCCESS);
		const UINT sent = SendInput(2, input, sizeof(INPUT));
		if (sent != 2)
		{
			API::get()->log_warn("[CheatActions] SendInput sent %u/2 events (error %lu)", sent, GetLastError());
			return false;
		}
		return true;
	}

	void ProcessQueuedCheat()
	{
		if (queuedCheatIndex >= queuedCheat.size())
			return;

		const ULONGLONG now = GetTickCount64();
		if (now < nextQueuedCheatTime)
			return;
		if (gameWindowHandle == nullptr || !IsWindow(gameWindowHandle))
		{
			API::get()->log_warn("[CheatActions] game window disappeared while entering %s", queuedCheat.c_str());
			queuedCheat.clear();
			queuedCheatIndex = 0;
			return;
		}

		const WORD key = static_cast<WORD>(static_cast<unsigned char>(queuedCheat[queuedCheatIndex]));
		if (!SendCheatKey(key))
		{
			API::get()->log_warn("[CheatActions] failed entering %s", queuedCheat.c_str());
			queuedCheat.clear();
			queuedCheatIndex = 0;
			return;
		}
		++queuedCheatIndex;

		nextQueuedCheatTime = now + 35;
		if (queuedCheatIndex == queuedCheat.size())
		{
			API::get()->log_info("[CheatActions] completed %s", queuedCheat.c_str());
			queuedCheat.clear();
			queuedCheatIndex = 0;
		}
	}

	void EnforceFirstPersonCameraLock()
	{
		if (aircraftCameraRevealActive)
			return;

		if (!settingsManager.enableFirstPersonCameraLock || !playerManager.isInControl)
			return;

		// Keep scripted camera weapon and drive-by aim states under game control.
		if (cameraController.currentCameraMode == CameraController::Camera ||
			cameraController.currentCameraMode == CameraController::AimWeaponFromCar)
			return;

		if (playerManager.isInVehicle)
		{
			if (cameraController.currentVehicleCameraMode == CameraController::VehicleCameraMode::Close)
				return;

			const auto close = CameraController::VehicleCameraMode::Close;
			*(reinterpret_cast<CameraController::VehicleCameraMode*>(memoryManager.vehicleCameraModeAddress)) = close;
			cameraController.currentVehicleCameraMode = close;
		}
		else
		{
			if (cameraController.currentOnFootCameraMode == CameraController::OnFootCameraMode::Close)
				return;

			const auto close = CameraController::OnFootCameraMode::Close;
			*(reinterpret_cast<CameraController::OnFootCameraMode*>(memoryManager.onFootCameraModeAddress)) = close;
			cameraController.currentOnFootCameraMode = close;
		}
	}

	void UpdateAircraftCameraReveal()
	{
		const uint8_t requestMask = aircraftCameraRevealRequestMask.exchange(0U, std::memory_order_acq_rel);
		const bool eligible = playerManager.isInControl && playerManager.isInVehicle
			&& playerManager.vehicleType == PlayerManager::Plane
			&& IsRetractableLandingGearPlane(activeDrivingVehicleModelId);
		const ULONGLONG now = GetTickCount64();

		if (requestMask != 0U && eligible)
		{
			if (!aircraftCameraRevealActive)
				aircraftCameraRevealRestoreMode = cameraController.currentVehicleCameraMode;
			aircraftCameraRevealActive = true;
			aircraftCameraRevealRestoring = false;
			aircraftCameraRevealExpiresAt = now + 3000ULL;
			API::get()->log_info("[AircraftCamera] direct third-person reveal started reason=%s model=%d restore=%s durationMs=3000",
				(requestMask & 1U) != 0U && (requestMask & 2U) != 0U ? "gear+vtol"
				: (requestMask & 1U) != 0U ? "gear" : "vtol",
				activeDrivingVehicleModelId,
				cameraController.VehicleCameraModeToString(aircraftCameraRevealRestoreMode).c_str());
		}

		if (!aircraftCameraRevealActive)
			return;

		if (aircraftCameraRevealRestoring)
		{
			if (now >= aircraftCameraRevealExpiresAt)
			{
				aircraftCameraRevealActive = false;
				aircraftCameraRevealRestoring = false;
				aircraftCameraRevealExpiresAt = 0;
				API::get()->log_info("[AircraftCamera] FPS restore settled mode=%s",
					cameraController.VehicleCameraModeToString(aircraftCameraRevealRestoreMode).c_str());
			}
			return;
		}

		if (now >= aircraftCameraRevealExpiresAt)
		{
			*(reinterpret_cast<CameraController::VehicleCameraMode*>(memoryManager.vehicleCameraModeAddress))
				= aircraftCameraRevealRestoreMode;
			cameraController.currentVehicleCameraMode = aircraftCameraRevealRestoreMode;
			aircraftCameraRevealRestoring = true;
			aircraftCameraRevealExpiresAt = now + 150ULL;
			API::get()->log_info("[AircraftCamera] FPS restore requested mode=%s settleMs=150",
				cameraController.VehicleCameraModeToString(aircraftCameraRevealRestoreMode).c_str());
			return;
		}

		const auto exteriorMode = CameraController::VehicleCameraMode::Normal;
		if (cameraController.currentVehicleCameraMode != exteriorMode)
		{
			*(reinterpret_cast<CameraController::VehicleCameraMode*>(memoryManager.vehicleCameraModeAddress))
				= exteriorMode;
			cameraController.currentVehicleCameraMode = exteriorMode;
		}
	}

	void UpdatePauseHudVisibility()
	{
		if (!hudUiStateInitialized)
		{
			hudUiVisible = settingsManager.enableShowUiAtStartup;
			hudUiStateInitialized = true;
			previousControlStateForHud = playerManager.isInControl;
			return;
		}

		const ULONGLONG now = GetTickCount64();
		const bool enteredNoControlState = previousControlStateForHud && !playerManager.isInControl;
		const bool returnedToGameplay = !previousControlStateForHud && playerManager.isInControl;
		const bool explicitPauseRequest = pauseUiExplicitRequestExpiresAt != 0 &&
			now <= pauseUiExplicitRequestExpiresAt;
		const bool flybyWithoutControl = !playerManager.isInControl
			&& cameraController.currentCameraMode == CameraController::Flyby;
		const bool resultScreenCamera = flybyWithoutControl ||
			cameraController.currentCameraMode == CameraController::PlayerFallenWater ||
			cameraController.currentCameraMode == CameraController::PedDeadBaby ||
			cameraController.currentCameraMode == CameraController::ArrestCamOne ||
			cameraController.currentCameraMode == CameraController::ArrestCamTwo;
		if (settingsManager.enablePauseUiAutoShow && resultScreenCamera
			&& !resultScreenPresentationActive)
		{
			// Result/death cameras can arrive without a same-frame isInControl edge.
			// Own their presentation directly so mission-fail/retry UI reaches the HMD.
			resultScreenPresentationActive = true;
			resultScreenHudAutoRevealed = !hudUiVisible;
			SetHudVisibility(true, "result-screen camera reveal");
			CancelHudAutoHideTimer();

			bool screen2dWasEnabled = false;
			resultScreen2dAutoEnabled = settingsManager.GetPause2dScreenMode(screen2dWasEnabled)
				&& !screen2dWasEnabled
				&& settingsManager.SetPause2dScreenMode(true);
			API::get()->log_info(
				"[PauseUITrace] result presentation entered camera=%d hudOwned=%s 2dOwned=%s runtimeHud=%s runtime2d=%s",
				static_cast<int>(cameraController.currentCameraMode),
				resultScreenHudAutoRevealed ? "true" : "false",
				resultScreen2dAutoEnabled ? "true" : "false",
				ReadHudRuntimeValue().c_str(), ReadPause2dRuntimeValue().c_str());
		}
		else if (resultScreenPresentationActive && !resultScreenCamera
			&& playerManager.isInControl)
		{
			// Keep presentation through retry/quit menus after the result camera ends;
			// restore only when normal gameplay control has actually returned.
			if (resultScreen2dAutoEnabled && !pauseUi2dScreenAutoEnabled)
				settingsManager.SetPause2dScreenMode(false);
			if (resultScreenHudAutoRevealed && !hudUiPinned && !hudUiAutoRevealedForPause)
				SetHudVisibility(false, "restore hidden UI after result screen");
			else if (!resultScreenHudAutoRevealed && hudUiVisible && !hudUiPinned
				&& settingsManager.enableHudAutoHide)
				ResetHudAutoHideTimer();
			API::get()->log_info(
				"[PauseUITrace] result presentation restored camera=%d hudOwned=%s 2dOwned=%s runtimeHud=%s runtime2d=%s",
				static_cast<int>(cameraController.currentCameraMode),
				resultScreenHudAutoRevealed ? "true" : "false",
				resultScreen2dAutoEnabled ? "true" : "false",
				ReadHudRuntimeValue().c_str(), ReadPause2dRuntimeValue().c_str());
			resultScreenPresentationActive = false;
			resultScreenHudAutoRevealed = false;
			resultScreen2dAutoEnabled = false;
		}
		const bool use2dFallback = explicitPauseRequest || resultScreenCamera;
		if (enteredNoControlState || returnedToGameplay)
		{
			API::get()->log_info(
				"[PauseUITrace] controls=%s->%s autoShow=%s trackedVisible=%s runtime=%s autoRevealed=%s pinned=%s timer=%s remaining=%.2f",
				previousControlStateForHud ? "true" : "false",
				playerManager.isInControl ? "true" : "false",
				settingsManager.enablePauseUiAutoShow ? "true" : "false",
				hudUiVisible ? "true" : "false", ReadHudRuntimeValue().c_str(),
				hudUiAutoRevealedForPause ? "true" : "false", hudUiPinned ? "true" : "false",
				hudAutoHideTimerActive ? "true" : "false", hudAutoHideRemaining);
		}

		if (enteredNoControlState && !settingsManager.enablePauseUiAutoShow)
		{
			API::get()->log_info("%s", "[PauseUITrace] auto-show skipped reason=feature-disabled");
		}
		if (settingsManager.enablePauseUiAutoShow && enteredNoControlState)
		{
			const bool uiWasHidden = !hudUiVisible;
			if (SetHudVisibility(true, uiWasHidden ? "auto-show while paused" : "reassert UI for no-controls"))
			{
				if (uiWasHidden)
					hudUiAutoRevealedForPause = true;

				bool screen2dWasEnabled = false;
				if (use2dFallback && settingsManager.GetPause2dScreenMode(screen2dWasEnabled))
				{
					pauseUi2dScreenAutoEnabled = !screen2dWasEnabled
						&& settingsManager.SetPause2dScreenMode(true);
					API::get()->log_info("[PauseUITrace] 2D fallback previous=%s runtime=%s",
						screen2dWasEnabled ? "true" : "false", ReadPause2dRuntimeValue().c_str());
				}
				else if (use2dFallback)
				{
					pauseUi2dScreenAutoEnabled = false;
					API::get()->log_warn("%s", "[PauseUITrace] 2D fallback unavailable");
				}
				else
				{
					pauseUi2dScreenAutoEnabled = false;
					API::get()->log_info("[PauseUITrace] regular VR retained for interactive no-controls camera=%d",
						static_cast<int>(cameraController.currentCameraMode));
				}
			}
			pauseUiExplicitRequestExpiresAt = 0;
		}

		if (returnedToGameplay && pauseUi2dScreenAutoEnabled)
		{
			settingsManager.SetPause2dScreenMode(false);
			API::get()->log_info("[PauseUITrace] 2D fallback restored runtime=%s", ReadPause2dRuntimeValue().c_str());
			pauseUi2dScreenAutoEnabled = false;
		}

		if (returnedToGameplay && hudUiAutoRevealedForPause)
		{
			SetHudVisibility(false, "restore hidden UI after pause");
			hudUiAutoRevealedForPause = false;
		}
		if (returnedToGameplay)
			pauseUiExplicitRequestExpiresAt = 0;

		// Mission and camera transitions can rebuild UEVR's GUI presentation while
		// the desktop HUD remains visible. Keep pin ownership authoritative without
		// writing every frame: reassert once on a state transition, then poll the
		// actual runtime value at a low rate and repair only confirmed drift.
		const int currentPluginState = static_cast<int>(pluginStateApplied);
		const int currentCameraMode = static_cast<int>(cameraController.currentCameraMode);
		if (hudUiPinned)
		{
			const bool presentationTransition = enteredNoControlState || returnedToGameplay
				|| hudPinnedObservedPluginState != currentPluginState
				|| hudPinnedObservedCameraMode != currentCameraMode;
			if (presentationTransition || now >= hudPinnedRuntimeCheckAt)
			{
				bool runtimeVisible = false;
				const bool runtimeReadable = ReadHudRuntimeVisible(runtimeVisible);
				if (presentationTransition || !runtimeReadable || !runtimeVisible)
				{
					const char* reason = presentationTransition
						? "pinned HUD transition reassert"
						: "pinned HUD runtime drift recovery";
					SetHudVisibility(true, reason);
					API::get()->log_info(
						"[ThumbRestHud] pinned reconcile transition=%s readable=%s previousRuntime=%s pluginState=%d camera=%d",
						presentationTransition ? "true" : "false",
						runtimeReadable ? "true" : "false",
						runtimeVisible ? "true" : "false",
						currentPluginState, currentCameraMode);
				}
				hudPinnedRuntimeCheckAt = now + HudPinnedRuntimeCheckIntervalMs;
			}
			hudPinnedObservedPluginState = currentPluginState;
			hudPinnedObservedCameraMode = currentCameraMode;
		}
		else
		{
			hudPinnedRuntimeCheckAt = 0;
			hudPinnedObservedPluginState = -1;
			hudPinnedObservedCameraMode = -1;
		}

		if (!playerManager.isInControl)
		{
			if (pauseUiNoControlTraceNextAt == 0)
			{
				pauseUiNoControlTraceSamples = 0;
				pauseUiNoControlTraceNextAt = now;
			}
			if (pauseUiNoControlTraceSamples < PauseUiNoControlTraceSampleLimit &&
				now >= pauseUiNoControlTraceNextAt)
			{
				API::get()->log_info(
					"[PauseUITrace] no-control sample=%d trackedVisible=%s runtime=%s screen2d=%s autoShow=%s autoRevealed=%s pluginState=%d objects=%s inVehicle=%s vehicleType=%d camera=%d",
					pauseUiNoControlTraceSamples, hudUiVisible ? "true" : "false",
					ReadHudRuntimeValue().c_str(), ReadPause2dRuntimeValue().c_str(), settingsManager.enablePauseUiAutoShow ? "true" : "false",
					hudUiAutoRevealedForPause ? "true" : "false", static_cast<int>(pluginStateApplied),
					(playerManager.playerController != nullptr && playerManager.playerHead != nullptr) ? "ready" : "missing",
					playerManager.isInVehicle ? "true" : "false", static_cast<int>(playerManager.vehicleType),
					static_cast<int>(cameraController.currentCameraMode));
				++pauseUiNoControlTraceSamples;
				pauseUiNoControlTraceNextAt = now + PauseUiNoControlTraceIntervalMs;
			}
		}
		else
		{
			pauseUiNoControlTraceNextAt = 0;
			pauseUiNoControlTraceSamples = 0;
		}

		// isInControl also drops during loading and UEVR renderer resets, so it is
		// not a safe pause/menu signal. Driving VR_EnableGUI from that state can
		// make each GUI change trigger another reset and another control transition.
		// Manual UI controls remain available; automatic reveal needs a dedicated
		// pause/failure-screen signal before it can be safely restored.
		hudUiAutoRevealedForPause = false;
		previousControlStateForHud = playerManager.isInControl;
	}

	SettingsManager::CameraModeSettings GetDrivingCameraSettings()
	{
		switch (playerManager.vehicleType)
		{
		case PlayerManager::Plane:
		case PlayerManager::Helicopter:
			return SettingsManager::Flying;
		case PlayerManager::Bike:
			return SettingsManager::DrivingBike;
		case PlayerManager::CarOrBoat:
		default:
			return SettingsManager::DrivingCar;
		}
	}

	std::string GetDrivingCameraProfilePrefix(int modelId)
	{
		if (modelId >= 400 && modelId <= 611)
			return "VehicleCamera_" + std::to_string(modelId);

		return "";
	}

	void ApplyDrivingCameraSettings()
	{
		appliedDrivingCameraSettings = GetDrivingCameraSettings();
		appliedDrivingCameraProfilePrefix.clear();
		candidateDrivingVehicleModelId = -1;
		candidateDrivingVehicleModelSamples = 0;
		activeDrivingVehicleModelId = -1;

		// Apply the category fallback immediately, but do not let a transient vehicle
		// state save over it. Saving starts after one model ID is seen consistently.
		settingsManager.ApplyCameraSettings(appliedDrivingCameraSettings, "", false);
		drivingCameraProfileRefreshTimer = 0.0f;
		API::get()->log_info("[CameraProfiles] driving category active; waiting for stable vehicle model");
	}

	void RefreshDrivingCameraProfile(float delta)
	{
		drivingCameraProfileRefreshTimer -= delta;
		if (drivingCameraProfileRefreshTimer > 0.0f)
			return;

		drivingCameraProfileRefreshTimer = activeDrivingVehicleModelId >= 0
			? DrivingCameraProfileActiveSampleSeconds
			: DrivingCameraProfileSampleSeconds;
		const auto cameraSettings = GetDrivingCameraSettings();
		if (cameraSettings != appliedDrivingCameraSettings)
		{
			appliedDrivingCameraSettings = cameraSettings;
			appliedDrivingCameraProfilePrefix.clear();
			candidateDrivingVehicleModelId = -1;
			candidateDrivingVehicleModelSamples = 0;
			activeDrivingVehicleModelId = -1;
			settingsManager.ApplyCameraSettings(appliedDrivingCameraSettings, "", false);
			API::get()->log_info("[CameraProfiles] driving category changed; waiting for stable vehicle model");
		}

		const int modelId = memoryManager.ResolveCurrentVehicleModelId(static_cast<int>(playerManager.vehicleType));
		if (modelId < 400 || modelId > 611)
		{
			candidateDrivingVehicleModelId = -1;
			candidateDrivingVehicleModelSamples = 0;
			return;
		}

		if (modelId == activeDrivingVehicleModelId)
			return;

		if (modelId != candidateDrivingVehicleModelId)
		{
			candidateDrivingVehicleModelId = modelId;
			candidateDrivingVehicleModelSamples = 1;
			API::get()->log_info("[CameraProfiles] vehicle candidate model=%d sample=1/%d",
				modelId, DrivingCameraProfileRequiredSamples);
			return;
		}

		++candidateDrivingVehicleModelSamples;
		if (candidateDrivingVehicleModelSamples < DrivingCameraProfileRequiredSamples)
			return;

		activeDrivingVehicleModelId = modelId;
		appliedDrivingCameraProfilePrefix = GetDrivingCameraProfilePrefix(modelId);
		settingsManager.ApplyCameraSettings(appliedDrivingCameraSettings, appliedDrivingCameraProfilePrefix, true);
		API::get()->log_info("[CameraProfiles] vehicle profile active: %s",
			appliedDrivingCameraProfilePrefix.c_str());
	}

	void ApplyBaseState(bool applyOnFootCamera = true)
	{
		cameraController.camResetRequested = true;
		memoryManager.ToggleAllMemoryInstructions(false);
		if (playerManager.isInVehicle && cameraController.currentCameraMode == CameraController::AimWeaponFromCar)
		{
			memoryManager.RestoreCarAimingVectorInstructions();
			carAimingVectorRestoredForVehicleAim = true;
		}
		memoryManager.InstallBreakpoints();
		uevr::API::UObjectHook::set_disabled(false);
		weaponManager.ResetShootingState();
		if (applyOnFootCamera)
		{
			candidateDrivingVehicleModelId = -1;
			candidateDrivingVehicleModelSamples = 0;
			activeDrivingVehicleModelId = -1;
			appliedDrivingCameraProfilePrefix.clear();
			settingsManager.ApplyCameraSettings(SettingsManager::OnFoot);
		}
		pluginStateApplied = OnFoot;
		if (settingsManager.debugMod) API::get()->log_info("pluginStateApplied = OnFoot");
	}

	void ApplyDrivingState()
	{
		memoryManager.RestoreVehicleRelatedMemoryInstructions();
		ApplyDrivingCameraSettings();
		// Vehicle entry is a hard presentation boundary. Restore both native hands
		// before the bounded right-hand free-aim path decides whether to replace one.
		weaponManager.RestoreFreeAimWeaponHands();
		weaponManager.UnhookAndRepositionWeapon(true, true);
		pluginStateApplied = Driving;
		/*if (settingsManager.debugMod) */API::get()->log_error("pluginStateApplied = Driving");
	}

	void ApplyCameraWeaponState()
	{
		memoryManager.ToggleAllMemoryInstructions(true);
		pluginStateApplied = CameraWeapon;
		API::get()->log_error("pluginStateApplied = Camera");
	}

	void ApplyVRdisabledState()
	{
		weaponManager.ResetShootingState();
		memoryManager.RemoveBreakpoints();
		memoryManager.ToggleAllMemoryInstructions(true);
		cameraController.FixUnderwaterView(false);
		uevr::API::UObjectHook::set_disabled(true);
		playerManager.RepositionUnhookedUobjects();
		weaponManager.UnhookAndRepositionWeapon();
		settingsManager.ApplyCameraSettings(SettingsManager::Cutscene);
		pluginStateApplied = VRdisabled;
		API::get()->log_error("pluginStateApplied = NoControls");
	}

	void SendStatesToLua()
	{
		const bool meleeNativeTriggerBlocked = weaponManager.ShouldBlockNativeMeleeTriggerInput();
		if (!meleeNativeTriggerBlockStateInitialized
			|| meleeNativeTriggerBlocked != lastMeleeNativeTriggerBlockSentToLua)
		{
			meleeNativeTriggerBlockStateInitialized = true;
			lastMeleeNativeTriggerBlockSentToLua = meleeNativeTriggerBlocked;
			API::get()->dispatch_lua_event("meleeNativeTriggerBlockState",
				meleeNativeTriggerBlocked ? "true" : "false");
		}
		if (playerManager.isInControl != playerManager.wasInControl)
			API::get()->dispatch_lua_event("playerControlState", playerManager.isInControl ? "true" : "false");
		if (playerManager.vehicleType != playerManager.previousVehicleType)
			API::get()->dispatch_lua_event("playerState", playerManager.VehicleTypeToString(playerManager.vehicleType));
		if (activeDrivingVehicleModelId != lastDrivingVehicleModelIdSentToLua)
		{
			lastDrivingVehicleModelIdSentToLua = activeDrivingVehicleModelId;
			const std::string modelId = activeDrivingVehicleModelId >= 0
				? std::to_string(activeDrivingVehicleModelId)
				: "unknown";
			API::get()->dispatch_lua_event("vehicleModelState", modelId.c_str());
		}
		if (cameraController.previousOnFootCameraMode != cameraController.currentOnFootCameraMode)
			API::get()->dispatch_lua_event("onFootCameraMode", cameraController.OnFootCameraModeToString(cameraController.currentOnFootCameraMode));
		if (cameraController.previousVehicleCameraMode != cameraController.currentVehicleCameraMode)
			API::get()->dispatch_lua_event("vehicleCameraMode", cameraController.VehicleCameraModeToString(cameraController.currentVehicleCameraMode));
	}

	void UpdatePhoneRingingState()
	{
		bool currentRinging = false;
		if (!memoryManager.ReadPhoneRingingState(currentRinging))
			return;

		if (phoneRingingStateInitialized && currentRinging == phoneRinging)
			return;

		const bool wasInitialized = phoneRingingStateInitialized;
		phoneRinging = currentRinging;
		phoneRingingStateInitialized = true;
		API::get()->dispatch_lua_event("phoneRingingState", phoneRinging ? "true" : "false");
		if (wasInitialized || phoneRinging)
			API::get()->log_info("[Phone] ringing=%s", phoneRinging ? "true" : "false");
	}
};

// Actually creates the plugin. Very important that this global is created.
// The fact that it's using std::unique_ptr is not important, as long as the constructor is called in some way.
std::unique_ptr<GTASADE_Plugin> g_plugin{ new GTASADE_Plugin() };
