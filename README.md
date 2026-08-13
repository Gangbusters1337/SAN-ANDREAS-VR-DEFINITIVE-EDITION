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

- Point and fire firearms naturally with either controller, with weapon-origin aiming and visible shot trails.
- Grip shotguns and rifles with both hands and stabilize the barrel instead of fighting a flat-screen crosshair.
- Swing bats and melee weapons, punch with either fist, and use brass knuckles with independent hand control.
- Fight and aim from supported vehicles while keeping normal driving and acceleration available.
- Store weapons around the waist with per-weapon recall and use per-vehicle camera positions.
- Use the auto-hiding HUD, the A+X control guide, touch/D-pad shortcuts, phone grip tap, and Quest-friendly pause controls.
- Enjoy sharper textures, cleaner effects, a usable VR sniper scope, improved muzzle presentation, and graphics settings that leave room for extra quality.

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

## Why this build feels different

- **Full free aim:** point and fire naturally with either hand instead of steering a conventional crosshair.
- **Two-handed weapons:** shoulder shotguns and rifles, then use the forward hand to stabilize and direct the barrel.
- **Physical fighting:** motion-tracked bare fists, brass knuckles, bats, and melee weapons turn close combat into a genuine VR interaction.
- **Responsive combat feedback:** independent left/right strikes, motion-gated fist contact, weapon-aware impact sounds, and buffered semi-automatic trigger taps make combat more immediate without replacing GTA's native ammo, damage, recoil, or weapon cadence.
- **Free aim while driving:** keep driving normally while independently aiming and firing supported submachine guns through the vehicle window.
- **High-definition clarity:** VR-oriented texture, cvar, scope, reticle, muzzle-flash, and rendering choices deliver a much cleaner image.
- **Performance-aware tuning:** strong visual quality without needlessly spending all available GPU headroom, with room for stronger systems to push texture quality further.
- **Immersive interface:** an auto-hiding HUD reveals itself when useful and disappears when you want an unobstructed view of San Andreas.
- **Intuitive Quest controls:** weapon handling, HUD gestures, vehicle firing, camera controls, phone interactions, and contextual D-pad actions are designed around motion controllers.
- **Per-vehicle comfort:** camera offsets are remembered by vehicle model so cars, bikes, boats, and aircraft can retain their own tuned viewpoint.

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
