SA Improved Settings
====================

Run:
  SA-Improved-Settings.bat

Start Game And VR:
  Use Browse once to pick the GTA San Andreas Definitive Edition executable
  and UEVRInjector.exe if the app cannot find them automatically.

  Launch Game starts the game.
  Open UEVR starts the injector.
  Open UEVR prompts you to press Inject in UEVR after the injector opens.

  Auto-inject is not enabled because the supported UEVR flow still uses the
  injector frontend: select the running game process, confirm runtime, Inject.

Saved settings:
  %APPDATA%\UnrealVRMod\SanAndreas\UEVR_GTASADE_config.txt

App launcher settings:
  %APPDATA%\UnrealVRMod\SanAndreas\SAImprovedSettings.ini

Active status reported by the injected plugin:
  %APPDATA%\UnrealVRMod\SanAndreas\UEVR_GTASADE_status.txt

The app is split by player-facing purpose:
  Core Combat Feel
  VR Comfort And Camera
  Restart/Reinject Required
  Diagnostics

Saved means the value in the config file.
Active means the last value reported by the injected plugin.

Settings marked restart/reinject are intentionally separated because they
touch startup/runtime patch behavior and are safer to apply next session
instead of pretending they changed live.

The app makes a timestamped backup before saving:
  %APPDATA%\UnrealVRMod\SanAndreas\_Backups\settings-app
