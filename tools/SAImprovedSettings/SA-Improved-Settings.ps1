param(
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$ProfilePath = Join-Path $env:APPDATA "UnrealVRMod\SanAndreas"
$ConfigPath = Join-Path $ProfilePath "UEVR_GTASADE_config.txt"
$StatusPath = Join-Path $ProfilePath "UEVR_GTASADE_status.txt"
$LogPath = Join-Path $ProfilePath "log.txt"
$BackupRoot = Join-Path $ProfilePath "_Backups\settings-app"
$AppSettingsPath = Join-Path $ProfilePath "SAImprovedSettings.ini"

$FlagDefinitions = @(
    [pscustomobject]@{
        Key = "EnableCombatAssist"
        Label = "Combat assist"
        Section = "Core Combat Feel"
        Help = "Weapon/range/damage tuning. Restart or reinject is safest because it touches memory patches."
        Default = $false
        ApplyMode = "RestartOrReinject"
    },
    [pscustomobject]@{
        Key = "EnableCombatAssistAmmo"
        Label = "Ammo assist"
        Section = "Core Combat Feel"
        Help = "Ammo assist is startup-sensitive because vehicle and mission weapon state can use separate paths."
        Default = $true
        ApplyMode = "RestartOrReinject"
    },
    [pscustomobject]@{
        Key = "EnableCombatAssistDamage"
        Label = "Weapon damage boost"
        Section = "Core Combat Feel"
        Help = "Optional damage boost. Off means standard GTA damage."
        Default = $false
        ApplyMode = "RestartOrReinject"
    },
    [pscustomobject]@{
        Key = "EnableCombatAssistWeaponSkill"
        Label = "Weapon skill assist"
        Section = "Core Combat Feel"
        Help = "Keeps CJ weapon skill near the tested value. Separate from weapon table stat edits."
        Default = $false
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableWeaponNoSpread"
        Label = "Weapon no spread"
        Section = "Core Combat Feel"
        Help = "Forces bullet weapon table spread to zero and raises accuracy while combat assist is active."
        Default = $false
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableSilencedPistolAimStability"
        Label = "Silenced pistol aim stability"
        Section = "Core Combat Feel"
        Help = "Keeps the last good silenced pistol aim vector during reload/head-motion aim snaps."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "DebugSpreadProbe"
        Label = "Debug spread probe"
        Section = "Diagnostics"
        Help = "Logs weapon table spread, accuracy, and aim-vector mismatch once per detected shot."
        Default = $false
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableUtilityWeaponAimBypass"
        Label = "Utility weapon aim bypass"
        Section = "Core Combat Feel"
        Help = "Skips aim-latch and gun-mesh aim rewrite for spraycan/fire extinguisher so special utility weapons do not trap VR weapon cycling."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableUtilityWeaponCycleReset"
        Label = "Utility weapon cycle reset"
        Section = "VR Controls"
        Help = "For spraycan/fire extinguisher, briefly releases virtual aim/fire and uses the PC next-weapon path when grip cycling."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableDirectWeaponCycle"
        Label = "Direct weapon cycle"
        Section = "VR Controls"
        Help = "Cycles the native selected weapon slot directly after validating the player weapon table."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableLegacyDPadModulator"
        Label = "Legacy grip D-pad mapping [experimental]"
        Section = "VR Controls"
        Help = "Off by default. The older grip D-pad mapper rewrites face buttons and can conflict with dual grip aim/fire."
        Default = $false
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableABWeaponCycleTest"
        Label = "Right A / Left X weapon cycle test"
        Section = "VR Controls"
        Help = "Diagnostic mode: physical Right A sends next-weapon shoulder input and physical Left X sends previous-weapon shoulder input."
        Default = $false
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableChordPauseMenu"
        Label = "Right B + Left Y pause chord"
        Section = "VR Controls"
        Help = "Press physical Right B and Left Y together to send Start/pause without moving normal single-button actions."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableChordHudToggle"
		Label = "A + X control guide"
        Section = "VR Controls"
		Help = "On foot, press physical Right A and Left X together to open or close the full VR control guide."
		Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnablePauseUiAutoShow"
        Label = "Show UI while paused"
        Section = "VR Controls"
        Help = "Shows UI for any game pause route, then restores hidden UI after returning to gameplay only when it revealed the UI."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableHealthRecovery"
        Label = "Recover health to 50% after 10 seconds"
        Section = "Core Combat Feel"
        Help = "After 10 seconds without taking damage, restores CJ to half of current maximum health. It never heals above half."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableHudAutoHide"
        Label = "Auto-hide HUD after 20 seconds"
        Section = "VR Controls"
        Help = "Hides a visible HUD after 20 seconds of gameplay. A quick second A+X chord pins the HUD without a timer; repeat the double chord to unpin it."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableShortPressCameraSwitch"
        Label = "Short press left Quest menu camera switch"
        Section = "VR Controls"
        Help = "A short press of the left Quest menu button cycles GTA camera view. A long press does nothing here so Virtual Desktop can keep its normal system menu hold."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableFirstPersonCameraLock"
        Label = "Lock first-person camera"
        Section = "VR Comfort and Camera"
        Help = "Restores GTA's Close/FPS camera view when an unwanted camera cycle occurs. It leaves cutscenes, camera weapon mode, and drive-by aim alone."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableVehicleFaceButtonFire"
        Label = "Vehicle X/A left/right fire [experimental]"
        Section = "VR Controls"
        Help = "Physical left X sends the virtual left shoulder action and physical right A sends the virtual right shoulder action only while in a vehicle. Face button inputs are consumed."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableR3LeftStickDpad"
        Label = "R3 + left stick D-pad [experimental]"
        Section = "VR Controls"
        Help = "On foot, hold physical R3 and tilt the left stick to send D-pad directions instead of walking."
        Default = $false
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "DebugInputLayerProbe"
        Label = "Control stack input log"
        Section = "Diagnostics"
        Help = "Logs each press edge using the physical Quest button and the virtual Xbox button it became."
        Default = $false
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableShowUiAtStartup"
        Label = "Show UI at start"
        Section = "VR Controls"
        Help = "Shows UEVR game UI when the game starts. Pause can also temporarily reveal UI through its separate setting."
        Default = $true
        ApplyMode = "RestartOrReinject"
    },
    [pscustomobject]@{
        Key = "EnableAimAlignment"
        Label = "Aim alignment"
        Section = "Core Combat Feel"
        Help = "Keeps shots aligned with VR aim. Can usually apply after config reload."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableCameraProfiles"
        Label = "Camera profiles"
        Section = "VR Comfort And Camera"
        Help = "Uses separate camera/profile handling. Experimental but easy to disable."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableBodyVisibility"
        Label = "Body visibility"
        Section = "VR Comfort And Camera"
        Help = "Controls body visibility logic for comfort/presence."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableBulletTraceHidden"
        Label = "Hide bullet trace"
        Section = "Restart/Reinject Required"
        Help = "Hides the visible bullet trace. Startup-only right now."
        Default = $true
        ApplyMode = "RestartOrReinject"
    },
    [pscustomobject]@{
        Key = "debugMod"
        StatusKey = "DebugLogging"
        Label = "Debug logging"
        Section = "Diagnostics"
        Help = "Adds plugin logs. Useful when testing one problem, noisy otherwise."
        Default = $false
        ApplyMode = "LiveOnConfigReload"
    }
)

$FlagDefinitions += @(
    [pscustomobject]@{
        Key = "EnableVrScope"
        Label = "VR sniper scope"
        Section = "VR Weapons"
        Help = "Shows the motion-attached VR scope and red aiming point on the sniper rifle."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableCompactWeaponReticle"
        Label = "Small aiming reticle"
        Section = "VR Weapons"
        Help = "Uses the compact in-game aiming reticle."
        Default = $true
        ApplyMode = "RestartOrReinject"
    },
    [pscustomobject]@{
        Key = "EnableDualGripAimFire"
        Label = "Side-matched grip aim and trigger fire"
        Section = "VR Controls"
        Help = "Either grip aims from that side and either trigger fires from the matching side."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableAlternateWeaponHandsVisibility"
        Label = "Hide idle weapon and aiming hands"
        Section = "VR Controls"
        Help = "Hides the floating weapon while idle and hides animated hands while actively aiming or firing."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableGripWeaponCycle"
        Label = "Tap left grip to cycle weapons"
        Section = "VR Controls"
        Help = "A short left-grip tap advances to the next weapon; holding the grip remains aim."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    },
    [pscustomobject]@{
        Key = "EnableAircraftNativeControls"
        Label = "Aircraft controls"
        Section = "VR Controls"
        Help = "Uses the tested aircraft-specific control layer without changing normal driving controls."
        Default = $true
        ApplyMode = "LiveOnConfigReload"
    }
)

$visibleFlagKeys = @(
    "EnableCombatAssist",
    "EnableAimAlignment",
    "EnableWeaponNoSpread",
    "EnableCombatAssistAmmo",
    "EnableCombatAssistDamage",
    "EnableHealthRecovery",
    "EnableVrScope",
    "EnableCompactWeaponReticle",
    "EnableBulletTraceHidden",
    "EnableDualGripAimFire",
    "EnableAlternateWeaponHandsVisibility",
    "EnableGripWeaponCycle",
    "EnableChordPauseMenu",
    "EnableChordHudToggle",
    "EnableVehicleFaceButtonFire",
    "EnableAircraftNativeControls",
    "EnablePauseUiAutoShow",
    "EnableHudAutoHide",
    "EnableShowUiAtStartup",
    "EnableFirstPersonCameraLock",
    "EnableCameraProfiles",
    "EnableBodyVisibility"
)
$FlagDefinitions = @($FlagDefinitions | Where-Object { $_.Key -in $visibleFlagKeys })

$flagPresentation = @{
    EnableCombatAssist = @("Combat range and accuracy hooks [Recommended]", "Combat and Aiming", "Required foundation for player-only spread bypass, extended bullet range, and related weapon tuning.", "LiveOnConfigReload")
    EnableAimAlignment = @("Align shots with VR aim [Recommended]", "Combat and Aiming", "Keeps the game shot direction aligned with the motion-controller weapon direction.", "LiveOnConfigReload")
    EnableWeaponNoSpread = @("Remove player bullet spread [Recommended]", "Combat and Aiming", "Removes native random bullet spread for CJ while leaving enemy accuracy unchanged.", "LiveOnConfigReload")
    EnableCombatAssistAmmo = @("Unlimited reserve ammo", "Combat and Aiming", "Keeps player reserve ammunition supplied. Restart or reinject after changing it.", "RestartOrReinject")
    EnableCombatAssistDamage = @("Double weapon damage", "Combat and Aiming", "Optional player weapon damage boost. Off keeps standard damage.", "LiveOnConfigReload")
    EnableHealthRecovery = @("Recover to 50% health after 10 seconds", "Combat and Aiming", "After ten damage-free seconds, restores CJ to half of current maximum health.", "LiveOnConfigReload")
    EnableVrScope = @("VR sniper scope [Recommended]", "VR Weapons", "Shows the motion-attached VR sniper scope and aiming point.", "LiveOnConfigReload")
    EnableCompactWeaponReticle = @("Small aiming reticle", "VR Weapons", "Uses the compact in-game aiming reticle.", "RestartOrReinject")
    EnableBulletTraceHidden = @("Hide bullet tracers", "VR Weapons", "Hides the visible bullet trace effect.", "RestartOrReinject")
    EnableChordPauseMenu = @("B + Y opens pause menu", "VR Controls", "Press physical right B and left Y together to pause.", "LiveOnConfigReload")
    EnableChordHudToggle = @("A + X control-guide overlay", "VR Controls", "On foot, press Right A + Left X to open or close the full VR control guide.", "LiveOnConfigReload")
    EnableVehicleFaceButtonFire = @("Vehicle X/A left/right fire", "VR Controls", "In vehicles, physical X fires left and physical A fires right.", "LiveOnConfigReload")
    EnableAircraftNativeControls = @("Aircraft controls [Recommended]", "VR Controls", "Uses the tested aircraft-specific control layer.", "LiveOnConfigReload")
    EnablePauseUiAutoShow = @("Show HUD for pause and result screens", "HUD and Camera", "Temporarily reveals the HUD when gameplay is paused or a mission result needs input.", "LiveOnConfigReload")
    EnableHudAutoHide = @("Auto-hide HUD after 20 seconds", "HUD and Camera", "Hides a visible HUD after twenty seconds unless the HUD is pinned.", "LiveOnConfigReload")
    EnableShowUiAtStartup = @("Show HUD when game starts", "HUD and Camera", "Sets the initial HUD state for the next launch.", "RestartOrReinject")
    EnableFirstPersonCameraLock = @("Keep first-person camera [Recommended]", "HUD and Camera", "Restores the intended close first-person camera after unwanted camera changes.", "LiveOnConfigReload")
    EnableCameraProfiles = @("Automatic camera profiles [Experimental]", "Experimental", "Uses separate saved offsets for on-foot and vehicle camera states.", "LiveOnConfigReload")
    EnableBodyVisibility = @("VR body visibility updates [Experimental]", "Experimental", "Runs the configured VR body visibility behavior.", "LiveOnConfigReload")
}

foreach ($definition in $FlagDefinitions) {
    $presentation = $flagPresentation[$definition.Key]
    if ($presentation) {
        $definition.Label = $presentation[0]
        $definition.Section = $presentation[1]
        $definition.Help = $presentation[2]
        $definition.ApplyMode = $presentation[3]
    }
}

function ConvertTo-BoolText([bool]$Value) {
    if ($Value) { return "true" }
    return "false"
}

function ConvertFrom-BoolText($Value, [bool]$Default) {
    if ($null -eq $Value) { return $Default }
    $text = [string]$Value
    return $text.Trim().ToLowerInvariant() -in @("true", "1", "yes", "on")
}

function Read-KeyValueFile([string]$Path) {
    $map = @{}
    if (-not (Test-Path -LiteralPath $Path)) {
        return $map
    }

    foreach ($line in Get-Content -LiteralPath $Path) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -eq 0 -or $trimmed.StartsWith("[") -or $trimmed.StartsWith(";") -or $trimmed.StartsWith("#")) {
            continue
        }

        $parts = $trimmed.Split("=", 2)
        if ($parts.Count -eq 2) {
            $map[$parts[0].Trim()] = $parts[1].Trim()
        }
    }

    return $map
}

function Ensure-ConfigFile {
    if (Test-Path -LiteralPath $ConfigPath) {
        return
    }

    New-Item -ItemType Directory -Force -Path $ProfilePath | Out-Null
    @"
[Feature Flags :] -- Edited by SA Improved Settings. Restart/reinject flags apply next session.
EnableCombatAssist=true
EnableAimAlignment=true
EnableWeaponNoSpread=false
EnableCombatAssistAmmo=true
EnableCombatAssistDamage=false
EnableHealthRecovery=true
EnableCompactWeaponReticle=true
EnableVrScope=true
EnableDualGripAimFire=true
EnableAlternateWeaponHandsVisibility=true
EnableGripWeaponCycle=false
EnableChordPauseMenu=true
EnableChordHudToggle=true
EnablePauseUiAutoShow=true
EnableHudAutoHide=true
EnableFirstPersonCameraLock=true
EnableVehicleFaceButtonFire=true
EnableAircraftNativeControls=true
EnableShowUiAtStartup=true
EnableCameraProfiles=true
EnableBodyVisibility=true
EnableBulletTraceHidden=true
debugMod=false

"@ | Set-Content -LiteralPath $ConfigPath -Encoding ASCII
}

function Get-ConfigValues {
    Ensure-ConfigFile
    return Read-KeyValueFile $ConfigPath
}

function Get-StatusValues {
    return Read-KeyValueFile $StatusPath
}

function Get-AppSettings {
    return Read-KeyValueFile $AppSettingsPath
}

function Save-AppSettings([hashtable]$Values) {
    New-Item -ItemType Directory -Force -Path $ProfilePath | Out-Null
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add("[SA Improved Settings]")
    foreach ($key in ($Values.Keys | Sort-Object)) {
        $lines.Add("$key=$($Values[$key])")
    }
    Set-Content -LiteralPath $AppSettingsPath -Value $lines -Encoding ASCII
}

function Find-UevrInjector {
    $settings = Get-AppSettings
    if ($settings["UevrInjectorPath"] -and (Test-Path -LiteralPath $settings["UevrInjectorPath"])) {
        return $settings["UevrInjectorPath"]
    }

    $candidates = @(
        (Join-Path $PSScriptRoot "UEVRInjector.exe"),
        (Join-Path $env:USERPROFILE "Downloads\Compressed\uevr\UEVRInjector.exe"),
        (Join-Path $env:USERPROFILE "Downloads\uevr\UEVRInjector.exe")
    )

    foreach ($path in $candidates) {
        if (Test-Path -LiteralPath $path) {
            return $path
        }
    }

    $downloadMatches = @(Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE "Downloads") -Recurse -File -Filter "UEVRInjector.exe" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending)
    if ($downloadMatches.Count -gt 0) {
        return $downloadMatches[0].FullName
    }

    return ""
}

function Find-RunningGamePath {
    $names = @("SanAndreas", "SanAndreas-Win64-Shipping", "Gameface", "Gameface-Win64-Shipping")
    foreach ($name in $names) {
        $processes = @(Get-Process -Name $name -ErrorAction SilentlyContinue)
        foreach ($process in $processes) {
            try {
                if ($process.Path -and (Test-Path -LiteralPath $process.Path)) {
                    return $process.Path
                }
            } catch {}
        }
    }
    return ""
}

function Find-GameExecutable {
    $settings = Get-AppSettings
    if ($settings["GameExePath"] -and (Test-Path -LiteralPath $settings["GameExePath"])) {
        return $settings["GameExePath"]
    }
    if ($settings["GameFolderPath"]) {
        $fromFolder = Join-Path $settings["GameFolderPath"] "Gameface\Binaries\Win64\SanAndreas.exe"
        if (Test-Path -LiteralPath $fromFolder) {
            return $fromFolder
        }
    }

    $running = Find-RunningGamePath
    if ($running) {
        return $running
    }

    $commonRoots = @(
        (Join-Path $env:ProgramFiles "Rockstar Games"),
        (Join-Path ${env:ProgramFiles(x86)} "Steam\steamapps\common"),
        (Join-Path $env:ProgramFiles "Epic Games")
    )

    foreach ($root in $commonRoots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $matches = @(Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match "SanAndreas|Gameface" -and $_.Extension -eq ".exe" } |
            Sort-Object LastWriteTime -Descending)
        if ($matches.Count -gt 0) {
            return $matches[0].FullName
        }
    }

    return ""
}

function Select-ExecutablePath([string]$Title, [string]$InitialPath = "") {
    $dialog = [System.Windows.Forms.OpenFileDialog]::new()
    $dialog.Title = $Title
    $dialog.Filter = "Executable files (*.exe)|*.exe|All files (*.*)|*.*"
    if ($InitialPath -and (Test-Path -LiteralPath $InitialPath)) {
        $dialog.InitialDirectory = Split-Path -Parent $InitialPath
    }
    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
        return $dialog.FileName
    }
    return ""
}

function Save-LaunchPaths([string]$GamePath, [string]$UevrPath) {
    $settings = Get-AppSettings
    if ($GamePath) { $settings["GameExePath"] = $GamePath }
    if ($GamePath -and ($GamePath -match '(?i)\\Gameface\\Binaries\\Win64\\SanAndreas\.exe$')) {
        $settings["GameFolderPath"] = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $GamePath)))
    }
    if ($UevrPath) { $settings["UevrInjectorPath"] = $UevrPath }
    Save-AppSettings $settings
}

function Launch-Path([string]$Path, [string]$FriendlyName) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) {
        throw "$FriendlyName path is missing. Use Browse first."
    }
    if ([System.IO.Path]::GetFileName($Path) -ieq "SanAndreas.exe") {
        $guardedLauncher = Join-Path $ProfilePath "SAVR-Launch.ps1"
        if (Test-Path -LiteralPath $guardedLauncher -PathType Leaf) {
            $arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$guardedLauncher`" -GameExe `"$Path`""
            Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList $arguments
            return
        }
    }
    Start-Process -FilePath $Path -WorkingDirectory (Split-Path -Parent $Path)
}

function Backup-Config {
    if (-not (Test-Path -LiteralPath $ConfigPath)) {
        return $null
    }

    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $dir = Join-Path $BackupRoot $stamp
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $target = Join-Path $dir "UEVR_GTASADE_config.txt.bak"
    Copy-Item -LiteralPath $ConfigPath -Destination $target -Force
    return $target
}

function Save-ConfigValues($Rows) {
    Ensure-ConfigFile
    $backup = Backup-Config
    $lines = [System.Collections.Generic.List[string]]::new()
    if (Test-Path -LiteralPath $ConfigPath) {
        foreach ($line in Get-Content -LiteralPath $ConfigPath) {
            $lines.Add($line)
        }
    }

    $desired = @{}
    foreach ($row in $Rows) {
        $desired[$row.Definition.Key] = ConvertTo-BoolText $row.CheckBox.Checked
    }

    $seen = @{}
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $trimmed = $lines[$i].Trim()
        $parts = $trimmed.Split("=", 2)
        if ($parts.Count -eq 2) {
            $key = $parts[0].Trim()
            if ($desired.ContainsKey($key)) {
                $lines[$i] = "$key=$($desired[$key])"
                $seen[$key] = $true
            }
        }
    }

    $missing = @($desired.Keys | Where-Object { -not $seen.ContainsKey($_) })
    if ($missing.Count -gt 0) {
        $insert = [System.Collections.Generic.List[string]]::new()
        $insert.Add("")
        $insert.Add("[Feature Flags :] -- Edited by SA Improved Settings. Restart/reinject flags apply next session.")
        foreach ($key in $missing) {
            $insert.Add("$key=$($desired[$key])")
        }

        $newLines = [System.Collections.Generic.List[string]]::new()
        foreach ($line in $insert) { $newLines.Add($line) }
        foreach ($line in $lines) { $newLines.Add($line) }
        $lines = $newLines
    }

    Set-Content -LiteralPath $ConfigPath -Value $lines -Encoding ASCII
    return $backup
}

function Get-LastLogLines([int]$Count = 16) {
    if (-not (Test-Path -LiteralPath $LogPath)) {
        return "No UEVR log found yet."
    }

    $patterns = "Feature flag|Loaded flags|UEVR_GTASADE|GTASADE_FeatureFlags|setting file has been modified|Plugin Settings Updated"
    $matches = Select-String -LiteralPath $LogPath -Pattern $patterns -CaseSensitive:$false | Select-Object -Last $Count
    if (-not $matches) {
        return "No recent feature/status log lines found."
    }

    return ($matches | ForEach-Object { $_.Line }) -join [Environment]::NewLine
}

function Make-Label([string]$Text, [int]$Width, [System.Drawing.Font]$Font = $null) {
    $label = [System.Windows.Forms.Label]::new()
    $label.Text = $Text
    $label.AutoSize = $false
    $label.Width = $Width
    $label.Height = 34
    $label.TextAlign = [System.Drawing.ContentAlignment]::MiddleLeft
    if ($Font) { $label.Font = $Font }
    return $label
}

function Set-StatusColor($Label, [string]$Text) {
    $Label.Text = $Text
    if ($Text -match "Restart|Reinject|Pending") {
        $Label.ForeColor = [System.Drawing.Color]::FromArgb(180, 70, 40)
    } elseif ($Text -match "Active|Live|Synced") {
        $Label.ForeColor = [System.Drawing.Color]::FromArgb(30, 110, 65)
    } else {
        $Label.ForeColor = [System.Drawing.Color]::FromArgb(90, 90, 90)
    }
}

function Add-LaunchSection($Parent) {
    $section = [System.Windows.Forms.GroupBox]::new()
    $section.Text = "Start Game And VR"
    $section.Font = [System.Drawing.Font]::new("Segoe UI", 10, [System.Drawing.FontStyle]::Bold)
    $section.Dock = [System.Windows.Forms.DockStyle]::Top
    $section.Padding = [System.Windows.Forms.Padding]::new(12)
    $section.Width = 850
    $section.Height = 176

    $table = [System.Windows.Forms.TableLayoutPanel]::new()
    $table.Dock = [System.Windows.Forms.DockStyle]::Fill
    $table.ColumnCount = 4
    $table.RowCount = 4
    $table.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new([System.Windows.Forms.SizeType]::Absolute, 105)) | Out-Null
    $table.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new([System.Windows.Forms.SizeType]::Percent, 100)) | Out-Null
    $table.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new([System.Windows.Forms.SizeType]::Absolute, 92)) | Out-Null
    $table.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new([System.Windows.Forms.SizeType]::Absolute, 198)) | Out-Null
    $table.RowStyles.Add([System.Windows.Forms.RowStyle]::new([System.Windows.Forms.SizeType]::Absolute, 34)) | Out-Null
    $table.RowStyles.Add([System.Windows.Forms.RowStyle]::new([System.Windows.Forms.SizeType]::Absolute, 34)) | Out-Null
    $table.RowStyles.Add([System.Windows.Forms.RowStyle]::new([System.Windows.Forms.SizeType]::Absolute, 42)) | Out-Null
    $table.RowStyles.Add([System.Windows.Forms.RowStyle]::new([System.Windows.Forms.SizeType]::Percent, 100)) | Out-Null

    $gameLabel = Make-Label "Game exe" 95
    $gameText = [System.Windows.Forms.TextBox]::new()
    $gameText.Dock = [System.Windows.Forms.DockStyle]::Fill
    $gameText.ReadOnly = $true

    $gameBrowse = [System.Windows.Forms.Button]::new()
    $gameBrowse.Text = "Browse"
    $gameBrowse.Width = 82
    $gameBrowse.Height = 26

    $launchGame = [System.Windows.Forms.Button]::new()
    $launchGame.Text = "Launch Game"
    $launchGame.Width = 184
    $launchGame.Height = 28

    $uevrLabel = Make-Label "UEVR" 95
    $uevrText = [System.Windows.Forms.TextBox]::new()
    $uevrText.Dock = [System.Windows.Forms.DockStyle]::Fill
    $uevrText.ReadOnly = $true

    $uevrBrowse = [System.Windows.Forms.Button]::new()
    $uevrBrowse.Text = "Browse"
    $uevrBrowse.Width = 82
    $uevrBrowse.Height = 26

    $openUevr = [System.Windows.Forms.Button]::new()
    $openUevr.Text = "Open UEVR"
    $openUevr.Width = 184
    $openUevr.Height = 28

    $instruction = Make-Label "Flow: launch the game, open UEVR, select the San Andreas/Gameface process, then press Inject. Auto-inject is not enabled because UEVR does not expose a stable supported command for it." 610
    $instruction.Height = 48
    $instruction.ForeColor = [System.Drawing.Color]::FromArgb(75, 75, 75)

    $launchState = Make-Label "" 184
    $launchState.Height = 48
    $launchState.ForeColor = [System.Drawing.Color]::FromArgb(70, 70, 70)

    $table.Controls.Add($gameLabel, 0, 0)
    $table.Controls.Add($gameText, 1, 0)
    $table.Controls.Add($gameBrowse, 2, 0)
    $table.Controls.Add($launchGame, 3, 0)
    $table.Controls.Add($uevrLabel, 0, 1)
    $table.Controls.Add($uevrText, 1, 1)
    $table.Controls.Add($uevrBrowse, 2, 1)
    $table.Controls.Add($openUevr, 3, 1)
    $table.Controls.Add($instruction, 1, 2)
    $table.SetColumnSpan($instruction, 2)
    $table.Controls.Add($launchState, 3, 3)

    $section.Controls.Add($table)
    $Parent.Controls.Add($section)

    return [pscustomobject]@{
        GameText = $gameText
        UevrText = $uevrText
        GameBrowse = $gameBrowse
        UevrBrowse = $uevrBrowse
        LaunchGame = $launchGame
        OpenUevr = $openUevr
        State = $launchState
    }
}

function Refresh-LaunchUi($LaunchUi) {
    $gamePath = Find-GameExecutable
    $uevrPath = Find-UevrInjector
    $LaunchUi.GameText.Text = $gamePath
    $LaunchUi.UevrText.Text = $uevrPath
    if ($gamePath -and $uevrPath) {
        $LaunchUi.State.Text = "Ready"
        $LaunchUi.State.ForeColor = [System.Drawing.Color]::FromArgb(35, 105, 65)
    } elseif ($gamePath) {
        $LaunchUi.State.Text = "Pick UEVR path"
        $LaunchUi.State.ForeColor = [System.Drawing.Color]::FromArgb(145, 80, 30)
    } elseif ($uevrPath) {
        $LaunchUi.State.Text = "Pick game path"
        $LaunchUi.State.ForeColor = [System.Drawing.Color]::FromArgb(145, 80, 30)
    } else {
        $LaunchUi.State.Text = "Browse paths once"
        $LaunchUi.State.ForeColor = [System.Drawing.Color]::FromArgb(145, 80, 30)
    }
}

function Show-InjectPrompt {
    [System.Windows.Forms.MessageBox]::Show(
        "When San Andreas is running:`n`n1. In UEVR, select the San Andreas/Gameface process.`n2. Confirm the runtime you use for your headset.`n3. Press Inject.`n`nThis app opened the reliable parts. UEVR injection itself still needs the frontend button.",
        "Ready for UEVR Inject",
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Information
    ) | Out-Null
}

function Add-FlagSection($Parent, [string]$SectionName, $Definitions, $Rows) {
    $section = [System.Windows.Forms.GroupBox]::new()
    $section.Text = $SectionName
    $section.Font = [System.Drawing.Font]::new("Segoe UI", 10, [System.Drawing.FontStyle]::Bold)
    $section.Dock = [System.Windows.Forms.DockStyle]::Top
    $section.Padding = [System.Windows.Forms.Padding]::new(12)
    $section.Height = 44 + ($Definitions.Count * 58)

    $table = [System.Windows.Forms.TableLayoutPanel]::new()
    $table.Dock = [System.Windows.Forms.DockStyle]::Fill
    $table.ColumnCount = 4
    $table.RowCount = $Definitions.Count
    $table.AutoSize = $false
    $table.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new([System.Windows.Forms.SizeType]::Absolute, 190)) | Out-Null
    $table.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new([System.Windows.Forms.SizeType]::Absolute, 300)) | Out-Null
    $table.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new([System.Windows.Forms.SizeType]::Absolute, 165)) | Out-Null
    $table.ColumnStyles.Add([System.Windows.Forms.ColumnStyle]::new([System.Windows.Forms.SizeType]::Percent, 100)) | Out-Null

    foreach ($def in $Definitions) {
        $check = [System.Windows.Forms.CheckBox]::new()
        $labelText = $def.Label
        if ($def.ApplyMode -eq "RestartOrReinject") {
            $labelText = "$labelText NEEDS RESTART"
        }
        $check.Text = $labelText
        $check.AutoSize = $false
        $check.Width = 185
        $check.Height = 42
        $check.Font = [System.Drawing.Font]::new("Segoe UI", 10, [System.Drawing.FontStyle]::Regular)

        $help = Make-Label $def.Help 290
        $help.Font = [System.Drawing.Font]::new("Segoe UI", 8.5)
        $help.ForeColor = [System.Drawing.Color]::FromArgb(70, 70, 70)

        $savedActive = Make-Label "" 155
        $savedActive.Font = [System.Drawing.Font]::new("Segoe UI", 8.5)

        $apply = Make-Label "" 250
        $apply.Font = [System.Drawing.Font]::new("Segoe UI", 8.5, [System.Drawing.FontStyle]::Bold)

        $table.Controls.Add($check)
        $table.Controls.Add($help)
        $table.Controls.Add($savedActive)
        $table.Controls.Add($apply)

        $Rows.Add([pscustomobject]@{
            Definition = $def
            CheckBox = $check
            SavedActiveLabel = $savedActive
            ApplyLabel = $apply
        })
    }

    $section.Controls.Add($table)
    $Parent.Controls.Add($section)
}

function Refresh-Ui($Rows, $StatusBanner, $LogText) {
    $config = Get-ConfigValues
    $status = Get-StatusValues
    $runtimeStamp = $status["LastUpdated"]
    $runtimeReason = $status["Reason"]
    $anyRestart = ConvertFrom-BoolText $status["AnyRestartNeeded"] $false

    if ($runtimeStamp) {
        $banner = "Plugin status: last reported $runtimeStamp"
        if ($runtimeReason) { $banner += " ($runtimeReason)" }
        if ($anyRestart) { $banner += " - restart/reinject pending" }
        $StatusBanner.Text = $banner
        if ($anyRestart) {
            $StatusBanner.ForeColor = [System.Drawing.Color]::FromArgb(165, 65, 35)
        } else {
            $StatusBanner.ForeColor = [System.Drawing.Color]::FromArgb(35, 105, 65)
        }
    } else {
        $StatusBanner.Text = "Plugin status: not reported yet. Launch/inject the mod to see active values."
        $StatusBanner.ForeColor = [System.Drawing.Color]::FromArgb(110, 90, 35)
    }

    foreach ($row in $Rows) {
        $def = $row.Definition
        $statusKey = if ($def.StatusKey) { $def.StatusKey } else { $def.Key }
        $saved = ConvertFrom-BoolText $config[$def.Key] $def.Default
        $activeText = $status["${statusKey}_Active"]
        $active = ConvertFrom-BoolText $activeText $saved
        $needsRestart = ConvertFrom-BoolText $status["${statusKey}_NeedsRestart"] ($def.ApplyMode -eq "RestartOrReinject" -and $saved -ne $active)
        $applyMode = $status["${statusKey}_ApplyMode"]
        if (-not $applyMode) { $applyMode = $def.ApplyMode }

        $row.CheckBox.Checked = $saved
        $savedWord = if ($saved) { "On" } else { "Off" }
        $activeWord = if ($active) { "On" } else { "Off" }
        $row.SavedActiveLabel.Text = "Saved: $savedWord`nActive: $activeWord"

        if ($needsRestart) {
            Set-StatusColor $row.ApplyLabel "Restart/reinject pending"
        } elseif ($applyMode -eq "RestartOrReinject") {
            Set-StatusColor $row.ApplyLabel "Next restart/reinject"
        } else {
            Set-StatusColor $row.ApplyLabel "Applies while running"
        }
    }

    $LogText.Text = Get-LastLogLines 14
}

if ($SelfTest) {
    Ensure-ConfigFile
    $config = Get-ConfigValues
    $status = Get-StatusValues
    $gamePath = Find-GameExecutable
    $uevrPath = Find-UevrInjector
    $presentFlags = @($FlagDefinitions | Where-Object { $config.ContainsKey($_.Key) })
    $missingFlags = @($FlagDefinitions | Where-Object { -not $config.ContainsKey($_.Key) })
    "Config: $ConfigPath"
    "Status: $StatusPath"
    "Game exe: $gamePath"
    "UEVR injector: $uevrPath"
    "Flags in config: $($presentFlags.Count) / $($FlagDefinitions.Count)"
    if ($missingFlags.Count -gt 0) {
        "Missing keys: " + (($missingFlags | ForEach-Object { $_.Key }) -join ", ")
    }
    "Status timestamp: " + $status["LastUpdated"]
    exit 0
}

[System.Windows.Forms.Application]::EnableVisualStyles()

$form = [System.Windows.Forms.Form]::new()
$form.Text = "IMPROVEMENT OPTIONS"
$form.StartPosition = [System.Windows.Forms.FormStartPosition]::CenterScreen
$form.Size = [System.Drawing.Size]::new(920, 720)
$form.MinimumSize = [System.Drawing.Size]::new(840, 620)
$form.Font = [System.Drawing.Font]::new("Segoe UI", 9)

$main = [System.Windows.Forms.TableLayoutPanel]::new()
$main.Dock = [System.Windows.Forms.DockStyle]::Fill
$main.RowCount = 5
$main.ColumnCount = 1
$main.Padding = [System.Windows.Forms.Padding]::new(14)
$main.RowStyles.Add([System.Windows.Forms.RowStyle]::new([System.Windows.Forms.SizeType]::Absolute, 72)) | Out-Null
$main.RowStyles.Add([System.Windows.Forms.RowStyle]::new([System.Windows.Forms.SizeType]::Absolute, 180)) | Out-Null
$main.RowStyles.Add([System.Windows.Forms.RowStyle]::new([System.Windows.Forms.SizeType]::Percent, 100)) | Out-Null
$main.RowStyles.Add([System.Windows.Forms.RowStyle]::new([System.Windows.Forms.SizeType]::Absolute, 120)) | Out-Null
$main.RowStyles.Add([System.Windows.Forms.RowStyle]::new([System.Windows.Forms.SizeType]::Absolute, 48)) | Out-Null

$header = [System.Windows.Forms.Panel]::new()
$header.Dock = [System.Windows.Forms.DockStyle]::Fill
$title = Make-Label "IMPROVEMENT OPTIONS" 420 ([System.Drawing.Font]::new("Segoe UI", 16, [System.Drawing.FontStyle]::Bold))
$title.Location = [System.Drawing.Point]::new(0, 0)
$subtitle = Make-Label "Clear gameplay toggles, active status, and quick VR testing in one place." 700 ([System.Drawing.Font]::new("Segoe UI", 9))
$subtitle.Location = [System.Drawing.Point]::new(2, 34)
$subtitle.ForeColor = [System.Drawing.Color]::FromArgb(80, 80, 80)
$statusBanner = Make-Label "" 860 ([System.Drawing.Font]::new("Segoe UI", 9, [System.Drawing.FontStyle]::Bold))
$statusBanner.Location = [System.Drawing.Point]::new(2, 52)
$header.Controls.Add($title)
$header.Controls.Add($subtitle)
$header.Controls.Add($statusBanner)

$scroll = [System.Windows.Forms.Panel]::new()
$scroll.Dock = [System.Windows.Forms.DockStyle]::Fill
$scroll.AutoScroll = $true

$content = [System.Windows.Forms.FlowLayoutPanel]::new()
$content.FlowDirection = [System.Windows.Forms.FlowDirection]::TopDown
$content.WrapContents = $false
$content.Dock = [System.Windows.Forms.DockStyle]::Top
$content.AutoSize = $true
$content.Width = 860

$launchHost = [System.Windows.Forms.Panel]::new()
$launchHost.Dock = [System.Windows.Forms.DockStyle]::Fill
$launchUi = Add-LaunchSection $launchHost

$rows = [System.Collections.Generic.List[object]]::new()
foreach ($sectionName in @("Combat and Aiming", "VR Weapons", "VR Controls", "HUD and Camera", "Experimental")) {
    $defs = @($FlagDefinitions | Where-Object { $_.Section -eq $sectionName })
    Add-FlagSection $content $sectionName $defs $rows
}
$scroll.Controls.Add($content)

$logText = [System.Windows.Forms.TextBox]::new()
$logText.Dock = [System.Windows.Forms.DockStyle]::Fill
$logText.Multiline = $true
$logText.ReadOnly = $true
$logText.ScrollBars = [System.Windows.Forms.ScrollBars]::Vertical
$logText.Font = [System.Drawing.Font]::new("Consolas", 8.5)

$buttons = [System.Windows.Forms.FlowLayoutPanel]::new()
$buttons.Dock = [System.Windows.Forms.DockStyle]::Fill
$buttons.FlowDirection = [System.Windows.Forms.FlowDirection]::RightToLeft

$saveButton = [System.Windows.Forms.Button]::new()
$saveButton.Text = "Save Settings"
$saveButton.Width = 120
$saveButton.Height = 32

$refreshButton = [System.Windows.Forms.Button]::new()
$refreshButton.Text = "Refresh Status"
$refreshButton.Width = 120
$refreshButton.Height = 32

$openProfileButton = [System.Windows.Forms.Button]::new()
$openProfileButton.Text = "Open Folder"
$openProfileButton.Width = 110
$openProfileButton.Height = 32

$openLogButton = [System.Windows.Forms.Button]::new()
$openLogButton.Text = "Open Log"
$openLogButton.Width = 100
$openLogButton.Height = 32

$buttons.Controls.Add($saveButton)
$buttons.Controls.Add($refreshButton)
$buttons.Controls.Add($openProfileButton)
$buttons.Controls.Add($openLogButton)

$main.Controls.Add($header, 0, 0)
$main.Controls.Add($launchHost, 0, 1)
$main.Controls.Add($scroll, 0, 2)
$main.Controls.Add($logText, 0, 3)
$main.Controls.Add($buttons, 0, 4)
$form.Controls.Add($main)

$saveButton.Add_Click({
    try {
        $backup = Save-ConfigValues $rows
        Refresh-Ui $rows $statusBanner $logText
        $message = "Saved settings.`n`nSome settings can update while the mod is running. Restart/reinject settings apply next session."
        if ($backup) { $message += "`n`nBackup:`n$backup" }
        [System.Windows.Forms.MessageBox]::Show($message, "SA Improved Settings", [System.Windows.Forms.MessageBoxButtons]::OK, [System.Windows.Forms.MessageBoxIcon]::Information) | Out-Null
    } catch {
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, "Save failed", [System.Windows.Forms.MessageBoxButtons]::OK, [System.Windows.Forms.MessageBoxIcon]::Error) | Out-Null
    }
})

$refreshButton.Add_Click({ Refresh-Ui $rows $statusBanner $logText })
$openProfileButton.Add_Click({ if (Test-Path -LiteralPath $ProfilePath) { Start-Process explorer.exe $ProfilePath } })
$openLogButton.Add_Click({ if (Test-Path -LiteralPath $LogPath) { Start-Process notepad.exe $LogPath } })

$launchUi.GameBrowse.Add_Click({
    $selected = Select-ExecutablePath "Select GTA San Andreas Definitive Edition executable" $launchUi.GameText.Text
    if ($selected) {
        $launchUi.GameText.Text = $selected
        Save-LaunchPaths $launchUi.GameText.Text $launchUi.UevrText.Text
        Refresh-LaunchUi $launchUi
    }
})

$launchUi.UevrBrowse.Add_Click({
    $selected = Select-ExecutablePath "Select UEVRInjector.exe" $launchUi.UevrText.Text
    if ($selected) {
        $launchUi.UevrText.Text = $selected
        Save-LaunchPaths $launchUi.GameText.Text $launchUi.UevrText.Text
        Refresh-LaunchUi $launchUi
    }
})

$launchUi.LaunchGame.Add_Click({
    try {
        Launch-Path $launchUi.GameText.Text "Game"
        $launchUi.State.Text = "Game launched"
        $launchUi.State.ForeColor = [System.Drawing.Color]::FromArgb(35, 105, 65)
    } catch {
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, "Launch failed", [System.Windows.Forms.MessageBoxButtons]::OK, [System.Windows.Forms.MessageBoxIcon]::Error) | Out-Null
    }
})

$launchUi.OpenUevr.Add_Click({
    try {
        Launch-Path $launchUi.UevrText.Text "UEVR"
        $launchUi.State.Text = "UEVR opened"
        $launchUi.State.ForeColor = [System.Drawing.Color]::FromArgb(35, 105, 65)
        Show-InjectPrompt
    } catch {
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, "Launch failed", [System.Windows.Forms.MessageBoxButtons]::OK, [System.Windows.Forms.MessageBoxIcon]::Error) | Out-Null
    }
})

Refresh-LaunchUi $launchUi
Refresh-Ui $rows $statusBanner $logText
[void]$form.ShowDialog()
