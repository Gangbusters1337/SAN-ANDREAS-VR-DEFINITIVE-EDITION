# Quest 3 controls

This guide describes the current `overallvrinteractionupgrade` code. The Lua/C++ input path is authoritative; older mapping notes may describe disabled experiments.

## Base mapping

| Quest control | Game input |
|---|---|
| Right A | Xbox A |
| Right B | Xbox X |
| Left X | Xbox B |
| Left Y | Xbox Y |
| Left/right trigger | LT/RT |
| Left/right grip | LB/RB |
| Left stick | Movement or steering |
| Right stick | Look or turn |

Physical Start, Back, L3, and R3 remain native unless an optional remap is enabled.

## On foot: firearms

- Hold the grip on the side that should own/present the weapon.
- Pull either trigger to fire an ordinary firearm. The held grip/trigger selects the active side.
- Grips are weapon ownership/presentation controls; they do not cycle weapons.
- Left Quest X cycles to the next weapon. It emits one bounded native cycle pulse and consumes the X hold.
- Grenades and Molotovs: hold either grip to take ownership, move the controller naturally, and release the grip to throw with hand momentum. No trigger is required.
- Tear gas, satchel charges, and the detonator retain GTA's native interaction.
- Pistol, Sawn-off Shotgun, Micro Uzi, and Tec-9 support independent akimbo when the option is enabled: grip each weapon and use that hand's trigger to fire its own side.

## On foot: fists and melee

- Hold a hand's grip to arm that physical hand, then strike by moving it.
- Bare fists support both hands independently.
- With brass knuckles, the primary magnetic hand uses brass-knuckle damage and the other held hand acts as a bare fist.
- Triggers are clench state only for unarmed/melee weapons. Native trigger punching is blocked; physical contact owns attacks.
- A quick right-grip tap of 300 ms or less sends GTA's native RB context action while on foot: it answers a ringing phone or replaces the equipped weapon while standing over a same-slot ground-weapon prompt, even when the HUD is hidden. Holding grip remains normal weapon/fist ownership.
- R3 sends the same native RB context action on foot as a secondary fallback.

## Vehicles

- RT remains native acceleration.
- LT remains native brake/reverse.
- Left stick remains native steering.
- Left grip is not a drive-by fire request.
- When vehicle free aim is eligible, Right A fires the controller-held weapon. It is converted to GTA's validated vehicle-fire input without changing RT acceleration.
- Current eligibility: ordinary car/boat or motorized bike, compatible camera, one supported weapon from pistol through sniper, and no dual wield.
- This is one Right-A fire control, not the older proposed X/A left/right-fire layout.

## Pedal bicycles and motorbikes

- BMX, Bike, and Mountain Bike models 481, 509, and 510: RT pedals and A fires a supported equipped gun through the same vehicle free-aim path as cars.
- Left X remains a native bicycle fallback and does not cycle weapons on those three models.
- Unclassified bike models remain native.
- Motorized bikes use the ordinary vehicle free-aim gates.

## Aircraft

- Native aircraft controls remain enabled.
- Flight sticks and buttons pass through unchanged.
- Weapon cycling, vehicle face-fire remapping, and phone tap are disabled in aircraft.
- Left thumb-rest plus right-stick Up emits the Hydra Auto-Hover/VTOL D-pad action; other aircraft stick directions remain native.
- L3 landing-gear changes and Hydra VTOL changes briefly reveal the exterior camera on supported retractable-gear planes.
- The HUD touch gestures, pause chord, and short camera-switch press still run.

## HUD, pause, and camera

- Tap the left thumb-rest: show the HUD and start/reset its 20-second auto-hide timer.
- Double-tap the left thumb-rest: pin or unpin the HUD.
- Hold the left thumb-rest: reveal the HUD context while using the touch D-pad.
- Right B + Left Y: reveal pause UI and send a short Start pulse.
- Right A + Left X opens/closes the in-game control guide and quick settings. Follow the panel's prompts: left stick/A with Standard layout, right stick/X with Left-handed layout. Move the indicated stick left/right to browse the looping menu; the selected setting stays in the center. The indicated button changes Movement Direction, HUD Auto-Hide, D-pad Control, Control Layout, or the session-only Diagnostics mode. A + X closes without changing an option. Movement Direction chooses whether on-foot stick movement follows the game or your HMD; it does not directly turn CJ's body.
- D-pad Control defaults to **THUMBREST + R-STICK**: hold the left thumbrest and move the right stick. Controllers without a thumbrest can select **R3 + R-STICK** instead. Pressing R3 immediately pauses right-stick camera movement; hold it for about 0.25 seconds, then move the right stick for D-pad input. R3 also inherits the HUD tap, double-tap, and held-context behavior while this mode is selected, while a short on-foot click retains the pickup/context action. R3 D-pad mode is disabled in aircraft, and L3+R3 still opens UEVR.
- Diagnostics choices are Off, Vehicle Input, Save/Load, and Full. They always start Off after a restart and never enable experimental gameplay behavior.
- In a car, boat, or motorbike, hold the left Quest menu button for about one second to send GTA's native Back/View action and cycle its original vehicle cameras. A short press remains Pause.
- The camera hold is blocked on foot and in aircraft, so it cannot change those camera/control paths.
- HUD auto-hide defaults to 20 seconds; pause UI auto-show defaults on.
- Click L3 + R3 together to open or close the UEVR menu.
- With the UEVR menu open, hold RT and use the left stick for camera left/right/forward/back and the right stick for camera up/down. Per-vehicle offsets are saved automatically after they settle.

## Touch D-pad

- Hold the left thumb-rest touch and deflect the right stick to emit a D-pad direction.
- Right-stick look is replaced by D-pad input while the touch modifier is held on foot and in ordinary vehicles.
- Left is held for the View Stats direction; up, down, and right are pulses.
- Right changes the radio or sends GTA's positive-response action when that native context is available.
- Up runs GTA's gang/vehicle-submission action; down runs GTA's map-zoom/secondary action. The exact result remains context-sensitive.
- In aircraft, only the deliberate Up deflection is consumed; other right-stick directions remain native. Touch D-pad input is disabled in no-control and weapon-wheel states.

## Grip calibration

Calibration is disabled by default. When enabled:

1. Equip an eligible weapon and hold the grip for the hand being calibrated.
2. Hold both thumb-rest touches for at least 500 ms.
3. Position the weapon/hand.
4. Release to save the per-weapon record.

For a two-hand firearm, the first grip is primary and the other is support. Face buttons are not calibration inputs. Melee calibration is currently single-hand. Reset is available through the settings/event path, not a documented Quest button.

## Optional or disabled by default

- Manual reload.
- Legacy grip weapon cycling.
- A/B weapon-cycle test.
- Legacy grip + face-button D-pad mapper.
- R3 + left-stick D-pad.
- Two-hand stabilization.
- Grip calibration.
- Aim-calibration probe.
- Free-aim animated-hand option.

Control Layout is available in the A + X quick settings and SAVR's Improvements Settings: Standard or Left-handed. On-foot weapon handling is always ambidextrous: each physical grip/trigger operates its own hand, with unchanged melee, two-hand aiming, holsters, and calibration. Left-handed swaps the other buttons and sticks; vehicle free aim uses the left gun hand and keeps the right native hand on the wheel. The choice is saved across restarts. Older left-handed profiles migrate to this unified layout. Use this SAVR option instead of UEVR's native Swap Controller Inputs.

If gameplay gets stuck on a flat 2D screen, open A + X, browse to **Reset 3D VR**, and activate it with the indicated button. It shows **3D RESTORED** when successful; close the guide with A + X to check the view. The same action is in UEVR's Improvements Settings. Resume gameplay first if you are in a pause, cutscene, or result screen. This clears only 2D mode and stale SAVR 2D guard state; camera calibration, graphics, handedness, and HUD preferences are not reset. No restart is needed.

## Known control issues to verify

- The vehicle camera action uses UEVR's original System-button long-hold mapping to native Xbox Back/View; it does not post keyboard `V` or synthesize D-pad input.
- The optional feature-flags UI still contains stale labels for legacy grip cycling and an older vehicle X/A fire proposal.
- The older `CONTROL_MAPPING_NOTES.txt` grip + ABXY D-pad and medium-hold camera descriptions are obsolete.
- Quick Left-X weapon cycling can briefly expose GTA's native weapon wheel because it uses a bounded LB pulse; deliberate hold-to-open weapon-wheel behavior is not implemented.
- All chords must be retested in shops, missions, result screens, vehicles, and no-control transitions before release.
