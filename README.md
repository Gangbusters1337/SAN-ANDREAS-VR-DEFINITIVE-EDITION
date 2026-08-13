# SAN ANDREAS VR DEFINITIVE EDITION

<p align="center">
  <a href="Documentation/Quest3-Control-Layout.png">
    <img src="Documentation/Quest3-Control-Layout.png" alt="San Andreas VR Quest 3 controls and quick VR options" width="100%">
  </a>
</p>

> **PRE-RELEASE BETA v0.2** - Click the control guide above to open the full-resolution image.

Want to walk through San Andreas instead of looking at it through a flat screen? This mod turns **Grand Theft Auto: San Andreas - The Definitive Edition** into a much clearer, more physical, and more immersive VR experience through Praydog's UEVR.

The included profile is tuned for sharp visuals and good performance, with plenty of headroom left for stronger PCs to turn the graphics up. The HUD hides itself when you do not need it, the controls are built around Quest motion controllers, and the control guide is available both at startup and in VR.

The big difference is how physical the game feels:

- **All weapons are motion tracked:** point, aim, and fire with your controllers instead of dragging a flat-screen crosshair around.
- **Two-handed aiming and stabilization:** grip rifles and shotguns with both hands, steady the rear grip, and use the forward hand to guide the barrel.
- **Animated VR hands:** controller-tracked open, grip, trigger, and clenched-hand poses keep the hands feeling connected to what you are doing.
- **Refined, accurate melee:** swing bats and melee weapons, punch with either fist, and fight with a melee weapon in one hand while the other hand fights freely.
- **Punch and melee weapon sound effects:** hits feel physical, with distinct feedback for fists, knuckles, blunt weapons, sharp weapons, and vehicles.
- **Excellent vehicle free aim:** aim and fire from supported cars and bikes while keeping driving controls available.
- **Great-feeling driving controls:** drive, steer, accelerate, brake, and use vehicle weapons without fighting the VR controls.
- **Custom refined flight controls:** aircraft handling, Hydra VTOL support, and landing-gear camera feedback are tuned separately from ground vehicles.
- **Vehicle cameras that remember you:** calibrate a car or other vehicle once and its camera position is saved and recalled automatically every time you get back in. Many cars, bikes, boats, and aircraft already have tuned profiles.
- **Natural weapon handling:** drop weapons into magnetic waist holsters, recall them per weapon, and keep two-hand grips stable through regrips.
- **A cleaner VR interface:** auto-hiding HUD, A+X control guide, touch/D-pad shortcuts, phone grip tap, and Quest-friendly pause controls.
- **Sharper, clearer visuals:** improved textures, scope and muzzle presentation, cleaner effects, and graphics settings that leave room for extra quality.

This is still beta software, so back up your existing UEVR `SanAndreas` profile before installing. A few missions, unusual vehicles, and weapon combinations may still need refinement.

## Requirements

- A legally installed copy of GTA San Andreas – The Definitive Edition for Windows.
- The stable UEVR release. The package does **not** include UEVR or game files.
- OpenXR and Quest Touch-style controls are the primary tested setup.
- Supported executable used during development: `SanAndreas.exe` SHA-256 `CF677214A8AB317B3BC8811C64BBC60742204D021132C79E7C0583322CF5BA17`.

## Downloads

The latest release has separate downloads for VRHub and regular users:

- **`San-Andreas-VR-DE-v0.2-LATEST-Manual.zip`** - versioned manual download.
- **`San-Andreas-VR-DE-v0.2-LATEST-Installer.zip`** - versioned installer with `Auto Detect` and `Manual Path` modes.
- **`San-Andreas-VR-DE-Manual.zip`** - plain manual filename for VRHub automation.

Open `VERSION.txt` inside an archive for its exact version and key hashes; `SHA256SUMS.txt` covers every packaged file.

The Quest 3 layout is included as `Documentation/Quest3-Control-Layout.png` in both archives, displayed prominently above, and available in VR through the A+X control-guide shortcut.

See [CHANGELOG.md](CHANGELOG.md) for the full v0.2 change list.

## What you get in VR

### Weapons and hands

- **All weapons are motion tracked:** point, aim, and fire with your controllers instead of dragging a flat-screen crosshair around.
- **Two-handed aiming and stabilization:** grip rifles, shotguns, and other long weapons with both hands and steady the rear grip while the forward hand guides the barrel.
- **Animated controller-tracked hands:** open, grip, trigger, and clenched-hand poses follow what you are actually doing.
- **Refined physical melee:** fists, brass knuckles, bats, and melee weapons support independent left/right hand fighting, including a melee weapon in one hand and a fist in the other.
- **Punch and melee weapon sound FX:** clear contact feedback for fists, knuckles, blunt weapons, sharp weapons, and vehicle impacts.
- **Natural holsters:** drop weapons into lowered magnetic waist positions, keep their remembered per-weapon placement, and regrip without losing the correct hand relationship.
- **Sniper and long-gun polish:** usable scope presentation, steadier two-hand handling, improved muzzle presentation, and cleaner weapon visibility.

### Cars, bikes, boats, and aircraft

- **Free aim in cars:** aim and fire supported vehicle weapons while keeping acceleration and driving controls available.
- **Great-feeling driving controls:** steering, acceleration, braking, vehicle hands, bicycle pedals, motorbike handling, and vehicle firing are separated so they do not fight each other.
- **Camera calibration that sticks:** open the UEVR camera controls, position the view once, and the mod saves it per vehicle model and restores it automatically on future entries.
- **Many pre-calibrated vehicles:** tuned profiles are included for a broad set of cars and other vehicle types; uncalibrated models fall back safely and can be adjusted the same way.
- **Custom refined flight controls:** aircraft controls stay native where appropriate, with Hydra VTOL support, aircraft-specific input handling, and brief exterior-camera feedback for supported landing-gear changes.
- **HMD-oriented movement support:** use the UEVR body/movement option that suits your standing or seated setup while retaining stick locomotion.

### Comfort, controls, and presentation

- **Auto-hiding HUD for maximum immersion in sunny San Andreas:** show it with the left touch, pin or unpin it with a double-tap, and use the touch modifier with the right stick for contextual D-pad actions.
- **In-game control guide:** A+X opens the Quest 3 layout and quick VR options without needing to leave the game.
- **Useful interaction shortcuts:** phone answering, pause controls, camera switching, weapon cycling, vehicle fire, and aircraft-specific controls are mapped for motion controllers.
- **Sharper high-definition VR:** improved textures, scope and reticle clarity, cleaner effects, better muzzle flashes, and graphics settings with headroom for stronger PCs.
- **Installer safety:** Auto Detect and Manual Path modes, OneDrive-aware profile handling, backups, guarded single-instance launching, and package hashes help protect existing settings.

Physical grenade/Molotov throwing is not enabled in this beta; throwables retain GTA's native handling while the motion implementation remains under development.

## Installer

1. Close GTA San Andreas DE and UEVR.
2. Extract the installer archive completely.
3. Run `INSTALL-SAVR.bat`.
4. Choose:
   - `1 — Auto Detect`: checks common Steam, Rockstar, Epic, and game-library locations.
   - `2 — Manual Path`: paste the GTA San Andreas DE folder containing `Gameface`.
5. Confirm the detected paths. The installer backs up the existing UEVR profile and GTA settings before copying files.

The installer resolves Windows' real Documents known folder, including OneDrive redirection, and installs the tested GTA VR settings with Free Aim. It backs up the existing UEVR profile, GTA settings, and every game-folder file it replaces—including the original startup movie. Use the advanced `-SkipGameSettings` switch only if you deliberately want to retain your existing GTA graphics/gameplay configuration.

Launch through the installed **GTA San Andreas DE VR** desktop, Start-menu, or taskbar shortcut. SAVR's guarded launcher prevents rapid double-clicks from starting two game instances and saves `GameUserSettings.SAVR-last-launch.ini` before each launch. The installer also upgrades existing user shortcuts and taskbar pins that point to the detected game executable. Do not pin or launch the raw `SanAndreas.exe` directly, because Windows shortcuts cannot intercept a direct executable launch without unsafe game-file or system-level modification.

The installer never installs UEVR or the game.

## Manual installation

Close the game and UEVR first.

### 1. UEVR profile

Copy the contents of:

```text
UnrealVRMod\SanAndreas\
```

to:

```text
%APPDATA%\UnrealVRMod\SanAndreas\
```

The runtime plugin must end up at:

```text
%APPDATA%\UnrealVRMod\SanAndreas\plugins\UEVR_GTASADE.dll
```

Lua scripts must end up under:

```text
%APPDATA%\UnrealVRMod\SanAndreas\scripts\
```

### 2. Texture PAKs

Copy:

```text
GameFolder\Gameface\Content\Paks\~mods\
```

into the GTA San Andreas Definitive Edition installation folder, preserving the `Gameface\Content\Paks\~mods` path.

### 3. Recommended GTA settings

The manual archive contains:

```text
GameSettings\GameUserSettings.ini
```

Press `Win+R`, enter `shell:Personal`, and open:

```text
Rockstar Games\GTA San Andreas Definitive Edition\Config\WindowsNoEditor\
```

Back up the existing file, then copy the packaged `GameUserSettings.ini` there. Using `shell:Personal` is important because Windows may redirect Documents into OneDrive.

### 4. Startup guide and control image

The package replaces the Rockstar startup stinger with a 30-second control-guide version. Manual installers should back up this file first:

```text
Gameface\Content\Movies\1080\GTA_SA_RSTAR_STINGER_FINAL_1920x1080.mp4
```

The same full-resolution layout is included under `Documentation` and in the UEVR profile for the A+X in-game guide.

## First launch

1. Start GTA San Andreas DE.
2. Start UEVR and inject into `SanAndreas`/`Gameface` using OpenXR.
3. The included profile, bindings, plugin, and Lua scripts load automatically.
4. Use the separate SA Improved Settings utility for optional and experimental flags.

See [CONTROLS.md](CONTROLS.md), [FEATURES.md](FEATURES.md), and [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Questions and bug reports

For questions, bug reports, feedback, or other help, send a direct message on X to [@Yobrohy2o](https://x.com/Yobrohy2o).

## Updating and uninstalling

- Installer updates create timestamped profile, GTA-settings, and replaced-game-file backups under `%APPDATA%\UnrealVRMod\_Backups`.
- The installer also backs up the active GTA settings and leaves `GameUserSettings.SAVR-recommended.ini` beside the active file for quick recovery if GTA runs first-launch defaults again.
- Personal vehicle camera and grip calibration files should be copied somewhere safe before replacing the profile.
- To uninstall manually, restore your prior `SanAndreas` profile and remove only the two packaged PAKs from the game's `~mods` folder.

## Building from source

Use Visual Studio 2022 Build Tools and build `UEVR_GTASADE.sln` as `Release|x64`. Visual Studio 18/2026 currently resolves incompatible C++ target paths for this project.

The repository source tree and downloadable runtime archives are intentionally separate. Generated intermediates, PDBs, logs, crash dumps, personal profiles, saves, and internal development snapshots are not release files.

## Credits and license

- Based on [Holydh/UEVR_GTASADE](https://github.com/Holydh/UEVR_GTASADE), licensed under MIT.
- UEVR by Praydog.
- Scope work builds on Mutar's shared UEVR scope techniques.
- Thanks to the Flat2VR/UEVR modding community and project testers.

This repository is distributed under the included [MIT License](LICENSE). Grand Theft Auto, Rockstar Games, and related assets are trademarks/property of their respective owners. No original game executable or game-owned data is included.
