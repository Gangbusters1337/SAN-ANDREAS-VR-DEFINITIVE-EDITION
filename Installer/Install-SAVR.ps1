param(
    [ValidateSet("Auto", "Manual")][string]$Mode,
    [string]$ProfilePath,
    [string]$GamePath,
    [switch]$NoPrompt,
    [switch]$SkipShortcuts
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$PackageRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$PayloadRoot = Join-Path $PackageRoot "Payload"
$LogPath = Join-Path $PackageRoot "install-log.txt"

function Write-Step([string]$Text) {
    Write-Host ""
    Write-Host "== $Text ==" -ForegroundColor Cyan
    Add-Content -LiteralPath $LogPath -Encoding UTF8 -Value "[$(Get-Date -Format s)] $Text"
}

function Normalize-GamePath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    $expanded = [Environment]::ExpandEnvironmentVariables($Path.Trim().Trim('"'))
    if (-not (Test-Path -LiteralPath $expanded -PathType Container)) { return $null }
    $resolved = (Resolve-Path -LiteralPath $expanded).Path
    if ((Split-Path -Leaf $resolved) -ieq "Gameface") { $resolved = Split-Path -Parent $resolved }
    $exe = Join-Path $resolved "Gameface\Binaries\Win64\SanAndreas.exe"
    $paks = Join-Path $resolved "Gameface\Content\Paks"
    if ((Test-Path -LiteralPath $exe -PathType Leaf) -and (Test-Path -LiteralPath $paks -PathType Container)) {
        return $resolved
    }
    return $null
}

function Get-AutoCandidates {
    $roots = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($root in @(
        "$env:ProgramFiles\Rockstar Games",
        "${env:ProgramFiles(x86)}\Rockstar Games",
        "$env:ProgramFiles\Steam\steamapps\common",
        "${env:ProgramFiles(x86)}\Steam\steamapps\common",
        "C:\Games", "D:\Games", "E:\Games",
        "C:\SteamLibrary\steamapps\common", "D:\SteamLibrary\steamapps\common", "E:\SteamLibrary\steamapps\common"
    )) {
        if ($root -and (Test-Path -LiteralPath $root -PathType Container)) { [void]$roots.Add($root) }
    }

    $steamVdf = "${env:ProgramFiles(x86)}\Steam\steamapps\libraryfolders.vdf"
    if (Test-Path -LiteralPath $steamVdf) {
        foreach ($line in Get-Content -LiteralPath $steamVdf) {
            if ($line -match '"path"\s+"([^"]+)"') {
                $library = $Matches[1] -replace '\\\\', '\'
                $common = Join-Path $library "steamapps\common"
                if (Test-Path -LiteralPath $common -PathType Container) { [void]$roots.Add($common) }
            }
        }
    }

    $results = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($root in $roots) {
        foreach ($candidate in @($root) + @(Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue | ForEach-Object FullName)) {
            $valid = Normalize-GamePath $candidate
            if ($valid) { [void]$results.Add($valid) }
            if (-not $valid) {
                foreach ($child in Get-ChildItem -LiteralPath $candidate -Directory -ErrorAction SilentlyContinue) {
                    $nested = Normalize-GamePath $child.FullName
                    if ($nested) { [void]$results.Add($nested) }
                }
            }
        }
    }
    return @($results)
}

function Read-ManualGamePath {
    while ($true) {
        $entry = Read-Host "Paste the GTA San Andreas Definitive Edition folder (the folder containing Gameface)"
        $valid = Normalize-GamePath $entry
        if ($valid) { return $valid }
        Write-Host "That folder does not contain Gameface\Binaries\Win64\SanAndreas.exe and Gameface\Content\Paks." -ForegroundColor Yellow
    }
}

function Select-AutoGamePath {
    Write-Step "Searching common game-library locations"
    $found = @(Get-AutoCandidates)
    if ($found.Count -eq 0) {
        Write-Host "No installation was found automatically. Switching to manual path mode." -ForegroundColor Yellow
        return Read-ManualGamePath
    }
    if ($found.Count -eq 1 -or $NoPrompt) { return $found[0] }
    for ($i = 0; $i -lt $found.Count; $i++) { Write-Host "[$($i + 1)] $($found[$i])" }
    while ($true) {
        $choice = Read-Host "Choose the installation number"
        if ($choice -match '^\d+$' -and [int]$choice -ge 1 -and [int]$choice -le $found.Count) {
            return $found[[int]$choice - 1]
        }
    }
}

try {
    Set-Content -LiteralPath $LogPath -Encoding UTF8 -Value "San Andreas VR Improved installer"
    if (-not (Test-Path -LiteralPath (Join-Path $PayloadRoot "UnrealVRMod\SanAndreas") -PathType Container)) {
        throw "Installer payload is incomplete. Extract the full installer archive before running it."
    }
    if (Get-Process SanAndreas -ErrorAction SilentlyContinue) {
        throw "SanAndreas.exe is running. Close the game and UEVR, then run the installer again."
    }

    if (-not $Mode) {
        Write-Host "San Andreas VR Improved - Beta Installer" -ForegroundColor Cyan
        Write-Host "[1] Auto Detect (recommended)"
        Write-Host "[2] Manual Paths"
        do { $choice = Read-Host "Choose 1 or 2" } until ($choice -in @("1", "2"))
        $Mode = if ($choice -eq "1") { "Auto" } else { "Manual" }
    }

    if ($Mode -eq "Auto") {
        if (-not $ProfilePath) { $ProfilePath = Join-Path ([Environment]::GetFolderPath("ApplicationData")) "UnrealVRMod\SanAndreas" }
        if (-not $GamePath) { $GamePath = Select-AutoGamePath }
    } else {
        if (-not $ProfilePath) {
            $defaultProfile = Join-Path ([Environment]::GetFolderPath("ApplicationData")) "UnrealVRMod\SanAndreas"
            $entry = Read-Host "UEVR SanAndreas profile folder [$defaultProfile]"
            $ProfilePath = if ([string]::IsNullOrWhiteSpace($entry)) { $defaultProfile } else { $entry.Trim().Trim('"') }
        }
        if (-not $GamePath) { $GamePath = Read-ManualGamePath }
    }

    $GamePath = Normalize-GamePath $GamePath
    if (-not $GamePath) { throw "The selected GTA San Andreas DE folder is invalid." }
    $ProfilePath = [IO.Path]::GetFullPath([Environment]::ExpandEnvironmentVariables($ProfilePath))

    Write-Host ""
    Write-Host "UEVR profile: $ProfilePath"
    Write-Host "Game folder:  $GamePath"
    if (-not $NoPrompt) {
        $confirm = Read-Host "Install to these locations? Y/N"
        if ($confirm -notmatch '^[Yy]$') { throw "Installation cancelled by user." }
    }

    if (Test-Path -LiteralPath $ProfilePath) {
        Write-Step "Backing up the existing UEVR profile"
        $uevrRoot = Split-Path -Parent $ProfilePath
        $backupRoot = Join-Path $uevrRoot "_Backups"
        New-Item -ItemType Directory -Force -Path $backupRoot | Out-Null
        $backup = Join-Path $backupRoot ("SanAndreas_{0}" -f (Get-Date -Format "yyyyMMdd_HHmmss"))
        Copy-Item -LiteralPath $ProfilePath -Destination $backup -Recurse -Force
        Write-Host "Backup: $backup" -ForegroundColor Green
    }

    Write-Step "Installing the UEVR profile"
    New-Item -ItemType Directory -Force -Path $ProfilePath | Out-Null
    Copy-Item -Path (Join-Path $PayloadRoot "UnrealVRMod\SanAndreas\*") -Destination $ProfilePath -Recurse -Force

    Write-Step "Installing the game-folder texture PAKs"
    Copy-Item -Path (Join-Path $PayloadRoot "GameFolder\*") -Destination $GamePath -Recurse -Force

    $installedDll = Join-Path $ProfilePath "plugins\UEVR_GTASADE.dll"
    if (-not (Test-Path -LiteralPath $installedDll -PathType Leaf)) { throw "Plugin verification failed: $installedDll" }

    if (-not $SkipShortcuts) {
        $desktop = [Environment]::GetFolderPath("Desktop")
        $shell = New-Object -ComObject WScript.Shell
        $gameExe = Join-Path $GamePath "Gameface\Binaries\Win64\SanAndreas.exe"
        $gameShortcut = $shell.CreateShortcut((Join-Path $desktop "GTA San Andreas DE VR.lnk"))
        $gameShortcut.TargetPath = $gameExe
        $gameShortcut.WorkingDirectory = Split-Path -Parent $gameExe
        $gameShortcut.Save()
        $settings = Join-Path $ProfilePath "SAImprovedSettings\SA-Improved-Settings.bat"
        if (Test-Path -LiteralPath $settings) {
            $settingsShortcut = $shell.CreateShortcut((Join-Path $desktop "SA Improved Settings.lnk"))
            $settingsShortcut.TargetPath = $settings
            $settingsShortcut.WorkingDirectory = Split-Path -Parent $settings
            $settingsShortcut.Save()
        }
    }

    Write-Step "Installation complete"
    Write-Host "Launch GTA San Andreas DE, then inject UEVR using OpenXR." -ForegroundColor Green
    exit 0
}
catch {
    Write-Host ""
    Write-Host "INSTALL FAILED: $($_.Exception.Message)" -ForegroundColor Red
    Add-Content -LiteralPath $LogPath -Encoding UTF8 -Value "FAILED: $($_.Exception.Message)"
    exit 1
}
