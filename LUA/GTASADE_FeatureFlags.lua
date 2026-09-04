local panel_registered = false
local panel_attempted = false
local floating_visible = false
local quick_options_visible = false
local logged_imgui_missing = false
local aim_calibration_result = "No accepted shots yet"
local quick_movement_orientation = 0
local left_handed_mode = 0
local reset_3d_status = "Ready"

local sections = {
    {
        title = "COMBAT AND AIMING",
        flags = {
            { key = "EnableCombatAssist", label = "Combat range and accuracy hooks [Recommended]" },
            { key = "EnableAimAlignment", label = "Align shots with VR aim [Recommended]" },
            { key = "EnableWeaponNoSpread", label = "Remove player bullet spread [Recommended]" },
            { key = "EnableCombatAssistAmmo", label = "Unlimited ammo supply (restart/reinject)" },
            { key = "EnableCombatAssistDamage", label = "Double weapon damage" },
            { key = "EnableHealthRecovery", label = "Recover to 50% health after 10 seconds" },
            { key = "EnableBulletDamageResistance", label = "50% incoming bullet damage [Recommended]" },
        },
    },
    {
        title = "VR WEAPONS",
        flags = {
            { key = "EnableVrScope", label = "VR sniper scope [Recommended]" },
            { key = "EnableCompactWeaponReticle", label = "Small aiming reticle (restart/reinject)" },
            { key = "EnableBulletTraceHidden", label = "Hide bullet tracers (restart/reinject)" },
        },
    },
    {
        title = "VR CONTROLS",
        flags = {
            { key = "EnableDualGripAimFire", label = "Side-matched grip aim and trigger fire [Recommended]" },
			{ key = "EnableCustomAkimbo", label = "Independent per-hand akimbo firing [Experimental]" },
            { key = "EnableAlternateWeaponHandsVisibility", label = "Hide idle weapon and aiming hands [Recommended]" },
            { key = "EnableTwoHandStabilization", label = "Two-hand weapon stabilization" },
            { key = "EnableGripCalibration", label = "Weapon hand-placement calibration" },
            { key = "EnableGripWeaponCycle", label = "Tap left grip to cycle weapons [Recommended]" },
            { key = "EnableManualReloadMode", label = "Tap right grip to reload (restart/reinject) [Experimental]" },
            { key = "EnablePhoneAnswerGripTap", label = "Tap right grip to answer ringing phone [Recommended]" },
            { key = "EnableChordPauseMenu", label = "B + Y opens pause menu" },
            { key = "EnableChordHudToggle", label = "A + X control-guide overlay" },
            { key = "EnableVehicleFaceButtonFire", label = "Vehicle A fire" },
            { key = "EnableAircraftNativeControls", label = "Aircraft controls [Recommended]" },
        },
    },
    {
        title = "HUD AND CAMERA",
        flags = {
            { key = "EnablePauseUiAutoShow", label = "Show HUD for pause and result screens" },
            { key = "EnableHudAutoHide", label = "Auto-hide HUD after 20 seconds" },
            { key = "EnableShowUiAtStartup", label = "Show HUD when game starts (next launch)" },
            { key = "EnableFirstPersonCameraLock", label = "Keep first-person camera [Recommended]" },
        },
    },
    {
        title = "EXPERIMENTAL",
		flags = {
			{ key = "EnableCameraProfiles", label = "Automatic camera profiles [Experimental]" },
			{ key = "EnableBodyVisibility", label = "VR body visibility updates [Experimental]" },
			{ key = "EnableAimCalibrationProbe", label = "Native aim calibration probe [Experimental]" },
        },
    },
}

local values = {
    EnableCombatAssist = true,
    EnableCombatAssistAmmo = true,
    EnableCombatAssistDamage = false,
    EnableWeaponNoSpread = false,
    EnableCompactWeaponReticle = true,
    EnableVrScope = true,
    EnableHealthRecovery = true,
    EnableBulletDamageResistance = true,
    EnableAimAlignment = true,
    EnableDualGripAimFire = true,
	EnableCustomAkimbo = false,
    EnableAlternateWeaponHandsVisibility = true,
    EnableTwoHandStabilization = false,
    EnableGripCalibration = false,
    EnableGripWeaponCycle = false,
    EnableManualReloadMode = false,
    EnablePhoneAnswerGripTap = true,
    EnableChordPauseMenu = true,
	EnableChordHudToggle = true,
    EnablePauseUiAutoShow = true,
    EnableHudAutoHide = true,
    EnableFirstPersonCameraLock = true,
    EnableVehicleFaceButtonFire = true,
    EnableAircraftNativeControls = true,
    EnableShowUiAtStartup = true,
    EnableCameraProfiles = true,
	EnableBodyVisibility = true,
    EnableBulletTraceHidden = true,
    EnableAimCalibrationProbe = false,
}

local function log(message)
    local line = "[GTASADE_FeatureFlags] " .. message

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

local function parse_bool(value)
    return value == "true" or value == "1" or value == "on"
end

local function parse_payload(payload)
    local name, value = (payload or ""):match("^([^=]+)=([^;]+)")
    if name and value and values[name] ~= nil then
        values[name] = parse_bool(value)
    end
end

local function dispatch_setting(event_name, payload)
    if uevr and uevr.api and uevr.api.dispatch_custom_event then
        local ok = pcall(function()
            uevr.api:dispatch_custom_event(event_name, tostring(payload))
        end)
        if ok then
            return true
        end
    end

    if uevr and uevr.params and uevr.params.functions and uevr.params.functions.dispatch_custom_event then
        local ok = pcall(function()
            uevr.params.functions.dispatch_custom_event(event_name, tostring(payload))
        end)
        if ok then
            return true
        end
    end

    log("Could not dispatch " .. event_name .. "=" .. tostring(payload))
    return false
end

local function dispatch_flag(name, enabled)
    local payload = name .. "=" .. tostring(enabled)
    dispatch_setting("GTASADE.SetFeatureFlag", payload)
end

local function dispatch_reset_aim_calibration()
    if uevr and uevr.api and uevr.api.dispatch_custom_event then
        uevr.api:dispatch_custom_event("GTASADE.ResetAimCalibration", "reset")
    elseif uevr and uevr.params and uevr.params.functions and uevr.params.functions.dispatch_custom_event then
        uevr.params.functions.dispatch_custom_event("GTASADE.ResetAimCalibration", "reset")
    end
end

local function draw_flag(flag)
    local current = values[flag.key] == true
    local changed, new_value = imgui.checkbox(flag.label, current)
    if changed then
        values[flag.key] = new_value
        dispatch_flag(flag.key, new_value)
        log(flag.key .. " = " .. tostring(new_value))
    end
end

local function draw_panel_body()
    imgui.text("IMPROVEMENTS SETTINGS")
    imgui.spacing()

    imgui.text("CONTROL LAYOUT")
    local handedness_label = left_handed_mode == 0 and "Standard" or "Left-handed"
    if imgui.button("Control layout: " .. handedness_label .. "##SAVRHandedness") then
        left_handed_mode = left_handed_mode == 0 and 2 or 0
        dispatch_setting("GTASADE.SetLeftHandedMode", left_handed_mode)
    end
    imgui.text("Use this SAVR setting instead of UEVR Swap Controller Inputs.")
    imgui.text("On-foot grips and triggers always follow their physical hand.")
    imgui.text("Also available in the A + X quick settings.")
    imgui.spacing()

    if imgui.button("Reset 3D VR##SAVRReset3D") then
        reset_3d_status = "Resetting..."
        dispatch_setting("GTASADE.Reset3dVr", "reset")
    end
    imgui.text("Stuck in a flat screen during gameplay? " .. reset_3d_status)
    imgui.spacing()

    for _, section in ipairs(sections) do
        imgui.text(section.title)
        for _, flag in ipairs(section.flags) do
            draw_flag(flag)
        end
        imgui.spacing()
    end

    if values.EnableAimCalibrationProbe then
        imgui.text("AIM CALIBRATION RESULT")
        imgui.text(aim_calibration_result)
        if imgui.button("Reset aim samples") then
            dispatch_reset_aim_calibration()
        end
        imgui.spacing()
    end

end

local function draw_quick_options()
    imgui.text("SAVR COMFORT & CONTROLS")
    imgui.text("Independent preferences are safer than a hidden standing/seated preset.")
    imgui.spacing()

    local changed, enabled = imgui.checkbox("Movement Direction: Head/HMD (off = Game)",
        quick_movement_orientation == 1)
    if changed then
        quick_movement_orientation = enabled and 1 or 0
        dispatch_setting("GTASADE.SetMovementOrientation", quick_movement_orientation)
    end

    imgui.spacing()
    draw_flag({ key = "EnableHudAutoHide", label = "Auto-hide HUD after 20 seconds" })
    draw_flag({ key = "EnableManualReloadMode", label = "Manual reload (restart / reinject)" })
    draw_flag({ key = "EnableTwoHandStabilization", label = "Two-hand weapon stabilization" })
    draw_flag({ key = "EnableGripCalibration", label = "Weapon hand-placement calibration" })
    imgui.spacing()

    if imgui.button("Show all SAVR settings") then
        floating_visible = true
    end
    imgui.text("A + X closes the guide and this window.")
end

local function try_register_panel()
    if panel_attempted then
        return
    end
    panel_attempted = true

    if not (uevr and uevr.lua and uevr.lua.add_script_panel) then
        log("uevr.lua.add_script_panel unavailable; using floating fallback")
        return
    end

    local ok, err = pcall(function()
        uevr.lua.add_script_panel("IMPROVEMENTS SETTINGS", function()
            if imgui then
                draw_panel_body()
            end
        end)
    end)

    if ok then
        panel_registered = true
        log("Registered Script UI panel: IMPROVEMENTS SETTINGS")
    else
        log("Script UI panel registration failed: " .. tostring(err))
    end
end

uevr.sdk.callbacks.on_lua_event(function(event_name, event_string)
    if event_name == "featureFlagState" then
        parse_payload(event_string)
    elseif event_name == "reset3dVrStatus" then
        reset_3d_status = event_string or "Ready"
    elseif event_name == "playerIsLeftHanded" then
        local mode = tonumber(event_string)
        if mode and mode >= 0 and mode <= 2 and mode == math.floor(mode) then
            left_handed_mode = mode
        end
    elseif event_name == "movementOrientation" then
        local orientation = tonumber(event_string)
        if orientation and orientation >= 0 and orientation <= 2 and orientation == math.floor(orientation) then
            quick_movement_orientation = orientation
        end
    elseif event_name == "aimCalibrationResult" then
        aim_calibration_result = event_string or "No accepted shots yet"
    elseif event_name == "controlGuideOptionsVisible" then
        quick_options_visible = parse_bool(event_string)
    end
end)

try_register_panel()

uevr.sdk.callbacks.on_frame(function()
    try_register_panel()

    if not imgui then
        if not logged_imgui_missing then
            log("imgui unavailable in on_frame")
            logged_imgui_missing = true
        end
        return
    end

    if quick_options_visible then
        if imgui.set_next_window_pos then
            imgui.set_next_window_pos({ 1190, 150 }, 4)
        end
        imgui.set_next_window_size({ 650, 570 }, 4)
        local quick_open = imgui.begin_window("SAVR COMFORT & CONTROLS", true, 0)
        if quick_open then
            draw_quick_options()
        end
        imgui.end_window()
    end

    if floating_visible then
        imgui.set_next_window_size({ 460, 650 }, 4)
        local open = imgui.begin_window("IMPROVEMENTS SETTINGS", true, 0)
        if not open then
            floating_visible = false
            imgui.end_window()
            return
        end

        draw_panel_body()
        imgui.end_window()
    elseif not panel_registered then
        imgui.set_next_window_size({ 170, 58 }, 4)
        local launcher_open = imgui.begin_window("SA Settings", true, 0)
        if launcher_open and imgui.button("Show settings") then
            floating_visible = true
        end
        imgui.end_window()
    end
end)

log("Loaded")
