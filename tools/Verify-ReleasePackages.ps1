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
Expand-Archive -LiteralPath (Join-Path $output "SAVR-Improved-Installer.zip") -DestinationPath $installerExtract
$installer = Get-ChildItem -LiteralPath $installerExtract -Recurse -Filter "Install-SAVR.ps1" | Select-Object -First 1
if (-not $installer) { throw "Installer script missing from archive." }

function Test-Installer([string]$Name, [string]$Mode) {
    $root = Join-Path $test $Name
    $profile = Join-Path $root "Roaming\UnrealVRMod\SanAndreas"
    $game = Join-Path $root "Game"
    New-Item -ItemType Directory -Force -Path (Join-Path $game "Gameface\Binaries\Win64"),(Join-Path $game "Gameface\Content\Paks") | Out-Null
    New-Item -ItemType File -Force -Path (Join-Path $game "Gameface\Binaries\Win64\SanAndreas.exe") | Out-Null
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $installer.FullName -Mode $Mode -ProfilePath $profile -GamePath $game -NoPrompt -SkipShortcuts
    if ($LASTEXITCODE -ne 0) { throw "$Mode installer test failed." }

    $payload = Join-Path $installer.DirectoryName "Payload"
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
    Write-Host "$Mode clean install PASS"
}

Test-Installer "auto" "Auto"
Test-Installer "manual" "Manual"

$manualExtract = Join-Path $test "manual-extracted"
Expand-Archive -LiteralPath (Join-Path $output "SAVR-Improved-Manual.zip") -DestinationPath $manualExtract
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
