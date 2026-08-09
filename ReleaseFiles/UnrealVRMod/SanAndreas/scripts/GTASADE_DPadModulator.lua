print("GTASADE_DPadModulator.lua loaded")

-- DUALGRIP owns the normal control path. This older mapper is opt-in so it
-- cannot rewrite the same virtual buttons after DUALGRIP has handled them.
local enableLegacyDPadModulator = false

local DPAD_UP = XINPUT_GAMEPAD_DPAD_UP or 0x0001
local DPAD_DOWN = XINPUT_GAMEPAD_DPAD_DOWN or 0x0002
local DPAD_LEFT = XINPUT_GAMEPAD_DPAD_LEFT or 0x0004
local DPAD_RIGHT = XINPUT_GAMEPAD_DPAD_RIGHT or 0x0008
local START = XINPUT_GAMEPAD_START or 0x0010
local BACK = XINPUT_GAMEPAD_BACK or 0x0020
local LEFT_THUMB = XINPUT_GAMEPAD_LEFT_THUMB or 0x0040
local RIGHT_THUMB = XINPUT_GAMEPAD_RIGHT_THUMB or 0x0080
local LEFT_SHOULDER = XINPUT_GAMEPAD_LEFT_SHOULDER or 0x0100
local RIGHT_SHOULDER = XINPUT_GAMEPAD_RIGHT_SHOULDER or 0x0200
local BUTTON_A = XINPUT_GAMEPAD_A or 0x1000
local BUTTON_B = XINPUT_GAMEPAD_B or 0x2000
local BUTTON_X = XINPUT_GAMEPAD_X or 0x4000
local BUTTON_Y = XINPUT_GAMEPAD_Y or 0x8000

local function has_button(buttons, mask)
    return math.floor(buttons / mask) % 2 >= 1
end

local function set_button(buttons, mask)
    if has_button(buttons, mask) then
        return buttons
    end

    return buttons + mask
end

local function clear_button(buttons, mask)
    if has_button(buttons, mask) then
        return buttons - mask
    end

    return buttons
end

uevr.sdk.callbacks.on_xinput_get_state(function(retval, user_index, state)
    if not enableLegacyDPadModulator then
        return
    end

    local buttons = state.Gamepad.wButtons
    local rightGripHeld = has_button(buttons, RIGHT_SHOULDER)
    local leftGripHeld = has_button(buttons, LEFT_SHOULDER)
    if not rightGripHeld and not leftGripHeld then
        return
    end

    local remapped = false

    -- Face-button diamond to D-pad diamond while right grip/RB is held:
    -- Y = Up, B = Right, A = Down, X = Left.
    if rightGripHeld then
        if has_button(buttons, BUTTON_Y) then
            buttons = set_button(buttons, DPAD_UP)
            remapped = true
        end
        if has_button(buttons, BUTTON_B) then
            buttons = set_button(buttons, DPAD_RIGHT)
            remapped = true
        end
        if has_button(buttons, BUTTON_A) then
            buttons = set_button(buttons, DPAD_DOWN)
            remapped = true
        end
        if has_button(buttons, BUTTON_X) then
            buttons = set_button(buttons, DPAD_LEFT)
            remapped = true
        end
    end

    -- Extra system buttons while left grip/LB is held:
    -- A = Start, B = Back/Esc-like, X = left stick click, Y = right stick click.
    if leftGripHeld then
        if has_button(buttons, BUTTON_A) then
            buttons = set_button(buttons, START)
            remapped = true
        end
        if has_button(buttons, BUTTON_B) then
            buttons = set_button(buttons, BACK)
            remapped = true
        end
        if has_button(buttons, BUTTON_X) then
            buttons = set_button(buttons, LEFT_THUMB)
            remapped = true
        end
        if has_button(buttons, BUTTON_Y) then
            buttons = set_button(buttons, RIGHT_THUMB)
            remapped = true
        end
    end

    if not remapped then
        return
    end

    buttons = clear_button(buttons, BUTTON_A)
    buttons = clear_button(buttons, BUTTON_B)
    buttons = clear_button(buttons, BUTTON_X)
    buttons = clear_button(buttons, BUTTON_Y)
    state.Gamepad.wButtons = buttons
end)

uevr.sdk.callbacks.on_lua_event(function(event_name, event_string)
    if event_name ~= "featureFlagState" then
        return
    end

    local name, value = (event_string or ""):match("^([^=]+)=([^;]+)")
    if name == "EnableLegacyDPadModulator" then
        enableLegacyDPadModulator = value == "true" or value == "1" or value == "on"
        print("GTASADE_DPadModulator legacy mapping = " .. tostring(enableLegacyDPadModulator))
    end
end)
