local panel_registered = false
local panel_attempted = false
local floating_visible = true
local logged_imgui_missing = false
local aim_calibration_result = "No accepted shots yet"

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
            { key = "EnableAlternateWeaponHandsVisibility", label = "Hide idle weapon and aiming hands [Recommended]" },
            { key = "EnableGripWeaponCycle", label = "Tap left grip to cycle weapons [Recommended]" },
            { key = "EnableManualReloadMode", label = "Tap right grip to reload (restart/reinject) [Experimental]" },
            { key = "EnablePhoneAnswerGripTap", label = "Tap right grip to answer ringing phone [Recommended]" },
            { key = "EnableChordPauseMenu", label = "B + Y opens pause menu" },
            { key = "EnableChordHudToggle", label = "A + X control-guide overlay" },
            { key = "EnableVehicleFaceButtonFire", label = "Vehicle X/A left/right fire" },
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
    EnableAlternateWeaponHandsVisibility = true,
    EnableGripWeaponCycle = false,
    EnableManualReloadMode = false,
    EnablePhoneAnswerGripTap = true,
    EnableChordPauseMenu = true,
    EnableChordHudToggle = false,
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

local function dispatch_flag(name, enabled)
    local payload = name .. "=" .. tostring(enabled)

    if uevr and uevr.api and uevr.api.dispatch_custom_event then
        local ok = pcall(function()
            uevr.api:dispatch_custom_event("GTASADE.SetFeatureFlag", payload)
        end)
        if ok then
            return
        end
    end

    if uevr and uevr.params and uevr.params.functions and uevr.params.functions.dispatch_custom_event then
        local ok = pcall(function()
            uevr.params.functions.dispatch_custom_event("GTASADE.SetFeatureFlag", payload)
        end)
        if ok then
            return
        end
    end

    log("Could not dispatch " .. payload)
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
    elseif event_name == "aimCalibrationResult" then
        aim_calibration_result = event_string or "No accepted shots yet"
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

    if panel_registered then
        return
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
    else
        imgui.set_next_window_size({ 170, 58 }, 4)
        local launcher_open = imgui.begin_window("SA Settings", true, 0)
        if launcher_open and imgui.button("Show settings") then
            floating_visible = true
        end
        imgui.end_window()
    end
end)

log("Loaded")
