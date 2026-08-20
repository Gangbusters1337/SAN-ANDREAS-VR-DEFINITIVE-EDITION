# Changelog

## v0.3.0-beta — Motion Throwables and Independent Akimbo

This beta adds two of the largest interaction upgrades yet while preserving GTA's native combat ownership wherever it matters.

### Physics-based grenades and Molotovs

- Added controller-momentum physical throwing for grenades and Molotovs: grip, swing, and release without using the native trigger curve.
- Kept the visible bottle/grenade flight custom and responsive while handing the resolved impact point to GTA's native explosion lifecycle.
- Restored native Molotov fire, spreading ground flames, damage, sound, smoke, reactions, and vehicle/ped effects at the custom impact location.
- Added grenade-specific launch speed and native explosion behavior, with Molotovs tuned slightly slower but still easy to throw naturally.
- Added impact collision handling, rapid re-arm, visibility/lifecycle cleanup, and death/respawn restoration without reintroducing native animated-hand artifacts.

### Independent VR akimbo

- Added custom akimbo support for Pistol, Sawn-off Shotgun, Micro Uzi, and Tec-9.
- Both guns are controller-driven rather than attaching one side to CJ's native body animation.
- Each physical trigger owns its corresponding hand, aim ray, accepted shot, visible muzzle, smoke, and presentation sequence.
- Native GTA ammo, damage, weapon cadence, audio, and impact handling remain authoritative.
- Corrected first-grip native-model flashes, one-sided muzzle effects, pistol flash-size imbalance, and Micro Uzi/Tec-9 muzzle placement.

### Release polish

- Preserved the animated/clenched split-hand assets, launch race guard, A+X settings guard, physical melee/audio refinements, magnetic holsters, and tested graphics/profile settings from the preceding candidate.
- Updated the control guide, feature documentation, release staging, and stable-name package metadata for v0.3.

## v0.2.0-beta — San Andreas VR Definitive Edition Beta

This beta brings the tested interaction, presentation, control, graphics, and packaging improvements together in one easier-to-install release.

### VR interaction and combat

- Reworked physical melee around swept motion contact instead of synthetic trigger attacks.
- Added native pedestrian damage and reactions for supported melee weapons, brass knuckles, and independent bare-fist strikes.
- Added two-hand fist and one-knuckle/one-fist fighting while preventing trigger input from starting unwanted native punching.
- Added independent weapon-hand/free-fist combinations for held melee weapons, so both hands retain separate strike ownership and cooldowns.
- Improved fast-swing contact coverage, short forward prediction, per-swing target deduplication, cooldown behavior, and transition cleanup.
- Required real tracking-space movement for fist and brass-knuckle damage, preventing pedestrians from taking damage simply by walking into a stationary clenched hand.
- Added weapon-aware contact audio for fists/knuckles, blunt weapons, sharp weapons, and vehicles, with safe packaged-asset fallbacks.
- Removed the melee collision visualizer from normal play.
- Added split left/right clenched-hand assets and controller-tracked presentation without the duplicate attached hand from the original native two-hand mesh.
- Corrected two-hand rifle/shotgun attachment roles, rear-hand orientation, regrip behavior, and support-hand stability.
- Preserved full weapon-origin damage rays and visible trails for normal and supported vehicle firing.
- Added a bounded single-request buffer for rapid semi-automatic trigger taps so a valid pull is not discarded merely because GTA is still finishing the previous shot's cooldown.
- Limited first-shot fire-task prewarming to pistols and SMGs, avoiding the slow ready-walk stance when merely gripping shotguns or rifles.

### Controls and HUD

- Added the large A+X in-game control guide with quick VR options.
- Guarded the A+X close action so closing the guide cannot also change the highlighted quick option.
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
- Added a guarded launcher for desktop, Start-menu, and upgraded taskbar shortcuts to prevent accidental duplicate game launches and preserve a last-launch GTA settings recovery copy.
- Added backups for the existing UEVR profile, GTA settings, and every game-folder file replaced by the installer.
- Added clean-install verification for both installer modes and checksum verification for the manual archive.
- Kept the stable archive names `San-Andreas-VR-DE-Manual.zip` and `San-Andreas-VR-DE-Installer.zip`; exact version and hashes are stored inside each archive.
- Kept internal snapshots, logs, dumps, personal paths, development instructions, and game executables out of the public repository and release archives.

### Beta limitations

- This remains a pre-release beta tested primarily with Quest Touch-style controls and the supported SanAndreas executable listed in the README.
- Some missions, uncommon vehicles, weapon combinations, and uncalibrated cameras may still require refinement.
- Structural vehicle deformation from physical melee is not claimed; pedestrian melee reactions are currently the more complete path.
- Physical grenade/Molotov throwing remains disabled; this beta keeps GTA's native throwable behavior.
- Grip calibration and experimental options remain available through the UEVR/plugin settings for users who need them.
