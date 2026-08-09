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
- Grenades, tear gas, and Molotovs retain GTA's native cook/throw interaction.

## On foot: fists and melee

- Hold a hand's grip to arm that physical hand, then strike by moving it.
- Bare fists support both hands independently.
- With brass knuckles, the primary magnetic hand uses brass-knuckle damage and the other held hand acts as a bare fist.
- Triggers are clench state only for unarmed/melee weapons. Native trigger punching is blocked; physical contact owns attacks.
- A quick right-grip tap of 300 ms or less answers a ringing phone while on foot.

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
- The experimental in-game A+X control-guide overlay is disabled in this beta because UEVR does not present its render target reliably. Open `Documentation/Quest3-Control-Layout.png` outside the game instead.
- Short press of the left Quest menu action (350 ms or less): cycle the camera.
- A longer left Quest menu hold does not cycle the camera.
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

Left-handed mode is configured in settings: disabled, trigger swap, or full input swap. The on-foot-only setting remains the default behavior.

## Known control issues to verify

- The left Quest menu camera action depends on the installed UEVR profile routing that action through D-pad Left.
- The optional feature-flags UI still contains stale labels for legacy grip cycling and an older vehicle X/A fire proposal.
- The older `CONTROL_MAPPING_NOTES.txt` grip + ABXY D-pad and medium-hold camera descriptions are obsolete.
- Quick Left-X weapon cycling can briefly expose GTA's native weapon wheel because it uses a bounded LB pulse; deliberate hold-to-open weapon-wheel behavior is not implemented.
- All chords must be retested in shops, missions, result screens, vehicles, and no-control transitions before release.
