param([string]$RepoRoot = (Split-Path -Parent $PSScriptRoot))

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
$output = Join-Path $RepoRoot "output"
$test = Join-Path $output "clean-test"
if (-not ([IO.Path]::GetFullPath($test).StartsWith([IO.Path]::GetFullPath($output), [StringComparison]::OrdinalIgnoreCase))) {
    throw "Unsafe test path."
}
if (Test-Path -LiteralPath $test) { Remove-Item -LiteralPath $test -Recurse -Force }
New-Item -ItemType Directory -Force -Path $test | Out-Null

$installerExtract = Join-Path $test "installer-extracted"
Expand-Archive -LiteralPath (Join-Path $output "San-Andreas-VR-DE-Installer.zip") -DestinationPath $installerExtract
$installer = Get-ChildItem -LiteralPath $installerExtract -Recurse -Filter "Install-SAVR.ps1" | Select-Object -First 1
if (-not $installer) { throw "Installer script missing from archive." }

function Test-Installer([string]$Name, [string]$Mode) {
    $root = Join-Path $test $Name
    $profile = Join-Path $root "Roaming\UnrealVRMod\SanAndreas"
    $game = Join-Path $root "Game"
    $documents = Join-Path $root "Documents"
    New-Item -ItemType Directory -Force -Path (Join-Path $game "Gameface\Binaries\Win64"),(Join-Path $game "Gameface\Content\Paks") | Out-Null
    New-Item -ItemType File -Force -Path (Join-Path $game "Gameface\Binaries\Win64\SanAndreas.exe") | Out-Null
    $existingMovie = Join-Path $game "Gameface\Content\Movies\1080\GTA_SA_RSTAR_STINGER_FINAL_1920x1080.mp4"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $existingMovie) | Out-Null
    Set-Content -LiteralPath $existingMovie -Encoding ASCII -Value "pre-install movie sentinel"
    $existingMovieHash = (Get-FileHash -LiteralPath $existingMovie -Algorithm SHA256).Hash
    $existingSettings = Join-Path $documents "Rockstar Games\GTA San Andreas Definitive Edition\Config\WindowsNoEditor\GameUserSettings.ini"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $existingSettings) | Out-Null
    Set-Content -LiteralPath $existingSettings -Encoding ASCII -Value "pre-install settings sentinel"
    $existingSettingsHash = (Get-FileHash -LiteralPath $existingSettings -Algorithm SHA256).Hash
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $installer.FullName -Mode $Mode -ProfilePath $profile -GamePath $game -DocumentsPath $documents -NoPrompt -SkipShortcuts
    if ($LASTEXITCODE -ne 0) { throw "$Mode installer test failed." }

    $payload = Join-Path $installer.DirectoryName "Payload"
    $guardedLauncher = Join-Path $profile "SAVR-Launch.ps1"
    if (-not (Test-Path -LiteralPath $guardedLauncher -PathType Leaf)) {
        throw "Missing $Mode guarded launcher."
    }
    $launcherText = Get-Content -LiteralPath $guardedLauncher -Raw
    if ($launcherText -notmatch 'SAVR_SanAndreas_SingleLaunch' -or $launcherText -notmatch 'SAVR-last-launch') {
        throw "$Mode guarded launcher is missing its single-instance or settings-recovery protection."
    }
    foreach ($pair in @(
        @{ Source = Join-Path $payload "UnrealVRMod\SanAndreas"; Target = $profile },
        @{ Source = Join-Path $payload "GameFolder"; Target = $game }
    )) {
        foreach ($file in Get-ChildItem -LiteralPath $pair.Source -Recurse -File) {
            $relative = $file.FullName.Substring($pair.Source.Length + 1)
            $targetFile = Join-Path $pair.Target $relative
            if (-not (Test-Path -LiteralPath $targetFile -PathType Leaf)) { throw "Missing $Mode output: $relative" }
            if ((Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath $targetFile).Hash) {
                throw "Hash mismatch in $Mode output: $relative"
            }
        }
    }
    $recommended = Join-Path $payload "GameSettings\GameUserSettings.ini"
    $installedSettings = Join-Path $documents "Rockstar Games\GTA San Andreas Definitive Edition\Config\WindowsNoEditor\GameUserSettings.ini"
    $recoverySettings = Join-Path (Split-Path -Parent $installedSettings) "GameUserSettings.SAVR-recommended.ini"
    foreach ($targetSettings in @($installedSettings, $recoverySettings)) {
        if (-not (Test-Path -LiteralPath $targetSettings -PathType Leaf)) { throw "Missing $Mode GTA settings output: $targetSettings" }
        if ((Get-FileHash -LiteralPath $recommended).Hash -ne (Get-FileHash -LiteralPath $targetSettings).Hash) {
            throw "Hash mismatch in $Mode GTA settings output: $targetSettings"
        }
    }
    $settingsBackups = @(Get-ChildItem -LiteralPath (Join-Path (Split-Path -Parent $profile) "_Backups") -Filter "GameUserSettings_*.ini" -File)
    if ($settingsBackups.Count -ne 1) { throw "Expected one $Mode GTA settings backup; found $($settingsBackups.Count)." }
    if ((Get-FileHash -LiteralPath $settingsBackups[0].FullName -Algorithm SHA256).Hash -ne $existingSettingsHash) {
        throw "$Mode GTA settings backup does not match the pre-install file."
    }
    $gameBackups = @(Get-ChildItem -LiteralPath (Join-Path (Split-Path -Parent $profile) "_Backups") -Directory -Filter "GameFolder_*")
    if ($gameBackups.Count -ne 1) { throw "Expected one $Mode game-folder backup; found $($gameBackups.Count)." }
    $movieBackup = Join-Path $gameBackups[0].FullName "Gameface\Content\Movies\1080\GTA_SA_RSTAR_STINGER_FINAL_1920x1080.mp4"
    if (-not (Test-Path -LiteralPath $movieBackup -PathType Leaf)) { throw "Missing $Mode startup-movie backup." }
    if ((Get-FileHash -LiteralPath $movieBackup -Algorithm SHA256).Hash -ne $existingMovieHash) {
        throw "$Mode startup-movie backup does not match the pre-install file."
    }
    Write-Host "$Mode clean install PASS"
}

Test-Installer "auto" "Auto"
Test-Installer "manual" "Manual"

$manualExtract = Join-Path $test "manual-extracted"
Expand-Archive -LiteralPath (Join-Path $output "San-Andreas-VR-DE-Manual.zip") -DestinationPath $manualExtract
$sumFile = Get-ChildItem -LiteralPath $manualExtract -Recurse -Filter "SHA256SUMS.txt" | Select-Object -First 1
if (-not $sumFile) { throw "Manual package checksum manifest missing." }
$root = $sumFile.DirectoryName
foreach ($line in Get-Content -LiteralPath $sumFile.FullName) {
    if ($line -notmatch '^([0-9a-f]{64})  (.+)$') { throw "Malformed checksum line: $line" }
    $path = Join-Path $root ($Matches[2].Replace('/', '\'))
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing manual package file: $($Matches[2])" }
    if ((Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() -ne $Matches[1]) { throw "Manual checksum mismatch: $($Matches[2])" }
}
Write-Host "Manual archive manifest PASS"

$expectedVersion = (Get-Content -LiteralPath (Join-Path $RepoRoot "RELEASE_VERSION.txt") -Raw).Trim()
foreach ($archiveRoot in @($root, $installer.DirectoryName)) {
    $versionFile = Join-Path $archiveRoot "VERSION.txt"
    if (-not (Test-Path -LiteralPath $versionFile -PathType Leaf)) { throw "Package VERSION.txt missing: $archiveRoot" }
    if (-not (Select-String -LiteralPath $versionFile -SimpleMatch "Version: $expectedVersion" -Quiet)) {
        throw "Package version mismatch in $versionFile"
    }
    if (-not (Select-String -LiteralPath $versionFile -SimpleMatch "UEVR_GTASADE.dll SHA-256:" -Quiet)) {
        throw "Plugin hash missing from $versionFile"
    }
}
Write-Host "Stable package names and VERSION.txt PASS"
