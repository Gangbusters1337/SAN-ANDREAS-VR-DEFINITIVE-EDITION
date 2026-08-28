Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$profile = Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'UnrealVRMod\SanAndreas'
$documents = [Environment]::GetFolderPath('MyDocuments')
$supportHome = Join-Path $documents 'San Andreas VR'
$recovery = Join-Path $profile 'SAVR-Recovery.ini'
$userRecoveryName = 'SAVR Emergency Diagnostics Switch.ini'
$packageFolder = Join-Path $supportHome 'Support Packages'

function Ensure-RecoveryFile([bool]$forceOff) {
    New-Item -ItemType Directory -Force -Path $profile | Out-Null
    New-Item -ItemType Directory -Force -Path $supportHome | Out-Null
	$startMode = 'Off'
	foreach ($candidate in @((Join-Path $supportHome $userRecoveryName), $recovery)) {
		if (Test-Path -LiteralPath $candidate -PathType Leaf) {
			$stored = (Get-Content -LiteralPath $candidate | Where-Object { $_ -match '^StartMode=' } | Select-Object -First 1)
			if ($stored) { $startMode = ($stored -split '=', 2)[1].Trim(); break }
		}
	}
	$text = "[Diagnostics]`r`nForceOff=$($forceOff.ToString().ToLowerInvariant())`r`nStartMode=$startMode`r`n"
    [IO.File]::WriteAllText($recovery, $text, [Text.Encoding]::ASCII)
    [IO.File]::WriteAllText((Join-Path $supportHome $userRecoveryName), $text, [Text.Encoding]::ASCII)
}

function Sanitize-TextFile([string]$path) {
    $text = [IO.File]::ReadAllText($path)
    foreach ($value in @($env:USERPROFILE, $env:OneDrive, [Environment]::UserName)) {
        if (-not [string]::IsNullOrWhiteSpace($value)) {
            $text = $text.Replace($value, '<redacted-user-path>')
        }
    }
    [IO.File]::WriteAllText($path, $text, [Text.UTF8Encoding]::new($false))
}

function Add-IfPresent([string]$source, [string]$destination, [bool]$sanitize = $false) {
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { return }
    $parent = Split-Path -Parent $destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
    if ($sanitize) { Sanitize-TextFile $destination }
}

function New-SupportPackage {
    New-Item -ItemType Directory -Force -Path $packageFolder | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $staging = Join-Path $env:TEMP "SAVR-Support-$stamp"
    $zip = Join-Path $packageFolder "SAVR-Support-$stamp.zip"
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $staging | Out-Null
    try {
        foreach ($name in @('log.txt','status.txt','config.txt','cameras.txt','cvars_data.txt',
                'cvars_standard.txt','UEVR_GTASADE_config.txt','SAVR-Recovery.ini',
                'SAVR_diagnostics_active.flag','SAVR_diagnostics_previous_interrupted.flag')) {
            Add-IfPresent (Join-Path $profile $name) (Join-Path $staging "UEVR\$name") $true
        }
        Add-IfPresent (Join-Path $profile 'crash.dmp') (Join-Path $staging 'UEVR\crash.dmp')

        $gameConfig = Join-Path $documents 'Rockstar Games\GTA San Andreas Definitive Edition\Config\WindowsNoEditor\GameUserSettings.ini'
        Add-IfPresent $gameConfig (Join-Path $staging 'Game\GameUserSettings.ini') $true
        foreach ($candidate in @(
            (Join-Path $env:LOCALAPPDATA 'GTA San Andreas Definitive Edition\Saved\Logs'),
            (Join-Path $env:LOCALAPPDATA 'SanAndreas\Saved\Logs'),
            (Join-Path $documents 'Rockstar Games\GTA San Andreas Definitive Edition\Logs'))) {
            if (Test-Path -LiteralPath $candidate -PathType Container) {
                foreach ($file in Get-ChildItem -LiteralPath $candidate -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 5) {
                    Add-IfPresent $file.FullName (Join-Path $staging "Game\Logs\$($file.Name)") $true
                }
            }
        }

        $manifest = [Collections.Generic.List[string]]::new()
        $manifest.Add('Product: San Andreas VR Definitive Edition')
        $manifest.Add("Created: $((Get-Date).ToString('o'))")
        $manifest.Add("Windows: $([Environment]::OSVersion.VersionString)")
        $manifest.Add('Diagnostics are local-only; this package is not uploaded automatically.')
		$uevrLog = Join-Path $profile 'log.txt'
		if (Test-Path -LiteralPath $uevrLog -PathType Leaf) {
			# Build date/time are malformed in some UEVR logs; use the stable commit
			# identifiers instead of copying misleading placeholder text.
			$uevrBuild = @(Select-String -LiteralPath $uevrLog -Pattern 'Commit hash:|Commits past tag:|Total commits:' |
				Select-Object -First 3 | ForEach-Object { ($_.Line -replace '^.*\] ', '').Trim() })
			if ($uevrBuild.Count -gt 0) {
				$manifest.Add("UEVR build: $($uevrBuild -join '; ')")
			} else {
				$manifest.Add('UEVR build: not found in log.txt')
			}
		} else {
			$manifest.Add('UEVR build: log.txt missing')
		}
		$statusSource = Join-Path $profile 'status.txt'
		$manifest.Add("UEVR status.txt: $(if (Test-Path -LiteralPath $statusSource -PathType Leaf) { 'included' } else { 'not present in profile' })")
        $manifest.Add('')
        foreach ($relative in @('plugins\UEVR_GTASADE.dll','scripts\DUALGRIP.lua','scripts\GTASADE_FeatureFlags.lua')) {
            $path = Join-Path $profile $relative
            if (Test-Path -LiteralPath $path -PathType Leaf) {
                $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
                $manifest.Add("SHA256 $relative = $hash")
            }
        }
        $versionSource = Join-Path $PSScriptRoot 'VERSION.txt'
        if (-not (Test-Path -LiteralPath $versionSource)) { $versionSource = Join-Path (Split-Path -Parent $PSScriptRoot) 'VERSION.txt' }
        if (Test-Path -LiteralPath $versionSource) {
            $manifest.Add('')
            $manifest.AddRange([string[]](Get-Content -LiteralPath $versionSource))
        }
        [IO.File]::WriteAllLines((Join-Path $staging 'MANIFEST.txt'), $manifest, [Text.UTF8Encoding]::new($false))
        Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $zip -CompressionLevel Optimal
        return $zip
    }
    finally {
        if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
    }
}

$form = New-Object Windows.Forms.Form
$form.Text = 'SAVR Support & Recovery'
$form.Size = New-Object Drawing.Size(440,315)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false

$title = New-Object Windows.Forms.Label
$title.Text = 'San Andreas VR - Support & Recovery'
$title.Font = New-Object Drawing.Font('Segoe UI',12,[Drawing.FontStyle]::Bold)
$title.AutoSize = $true
$title.Location = New-Object Drawing.Point(20,18)
$form.Controls.Add($title)

$status = New-Object Windows.Forms.Label
$status.AutoSize = $false
$status.Size = New-Object Drawing.Size(390,42)
$status.Location = New-Object Drawing.Point(20,52)
$status.Text = 'The selected diagnostic mode persists after restart or force-quit.'
$form.Controls.Add($status)

function Add-Button([string]$text, [int]$top, [scriptblock]$action) {
    $button = New-Object Windows.Forms.Button
    $button.Text = $text
    $button.Size = New-Object Drawing.Size(390,36)
    $button.Location = New-Object Drawing.Point(20,$top)
    $button.Add_Click($action)
    $form.Controls.Add($button)
}

Add-Button 'Disable diagnostics now and on next launch' 98 {
    Ensure-RecoveryFile $true
    $status.Text = 'ForceOff is enabled. The plugin will disable diagnostics within two seconds.'
}
Add-Button 'Allow in-game diagnostics controls' 140 {
    Ensure-RecoveryFile $false
	$status.Text = 'ForceOff is cleared. The last selected mode will resume on the next launch.'
}
Add-Button 'Create support package' 182 {
    try {
        $zip = New-SupportPackage
        $status.Text = "Created: $zip"
        Start-Process explorer.exe -ArgumentList "/select,`"$zip`""
    } catch { $status.Text = "Package failed: $($_.Exception.Message)" }
}
Add-Button 'Open UEVR logs folder' 224 {
    New-Item -ItemType Directory -Force -Path $profile | Out-Null
    Start-Process explorer.exe -ArgumentList "`"$profile`""
}

[void]$form.ShowDialog()
