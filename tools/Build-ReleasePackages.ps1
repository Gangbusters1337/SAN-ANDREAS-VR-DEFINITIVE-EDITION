param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$OutputRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) "output"),
    [string]$ReleaseVersion
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$releaseFiles = Join-Path $RepoRoot "ReleaseFiles"
$documentation = Join-Path $RepoRoot "Documentation"
$installerSource = Join-Path $RepoRoot "Installer"
$releaseVersionFile = Join-Path $RepoRoot "RELEASE_VERSION.txt"
$manualRoot = Join-Path $OutputRoot "manual\SAVR-Improved-Manual"
$installerRoot = Join-Path $OutputRoot "installer\SAVR-Improved-Installer"

if ([string]::IsNullOrWhiteSpace($ReleaseVersion)) {
    if (-not (Test-Path -LiteralPath $releaseVersionFile -PathType Leaf)) {
        throw "Missing release version source: $releaseVersionFile"
    }
    $ReleaseVersion = (Get-Content -LiteralPath $releaseVersionFile -Raw).Trim()
}
if ($ReleaseVersion -notmatch '^v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') {
    throw "ReleaseVersion must look like v0.1.0-beta; got '$ReleaseVersion'."
}

foreach ($required in @(
    (Join-Path $releaseFiles "UnrealVRMod\SanAndreas\plugins\UEVR_GTASADE.dll"),
    (Join-Path $releaseFiles "UnrealVRMod\SanAndreas\SAVR-Launch.ps1"),
    (Join-Path $releaseFiles "UnrealVRMod\SanAndreas\scripts\DUALGRIP.lua"),
    (Join-Path $releaseFiles "GameFolder\Gameface\Content\Paks\~mods\500-Holydh_ReducedMuzzleFlash.pak"),
    (Join-Path $releaseFiles "GameFolder\Gameface\Content\Movies\1080\GTA_SA_RSTAR_STINGER_FINAL_1920x1080.mp4"),
    (Join-Path $releaseFiles "GameSettings\GameUserSettings.ini"),
    (Join-Path $documentation "Quest3-Control-Layout.png"),
    (Join-Path $installerSource "Install-SAVR.ps1")
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing release input: $required" }
}

$resolvedOutput = [IO.Path]::GetFullPath($OutputRoot).TrimEnd('\')
if (-not $resolvedOutput.StartsWith($RepoRoot.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputRoot must remain inside RepoRoot."
}
if (Test-Path -LiteralPath $OutputRoot) { Remove-Item -LiteralPath $OutputRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $manualRoot,$installerRoot | Out-Null

function Copy-Docs([string]$Target) {
    New-Item -ItemType Directory -Force -Path (Join-Path $Target "Documentation") | Out-Null
    foreach ($file in @("README.md", "CHANGELOG.md", "FEATURES.md", "CONTROLS.md", "THIRD_PARTY_NOTICES.md", "LICENSE")) {
        Copy-Item -LiteralPath (Join-Path $RepoRoot $file) -Destination (Join-Path $Target $file) -Force
    }
    Copy-Item -LiteralPath (Join-Path $documentation "Quest3-Control-Layout.png") -Destination (Join-Path $Target "Documentation\Quest3-Control-Layout.png") -Force
}

Copy-Item -LiteralPath (Join-Path $releaseFiles "UnrealVRMod") -Destination $manualRoot -Recurse -Force
Copy-Item -LiteralPath (Join-Path $releaseFiles "GameFolder") -Destination $manualRoot -Recurse -Force
Copy-Item -LiteralPath (Join-Path $releaseFiles "GameSettings") -Destination $manualRoot -Recurse -Force
Copy-Docs $manualRoot

Copy-Item -LiteralPath (Join-Path $installerSource "INSTALL-SAVR.bat") -Destination $installerRoot -Force
Copy-Item -LiteralPath (Join-Path $installerSource "Install-SAVR.ps1") -Destination $installerRoot -Force
New-Item -ItemType Directory -Force -Path (Join-Path $installerRoot "Payload") | Out-Null
Copy-Item -LiteralPath (Join-Path $releaseFiles "UnrealVRMod") -Destination (Join-Path $installerRoot "Payload") -Recurse -Force
Copy-Item -LiteralPath (Join-Path $releaseFiles "GameFolder") -Destination (Join-Path $installerRoot "Payload") -Recurse -Force
Copy-Item -LiteralPath (Join-Path $releaseFiles "GameSettings") -Destination (Join-Path $installerRoot "Payload") -Recurse -Force
Copy-Docs $installerRoot

function Write-VersionInfo([string]$Root, [string]$PackageKind, [string]$DllRelativePath) {
    $dll = Join-Path $Root $DllRelativePath
    $guide = Join-Path $Root "Documentation\Quest3-Control-Layout.png"
    $lines = @(
        "Product: San Andreas VR Definitive Edition",
        "Version: $ReleaseVersion",
        "Package: $PackageKind",
        "Supported SanAndreas.exe SHA-256: CF677214A8AB317B3BC8811C64BBC60742204D021132C79E7C0583322CF5BA17",
        "UEVR_GTASADE.dll SHA-256: $((Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash)",
        "Quest3-Control-Layout.png SHA-256: $((Get-FileHash -LiteralPath $guide -Algorithm SHA256).Hash)",
        "All packaged-file hashes: SHA256SUMS.txt",
        "The ZIP SHA-256 is published beside the release asset; it cannot be embedded inside the ZIP it hashes."
    )
    Set-Content -LiteralPath (Join-Path $Root "VERSION.txt") -Encoding ASCII -Value $lines
}

function Write-Manifest([string]$Root) {
    $entries = Get-ChildItem -LiteralPath $Root -Recurse -File | Where-Object Name -ne "SHA256SUMS.txt" | Sort-Object FullName
    $lines = foreach ($entry in $entries) {
        $relative = $entry.FullName.Substring($Root.Length + 1).Replace('\','/')
        $hash = (Get-FileHash -LiteralPath $entry.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $relative"
    }
    Set-Content -LiteralPath (Join-Path $Root "SHA256SUMS.txt") -Encoding ASCII -Value $lines
}

Write-VersionInfo $manualRoot "Manual" "UnrealVRMod\SanAndreas\plugins\UEVR_GTASADE.dll"
Write-VersionInfo $installerRoot "Installer" "Payload\UnrealVRMod\SanAndreas\plugins\UEVR_GTASADE.dll"
Write-Manifest $manualRoot
Write-Manifest $installerRoot
$manualArchive = Join-Path $OutputRoot "San-Andreas-VR-DE-Manual.zip"
$installerArchive = Join-Path $OutputRoot "San-Andreas-VR-DE-Installer.zip"
Compress-Archive -LiteralPath $manualRoot -DestinationPath $manualArchive -CompressionLevel Optimal
Compress-Archive -LiteralPath $installerRoot -DestinationPath $installerArchive -CompressionLevel Optimal

Get-FileHash -Algorithm SHA256 -LiteralPath $manualArchive,$installerArchive |
    Select-Object Path, Hash
