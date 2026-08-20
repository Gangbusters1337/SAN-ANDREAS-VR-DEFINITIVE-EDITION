param([Parameter(Mandatory)][string]$GameExe, [string]$GameArguments = "", [switch]$Silent)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms

function Show-Notice([string]$Text, [string]$Title = "San Andreas VR") {
    if (-not $Silent) {
        [Windows.Forms.MessageBox]::Show($Text, $Title, "OK", "Information") | Out-Null
    }
}

$mutex = [Threading.Mutex]::new($false, "Local\SAVR_SanAndreas_SingleLaunch")
$ownsMutex = $false
try {
    try { $ownsMutex = $mutex.WaitOne(5000) }
    catch [Threading.AbandonedMutexException] { $ownsMutex = $true }
    if (-not $ownsMutex) { Show-Notice "San Andreas is already starting."; exit 0 }
    if (@(Get-Process SanAndreas -ErrorAction SilentlyContinue).Count) {
        Show-Notice "San Andreas is already running. A second copy was not started."
        exit 0
    }
    $validExe = (Test-Path -LiteralPath $GameExe -PathType Leaf) `
        -and [IO.Path]::GetFileName($GameExe) -ieq "SanAndreas.exe"
    if (-not $validExe) {
        throw "The configured SanAndreas.exe path is invalid."
    }

    $settings = Join-Path ([Environment]::GetFolderPath("MyDocuments")) `
        "Rockstar Games\GTA San Andreas Definitive Edition\Config\WindowsNoEditor\GameUserSettings.ini"
    if (Test-Path -LiteralPath $settings -PathType Leaf) {
        Copy-Item $settings (Join-Path (Split-Path $settings) "GameUserSettings.SAVR-last-launch.ini") -Force
    }

    $launch = @{ FilePath = $GameExe; WorkingDirectory = (Split-Path $GameExe); PassThru = $true }
    if ($GameArguments) { $launch.ArgumentList = $GameArguments }
    Start-Process @launch | Out-Null
}
catch { Show-Notice $_.Exception.Message "San Andreas VR launch failed"; exit 1 }
finally {
    if ($ownsMutex) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
