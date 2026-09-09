param(
    [ValidateSet('Gui','Collect','FullOn','StartupOff','Stop','Allow','Preview')][string]$Action = 'Gui',
    [string]$ProfilePath = (Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'UnrealVRMod\SanAndreas'),
    [string]$DocumentsPath = [Environment]::GetFolderPath('MyDocuments'),
    [string]$LocalAppDataPath = [Environment]::GetFolderPath('LocalApplicationData'),
    [string]$GameExe = '',
    [ValidatePattern('^[A-Za-z0-9-]{0,80}$')][string]$RequestId = '',
    [switch]$NoDiscover,
    [string]$PreviewPath = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'SAVR-SupportCore.ps1')
$context = New-SavrContext $ProfilePath $DocumentsPath $LocalAppDataPath $GameExe $NoDiscover.IsPresent

function Write-Result([string]$State, [string]$PackageName = '') {
    if ($RequestId) {
        Write-SavrText (Join-Path $context.Profile "SAVR_support_$RequestId.ini") (
            "[Support]`r`nRequestId=$RequestId`r`nState=$State`r`nPackageName=$PackageName`r`n")
    }
}
if ($Action -notin 'Gui','Preview') {
    try {
        if ($Action -eq 'Collect') {
            $zip = New-SavrPackage $context
            Write-Result 'saved' (Split-Path -Leaf $zip)
            Write-Output $zip
        } else { Set-SavrDiagnostics $context $Action }
        exit 0
    } catch { Write-Result 'failed'; Write-Error $_ -ErrorAction Continue; exit 1 }
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$form = [Windows.Forms.Form]::new()
$form.Text = 'SAVR Support & Recovery'; $form.ClientSize = [Drawing.Size]::new(580,610)
$form.AutoScaleMode = 'Dpi'; $form.Font = [Drawing.Font]::new('Segoe UI',10)
$form.BackColor = [Drawing.Color]::FromArgb(248,248,248)
$form.StartPosition = 'CenterScreen'; $form.FormBorderStyle = 'FixedDialog'; $form.MaximizeBox = $false
function Add-Label($Text, $X, $Y, $W, $H, [int]$Size = 10, [bool]$Bold = $false) {
    $c = [Windows.Forms.Label]::new(); $c.Text = $Text
    $c.Location = [Drawing.Point]::new($X,$Y); $c.Size = [Drawing.Size]::new($W,$H)
    $style = if ($Bold) {[Drawing.FontStyle]::Bold} else {[Drawing.FontStyle]::Regular}
    $c.Font = [Drawing.Font]::new('Segoe UI',$Size,$style)
    $form.Controls.Add($c); return $c
}
function Add-Button($Text, $X, $Y, $W, $H, [scriptblock]$Handler) {
    $b = [Windows.Forms.Button]::new(); $b.Text=$Text; $b.Location=[Drawing.Point]::new($X,$Y); $b.Size=[Drawing.Size]::new($W,$H)
    $b.FlatStyle='Flat'; $b.Add_Click($Handler); $form.Controls.Add($b); return $b
}
[void](Add-Label 'Support & recovery' 24 20 530 40 22 $true)
[void](Add-Label 'San Andreas VR Definitive Edition' 26 66 520 25)
[void](Add-Label 'DIAGNOSTICS' 26 104 500 25 11 $true)
[void](Add-Label 'Full diagnostics on startup' 26 139 400 30 12 $true)
$full = [Windows.Forms.CheckBox]::new()
$full.Appearance='Button'; $full.TextAlign='MiddleCenter'; $full.FlatStyle='Flat'
$full.Location=[Drawing.Point]::new(464,134); $full.Size=[Drawing.Size]::new(88,34); $form.Controls.Add($full)
[void](Add-Label "Records extra detail when the mod initializes.`r`nStays enabled after restarts and crashes." 26 180 525 45)
$blockedLabel = Add-Label 'Emergency stop is active.' 26 231 285 25
$allow = Add-Button 'Re-enable diagnostics' 326 225 226 32 {
    try { Set-SavrDiagnostics $context 'Allow'; $status.Text='Emergency stop cleared. In-game diagnostics can be used again.'; Refresh-Diagnostics }
    catch { $status.Text=$_.Exception.Message }
}
[void](Add-Label 'SUPPORT PACKAGE' 26 278 500 25 11 $true)
[void](Add-Label "Game version and installation check`r`nUEVR, mod and game logs`r`nAvailable crash reports" 26 313 525 70)
$create = Add-Button 'Create support ZIP' 26 396 526 42 { Start-Collection }
$create.BackColor=[Drawing.Color]::FromArgb(43,113,58); $create.ForeColor=[Drawing.Color]::White
[void](Add-Label 'Saved locally. Folder opens when ready. Nothing is uploaded.' 26 447 526 22)
[void](Add-Label 'Crash dumps may contain private data. Review before sharing.' 26 470 526 22 9)
$stop = Add-Button 'Stop all diagnostics' 26 507 253 36 {
    try { Set-SavrDiagnostics $context 'Stop'; $status.Text='Diagnostics blocked now and on startup. Re-enable them when ready.'; Refresh-Diagnostics }
    catch { $status.Text=$_.Exception.Message }
}
$stop.ForeColor=[Drawing.Color]::DarkRed
$open = Add-Button 'Open logs folder' 299 507 253 36 {
    if($script:lastPackage){Start-Process explorer.exe -ArgumentList ('/select,"'+$script:lastPackage+'"')}
    elseif(Test-Path -LiteralPath $context.Profile){Start-Process explorer.exe -ArgumentList ('"'+$context.Profile+'"')}
}
$status = Add-Label 'Ready. Launch the game, then inject UEVR.' 26 558 526 45 9
$script:refreshing=$false; $script:worker=$null; $script:resultFile=''; $script:workerId=''; $script:lastPackage=''
function Refresh-Diagnostics {
    $script:refreshing=$true
    try {
        $state=Get-SavrDiagnostics $context
        $full.Checked=($state.Mode -ieq 'Full' -and !$state.Blocked)
        $full.Text=if($full.Checked){'ON'}else{'OFF'}
        $full.BackColor=if($full.Checked){[Drawing.Color]::FromArgb(43,113,58)}else{[Drawing.Color]::White}
        $full.ForeColor=if($full.Checked){[Drawing.Color]::White}else{[Drawing.Color]::Black}
        $blockedLabel.Visible=$state.Blocked; $allow.Visible=$state.Blocked
    } finally {$script:refreshing=$false}
}
$full.Add_CheckedChanged({
    if ($script:refreshing) {return}
    try {
        Set-SavrDiagnostics $context $(if($full.Checked){'FullOn'}else{'StartupOff'})
        $status.Text=if($full.Checked){'Full diagnostics enabled for the next launch and later sessions.'}else{'Startup logging is off. Use Stop all diagnostics to stop a running session.'}
        Refresh-Diagnostics
    } catch {$status.Text=$_.Exception.Message; Refresh-Diagnostics}
})
function Start-Collection {
    if($script:worker){return}
    try {
        if (!(Find-SavrGame $context)) {
            $pick=[Windows.Forms.OpenFileDialog]::new(); $pick.Title='Locate GTA San Andreas DE'; $pick.Filter='SanAndreas.exe|SanAndreas.exe'
            try {
                if($pick.ShowDialog($form) -eq 'OK'){$context.GameExe=$pick.FileName}
                else {$status.Text='Game not selected. Package creation cancelled.';return}
            } finally {$pick.Dispose()}
        }
        $script:workerId=[guid]::NewGuid().ToString('N')
        $script:resultFile=Join-Path $context.Profile "SAVR_support_$script:workerId.ini"
        $psi=[Diagnostics.ProcessStartInfo]::new()
        $psi.FileName=Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $psi.UseShellExecute=$false; $psi.CreateNoWindow=$true; $psi.WindowStyle='Hidden'
        $psi.Arguments='-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File "'+$PSCommandPath+'" -Action Collect -RequestId '+$script:workerId+
            ' -ProfilePath "'+$context.Profile+'" -DocumentsPath "'+$context.Documents+'" -LocalAppDataPath "'+$context.Local+'"'
        if($context.GameExe){$psi.Arguments+=' -GameExe "'+$context.GameExe+'"'}
        if($context.NoDiscover){$psi.Arguments+=' -NoDiscover'}
        $script:worker=[Diagnostics.Process]::Start($psi)
        $create.Enabled=$false; $create.Text='Creating...'; $status.Text='Collecting locally. The game can stay open.'
    } catch {$status.Text='Could not start collection: '+$_.Exception.Message}
}
$script:refreshCount=0
$timer=[Windows.Forms.Timer]::new(); $timer.Interval=500
$timer.Add_Tick({
    $script:refreshCount++
    if($script:refreshCount % 4 -eq 0){try {Refresh-Diagnostics} catch {}}
    if(!$script:worker -or !$script:worker.HasExited){return}
    try {
        if($script:worker.ExitCode -ne 0){throw 'Collection failed or another package is being created. Try again.'}
        $result=@(Get-Content -LiteralPath $script:resultFile)
        if($result -notcontains 'State=saved' -or $result -notcontains "RequestId=$script:workerId"){throw 'No matching completed package was reported.'}
        $leaf=($result | Where-Object {$_ -like 'PackageName=*'} | Select-Object -First 1) -replace '^PackageName=',''
        if($leaf -notmatch '^SAVR-Support-[A-Za-z0-9_-]+\.zip$'){throw 'Invalid package result.'}
        $script:lastPackage=Join-Path (Join-Path $context.Home 'Support Packages') $leaf
        if(!(Test-Path -LiteralPath $script:lastPackage -PathType Leaf)){throw 'The reported ZIP could not be found.'}
        $status.Text='Saved to Documents > San Andreas VR > Support Packages.'; $open.Text='Open package folder'
        # Desktop GUI only. The headless in-game collector exits before this timer exists.
        try { Start-Process explorer.exe -ArgumentList ('/select,"'+$script:lastPackage+'"') }
        catch { $status.Text='ZIP saved. Could not open Explorer; use Open package folder.' }
    } catch {$status.Text=$_.Exception.Message}
    finally {
        $script:worker.Dispose(); $script:worker=$null; $create.Enabled=$true; $create.Text='Create support ZIP'
        if(Test-Path -LiteralPath $script:resultFile){[IO.File]::Delete($script:resultFile)}
    }
})
Refresh-Diagnostics
if($Action -eq 'Preview') {
    if(!$PreviewPath){throw 'PreviewPath is required.'}
    # Render from the same control positions/text/fonts without showing a window.
    $bmp=[Drawing.Bitmap]::new($form.ClientSize.Width,$form.ClientSize.Height)
    $graphics=[Drawing.Graphics]::FromImage($bmp)
    try {
        $graphics.Clear($form.BackColor); $graphics.TextRenderingHint='ClearTypeGridFit'
        $state=Get-SavrDiagnostics $context
        foreach($c in $form.Controls) {
            if(($c -eq $allow -or $c -eq $blockedLabel) -and !$state.Blocked){continue}
            $rect=[Drawing.RectangleF]::new($c.Left,$c.Top,$c.Width,$c.Height)
            $brush=[Drawing.SolidBrush]::new($c.ForeColor)
            $format=[Drawing.StringFormat]::new()
            try {
                if($c -is [Windows.Forms.ButtonBase]) {
                    $bg=[Drawing.SolidBrush]::new($c.BackColor)
                    try {$graphics.FillRectangle($bg,$rect)} finally {$bg.Dispose()}
                    $graphics.DrawRectangle([Drawing.Pens]::Gray,$c.Left,$c.Top,$c.Width-1,$c.Height-1)
                    $format.Alignment='Center';$format.LineAlignment='Center'
                }
                $graphics.DrawString($c.Text,$c.Font,$brush,$rect,$format)
            } finally {$brush.Dispose();$format.Dispose()}
        }
        $bmp.Save($PreviewPath,[Drawing.Imaging.ImageFormat]::Png)
    } finally {$graphics.Dispose();$bmp.Dispose();$timer.Dispose();$form.Dispose()}
    exit 0
}
$timer.Start()
try {[void]$form.ShowDialog()} finally {$timer.Dispose();if($script:worker){$script:worker.Dispose()};$form.Dispose()}
