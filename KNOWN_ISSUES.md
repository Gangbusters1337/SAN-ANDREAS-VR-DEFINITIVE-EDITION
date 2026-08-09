# Beta known issues

- The experimental in-game A+X control-guide overlay is disabled. Its input toggle worked, but the UEVR compositor did not reliably display the rendered image.
- Physical melee can still miss during edge-case sweeps; vehicle structural deformation, glass damage, sparks, and native impact audio are incomplete.
- True independent native akimbo damage-ray/tracer ownership is not complete.
- Some long guns still need complete left/right primary and support calibration coverage.
- Only a subset of vehicle camera models has hand-tuned profiles. Uncalibrated models use class defaults and can be adjusted through UEVR.
- Vehicle free aim remains constrained by GTA's native weapon/vehicle/mission logic.
- Torso and arm embodiment is disabled or limited because native animation can clip through the first-person camera.
- UI ownership around unusual mission-result, shop, pause, busted, or wasted screens may still need refinement.
- Optional manual reload behavior is disabled by default and is not the recommended beta configuration.
- Quest 3/OpenXR is the main tested control target; other controller bindings are inherited and less thoroughly tested.
