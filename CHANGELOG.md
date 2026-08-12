# Changelog

## v0.1.0-beta — San Andreas VR Definitive Edition Pre-Release Beta

This release candidate updates the original public pre-release beta with the tested interaction, presentation, control, and packaging work completed since that build.

### VR interaction and combat

- Reworked physical melee around swept motion contact instead of synthetic trigger attacks.
- Added native pedestrian damage and reactions for supported melee weapons, brass knuckles, and independent bare-fist strikes.
- Added two-hand fist and one-knuckle/one-fist fighting while preventing trigger input from starting unwanted native punching.
- Improved fast-swing contact coverage, per-swing target deduplication, cooldown behavior, and transition cleanup.
- Corrected two-hand rifle/shotgun attachment roles, rear-hand orientation, regrip behavior, and support-hand stability.
- Preserved full weapon-origin damage rays and visible trails for normal and supported vehicle firing.

### Controls and HUD

- Added the large A+X in-game control guide with quick VR options.
- Consolidated HUD interaction on the left thumb rest: tap for timed HUD, double-tap to pin/unpin, and hold with the right stick for contextual D-pad input.
- Added touch feedback patterns and action-handle recovery after UEVR device/session resets.
- Preserved A as vehicle fire while keeping the right trigger as accelerator.
- Separated pedal-bicycle input from motorbike and ordinary vehicle controls.
- Added the updated Quest 3 control layout to the repository, both packages, the UEVR profile, and a 30-second startup guide.

### Weapons, holsters, and cameras

- Revised body-local magnetic holsters with per-weapon recall, stable settling/rebase behavior, and weapon-class presentation orientation.
- Improved pistol and long-gun fallback placement and downward holster presentation.
- Added and refined tested per-vehicle camera profiles, including newly captured vehicle IDs 600 and 606.
- Updated the tested profile to HMD-oriented movement while retaining stick locomotion and Synced Sequential rendering.
- Updated grip calibration data and sniper/scope presentation assets.

### Reliability and distribution

- Updated the tested GTA graphics/gameplay settings and UEVR profile shipped with the release.
- Added installer Auto Detect and Manual Path modes with OneDrive-aware Documents resolution.
- Added backups for the existing UEVR profile, GTA settings, and every game-folder file replaced by the installer.
- Added clean-install verification for both installer modes and checksum verification for the manual archive.
- Kept internal snapshots, logs, dumps, personal paths, development instructions, and game executables out of the public repository and release archives.

### Beta limitations

- This remains a pre-release beta tested primarily with Quest Touch-style controls and the supported SanAndreas executable listed in the README.
- Some missions, uncommon vehicles, weapon combinations, and uncalibrated cameras may still require refinement.
- Structural vehicle deformation from physical melee is not claimed; pedestrian melee reactions are currently the more complete path.
- Grip calibration and experimental options remain available through the UEVR/plugin settings for users who need them.
