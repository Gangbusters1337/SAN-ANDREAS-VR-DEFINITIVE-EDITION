print("DUALGRIP.lua loaded")

local vr = uevr.params.vr

local LEFT_SHOULDER = XINPUT_GAMEPAD_LEFT_SHOULDER or 0x0100
local RIGHT_SHOULDER = XINPUT_GAMEPAD_RIGHT_SHOULDER or 0x0200
local BUTTON_START = XINPUT_GAMEPAD_START or 0x0010
local BUTTON_BACK = XINPUT_GAMEPAD_BACK or 0x0020
local LEFT_THUMB = XINPUT_GAMEPAD_LEFT_THUMB or 0x0040
local RIGHT_THUMB = XINPUT_GAMEPAD_RIGHT_THUMB or 0x0080
local BUTTON_A = XINPUT_GAMEPAD_A or 0x1000
local BUTTON_B = XINPUT_GAMEPAD_B or 0x2000
local BUTTON_X = XINPUT_GAMEPAD_X or 0x4000
local BUTTON_Y = XINPUT_GAMEPAD_Y or 0x8000
local DPAD_UP = XINPUT_GAMEPAD_DPAD_UP or 0x0001
local DPAD_DOWN = XINPUT_GAMEPAD_DPAD_DOWN or 0x0002
local DPAD_LEFT = XINPUT_GAMEPAD_DPAD_LEFT or 0x0004
local DPAD_RIGHT = XINPUT_GAMEPAD_DPAD_RIGHT or 0x0008

local isPlayerDriving = false
local playerState = "OnFoot"
local isPlayerAircraft = false
local currentVehicleModelId = -1
local PEDAL_BIKE_MODELS = {
    [481] = true,
    [509] = true,
    [510] = true
}
local playerInputEnabled = true
local activeHandSide = "right"
local activeVisibilityState = "idle"
local enableDualGripAimFire = true
local enableAlternateWeaponHandsVisibility = true
-- Weapon cycling belongs exclusively to the explicit Quest X action. Grip
-- transitions must never synthesize shoulder/mouse-wheel cycle input.
local enableGripWeaponCycle = false
local enableManualReloadMode = false
local enablePhoneAnswerGripTap = true
local enableUtilityWeaponCycleReset = true
local enableABWeaponCycleTest = false
local enableChordPauseMenu = true
local enableChordHudToggle = false
local enableShortPressCameraSwitch = true
local enableVehicleFaceButtonFire = false
local vehicleFreeAimActive = false
local previousVehicleFaceFireHeld = false
local vehicleFaceFireSyntheticLeftShoulder = false
local vehicleFaceFireLastProofAt = -math.huge
local VEHICLE_FACE_FIRE_PROOF_INTERVAL_SECONDS = 0.5
local enableAircraftNativeControls = true
local enableR3LeftStickDpad = false
-- The C++ XInput layer owns this remap now. Keep the old Lua implementation
-- disabled so it cannot duplicate or overwrite the final synthetic pulse.
local enableDualGripDpad = false
local lastDualGripDpadDirection = nil
local leftThumbRestTouchAction = nil
local leftThumbRestTouchSource = nil
local leftThumbRestTouchResolvedPath = nil
local leftThumbRestProbeState = nil
local leftThumbRestLookupLastAttemptAt = -math.huge
-- UEVR normalizes action names when it creates the runtime handles. Try the
-- manifest spelling first, then the spellings shown by the injector log.
local LEFT_THUMB_REST_ACTION_PATHS = {
    "/actions/default/in/ThumbRestTouchLeft",
    "/actions/default/in/ThumbrestTouchLeft",
    "/actions/default/in/thumbresttouchleft",
}
local LEFT_THUMB_REST_LOOKUP_RETRY_SECONDS = 1.0
local debugInputLayerProbe = false
local currentWeaponId = 0
local authoritativeMeleeTriggerBlock = false
local lastManualReloadWeaponId = 0
local manualReloadEmptyFallbackArmed = false
local nativeSpecialWeaponFallbackArmed = false
local manualReloadWeaponEmpty = false
local weaponPreviewUntil = nil
local WEAPON_PREVIEW_SECONDS = 2.5
local TAP_CYCLE_WINDOW_SECONDS = 0.34
local SIDE_SWITCH_HOLD_SECONDS = 0.08
local CYCLE_PULSE_DELAY_SECONDS = 0.0
local CYCLE_PULSE_SECONDS = 0.12
local UTILITY_CYCLE_RESET_DELAY_SECONDS = 0.18
local UTILITY_CYCLE_RESET_SECONDS = 0.30
local CYCLE_BUTTON = LEFT_SHOULDER
local leftGripPressedAt = nil
local leftGripHeldLongEnough = false
local leftGripCycleFireSeen = false
local leftGripCyclePulseStart = nil
local leftGripCyclePulseUntil = nil
local leftGripCycleUsesMouseWheel = false
local leftGripCycleMouseWheelSent = false
local leftGripCycleShoulderLogged = false
local clear_cycle_pulse
local lastCycleReleaseLogAt = 0.0
local abCyclePulseButton = nil
local abCyclePulseUntil = nil
local abCycleLastA = false
local abCycleLastB = false
local lastABCycleLogAt = 0.0
local previousQuestXCycleHeld = false
local previousBikeRightPedalHeld = false
local cycleProbe = nil
local chordHudHeldLastFrame = false
local controlGuideVisible = false
local controlGuideInputArmed = false
local controlGuideStickLatched = false
local controlGuideToggleHeld = false
local chordPauseHeldLastFrame = false
local chordPausePulseUntil = nil
local chordPauseCaptureUntil = nil
local chordPauseCapturedButton = nil
local chordPauseCaptureStartedAt = nil
local chordPauseReplayUntil = nil
local chordPauseIgnoreUntilRelease = false
local lastChordLogAt = 0.0
local VEHICLE_PAUSE_CHORD_WINDOW_SECONDS = 0.07
local VEHICLE_SINGLE_BUTTON_REPLAY_SECONDS = 0.08
local lastInputProbeSignature = nil
local previousFireInputHeld = false
local triggerTimingSequence = 0
local LEFT_SYSTEM_SHORT_PRESS_SECONDS = 0.35
local DPAD_STICK_THRESHOLD = 16000
local DPAD_STICK_RELEASE_THRESHOLD = 8000
local leftSystemAction = nil
local leftSystemSource = nil
local leftSystemPressedAt = nil
local leftSystemWasHeld = false
local vehicleLeftFireLogged = false
local vehicleRightFireLogged = false
local phoneRinging = false
local rightGripPhonePressedAt = nil
local previousRightGripPhoneHeld = false
local phoneAnswerPulseUntil = nil
local PHONE_ANSWER_TAP_SECONDS = 0.30
-- Mission script input is sampled less consistently than ordinary gameplay
-- input. Keep the synthesized RB edge long enough to survive a slow script tick.
local PHONE_ANSWER_PULSE_SECONDS = 0.25
local rightGripReloadPressedAt = nil
local previousRightGripReloadHeld = false
local rightGripReloadFireSeen = false
local rightGripReloadTwoHandSeen = false
local manualReloadAimPulseUntil = nil
local MANUAL_RELOAD_TAP_SECONDS = 0.30
local MANUAL_RELOAD_AIM_PULSE_SECONDS = 0.12
local heldVisualUntil = nil
local HELD_VISUAL_GRACE_SECONDS = 2.0
local AK47_HELD_VISUAL_GRACE_SECONDS = 1.0
local heldVisualGraceDuration = HELD_VISUAL_GRACE_SECONDS
local heldVisualGraceActive = false
local heldVisualGraceSuppressedState = nil
local GRACE_ACTION_STICK_THRESHOLD = 12000

local function log(message)
    local line = "[DUALGRIP] " .. message

    if uevr and uevr.params and uevr.params.functions and uevr.params.functions.log_info then
        local ok = pcall(function()
            uevr.params.functions.log_info(line)
        end)
        if ok then
            return
        end
    end

    print(line)
end

local function log_control_debug(message)
    if debugInputLayerProbe then
        log(message)
    end
end

local function parse_bool(value)
    return value == "true" or value == "1" or value == "on"
end

local function has_button(buttons, mask)
    return math.floor(buttons / mask) % 2 >= 1
end

local function clear_button(buttons, mask)
    if has_button(buttons, mask) then
        return buttons - mask
    end

    return buttons
end

local function set_button(buttons, mask)
    if has_button(buttons, mask) then
        return buttons
    end

    return buttons + mask
end

local function dispatch_cycle_weapon()
    if uevr and uevr.api and uevr.api.dispatch_custom_event then
        uevr.api:dispatch_custom_event("DUALGRIP.CycleWeapon", "next")
    elseif uevr and uevr.params and uevr.params.functions and uevr.params.functions.dispatch_custom_event then
        uevr.params.functions.dispatch_custom_event("DUALGRIP.CycleWeapon", "next")
    end
end

local function dispatch_control_guide(show)
    local value = show and "show" or "hide"
    if uevr and uevr.api and uevr.api.dispatch_custom_event then
        uevr.api:dispatch_custom_event("DUALGRIP.ControlGuide", value)
    elseif uevr and uevr.params and uevr.params.functions and uevr.params.functions.dispatch_custom_event then
        uevr.params.functions.dispatch_custom_event("DUALGRIP.ControlGuide", value)
    end
end

local function dispatch_control_guide_navigation(action)
    if uevr and uevr.api and uevr.api.dispatch_custom_event then
        uevr.api:dispatch_custom_event("DUALGRIP.ControlGuideNav", action)
    elseif uevr and uevr.params and uevr.params.functions and uevr.params.functions.dispatch_custom_event then
        uevr.params.functions.dispatch_custom_event("DUALGRIP.ControlGuideNav", action)
    end
end

local function dispatch_manual_reload()
    if uevr and uevr.api and uevr.api.dispatch_custom_event then
        uevr.api:dispatch_custom_event("GTASADE.CheatAction", "ReloadCurrentWeapon")
    elseif uevr and uevr.params and uevr.params.functions and uevr.params.functions.dispatch_custom_event then
        uevr.params.functions.dispatch_custom_event("GTASADE.CheatAction", "ReloadCurrentWeapon")
    end
end

local function is_pedal_bike_vehicle()
    return playerState == "Bike" and PEDAL_BIKE_MODELS[currentVehicleModelId] == true
end

local function is_motorized_bike_vehicle()
    return playerState == "Bike"
        and currentVehicleModelId >= 0
        and not is_pedal_bike_vehicle()
end

local function apply_quest_x_weapon_cycle(state, buttons, now)
    -- UEVR maps the physical Quest left X button to Xbox B in this profile.
    -- Calibration is thumb-rest-only, so X always owns weapon cycling even
    -- while a weapon grip is held. A+X is consumed earlier by the HUD chord.
    local held = has_button(buttons, BUTTON_B)
	local rightTriggerPedalHeld = state.Gamepad.bRightTrigger > 30
	if is_pedal_bike_vehicle() then
		-- On pedal bicycles RT is locomotion, never fire. Translate it to GTA's
		-- native Xbox-B pedal action and clear RT so A can exclusively own the
		-- proven vehicle-fire request. Left Quest X/Xbox B remains a fallback.
		state.Gamepad.bRightTrigger = 0
		if rightTriggerPedalHeld then
			buttons = set_button(buttons, BUTTON_B)
		end
		if rightTriggerPedalHeld ~= previousBikeRightPedalHeld then
			log_control_debug(rightTriggerPedalHeld
				and "Bike RT -> native pedal pressed"
				or "Bike RT -> native pedal released")
		end
		previousBikeRightPedalHeld = rightTriggerPedalHeld
		previousQuestXCycleHeld = held
		clear_cycle_pulse()
		state.Gamepad.wButtons = buttons
		return buttons, false
	elseif playerState == "Bike" and not is_motorized_bike_vehicle() then
		-- Do not guess while the engine-thread model handoff is unresolved. Leave
		-- the native sample untouched until this bike is classified.
		if previousBikeRightPedalHeld then
			previousBikeRightPedalHeld = false
		end
		previousQuestXCycleHeld = held
		clear_cycle_pulse()
		state.Gamepad.wButtons = buttons
		return buttons, false
	elseif previousBikeRightPedalHeld then
		previousBikeRightPedalHeld = false
		log_control_debug("Bike right B pedal mapping inactive")
	end
    local pressed = held and not previousQuestXCycleHeld
    previousQuestXCycleHeld = held
    if isPlayerAircraft then
        -- GTA owns this face button while riding a bicycle (pedalling). Never
        -- replace it with the on-foot weapon-cycle pulse.
        clear_cycle_pulse()
        return buttons, false
    end
    if pressed then
		dispatch_cycle_weapon()
        -- Feed GTA the same short native shoulder pulse that previously proved
        -- weapon cycling. C++ observes the physical input before this Lua layer,
        -- so this synthetic bit cannot become a magnetic grip transition.
        leftGripCyclePulseStart = now
        leftGripCyclePulseUntil = now + CYCLE_PULSE_SECONDS
        leftGripCycleUsesMouseWheel = false
        leftGripCycleMouseWheelSent = false
        leftGripCycleShoulderLogged = true
        log_control_debug("Quest X -> native weapon-cycle pulse")
    end

    local pulseActive = leftGripCyclePulseUntil ~= nil and now <= leftGripCyclePulseUntil
    if not pulseActive and leftGripCyclePulseUntil ~= nil then
        clear_cycle_pulse()
    end
    if not held and not pulseActive then
        return buttons, false
    end

    -- Consume X for its full hold and isolate the synthetic cycle pulse from
    -- physical grips. GTA receives exactly one short LB cycle action, while the
    -- plugin's magnetic hand ownership remains based on the real controllers.
    buttons = clear_button(buttons, BUTTON_B)
    buttons = clear_button(buttons, LEFT_SHOULDER)
    buttons = clear_button(buttons, RIGHT_SHOULDER)
    if pulseActive then
        buttons = set_button(buttons, CYCLE_BUTTON)
    end
    state.Gamepad.wButtons = buttons
    return buttons, true
end

local function dispatch_vehicle_face_fire(held)
    local value = held and "pressed" or "released"
    if uevr and uevr.api and uevr.api.dispatch_custom_event then
        uevr.api:dispatch_custom_event("DUALGRIP.VehicleFaceFire", value)
    elseif uevr and uevr.params and uevr.params.functions and uevr.params.functions.dispatch_custom_event then
        uevr.params.functions.dispatch_custom_event("DUALGRIP.VehicleFaceFire", value)
    end
end

local function dispatch_trigger_timing(eventType, sequence, side, weapon, aimRequested, eligible, now)
    local payload = string.format(
        "type=%s;seq=%d;side=%s;weapon=%d;aim=%s;eligible=%s;input=%s;vehicle=%s;gate=%s;lua=%.6f",
        eventType,
        sequence,
        side,
        tonumber(weapon) or 0,
        aimRequested and "true" or "false",
        eligible and "true" or "false",
        playerInputEnabled and "true" or "false",
        (isPlayerDriving or isPlayerAircraft) and "true" or "false",
        enableDualGripAimFire and "true" or "false",
        now)
    if uevr and uevr.api and uevr.api.dispatch_custom_event then
        uevr.api:dispatch_custom_event("DUALGRIP.TriggerTiming", payload)
    elseif uevr and uevr.params and uevr.params.functions and uevr.params.functions.dispatch_custom_event then
        uevr.params.functions.dispatch_custom_event("DUALGRIP.TriggerTiming", payload)
    end
end

local function dispatch_hud_toggle()
    if uevr and uevr.api and uevr.api.dispatch_custom_event then
        uevr.api:dispatch_custom_event("DUALGRIP.ToggleHudUi", "")
    elseif uevr and uevr.params and uevr.params.functions and uevr.params.functions.dispatch_custom_event then
        uevr.params.functions.dispatch_custom_event("DUALGRIP.ToggleHudUi", "")
    end
end

local function dispatch_hud_pin_toggle()
    if uevr and uevr.api and uevr.api.dispatch_custom_event then
        uevr.api:dispatch_custom_event("DUALGRIP.ToggleHudUiPinned", "")
    elseif uevr and uevr.params and uevr.params.functions and uevr.params.functions.dispatch_custom_event then
        uevr.params.functions.dispatch_custom_event("DUALGRIP.ToggleHudUiPinned", "")
    end
end

local function dispatch_pause_ui_reveal()
    if uevr and uevr.api and uevr.api.dispatch_custom_event then
        uevr.api:dispatch_custom_event("DUALGRIP.PauseUiReveal", "")
    elseif uevr and uevr.params and uevr.params.functions and uevr.params.functions.dispatch_custom_event then
        uevr.params.functions.dispatch_custom_event("DUALGRIP.PauseUiReveal", "")
    end
end

local function dispatch_camera_cycle()
    if uevr and uevr.api and uevr.api.dispatch_custom_event then
        uevr.api:dispatch_custom_event("DUALGRIP.CycleCameraView", "")
    elseif uevr and uevr.params and uevr.params.functions and uevr.params.functions.dispatch_custom_event then
        uevr.params.functions.dispatch_custom_event("DUALGRIP.CycleCameraView", "")
    end
end

local function is_left_system_button_active()
    if not (vr and vr.get_action_handle and vr.get_left_joystick_source and vr.is_action_active) then
        return false
    end

    if leftSystemAction == nil or leftSystemSource == nil then
        local actionOk, action = pcall(function()
            return vr.get_action_handle("/actions/default/in/DPad_Left")
        end)
        local sourceOk, source = pcall(function()
            return vr.get_left_joystick_source()
        end)
        if not actionOk or not sourceOk or action == nil or source == nil then
            return false
        end
        leftSystemAction = action
        leftSystemSource = source
    end

    local activeOk, active = pcall(function()
        return vr.is_action_active(leftSystemAction, leftSystemSource)
    end)
    return activeOk and active
end

local function apply_short_press_camera_switch(state, buttons, now)
    if not enableShortPressCameraSwitch then
        leftSystemPressedAt = nil
        leftSystemWasHeld = false
        return buttons
    end

    local systemHeld = is_left_system_button_active()
    if systemHeld then
        -- The left Quest menu is deliberately routed to DPad_Left in this
        -- profile. Consume it while deciding whether this is a short press.
        buttons = clear_button(buttons, DPAD_LEFT)
        if not leftSystemWasHeld then
            leftSystemPressedAt = now
        end
        leftSystemWasHeld = true
    elseif leftSystemWasHeld then
        local heldSeconds = now - (leftSystemPressedAt or now)
        leftSystemPressedAt = nil
        leftSystemWasHeld = false
        if heldSeconds <= LEFT_SYSTEM_SHORT_PRESS_SECONDS then
            dispatch_camera_cycle()
            log(string.format("left Quest menu short press -> camera cycle (%.3fs)", heldSeconds))
        end
    end

    state.Gamepad.wButtons = buttons
    return buttons
end

local function apply_vehicle_face_button_fire(state, buttons, now)
    -- The face button is an explicit vehicle-fire request. It is consumed only
    -- after C++ has confirmed the supported controller-pistol vehicle mode, so
    -- the right trigger remains the native accelerator and ordinary controls
    -- are untouched everywhere else.
	local eligible = isPlayerDriving
        and vehicleFreeAimActive
        and not isPlayerAircraft
		and (playerState ~= "Bike" or is_motorized_bike_vehicle() or is_pedal_bike_vehicle())
        and enableVehicleFaceButtonFire
    local physicalLeftShoulderHeld = has_button(buttons, LEFT_SHOULDER)
    if not eligible then
        local hadVehicleFaceFireState = previousVehicleFaceFireHeld
            or vehicleFaceFireSyntheticLeftShoulder
        if previousVehicleFaceFireHeld then
            dispatch_vehicle_face_fire(false)
            previousVehicleFaceFireHeld = false
        end
        if vehicleFaceFireSyntheticLeftShoulder and not physicalLeftShoulderHeld then
            buttons = clear_button(buttons, LEFT_SHOULDER)
        end
        vehicleFaceFireSyntheticLeftShoulder = false
        vehicleFaceFireLastProofAt = -math.huge
        if hadVehicleFaceFireState then
            log(string.format(
                "vehicle face-fire released/ineligible -> wButtons=0x%04X (physical LB preserved=%s)",
                buttons,
                physicalLeftShoulderHeld and "true" or "false"
            ))
        end
        return buttons, false
    end

    -- In supported vehicle free aim, physical LB is a grip only. Suppress it
    -- before synthesizing GTA's native vehicle-fire bit from A.
    buttons = clear_button(buttons, LEFT_SHOULDER)
    local faceHeld = has_button(buttons, BUTTON_A)
    local wasFaceHeld = previousVehicleFaceFireHeld
    if faceHeld ~= previousVehicleFaceFireHeld then
        dispatch_vehicle_face_fire(faceHeld)
        previousVehicleFaceFireHeld = faceHeld
        log(string.format("vehicle face-fire %s (A)", faceHeld and "pressed" or "released"))
    end

    if faceHeld then
        -- A is the explicit vehicle-fire request. Reuse GTA's validated LB
        -- vehicle-fire input on every sample, while leaving RT untouched for
        -- acceleration. Physical LB was deliberately removed above.
        buttons = clear_button(buttons, BUTTON_A)
        buttons = set_button(buttons, LEFT_SHOULDER)
        vehicleFaceFireSyntheticLeftShoulder = true
        if now - vehicleFaceFireLastProofAt >= VEHICLE_FACE_FIRE_PROOF_INTERVAL_SECONDS then
            log(string.format(
                "vehicle face-fire A held -> wButtons=0x%04X (synthetic LB)",
                buttons
            ))
            vehicleFaceFireLastProofAt = now
        end
        state.Gamepad.wButtons = buttons
        vehicleLeftFireLogged = false
        vehicleRightFireLogged = false
        return buttons, true
    end

    if vehicleFaceFireSyntheticLeftShoulder then
        buttons = clear_button(buttons, LEFT_SHOULDER)
        vehicleFaceFireSyntheticLeftShoulder = false
        vehicleFaceFireLastProofAt = -math.huge
        if wasFaceHeld then
            log(string.format(
                "vehicle face-fire A released -> wButtons=0x%04X (physical LB suppressed=%s)",
                buttons,
                physicalLeftShoulderHeld and "true" or "false"
            ))
        end
    end

    return buttons, false
end

local function apply_r3_left_stick_dpad(state, buttons, dualGripHeld)
    -- A dual weapon grip must never consume the left stick: it remains the
    -- native walking/steering axis even if the optional R3 remap is enabled.
    if dualGripHeld or isPlayerDriving or not enableR3LeftStickDpad
        or not has_button(buttons, RIGHT_THUMB) then
        return buttons
    end

    local x = state.Gamepad.sThumbLX or 0
    local y = state.Gamepad.sThumbLY or 0
    buttons = clear_button(buttons, RIGHT_THUMB)
    state.Gamepad.sThumbLX = 0
    state.Gamepad.sThumbLY = 0

    if math.abs(x) >= DPAD_STICK_THRESHOLD or math.abs(y) >= DPAD_STICK_THRESHOLD then
        if math.abs(x) >= math.abs(y) then
            buttons = set_button(buttons, x < 0 and DPAD_LEFT or DPAD_RIGHT)
        else
            buttons = set_button(buttons, y < 0 and DPAD_DOWN or DPAD_UP)
        end
    end

    state.Gamepad.wButtons = buttons
    return buttons
end

local function is_left_thumb_rest_touch_active()
    if not (vr and vr.get_action_handle and vr.get_left_joystick_source and vr.is_action_active) then
        if debugInputLayerProbe and leftThumbRestProbeState ~= "api-unavailable" then
            log("thumb-rest probe unavailable: required Lua API missing")
            leftThumbRestProbeState = "api-unavailable"
        end
        return false
    end

    local now = os.clock()
    if leftThumbRestTouchAction == nil or leftThumbRestTouchAction == 0
        or leftThumbRestTouchSource == nil or leftThumbRestTouchSource == 0 then
        if now - leftThumbRestLookupLastAttemptAt < LEFT_THUMB_REST_LOOKUP_RETRY_SECONDS then
            return false
        end
        leftThumbRestLookupLastAttemptAt = now
        local sourceOk, source = pcall(function()
            return vr.get_left_joystick_source()
        end)
        if not sourceOk or source == nil or source == 0 then
            if debugInputLayerProbe and leftThumbRestProbeState ~= "source-unavailable" then
                log("thumb-rest probe unavailable: left joystick source missing")
                leftThumbRestProbeState = "source-unavailable"
            end
            return false
        end

        local action = nil
        local resolvedPath = nil
        for _, actionPath in ipairs(LEFT_THUMB_REST_ACTION_PATHS) do
            local actionOk, candidate = pcall(function()
                return vr.get_action_handle(actionPath)
            end)
            if actionOk and candidate ~= nil and candidate ~= 0 then
                action = candidate
                resolvedPath = actionPath
                break
            end
        end
        if action == nil then
            if debugInputLayerProbe and leftThumbRestProbeState ~= "action-unavailable" then
                log("thumb-rest probe unavailable: action handle missing")
                leftThumbRestProbeState = "action-unavailable"
            end
            return false
        end

        leftThumbRestTouchAction = action
        leftThumbRestTouchSource = source
        leftThumbRestTouchResolvedPath = resolvedPath
        if debugInputLayerProbe and leftThumbRestProbeState ~= "bound" then
            log("thumb-rest probe bound: " .. tostring(resolvedPath))
            leftThumbRestProbeState = "bound"
        end
    end

    local activeOk, active = pcall(function()
        return vr.is_action_active(leftThumbRestTouchAction, leftThumbRestTouchSource)
    end)
    if not activeOk then
        if debugInputLayerProbe and leftThumbRestProbeState ~= "query-error" then
            log("thumb-rest probe query failed")
            leftThumbRestProbeState = "query-error"
        end
        -- Handles can be created before the action/source is fully registered.
        -- Drop both so the throttled lookup can recover instead of remaining
        -- permanently bound to a stale pair.
        leftThumbRestTouchAction = nil
        leftThumbRestTouchSource = nil
        leftThumbRestTouchResolvedPath = nil
        return false
    end

    -- UEVR's boolean action wrapper is normally a Lua boolean, but accept the
    -- numeric true form used by some older Lua bindings without treating 0 as
    -- active.
    local activeBool = active == true or active == 1
    if debugInputLayerProbe then
        local state = activeBool and "active" or "inactive"
        if leftThumbRestProbeState ~= state then
            log("thumb-rest probe " .. state)
            leftThumbRestProbeState = state
        end
    end
    return activeBool
end

local function apply_thumb_rest_right_stick_dpad(state, buttons)
    -- Never consume the right stick in aircraft; the native flight layer owns
    -- both sticks regardless of the optional gameplay remap.
    if not enableDualGripDpad or isPlayerAircraft then
        lastDualGripDpadDirection = nil
        return buttons
    end

    local modifierActive = is_left_thumb_rest_touch_active()
    if not modifierActive then
        -- Releasing the touch modifier re-arms the next right-stick deflection.
        lastDualGripDpadDirection = nil
        return buttons
    end

    -- Only the right/look stick is suppressed by the modifier. The left stick
    -- is deliberately untouched so walking and vehicle steering remain native.
    local x = state.Gamepad.sThumbRX or 0
    local y = state.Gamepad.sThumbRY or 0
    state.Gamepad.sThumbRX = 0
    state.Gamepad.sThumbRY = 0
    -- The modifier owns the D-pad while active. Clear physical D-pad bits first
    -- so the synthetic event below is a single clean pulse, not a latched mix.
    buttons = clear_button(buttons, DPAD_UP)
    buttons = clear_button(buttons, DPAD_DOWN)
    buttons = clear_button(buttons, DPAD_LEFT)
    buttons = clear_button(buttons, DPAD_RIGHT)

    local direction = nil
    if math.abs(x) >= DPAD_STICK_THRESHOLD or math.abs(y) >= DPAD_STICK_THRESHOLD then
        if math.abs(x) >= math.abs(y) then
            direction = x < 0 and "left" or "right"
        else
            direction = y < 0 and "down" or "up"
        end
    end

    if direction == nil then
        -- Use hysteresis: a deflection must return close to center before the
        -- next pulse is armed. This prevents noisy values near the edge threshold
        -- from generating repeated weapon/radio changes.
        if math.max(math.abs(x), math.abs(y)) <= DPAD_STICK_RELEASE_THRESHOLD then
            lastDualGripDpadDirection = nil
        end
        state.Gamepad.wButtons = buttons
        return buttons
    end

    -- Emit one native D-pad pulse per deflection. Holding the stick does not
    -- repeat or leave a D-pad button latched until it is re-centered/released.
    if lastDualGripDpadDirection == nil then
        if direction == "left" then
            buttons = set_button(buttons, DPAD_LEFT)
        elseif direction == "right" then
            buttons = set_button(buttons, DPAD_RIGHT)
        elseif direction == "down" then
            buttons = set_button(buttons, DPAD_DOWN)
        else
            buttons = set_button(buttons, DPAD_UP)
        end
        -- This is one event per deflection; keep diagnostics behind the
        -- existing input-probe flag so normal play stays quiet.
        log_control_debug("thumb-rest/right-stick D-pad -> " .. direction)
        lastDualGripDpadDirection = direction
    end

    state.Gamepad.wButtons = buttons
    return buttons
end

local function log_chord(message, now)
    if now - lastChordLogAt < 0.2 then
        return
    end

    lastChordLogAt = now
    log(message)
end

local function log_input_layer(buttons, state)
    if not debugInputLayerProbe then
        return
    end

    -- UEVR's default Quest mapping is hand-based, not an Xbox-shaped face diamond.
    local pressed = {}
    if has_button(buttons, BUTTON_A) then table.insert(pressed, "Right A -> Xbox A") end
    if has_button(buttons, BUTTON_X) then table.insert(pressed, "Right B -> Xbox X") end
    if has_button(buttons, BUTTON_B) then table.insert(pressed, "Left X -> Xbox B") end
    if has_button(buttons, BUTTON_Y) then table.insert(pressed, "Left Y -> Xbox Y") end
    if has_button(buttons, LEFT_SHOULDER) then table.insert(pressed, "Left grip -> LB") end
    if has_button(buttons, RIGHT_SHOULDER) then table.insert(pressed, "Right grip -> RB") end
    if has_button(buttons, LEFT_THUMB) then table.insert(pressed, "Left stick press -> L3") end
    if has_button(buttons, RIGHT_THUMB) then table.insert(pressed, "Right stick press -> R3") end
    if has_button(buttons, BUTTON_START) then table.insert(pressed, "Start") end
    if has_button(buttons, BUTTON_BACK) then table.insert(pressed, "Back") end
    if state.Gamepad.bLeftTrigger > 30 then table.insert(pressed, "Left trigger -> LT") end
    if state.Gamepad.bRightTrigger > 30 then table.insert(pressed, "Right trigger -> RT") end

    local signature = table.concat(pressed, " + ")
    if signature == lastInputProbeSignature then
        return
    end

    lastInputProbeSignature = signature
    log("input before mod scripts: " .. (signature == "" and "released" or signature))
end

local function apply_chord_controls(state, buttons, now)
    local handled = false
    local guideCanOpen = playerInputEnabled and not isPlayerDriving and not isPlayerAircraft
    local abHeld = enableChordHudToggle and (controlGuideVisible or guideCanOpen)
        and has_button(buttons, BUTTON_A) and has_button(buttons, BUTTON_B)
    local xyHeld = enableChordPauseMenu and has_button(buttons, BUTTON_X) and has_button(buttons, BUTTON_Y)

    -- In a vehicle, Left Y exits the car. Capture the first pause-chord button for
    -- a brief interval so an almost-simultaneous X+Y cannot leak an exit command.
    if enableChordPauseMenu and isPlayerDriving and playerInputEnabled then
        local pauseButtonHeld = has_button(buttons, BUTTON_X) or has_button(buttons, BUTTON_Y)
        if not pauseButtonHeld then
            chordPauseIgnoreUntilRelease = false
        end

        if chordPauseReplayUntil ~= nil then
            if now <= chordPauseReplayUntil then
                buttons = set_button(buttons, chordPauseCapturedButton)
                state.Gamepad.wButtons = buttons
                return true
            end
            chordPauseReplayUntil = nil
        end

        if chordPauseCaptureUntil ~= nil and not xyHeld then
            if not pauseButtonHeld then
                chordPauseCaptureUntil = nil
                chordPauseCapturedButton = nil
                chordPauseCaptureStartedAt = nil
                return false
            end

            if now <= chordPauseCaptureUntil then
                buttons = clear_button(buttons, chordPauseCapturedButton)
                state.Gamepad.wButtons = buttons
                return true
            end

            -- It was a genuine single press: replay it after the capture window.
            chordPauseReplayUntil = now + VEHICLE_SINGLE_BUTTON_REPLAY_SECONDS
            chordPauseCaptureUntil = nil
            chordPauseIgnoreUntilRelease = true
            buttons = set_button(buttons, chordPauseCapturedButton)
            log_control_debug(string.format(
                "vehicle pause chord single-button replay after %.0fms",
                ((now - (chordPauseCaptureStartedAt or now)) * 1000.0)
            ))
            chordPauseCaptureStartedAt = nil
            state.Gamepad.wButtons = buttons
            return true
        end

        if not xyHeld and not chordPauseIgnoreUntilRelease and chordPauseCaptureUntil == nil then
            if has_button(buttons, BUTTON_Y) then
                chordPauseCapturedButton = BUTTON_Y
                chordPauseCaptureStartedAt = now
                chordPauseCaptureUntil = now + VEHICLE_PAUSE_CHORD_WINDOW_SECONDS
                buttons = clear_button(buttons, BUTTON_Y)
                state.Gamepad.wButtons = buttons
                return true
            elseif has_button(buttons, BUTTON_X) then
                chordPauseCapturedButton = BUTTON_X
                chordPauseCaptureStartedAt = now
                chordPauseCaptureUntil = now + VEHICLE_PAUSE_CHORD_WINDOW_SECONDS
                buttons = clear_button(buttons, BUTTON_X)
                state.Gamepad.wButtons = buttons
                return true
            end
        end
    end

    if abHeld then
        buttons = clear_button(buttons, BUTTON_A)
        buttons = clear_button(buttons, BUTTON_B)
        if not chordHudHeldLastFrame then
            controlGuideVisible = not controlGuideVisible
            controlGuideInputArmed = false
            controlGuideStickLatched = false
            controlGuideToggleHeld = true
            dispatch_control_guide(controlGuideVisible)
            log_chord(controlGuideVisible and "A+X control guide opened" or "A+X control guide closed", now)
        end
        handled = true
    end
    chordHudHeldLastFrame = abHeld

    if controlGuideVisible then
        -- The guide owns a fixed controller-operated options panel. Require a
        -- neutral frame after opening so the A in A+X cannot also toggle an option.
        local guideToggleHeld = has_button(buttons, BUTTON_A)
        local guideStickY = state.Gamepad.sThumbLY or 0
        local guideNeutral = not guideToggleHeld and math.abs(guideStickY) < DPAD_STICK_RELEASE_THRESHOLD
        if not controlGuideInputArmed then
            if guideNeutral then
                controlGuideInputArmed = true
                controlGuideToggleHeld = false
            end
        else
            if math.abs(guideStickY) < DPAD_STICK_RELEASE_THRESHOLD then
                controlGuideStickLatched = false
            elseif not controlGuideStickLatched then
                dispatch_control_guide_navigation(guideStickY > 0 and "up" or "down")
                controlGuideStickLatched = true
            end
            if guideToggleHeld and not controlGuideToggleHeld and not abHeld then
                dispatch_control_guide_navigation("toggle")
            end
            controlGuideToggleHeld = guideToggleHeld
        end

        -- Keep gameplay input inert while the guide is open. A+X above remains
        -- the close gesture; left-stick navigation and A toggles were consumed.
        state.Gamepad.wButtons = 0
        state.Gamepad.bLeftTrigger = 0
        state.Gamepad.bRightTrigger = 0
        state.Gamepad.sThumbLX = 0
        state.Gamepad.sThumbLY = 0
        state.Gamepad.sThumbRX = 0
        state.Gamepad.sThumbRY = 0
        return true
    end

    if xyHeld then
        if chordPauseCaptureStartedAt ~= nil then
            log_control_debug(string.format(
                "vehicle pause chord recognized after %.0fms",
                ((now - chordPauseCaptureStartedAt) * 1000.0)
            ))
        end
        chordPauseCaptureUntil = nil
        chordPauseCaptureStartedAt = nil
        chordPauseReplayUntil = nil
        chordPauseIgnoreUntilRelease = true
        buttons = clear_button(buttons, BUTTON_X)
        buttons = clear_button(buttons, BUTTON_Y)
        if not chordPauseHeldLastFrame then
            dispatch_pause_ui_reveal()
            chordPausePulseUntil = now + 0.16
            log_chord("X+Y pause chord", now)
        end
        handled = true
    end
    chordPauseHeldLastFrame = xyHeld

    if chordPausePulseUntil ~= nil then
        if now <= chordPausePulseUntil then
            buttons = set_button(buttons, BUTTON_START)
            handled = true
        else
            chordPausePulseUntil = nil
        end
    end

    if handled then
        state.Gamepad.wButtons = buttons
        return true
    end

    return false
end

local function is_utility_weapon()
    return currentWeaponId == 41 or currentWeaponId == 42
end

local function schedule_cycle_pulse(now)
    leftGripCycleUsesMouseWheel = enableUtilityWeaponCycleReset and is_utility_weapon()
    leftGripCycleMouseWheelSent = false
    leftGripCycleShoulderLogged = false
    if debugInputLayerProbe then
        cycleProbe = {
            start = now,
            startWeapon = currentWeaponId,
            usesMouseWheel = leftGripCycleUsesMouseWheel,
            check1Done = false,
            check2Done = false
        }
    else
        cycleProbe = nil
    end
    if leftGripCycleUsesMouseWheel then
        leftGripCyclePulseStart = now + UTILITY_CYCLE_RESET_DELAY_SECONDS
        leftGripCyclePulseUntil = leftGripCyclePulseStart + UTILITY_CYCLE_RESET_SECONDS
    else
        leftGripCyclePulseStart = now + CYCLE_PULSE_DELAY_SECONDS
        leftGripCyclePulseUntil = leftGripCyclePulseStart + CYCLE_PULSE_SECONDS
    end
    log_control_debug(string.format(
        "cycle scheduled weapon=%d utility=%s utilityReset=%s pulse=%s",
        currentWeaponId,
        tostring(is_utility_weapon()),
        tostring(enableUtilityWeaponCycleReset),
        leftGripCycleUsesMouseWheel and "mouse-wheel" or "shoulder"
    ))
end

local function log_cycle_release(now, heldSeconds, scheduled)
    if not debugInputLayerProbe then
        return
    end

    if now - lastCycleReleaseLogAt < 0.25 then
        return
    end

    lastCycleReleaseLogAt = now
    if scheduled then
        log(string.format("cycle release tap held=%.3fs weapon=%d", heldSeconds, currentWeaponId))
    else
        log(string.format("cycle release held=%.3fs weapon=%d", heldSeconds, currentWeaponId))
    end
end

clear_cycle_pulse = function()
    leftGripCyclePulseStart = nil
    leftGripCyclePulseUntil = nil
    leftGripCycleUsesMouseWheel = false
    leftGripCycleMouseWheelSent = false
    leftGripCycleShoulderLogged = false
end

local function apply_cycle_pulse(state, buttons, now)
    if leftGripCyclePulseUntil == nil then
        return false
    end

    if now > leftGripCyclePulseUntil then
        clear_cycle_pulse()
        return false
    end

    buttons = clear_button(buttons, LEFT_SHOULDER)
    buttons = clear_button(buttons, RIGHT_SHOULDER)
    state.Gamepad.bLeftTrigger = 0
    state.Gamepad.bRightTrigger = 0

    if leftGripCycleUsesMouseWheel then
        if leftGripCyclePulseStart ~= nil and now >= leftGripCyclePulseStart and not leftGripCycleMouseWheelSent then
            log_control_debug(string.format("cycle dispatch mouse-wheel weapon=%d", currentWeaponId))
            dispatch_cycle_weapon()
            leftGripCycleMouseWheelSent = true
        end
    elseif leftGripCyclePulseStart ~= nil and now >= leftGripCyclePulseStart then
        if not leftGripCycleShoulderLogged then
            log_control_debug(string.format("cycle dispatch shoulder weapon=%d", currentWeaponId))
            leftGripCycleShoulderLogged = true
        end
        buttons = set_button(buttons, CYCLE_BUTTON)
    end

    state.Gamepad.wButtons = buttons
    return true
end

local function clear_ab_cycle_pulse()
    abCyclePulseButton = nil
    abCyclePulseUntil = nil
end

local function apply_ab_weapon_cycle_test(state, buttons, now)
    if not enableABWeaponCycleTest or isPlayerDriving then
        clear_ab_cycle_pulse()
        abCycleLastA = false
        abCycleLastB = false
        return false
    end

    local aHeld = has_button(buttons, BUTTON_A)
    local bHeld = has_button(buttons, BUTTON_B)
    local aPressed = aHeld and not abCycleLastA
    local bPressed = bHeld and not abCycleLastB
    abCycleLastA = aHeld
    abCycleLastB = bHeld

    if aPressed then
        abCyclePulseButton = RIGHT_SHOULDER
        abCyclePulseUntil = now + CYCLE_PULSE_SECONDS
        if now - lastABCycleLogAt > 0.2 then
            log_control_debug("A/B cycle test: A -> right shoulder")
            lastABCycleLogAt = now
        end
    elseif bPressed then
        abCyclePulseButton = LEFT_SHOULDER
        abCyclePulseUntil = now + CYCLE_PULSE_SECONDS
        if now - lastABCycleLogAt > 0.2 then
            log_control_debug("A/B cycle test: B -> left shoulder")
            lastABCycleLogAt = now
        end
    end

    if not aHeld and not bHeld and abCyclePulseUntil ~= nil and now > abCyclePulseUntil then
        clear_ab_cycle_pulse()
    end

    if aHeld or bHeld or abCyclePulseButton ~= nil then
        buttons = clear_button(buttons, BUTTON_A)
        buttons = clear_button(buttons, BUTTON_B)
        buttons = clear_button(buttons, LEFT_SHOULDER)
        buttons = clear_button(buttons, RIGHT_SHOULDER)

        if abCyclePulseButton ~= nil and now <= abCyclePulseUntil then
            buttons = set_button(buttons, abCyclePulseButton)
        end

        state.Gamepad.wButtons = buttons
        return true
    end

    return false
end

local function set_hand_side(side)
    if activeHandSide == side then
        return
    end

    activeHandSide = side
    if uevr and uevr.api and uevr.api.dispatch_custom_event then
        uevr.api:dispatch_custom_event("DUALGRIP.HandSide", side)
    elseif uevr and uevr.params and uevr.params.functions and uevr.params.functions.dispatch_custom_event then
        uevr.params.functions.dispatch_custom_event("DUALGRIP.HandSide", side)
    end
end

local function set_visibility_state(state)
    local lowerPriorityDuringGrace = state == "idle"
        or state == "empty_idle"
        or state == "weapon_preview"
    local graceOwnsPresentation = heldVisualUntil ~= nil
        and os.clock() <= heldVisualUntil
        and playerInputEnabled
        and not isPlayerDriving

    if lowerPriorityDuringGrace and graceOwnsPresentation then
        if heldVisualGraceSuppressedState ~= state then
            heldVisualGraceSuppressedState = state
            log("held visual grace suppressed lower-priority state=" .. state)
        end
        return
    end

    if activeVisibilityState == state then
        return
    end

    heldVisualGraceSuppressedState = nil
    activeVisibilityState = state
    if uevr and uevr.api and uevr.api.dispatch_custom_event then
        uevr.api:dispatch_custom_event("DUALGRIP.VisibilityState", state)
    elseif uevr and uevr.params and uevr.params.functions and uevr.params.functions.dispatch_custom_event then
        uevr.params.functions.dispatch_custom_event("DUALGRIP.VisibilityState", state)
    end
end

local function cancel_held_visual_grace()
    heldVisualUntil = nil
    heldVisualGraceActive = false
    heldVisualGraceSuppressedState = nil
end

local function has_grace_interrupt_action(buttons, state)
    local digitalAction = has_button(buttons, BUTTON_START)
        or has_button(buttons, BUTTON_BACK)
        or has_button(buttons, LEFT_THUMB)
        or has_button(buttons, RIGHT_THUMB)
        or has_button(buttons, BUTTON_A)
        or has_button(buttons, BUTTON_B)
        or has_button(buttons, BUTTON_X)
        or has_button(buttons, BUTTON_Y)
        or has_button(buttons, DPAD_UP)
        or has_button(buttons, DPAD_DOWN)
        or has_button(buttons, DPAD_LEFT)
        or has_button(buttons, DPAD_RIGHT)
    local triggerAction = state.Gamepad.bLeftTrigger > 30 or state.Gamepad.bRightTrigger > 30
    -- Locomotion is compatible with the visual grace presentation. Only the
    -- right/look stick remains an interrupt; walking must not collapse the gun
    -- back to its native idle attachment before the grace timer expires.
    local lookAction = math.abs(state.Gamepad.sThumbRX) > GRACE_ACTION_STICK_THRESHOLD
        or math.abs(state.Gamepad.sThumbRY) > GRACE_ACTION_STICK_THRESHOLD
    return digitalAction or triggerAction or lookAction
end

local function set_idle_visibility_state(now)
    if manualReloadWeaponEmpty then
        set_visibility_state("empty_idle")
    elseif weaponPreviewUntil ~= nil and now <= weaponPreviewUntil then
        set_visibility_state("weapon_preview")
    else
        weaponPreviewUntil = nil
        set_visibility_state("idle")
    end
end

local function reset_dualgrip_state()
    set_hand_side("right")
    set_visibility_state("default")
    leftGripPressedAt = nil
    leftGripHeldLongEnough = false
    leftGripCycleFireSeen = false
    cancel_held_visual_grace()
    clear_cycle_pulse()
end

local function reset_phone_answer_state()
    rightGripPhonePressedAt = nil
    previousRightGripPhoneHeld = false
    phoneAnswerPulseUntil = nil
end

local function update_phone_answer_tap(rightGripHeld, now)
    if not enablePhoneAnswerGripTap or not phoneRinging or isPlayerDriving then
        reset_phone_answer_state()
        return
    end

    if rightGripHeld and not previousRightGripPhoneHeld then
        rightGripPhonePressedAt = now
    elseif rightGripHeld and rightGripPhonePressedAt ~= nil
        and now - rightGripPhonePressedAt > PHONE_ANSWER_TAP_SECONDS then
        rightGripPhonePressedAt = nil
    elseif not rightGripHeld and previousRightGripPhoneHeld then
        if rightGripPhonePressedAt ~= nil
            and now - rightGripPhonePressedAt <= PHONE_ANSWER_TAP_SECONDS then
            phoneAnswerPulseUntil = now + PHONE_ANSWER_PULSE_SECONDS
            log("phone answer tap -> right shoulder")
        end
        rightGripPhonePressedAt = nil
    end

    previousRightGripPhoneHeld = rightGripHeld
end

local function apply_phone_answer_pulse(state, buttons, now)
    if phoneAnswerPulseUntil == nil then
        return false
    end

    if now > phoneAnswerPulseUntil then
        phoneAnswerPulseUntil = nil
        return false
    end

    state.Gamepad.wButtons = set_button(buttons, RIGHT_SHOULDER)
    return true
end

local function reset_manual_reload_tap_state(clearAimPulse)
    rightGripReloadPressedAt = nil
    previousRightGripReloadHeld = false
    rightGripReloadFireSeen = false
    rightGripReloadTwoHandSeen = false
    if clearAimPulse then
        manualReloadAimPulseUntil = nil
    end
end

local function native_special_weapon_active()
	-- Sniper retains GTA's native ready/scope state. Free-fire launchers use the
	-- ordinary controller-held presentation and only request native aim while a
	-- shot is actually being fired, so merely gripping one cannot lock movement.
	return currentWeaponId == 34
		or (currentWeaponId == 0 and nativeSpecialWeaponFallbackArmed)
end

local function free_fire_rocket_active()
	return currentWeaponId == 35 or currentWeaponId == 36
end

local function held_visual_grace_seconds()
    local ak47Active = currentWeaponId == 30
        or (currentWeaponId == 0 and manualReloadEmptyFallbackArmed and lastManualReloadWeaponId == 30)
    return ak47Active and AK47_HELD_VISUAL_GRACE_SECONDS or HELD_VISUAL_GRACE_SECONDS
end

local function update_manual_reload_tap(rightGripHeld, leftGripHeld, fireHeld, now)
    local currentWeaponSupported = currentWeaponId >= 22 and currentWeaponId <= 33
    local supportedWeapon = currentWeaponSupported
        or (currentWeaponId == 0 and manualReloadEmptyFallbackArmed and lastManualReloadWeaponId >= 22 and lastManualReloadWeaponId <= 33)
    local allowed = enableManualReloadMode
        and playerInputEnabled
        and playerState == "OnFoot"
        and not phoneRinging
        and supportedWeapon

    if not allowed then
        reset_manual_reload_tap_state(false)
        return
    end

    if rightGripHeld and not previousRightGripReloadHeld then
        rightGripReloadPressedAt = now
        rightGripReloadFireSeen = fireHeld
        rightGripReloadTwoHandSeen = leftGripHeld
    elseif rightGripHeld then
        rightGripReloadFireSeen = rightGripReloadFireSeen or fireHeld
        rightGripReloadTwoHandSeen = rightGripReloadTwoHandSeen or leftGripHeld
        if rightGripReloadPressedAt ~= nil
            and now - rightGripReloadPressedAt > MANUAL_RELOAD_TAP_SECONDS then
            rightGripReloadPressedAt = nil
        end
    elseif previousRightGripReloadHeld then
        if rightGripReloadPressedAt ~= nil
            and not rightGripReloadFireSeen
            and not rightGripReloadTwoHandSeen
            and now - rightGripReloadPressedAt <= MANUAL_RELOAD_TAP_SECONDS then
            -- Reload takes priority over visual grace. A sustained synthetic
            -- aim input prevents GTA's native reload state from starting.
            cancel_held_visual_grace()
            dispatch_manual_reload()
            manualReloadAimPulseUntil = now + MANUAL_RELOAD_AIM_PULSE_SECONDS
            log("manual reload tap -> current weapon")
        elseif rightGripReloadPressedAt ~= nil and rightGripReloadTwoHandSeen then
            log_control_debug("manual reload tap suppressed: two-hand grip transition")
        end
        rightGripReloadPressedAt = nil
        rightGripReloadFireSeen = false
        rightGripReloadTwoHandSeen = false
    end

    previousRightGripReloadHeld = rightGripHeld
end

local function manual_reload_aim_pulse_active(now)
    if manualReloadAimPulseUntil == nil then
        return false
    end
    if now > manualReloadAimPulseUntil then
        manualReloadAimPulseUntil = nil
        return false
    end
    return true
end

uevr.sdk.callbacks.on_xinput_get_state(function(retval, user_index, state)
    local buttons = state.Gamepad.wButtons
    log_input_layer(buttons, state)
    local now = os.clock()
    local triggerTimingLeftTriggerHeld = state.Gamepad.bLeftTrigger > 30
    local triggerTimingRightTriggerHeld = state.Gamepad.bRightTrigger > 30
    local triggerTimingFireHeld = triggerTimingLeftTriggerHeld or triggerTimingRightTriggerHeld
	local meleeClenchTiming = authoritativeMeleeTriggerBlock
		or (currentWeaponId >= 0 and currentWeaponId <= 15)
	local firingTriggerEdge = not meleeClenchTiming
		and triggerTimingFireHeld and not previousFireInputHeld
	local firingTriggerRelease = not meleeClenchTiming
		and not triggerTimingFireHeld and previousFireInputHeld

    if firingTriggerEdge then
        triggerTimingSequence = triggerTimingSequence + 1
        local side = triggerTimingLeftTriggerHeld and triggerTimingRightTriggerHeld and "both"
            or (triggerTimingLeftTriggerHeld and "left" or "right")
        local aimRequested = has_button(buttons, LEFT_SHOULDER) or has_button(buttons, RIGHT_SHOULDER)
        local eligible = playerInputEnabled and not isPlayerDriving and not isPlayerAircraft
        dispatch_trigger_timing("edge", triggerTimingSequence, side, currentWeaponId, aimRequested, eligible, now)
        if debugInputLayerProbe then
            log(string.format(
                "trigger timing edge seq=%d side=%s weapon=%d aim=%s eligible=%s",
                triggerTimingSequence,
                side,
                tonumber(currentWeaponId) or 0,
                aimRequested and "true" or "false",
                eligible and "true" or "false"))
        end
    elseif firingTriggerRelease then
        dispatch_trigger_timing("release", triggerTimingSequence, "released", currentWeaponId, false, false, now)
    end
	-- Melee triggers are independent clench inputs, never firearm timing edges.
	-- C++ independently validates the authoritative engine weapon state before
	-- clearing native triggers, so a stale Lua weapon event cannot leak a punch.
	previousFireInputHeld = meleeClenchTiming and false or triggerTimingFireHeld

	if apply_chord_controls(state, buttons, now) then
        previousQuestXCycleHeld = has_button(buttons, BUTTON_B)
        reset_manual_reload_tap_state(true)
		return
	end

    -- Pause/front-end screens must receive GTA's native XInput layout. Vehicle
    -- fire, grip suppression, and D-pad modulation are gameplay-only layers.
	if not playerInputEnabled then
        previousQuestXCycleHeld = has_button(buttons, BUTTON_B)
        reset_manual_reload_tap_state(true)
		if previousVehicleFaceFireHeld then
			dispatch_vehicle_face_fire(false)
			previousVehicleFaceFireHeld = false
		end
		return
	end

	buttons = apply_short_press_camera_switch(state, buttons, now)

    local rawLeftGripHeld = has_button(buttons, LEFT_SHOULDER)
    local rawRightGripHeld = has_button(buttons, RIGHT_SHOULDER)
    local rawFireHeld = triggerTimingFireHeld
    update_manual_reload_tap(rawRightGripHeld, rawLeftGripHeld, rawFireHeld, now)

    local questXCycleConsumed
    buttons, questXCycleConsumed = apply_quest_x_weapon_cycle(state, buttons, now)
    if questXCycleConsumed then
		-- apply_quest_x_weapon_cycle already removed the physical shoulders and
		-- retained only its bounded synthetic LB pulse. Return before the ordinary
		-- grip layer can clear that pulse again.
        return
    end

	buttons = apply_thumb_rest_right_stick_dpad(state, buttons)

    local vehicleFaceFireConsumed
    buttons, vehicleFaceFireConsumed = apply_vehicle_face_button_fire(state, buttons, now)
    if vehicleFaceFireConsumed then
        if enableAlternateWeaponHandsVisibility then
            set_visibility_state("vehicle_firing")
        else
            set_visibility_state("default")
        end
        -- Accepted A->LB is final for this sample; do not clear it below.
        return
    end

    if apply_ab_weapon_cycle_test(state, buttons, now) then
        return
    end

    buttons = apply_r3_left_stick_dpad(state, buttons, rawLeftGripHeld and rawRightGripHeld)

    -- Universal pause/HUD/camera controls and the optional thumb-rest D-pad
    -- pulse run above. Ordinary aircraft inputs remain native from here on.
    if enableAircraftNativeControls and isPlayerAircraft then
        if enableAlternateWeaponHandsVisibility then
            set_idle_visibility_state(now)
        else
            set_visibility_state("default")
        end
        return
    end

    local phoneRightGripHeld = has_button(buttons, RIGHT_SHOULDER)
    update_phone_answer_tap(phoneRightGripHeld, now)
    if apply_phone_answer_pulse(state, buttons, now) then
        return
    end

    if not enableDualGripAimFire then
        reset_dualgrip_state()
        return
    end

    if isPlayerDriving then
        -- A fire returned above. Physical left grip is not a native drive-by
        -- request, while RT remains untouched as accelerator.
        buttons = clear_button(buttons, LEFT_SHOULDER)
        state.Gamepad.wButtons = buttons
        if enableAlternateWeaponHandsVisibility then
            set_idle_visibility_state(now)
        else
            set_visibility_state("default")
        end
        return
    end

	-- Grenades, tear gas, and Molotovs retain GTA's complete native throw state.
	-- Their release/cook animation is not compatible with the firearm magnetic
	-- ownership path, so do not consume grip/trigger input or request fake-hand
	-- presentation for only half of that interaction.
	if currentWeaponId >= 16 and currentWeaponId <= 18 then
		cancel_held_visual_grace()
		clear_cycle_pulse()
		set_visibility_state("default")
		return
	end

    local leftGripHeld = has_button(buttons, LEFT_SHOULDER)
    local rightGripHeld = phoneRightGripHeld
	local leftTriggerHeld = state.Gamepad.bLeftTrigger > 30
	local rightTriggerHeld = state.Gamepad.bRightTrigger > 30
	local fireTriggerPulse = triggerTimingFireHeld
	local leftGripAimHeld = leftGripHeld
	local manualReloadAimHeld = manual_reload_aim_pulse_active(now)
	local nativeSpecialWeapon = native_special_weapon_active()
	local freeFireRocket = free_fire_rocket_active()
	if nativeSpecialWeapon and heldVisualUntil ~= nil then
		cancel_held_visual_grace()
	end
	if heldVisualUntil ~= nil and has_grace_interrupt_action(buttons, state) then
		if heldVisualGraceActive then
			log("held visual grace cancelled by action input")
		end
		cancel_held_visual_grace()
	end
	-- Only a held grip arms the post-release visual grace. Trigger pulses are
	-- action inputs and must not restart grace on every shot.
	local heldVisualInputActive = not nativeSpecialWeapon
		and (leftGripHeld or rightGripHeld)
		if heldVisualInputActive then
			heldVisualGraceDuration = held_visual_grace_seconds()
			heldVisualUntil = now + heldVisualGraceDuration
			heldVisualGraceActive = false
			heldVisualGraceSuppressedState = nil
		end
	if leftGripHeld and (leftTriggerHeld or rightTriggerHeld) then
		leftGripCycleFireSeen = true
	end

	if not leftGripHeld and not rightGripHeld and not leftTriggerHeld and not rightTriggerHeld
        and not manualReloadAimHeld then
		leftGripPressedAt = nil
		leftGripHeldLongEnough = false
		leftGripCycleFireSeen = false
		clear_cycle_pulse()

		if enableAlternateWeaponHandsVisibility then
			if heldVisualUntil ~= nil and now <= heldVisualUntil then
				if not heldVisualGraceActive then
					heldVisualGraceActive = true
					log(string.format("held visual grace started (%.1fs)", heldVisualGraceDuration))
				end
				set_visibility_state("active_grace")
			else
				if heldVisualGraceActive then
					log("held visual grace expired -> idle")
				end
				heldVisualUntil = nil
				heldVisualGraceActive = false
				heldVisualGraceSuppressedState = nil
				set_idle_visibility_state(now)
			end
        else
            set_visibility_state("default")
        end
        return
    end

	if leftGripHeld then
		if leftGripPressedAt == nil then
			leftGripPressedAt = now
			leftGripHeldLongEnough = false
			-- Cancel any legacy pulse retained across a config/script transition.
			clear_cycle_pulse()
		end
		if now - leftGripPressedAt > TAP_CYCLE_WINDOW_SECONDS then
			leftGripHeldLongEnough = true
		end
	elseif not leftGripHeld then
		leftGripPressedAt = nil
		leftGripHeldLongEnough = false
		leftGripCycleFireSeen = false
		clear_cycle_pulse()
	end

    buttons = clear_button(buttons, LEFT_SHOULDER)
    buttons = clear_button(buttons, RIGHT_SHOULDER)

	state.Gamepad.wButtons = buttons

	-- Ordinary VR firearms use the physical weapon ray without forcing GTA's
	-- native LT aiming stance. Keep the native ready path only for special
	-- weapons and the optional manual-reload compatibility pulse.
	if manualReloadAimHeld
		or (nativeSpecialWeapon and (leftGripHeld or rightGripHeld or fireTriggerPulse))
		or (freeFireRocket and fireTriggerPulse) then
		state.Gamepad.bLeftTrigger = 255
	elseif currentWeaponId >= 22 and currentWeaponId <= 36 then
		state.Gamepad.bLeftTrigger = 0
	end

	if fireTriggerPulse and not meleeClenchTiming then
		state.Gamepad.bRightTrigger = 255
    end

	-- Physical melee contact is still proof-only. Do not let unarmed/melee grip
	-- transitions leak into GTA's native aim/attack inputs and cause stray hits.
	if meleeClenchTiming then
		state.Gamepad.bLeftTrigger = 0
		state.Gamepad.bRightTrigger = 0
	end

	-- Side selection remains responsive while grip itself is presentation-only.
    local leftGripSideHeld = leftGripHeld and leftGripPressedAt ~= nil
        and (now - leftGripPressedAt >= SIDE_SWITCH_HOLD_SECONDS)
    if leftGripSideHeld or leftTriggerHeld then
        set_hand_side("left")
	elseif rightGripHeld or rightTriggerHeld or manualReloadAimHeld then
		set_hand_side("right")
    end

    if enableAlternateWeaponHandsVisibility then
        set_visibility_state("active")
    else
        set_visibility_state("default")
    end
end)

uevr.sdk.callbacks.on_lua_event(function(event_name, event_string)
    if event_name == "playerState" then
        playerState = event_string
        isPlayerDriving = event_string ~= "OnFoot"
        isPlayerAircraft = event_string == "Helicopter" or event_string == "Plane"
        currentVehicleModelId = -1
        previousBikeRightPedalHeld = false
        vehicleFreeAimActive = false
        previousVehicleFaceFireHeld = false
        -- Reapply the post-vehicle visibility state even when the previous
        -- state was already labelled idle; the weapon mesh may have been
        -- recreated while firing from the vehicle.
        activeVisibilityState = nil
        activeHandSide = nil
        cancel_held_visual_grace()
        set_hand_side("right")
        set_idle_visibility_state(os.clock())
        chordPauseCaptureUntil = nil
        chordPauseCapturedButton = nil
        chordPauseCaptureStartedAt = nil
        chordPauseReplayUntil = nil
        chordPauseIgnoreUntilRelease = false
        leftGripPressedAt = nil
        leftGripHeldLongEnough = false
		leftGripCycleFireSeen = false
		clear_cycle_pulse()
		reset_phone_answer_state()
        reset_manual_reload_tap_state(true)
        log_control_debug(string.format(
            "player state=%s aircraft=%s nativeAircraft=%s",
            playerState,
            tostring(isPlayerAircraft),
            tostring(enableAircraftNativeControls)
        ))
    elseif event_name == "vehicleModelState" then
        currentVehicleModelId = tonumber(event_string) or -1
        previousBikeRightPedalHeld = false
        log_control_debug(string.format(
            "vehicle model state=%s",
            currentVehicleModelId >= 0 and tostring(currentVehicleModelId) or "unknown"
        ))
    elseif event_name == "vehicleFreeAimState" then
        vehicleFreeAimActive = event_string == "true"
        if not vehicleFreeAimActive and previousVehicleFaceFireHeld then
            dispatch_vehicle_face_fire(false)
            previousVehicleFaceFireHeld = false
        end
        log_control_debug(string.format("vehicle free-aim state=%s", tostring(vehicleFreeAimActive)))
    elseif event_name == "meleeNativeTriggerBlockState" then
		authoritativeMeleeTriggerBlock = event_string == "true"
		if authoritativeMeleeTriggerBlock then
			previousFireInputHeld = false
		end
    elseif event_name == "currentWeapon" then
        local nextWeaponId = tonumber(event_string) or 0
        if nextWeaponId ~= currentWeaponId then
            local previousWeaponId = currentWeaponId
			if nextWeaponId == 34 then
				nativeSpecialWeaponFallbackArmed = false
			elseif nextWeaponId == 0 and previousWeaponId == 34 then
				nativeSpecialWeaponFallbackArmed = true
			elseif nextWeaponId ~= 0 then
                nativeSpecialWeaponFallbackArmed = false
            end
            if nextWeaponId >= 22 and nextWeaponId <= 33 then
                lastManualReloadWeaponId = nextWeaponId
                manualReloadEmptyFallbackArmed = false
            elseif nextWeaponId == 0 and previousWeaponId >= 22 and previousWeaponId <= 33 then
                lastManualReloadWeaponId = previousWeaponId
                manualReloadEmptyFallbackArmed = true
            elseif nextWeaponId ~= 0 then
                manualReloadEmptyFallbackArmed = false
                manualReloadWeaponEmpty = false
            end
            currentWeaponId = nextWeaponId
            if native_special_weapon_active() then
                reset_manual_reload_tap_state(true)
                cancel_held_visual_grace()
            end
            weaponPreviewUntil = os.clock() + WEAPON_PREVIEW_SECONDS
            if enableAlternateWeaponHandsVisibility then
                set_visibility_state("weapon_preview")
            end
        end
	elseif event_name == "playerControlState" then
		playerInputEnabled = event_string == "true"
		if not playerInputEnabled then
			reset_phone_answer_state()
            reset_manual_reload_tap_state(true)
			cancel_held_visual_grace()
		end
    elseif event_name == "manualReloadEmptyState" then
        manualReloadWeaponEmpty = event_string == "true"
        if enableAlternateWeaponHandsVisibility then
            if manualReloadWeaponEmpty and activeVisibilityState ~= "active" then
                set_visibility_state("empty_idle")
            elseif not manualReloadWeaponEmpty and activeVisibilityState == "empty_idle" then
                set_idle_visibility_state(os.clock())
            end
        end
    elseif event_name == "phoneRingingState" then
        phoneRinging = event_string == "true"
		if not phoneRinging then
			reset_phone_answer_state()
		end
	elseif event_name == "featureFlagState" then
        local name, value = (event_string or ""):match("^([^=]+)=([^;]+)")
        if name == "EnableDualGripAimFire" then
            enableDualGripAimFire = parse_bool(value)
            if not enableDualGripAimFire then
                reset_dualgrip_state()
            end
        elseif name == "EnableAlternateWeaponHandsVisibility" then
            enableAlternateWeaponHandsVisibility = parse_bool(value)
            if not enableAlternateWeaponHandsVisibility then
                set_visibility_state("default")
            end
		elseif name == "EnableGripWeaponCycle" then
            enableGripWeaponCycle = parse_bool(value)
            if not enableGripWeaponCycle then
                leftGripPressedAt = nil
                leftGripHeldLongEnough = false
				leftGripCycleFireSeen = false
				clear_cycle_pulse()
			end
        elseif name == "EnableManualReloadMode" then
            enableManualReloadMode = parse_bool(value)
            if not enableManualReloadMode then
                reset_manual_reload_tap_state(true)
            end
		elseif name == "EnablePhoneAnswerGripTap" then
            enablePhoneAnswerGripTap = parse_bool(value)
            if not enablePhoneAnswerGripTap then
                reset_phone_answer_state()
            end
        elseif name == "EnableUtilityWeaponCycleReset" then
            enableUtilityWeaponCycleReset = parse_bool(value)
        elseif name == "EnableABWeaponCycleTest" then
            enableABWeaponCycleTest = parse_bool(value)
            if not enableABWeaponCycleTest then
                clear_ab_cycle_pulse()
                abCycleLastA = false
                abCycleLastB = false
            end
        elseif name == "EnableChordPauseMenu" then
            enableChordPauseMenu = parse_bool(value)
            if not enableChordPauseMenu then
                chordPauseHeldLastFrame = false
                chordPausePulseUntil = nil
                chordPauseCaptureUntil = nil
                chordPauseCapturedButton = nil
                chordPauseCaptureStartedAt = nil
                chordPauseReplayUntil = nil
                chordPauseIgnoreUntilRelease = false
            end
        elseif name == "EnableChordHudToggle" then
            enableChordHudToggle = parse_bool(value)
            if not enableChordHudToggle then
                chordHudHeldLastFrame = false
				if controlGuideVisible then
					controlGuideVisible = false
					controlGuideInputArmed = false
					controlGuideStickLatched = false
					controlGuideToggleHeld = false
					dispatch_control_guide(false)
				end
            end
        elseif name == "EnableShortPressCameraSwitch" then
            enableShortPressCameraSwitch = parse_bool(value)
            if not enableShortPressCameraSwitch then
                leftSystemPressedAt = nil
                leftSystemWasHeld = false
            end
        elseif name == "EnableVehicleFaceButtonFire" then
            enableVehicleFaceButtonFire = parse_bool(value)
        elseif name == "EnableAircraftNativeControls" then
            enableAircraftNativeControls = parse_bool(value)
            if enableAircraftNativeControls and isPlayerAircraft then
                reset_dualgrip_state()
            end
        elseif name == "EnableR3LeftStickDpad" then
            enableR3LeftStickDpad = parse_bool(value)
        elseif name == "DebugInputLayerProbe" then
            debugInputLayerProbe = parse_bool(value)
            lastInputProbeSignature = nil
        end
    end
end)

uevr.sdk.callbacks.on_frame(function()
    if cycleProbe == nil then
        return
    end

    local now = os.clock()
    local elapsed = now - cycleProbe.start

    if not cycleProbe.check1Done and elapsed >= 0.15 then
        cycleProbe.check1Done = true
        log_control_debug(string.format(
            "cycle probe +0.15s startWeapon=%d currentWeapon=%d pulse=%s",
            cycleProbe.startWeapon,
            currentWeaponId,
            cycleProbe.usesMouseWheel and "mouse-wheel" or "shoulder"
        ))
    end

    if not cycleProbe.check2Done and elapsed >= 0.45 then
        cycleProbe.check2Done = true
        log_control_debug(string.format(
            "cycle probe +0.45s startWeapon=%d currentWeapon=%d pulse=%s",
            cycleProbe.startWeapon,
            currentWeaponId,
            cycleProbe.usesMouseWheel and "mouse-wheel" or "shoulder"
        ))
        cycleProbe = nil
    end
end)
