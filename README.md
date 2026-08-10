# SAN ANDREAS VR DEFINITIVE EDITION

Experience San Andreas in glorious high-definition VR: dramatically clearer, sharper, more immersive, and built around full motion-controller interaction. This unofficial beta enhancement package transforms **Grand Theft Auto: San Andreas – The Definitive Edition** through Praydog's UEVR.

The included VR graphics profile is tuned for outstanding detail and strong performance, avoiding the worst visual artifacts while leaving capable PCs headroom to raise texture quality and other settings further. The HUD automatically gets out of your way, controls are designed to feel natural on motion controllers, and the world is presented with far greater clarity than a basic injected profile.

This is not just head tracking. Aim pistols freely, stabilize shotguns and rifles with both hands, swing bats and melee weapons using tracked physical motion, fight bare-knuckle with either fist, and aim submachine guns freely from inside supported vehicles. Weapon rays and visible trails follow the weapon itself instead of an invisible flat-screen crosshair.

![Quest 3 control layout](Documentation/Quest3-Control-Layout.png)

> Beta software: back up your existing UEVR `SanAndreas` profile. Mission scripts, unusual vehicles, and some weapon combinations may still expose edge cases.

## Requirements

- A legally installed copy of GTA San Andreas – The Definitive Edition for Windows.
- The stable UEVR release. The package does **not** include UEVR or game files.
- OpenXR and Quest Touch-style controls are the primary tested setup.
- Supported executable used during development: `SanAndreas.exe` SHA-256 `CF677214A8AB317B3BC8811C64BBC60742204D021132C79E7C0583322CF5BA17`.

## Downloads

Two separate archives are intended for GitHub Releases:

- **Manual package** — transparent folder trees for users who prefer placing every file themselves.
- **Installer package** — the same payload plus a Windows installer with `Auto Detect` and `Manual Path` modes.

The Quest 3 layout is included as `Documentation/Quest3-Control-Layout.png` in both archives and displayed prominently on the repository page.

## Why this build feels different

- **Full free aim:** point and fire naturally with either hand instead of steering a conventional crosshair.
- **Two-handed weapons:** shoulder shotguns and rifles, then use the forward hand to stabilize and direct the barrel.
- **Physical fighting:** motion-tracked bare fists, brass knuckles, bats, and melee weapons turn close combat into a genuine VR interaction.
- **Free aim while driving:** keep driving normally while independently aiming and firing supported submachine guns through the vehicle window.
- **High-definition clarity:** VR-oriented texture, cvar, scope, reticle, muzzle-flash, and rendering choices deliver a much cleaner image.
- **Performance-aware tuning:** strong visual quality without needlessly spending all available GPU headroom, with room for stronger systems to push texture quality further.
- **Immersive interface:** an auto-hiding HUD reveals itself when useful and disappears when you want an unobstructed view of San Andreas.
- **Intuitive Quest controls:** weapon handling, HUD gestures, vehicle firing, camera controls, phone interactions, and contextual D-pad actions are designed around motion controllers.
- **Per-vehicle comfort:** camera offsets are remembered by vehicle model so cars, bikes, boats, and aircraft can retain their own tuned viewpoint.

## Installer

1. Close GTA San Andreas DE and UEVR.
2. Extract the installer archive completely.
3. Run `INSTALL-SAVR.bat`.
4. Choose:
   - `1 — Auto Detect`: checks common Steam, Rockstar, Epic, and game-library locations.
   - `2 — Manual Path`: paste the GTA San Andreas DE folder containing `Gameface`.
5. Confirm the detected paths. The installer backs up an existing UEVR profile before copying files.

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

### 3. Optional startup guide

The control layout PNG is documentation, not a required runtime file. Keep it somewhere convenient or open it from the repository.

## First launch

1. Start GTA San Andreas DE.
2. Start UEVR and inject into `SanAndreas`/`Gameface` using OpenXR.
3. The included profile, bindings, plugin, and Lua scripts load automatically.
4. Use the separate SA Improved Settings utility for optional and experimental flags.

See [CONTROLS.md](CONTROLS.md), [FEATURES.md](FEATURES.md), and [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Questions and bug reports

For questions, bug reports, feedback, or other help, send a direct message on X to [@Yobrohy2o](https://x.com/Yobrohy2o).

## Updating and uninstalling

- Installer updates create a timestamped backup under `%APPDATA%\UnrealVRMod\_Backups`.
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
