# Shared by the desktop tool and the hidden in-game collector. No UI or startup writes.
Set-StrictMode -Version Latest

function New-SavrContext($ProfilePath, $DocumentsPath, $LocalAppDataPath, $GameExe, [bool]$NoDiscover) {
    [pscustomobject]@{
        Profile = [IO.Path]::GetFullPath($ProfilePath)
        Documents = [IO.Path]::GetFullPath($DocumentsPath)
        Local = [IO.Path]::GetFullPath($LocalAppDataPath)
        Home = Join-Path $DocumentsPath 'San Andreas VR'
        GameExe = $GameExe
        NoDiscover = $NoDiscover
        Report = [Collections.Generic.List[string]]::new()
        Bytes = [long]0
    }
}

function Get-SavrRecoveryPaths($Context) {
    (Join-Path $Context.Home 'SAVR Emergency Diagnostics Switch.ini')
    (Join-Path $Context.Profile 'SAVR-Recovery.ini')
}

function Get-SavrDiagnostics($Context) {
    $mode = $null; $blocked = $false
    foreach ($path in (Get-SavrRecoveryPaths $Context)) {
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $lines = @(Get-Content -LiteralPath $path)
        if ($lines -match '^\s*ForceOff\s*=\s*(true|1)\s*$') { $blocked = $true }
        if ($null -eq $mode) {
            foreach ($line in $lines) {
                if ($line -match '^\s*StartMode\s*=\s*(Off|Full|VehicleInput|Vehicle|SaveLoad|Lifecycle|vehicle-input|save-load)\s*$') {
                    $mode = $Matches[1]; break
                }
            }
        }
    }
    if (!$mode) { $mode = 'Off' }
    [pscustomobject]@{ Mode = $mode; Blocked = $blocked }
}

function Write-SavrText($Path, [string]$Text) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    $tmp = $Path + '.' + [guid]::NewGuid().ToString('N') + '.tmp'
    try {
        [IO.File]::WriteAllText($tmp, $Text, [Text.UTF8Encoding]::new($false))
        if ([IO.File]::Exists($Path)) { [IO.File]::Replace($tmp, $Path, [NullString]::Value) }
        else { [IO.File]::Move($tmp, $Path) }
    } finally { if ([IO.File]::Exists($tmp)) { [IO.File]::Delete($tmp) } }
}

function Set-SavrDiagnostics($Context, [ValidateSet('FullOn','StartupOff','Stop','Allow')]$Action) {
    $old = Get-SavrDiagnostics $Context
    $mode = $old.Mode; $blocked = $old.Blocked
    switch ($Action) {
        FullOn { $mode = 'Full'; $blocked = $false }
        StartupOff { $mode = 'Off'; $blocked = $false }
        Stop { $blocked = $true }
        Allow { $blocked = $false }
    }
    foreach ($path in (Get-SavrRecoveryPaths $Context)) {
        # Preserve other INI entries; these files are already the backend's authority.
        $lines = if (Test-Path -LiteralPath $path) { @(Get-Content -LiteralPath $path) } else { @('[Diagnostics]') }
        foreach ($pair in @(@('ForceOff',$blocked.ToString().ToLowerInvariant()), @('StartMode',$mode))) {
            $found = $false
            $lines = @($lines | ForEach-Object {
                if ($_ -match ('^\s*' + $pair[0] + '\s*=')) { $found = $true; $pair[0] + '=' + $pair[1] } else { $_ }
            })
            if (!$found) { $lines += $pair[0] + '=' + $pair[1] }
        }
        Write-SavrText $path (($lines -join "`r`n") + "`r`n")
    }
    $actual = Get-SavrDiagnostics $Context
    if ($actual.Mode -ine $mode -or $actual.Blocked -ne $blocked) { throw 'Could not save diagnostics settings.' }
}

function Protect-SavrText([string]$Text, $Context) {
    foreach ($value in @($Context.Documents, $Context.Local, $env:USERPROFILE, $env:OneDrive) | Sort-Object Length -Descending) {
        if (![string]::IsNullOrWhiteSpace($value)) { $Text = $Text.Replace($value, '<user-folder>') }
    }
    # CrashContext may contain account IDs, user names, command lines or personal paths.
    $Text = $Text -replace '(?is)<(UserName|LoginId|EpicAccountId|CommandLine|MachineId|MachineName)>.*?</\1>', '<$1>redacted</$1>'
    $Text = $Text -replace '(?i)[A-Z]:[\\/]Users[\\/][^\\/\r\n<>" ]+', '<user-folder>'
    $Text = $Text -replace '(?i)\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b', '<redacted-email>'
    return $Text
}

function Get-SavrFiles([string]$Root, [int]$Depth = 0, [int]$Limit = 2000) {
    if (!(Test-Path -LiteralPath $Root -PathType Container)) { return }
    if ((Get-Item -LiteralPath $Root).Attributes -band [IO.FileAttributes]::ReparsePoint) { return }
    $queue = [Collections.Generic.Queue[object]]::new()
    $queue.Enqueue(@($Root,0)); $count = 0
    while ($queue.Count -gt 0 -and $count -lt $Limit) {
        $node = $queue.Dequeue()
        foreach ($item in Get-ChildItem -LiteralPath $node[0] -Force -ErrorAction SilentlyContinue) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
            if ($item.PSIsContainer) {
                if ($node[1] -lt $Depth) { $queue.Enqueue(@($item.FullName,($node[1]+1))) }
            } else { $item; $count++; if ($count -ge $Limit) { break } }
        }
    }
}

function Find-SavrGame($Context) {
    $candidates = [Collections.Generic.List[string]]::new()
    if ($Context.GameExe) { $candidates.Add($Context.GameExe) }
    if (!$Context.NoDiscover) {
        try { Get-CimInstance Win32_Process -Filter "Name='SanAndreas.exe'" -ErrorAction Stop |
            ForEach-Object { if ($_.ExecutablePath) { $candidates.Add($_.ExecutablePath) } } } catch {}
        $log = Join-Path $Context.Profile 'log.txt'
        if (Test-Path -LiteralPath $log) {
            foreach ($line in Get-Content -LiteralPath $log -TotalCount 150) {
                if ($line -match '\[PluginLoader\] Module path (.+SanAndreas\.exe)\s*$') { $candidates.Add($Matches[1].Trim()) }
            }
        }
    }
    foreach ($candidate in $candidates) {
        # A path quoted by a log must not trigger access to an arbitrary network share.
        if ($candidate -notmatch '^[A-Za-z]:[\\/]') { continue }
        try { if ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($candidate)).DriveType -eq [IO.DriveType]::Network) { continue } } catch { continue }
        if ((Split-Path -Leaf $candidate) -ine 'SanAndreas.exe' -or !(Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
        $exe = (Get-Item -LiteralPath $candidate).FullName
        if ($exe -notmatch '(?i)^(.*)[\\/]Gameface[\\/]Binaries[\\/]Win64[\\/]SanAndreas\.exe$') { continue }
        return [pscustomobject]@{ Exe = $exe; Root = $Matches[1] }
    }
    return $null
}

function Add-SavrEvidence($Context, [string]$Source, [string]$Target, [bool]$Binary = $false) {
    if (!(Test-Path -LiteralPath $Source -PathType Leaf)) { $Context.Report.Add("MISSING: $Source"); return }
    try {
        $f = Get-Item -LiteralPath $Source
        if ($f.Attributes -band [IO.FileAttributes]::ReparsePoint) { $Context.Report.Add("SKIPPED link: $Source"); return }
        $limit = if ($Binary) { 64MB } else { 12MB }
        if (($Binary -and $f.Length -gt $limit) -or $Context.Bytes + [Math]::Min($f.Length,$limit) -gt 160MB) {
            $Context.Report.Add("SKIPPED size limit: $Source ($($f.Length) bytes)"); return
        }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Target) | Out-Null
        if (!$Binary -and $f.Length -gt $limit) {
            $stream = [IO.File]::Open($Source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
            try {
                [void]$stream.Seek(-$limit,[IO.SeekOrigin]::End)
                $reader = [IO.StreamReader]::new($stream)
                try { Write-SavrText $Target ("[TRUNCATED: newest 12 MB only]`r`n" + $reader.ReadToEnd()) }
                finally { $reader.Dispose() }
            } finally { $stream.Dispose() }
            $Context.Report.Add("TRUNCATED text: $Source; retained newest 12 MB")
        } else { Copy-Item -LiteralPath $Source -Destination $Target -Force }
        if (!$Binary) { Write-SavrText $Target (Protect-SavrText ([IO.File]::ReadAllText($Target)) $Context) }
        $Context.Bytes += [Math]::Min($f.Length,$limit)
        $Context.Report.Add("INCLUDED: $Source | modified=$($f.LastWriteTimeUtc.ToString('o')) | bytes=$($f.Length)")
    } catch { $Context.Report.Add("UNREADABLE: $Source | $($_.Exception.Message)") }
}

function Get-SavrFileRecord([string]$Path, [bool]$Hash) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return "MISSING | $Path" }
    try {
        $f = Get-Item -LiteralPath $Path
        if ($f.Attributes -band [IO.FileAttributes]::ReparsePoint) { return "SKIPPED LINK | $Path" }
        $sha = if ($Hash) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash } else { 'not hashed' }
        return "PRESENT | $Path | bytes=$($f.Length) | modified=$($f.LastWriteTimeUtc.ToString('o')) | SHA256=$sha"
    } catch { return "UNREADABLE | $Path | $($_.Exception.Message)" }
}

function Get-SavrPeIdentity([string]$Path) {
    $stream = $null; $reader = $null
    try {
        $stream = [IO.File]::Open($Path, 'Open', 'Read', [IO.FileShare]::ReadWrite)
        $reader = [IO.BinaryReader]::new($stream)
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5A4D) { return 'PE identity: invalid DOS header' }
        $stream.Position = 60; $offset = $reader.ReadInt32()
        if ($offset -lt 64 -or $offset -gt $stream.Length - 84) { return 'PE identity: invalid header offset' }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550) { return 'PE identity: invalid PE signature' }
        $machine = $reader.ReadUInt16(); [void]$reader.ReadUInt16(); $timestamp = $reader.ReadUInt32()
        $stream.Position = $offset + 24; $magic = $reader.ReadUInt16()
        if ($magic -ne 0x20B) { return 'PE identity: not a PE32+ executable' }
        $stream.Position = $offset + 80; $imageSize = $reader.ReadUInt32()
        return ('PE identity: Machine=0x{0:X}; SizeOfImage=0x{1:X}; TimeDateStamp=0x{2:X}' -f $machine,$imageSize,$timestamp)
    } catch { return ('PE identity: unreadable; ' + $_.Exception.Message) }
    finally { if ($reader) { $reader.Dispose() }; if ($stream) { $stream.Dispose() } }
}

function Get-SavrCompatibilitySummary($Context) {
    $report = [Collections.Generic.List[string]]::new()
    $report.Add('Compatibility evidence summary - not a crash diagnosis')
    $report.Add('INSTALLATION.txt contains the measured game EXE hash/version/PE identity and mod hashes.')
    $report.Add('UEVR/SAVR_compatibility.txt, when present, compares native patch bytes before SAVR modifies them.')
    $report.Add('Check report PID/UTC against this log: a leftover report from an older session is not current proof.')
    $report.Add('Native preflight requires Full diagnostics on startup before injection; enabling Full later cannot inspect original patch bytes.')
    $report.Add('A mismatch may indicate a different game build or another mod. Matching samples do not prove full support.')
    $path = Join-Path $Context.Profile 'log.txt'
    if (!(Test-Path -LiteralPath $path)) { $report.Add('UEVR log missing.'); return $report.ToArray() }
    $stream = $null; $reader = $null
    try {
        # Bound collection even for runaway logs. Read startup plus the latest tail.
        $stream = [IO.File]::Open($path,'Open','Read',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $reader = [IO.StreamReader]::new($stream)
        $chars = [char[]]::new(2MB)
        $count = $reader.ReadBlock($chars,0,$chars.Length)
        $text = [string]::new($chars,0,$count)
        if (!$reader.EndOfStream) {
            $reader.DiscardBufferedData()
            [void]$stream.Seek([Math]::Max($stream.Position,$stream.Length - 2MB),[IO.SeekOrigin]::Begin)
            $tailCount = $reader.ReadBlock($chars,0,$chars.Length)
            $text += "`n[Middle of large log omitted]`n" + [string]::new($chars,0,$tailCount)
            $report.Add('BOUNDED SCAN: first 2M characters plus up to last 2MB; the middle was not scanned.')
        }
        $report.Add('Preflight marker in sampled log: ' + [bool]($text -match '\[Compatibility\] preflight begin'))
        $report.Add('Plugin initialization complete marker in sampled log: ' + [bool]($text -match '\[Compatibility\] plugin initialization complete'))
        $report.Add('Missing markers may mean Full was off at startup, an older plugin, startup failure, or a different log; do not assume success.')
        $pattern = '(?im)^.*(?:\[Compatibility\]|(?:signature|pattern).*(?:not found|mismatch|invalid)|Failed to (?:find|get type info)|cannot resolve instruction|Could not find.*(?:function|offset|address)|Game Module Size:|Commit hash:).*$'
        $groups = [ordered]@{}; $dropped = 0
        foreach ($match in [regex]::Matches($text,$pattern)) {
            $line = $match.Value.Trim()
            $key = $line -replace '^\[[^\]]+\]\s*','' -replace '(?i)\b(?:0x)?[0-9a-f]{10,16}\b','<address>'
            if ($groups.Contains($key)) { $groups[$key].Count++; $groups[$key].Last=$line }
            elseif ($groups.Count -lt 60) { $groups[$key]=[pscustomobject]@{Count=1;First=$line;Last=$line} }
            else { $dropped++ }
        }
        $report.Add('Candidate lookup/patch messages (warnings alone do not prove incompatibility):')
        foreach ($item in $groups.Values) {
            $report.Add("count=$($item.Count) | first=$($item.First)")
            if ($item.Count -gt 1) { $report.Add('last=' + $item.Last) }
        }
        if (!$groups.Count) { $report.Add('No matching messages found in sampled regions.') }
        if ($dropped) { $report.Add("Additional unique messages omitted: $dropped") }
    } catch { $report.Add('Log scan failed: ' + $_.Exception.Message) }
    finally { if ($reader) { $reader.Dispose() }; if ($stream) { $stream.Dispose() } }
    return $report.ToArray()
}

function New-SavrPackage($Context) {
    $Context.Report.Clear(); $Context.Bytes = 0
    $hash = [Security.Cryptography.SHA256]::Create()
    try { $key = [BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($Context.Profile.ToLowerInvariant()))).Replace('-','') }
    finally { $hash.Dispose() }
    $mutex = [Threading.Mutex]::new($false, ('Local\SAVR_Support_' + $key.Substring(0,16)))
    $locked = $false; $staging = $null
    try {
        try { $locked = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked = $true }
        if (!$locked) { throw 'A support package is already being created.' }
        $stamp = (Get-Date -Format 'yyyyMMdd_HHmmss_fff') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8)
        $staging = Join-Path ([IO.Path]::GetTempPath()) "SAVR-Support-$stamp"
        New-Item -ItemType Directory -Path $staging | Out-Null
        $out = Join-Path $Context.Home 'Support Packages'
        New-Item -ItemType Directory -Force -Path $out | Out-Null
        $zip = Join-Path $out "SAVR-Support-$stamp.zip"
        foreach ($name in @('log.txt','status.txt','UEVR_GTASADE_status.txt','config.txt','cameras.txt','cvars_data.txt','cvars_standard.txt',
            'UEVR_GTASADE_config.txt','SAVR-Recovery.ini','SAVR_compatibility.txt','SAVR_diagnostics_active.flag','SAVR_diagnostics_previous_interrupted.flag')) {
            Add-SavrEvidence $Context (Join-Path $Context.Profile $name) (Join-Path $staging "UEVR\$name")
        }
        Add-SavrEvidence $Context (Join-Path $Context.Profile 'crash.dmp') (Join-Path $staging 'UEVR\crash.dmp') $true
        Add-SavrEvidence $Context (Join-Path $Context.Home 'SAVR Emergency Diagnostics Switch.ini') (Join-Path $staging 'UEVR\StartupDiagnostics.ini')
        Add-SavrEvidence $Context (Join-Path $Context.Documents 'Rockstar Games\GTA San Andreas Definitive Edition\Config\WindowsNoEditor\GameUserSettings.ini') (Join-Path $staging 'Game\GameUserSettings.ini')

        $report = [Collections.Generic.List[string]]::new()
        $report.Add("Profile: $($Context.Profile)")
        $report.Add("Documents: $($Context.Documents)")
        $report.Add('Inventory only: no EXE, DLL, ASI, PAK, asset or save files are copied into this ZIP.')
        foreach ($required in @('plugins\UEVR_GTASADE.dll','scripts\DUALGRIP.lua','scripts\GTASADE_FeatureFlags.lua',
            'scripts\GTASADE_LeftHanded.lua','config.txt','cameras.txt','SAVR_ControlGuide.png','UEVR_GTASADE_config.txt')) {
            $report.Add((Get-SavrFileRecord (Join-Path $Context.Profile $required) $true))
        }
        foreach ($dir in @('','plugins','scripts','uobjecthook','SanAndreas\plugins','SanAndreas\scripts')) {
            foreach ($f in Get-SavrFiles (Join-Path $Context.Profile $dir)) {
                if ($f.Extension -in '.dll','.asi','.lua','.json') { $report.Add((Get-SavrFileRecord $f.FullName $true)) }
            }
        }
        $game = Find-SavrGame $Context
        $roots = [ordered]@{}
        foreach ($name in @('Gameface','SanAndreas','GTA San Andreas Definitive Edition','Rockstar Games\GTA San Andreas Definitive Edition')) {
            $roots['Local_' + $name.Replace('\','_')] = Join-Path $Context.Local "$name\Saved"
        }
        $roots['Documents_Rockstar'] = Join-Path $Context.Documents 'Rockstar Games\GTA San Andreas Definitive Edition'
        if ($game) {
            $report.Add("Detected game: $($game.Root)")
            $report.Add('ACTUAL INSTALLED EXECUTABLE (measured, not package reference):')
            $report.Add((Get-SavrFileRecord $game.Exe $true))
            $report.Add((Get-SavrPeIdentity $game.Exe))
            $vi = [Diagnostics.FileVersionInfo]::GetVersionInfo($game.Exe)
            $report.Add("FileVersion=$($vi.FileVersion); ProductVersion=$($vi.ProductVersion)")
            foreach ($rel in @('Gameface\Content\Paks\~mods\500-Holydh_ReducedMuzzleFlash.pak',
                'Gameface\Content\Paks\~mods\501-Holydh_VR_Textures.pak',
                'Gameface\Content\Movies\1080\GTA_SA_RSTAR_STINGER_FINAL_1920x1080.mp4',
                'Gameface\Content\SAVRHands\SAVR_HandGrip_L.uasset','Gameface\Content\SAVRHands\SAVR_HandGrip_L.uexp',
                'Gameface\Content\SAVRHands\SAVR_HandGrip_R.uasset','Gameface\Content\SAVRHands\SAVR_HandGrip_R.uexp')) {
                $report.Add((Get-SavrFileRecord (Join-Path $game.Root $rel) $true))
            }
            foreach ($rel in @('Gameface\Binaries\Win64','Gameface\Content\Paks')) {
                foreach ($f in Get-SavrFiles (Join-Path $game.Root $rel) 2) {
                    if ($f.Extension -in '.dll','.asi','.pak','.ini') { $report.Add((Get-SavrFileRecord $f.FullName $false)) }
                }
            }
            $roots['Gameface_Install'] = Join-Path $game.Root 'Gameface\Saved'
            $roots['Game_Install'] = Join-Path $game.Root 'Saved'
        } else { $report.Add('GAME NOT LOCATED: select SanAndreas.exe in the desktop tool and collect again.') }

        foreach ($root in $roots.Keys) {
            foreach ($kind in @('Logs','Crashes')) {
                $path = Join-Path $roots[$root] $kind
                if (!(Test-Path -LiteralPath $path -PathType Container)) { $Context.Report.Add("MISSING DIRECTORY: $path"); continue }
                $limit = if ($kind -eq 'Logs') { 5 } else { 12 }
                $files = @(Get-SavrFiles $path 2 | Where-Object { $_.Extension -in '.log','.txt','.xml','.runtime-xml','.dmp' } |
                    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First $limit)
                if (!$files.Count) { $Context.Report.Add("NO MATCHING REPORTS: $path") }
                foreach ($f in $files) {
                    $relative = $f.FullName.Substring($path.TrimEnd('\').Length+1)
                    Add-SavrEvidence $Context $f.FullName (Join-Path $staging "Game\$kind\$root\$relative") ($f.Extension -eq '.dmp')
                }
            }
        }
        $manifest = @('Product: San Andreas VR Definitive Edition', "Created: $((Get-Date).ToString('o'))",
            "Windows: $([Environment]::OSVersion.VersionString)", 'Local-only. Nothing uploaded automatically.',
            'Crash dumps are binary memory reports and cannot be redacted. Review before sharing.',
            'See INSTALLATION.txt for actual hashes and COLLECTION.txt for missing/skipped/stale file timestamps.')
        $log = Join-Path $Context.Profile 'log.txt'
        if (Test-Path -LiteralPath $log) {
            $manifest += @(Get-Content -LiteralPath $log -TotalCount 80 | Select-String -Pattern 'Commit hash:|Commits past tag:|Total commits:' |
                Select-Object -First 3 | ForEach-Object { $_.Line })
        }
        $version = Join-Path $PSScriptRoot 'VERSION.txt'
        if (!(Test-Path -LiteralPath $version)) { $version = Join-Path (Split-Path -Parent $PSScriptRoot) 'VERSION.txt' }
        if (!(Test-Path -LiteralPath $version)) { $version = Join-Path $Context.Home 'VERSION.txt' }
        if (Test-Path -LiteralPath $version) { $manifest += @('', 'PACKAGE REFERENCE (not measured game identity):') + @(Get-Content -LiteralPath $version) }
        Write-SavrText (Join-Path $staging 'MANIFEST.txt') (Protect-SavrText ($manifest -join "`r`n") $Context)
        Write-SavrText (Join-Path $staging 'INSTALLATION.txt') (Protect-SavrText ($report -join "`r`n") $Context)
        Write-SavrText (Join-Path $staging 'COMPATIBILITY.txt') (Protect-SavrText ((Get-SavrCompatibilitySummary $Context) -join "`r`n") $Context)
        Write-SavrText (Join-Path $staging 'COLLECTION.txt') (Protect-SavrText ($Context.Report -join "`r`n") $Context)
        Compress-Archive -LiteralPath (Get-ChildItem -LiteralPath $staging).FullName -DestinationPath $zip -CompressionLevel Optimal
        return $zip
    } finally {
        if ($staging) {
            $absolute = [IO.Path]::GetFullPath($staging)
            $parent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
            if ((Split-Path -Parent $absolute) -eq $parent -and (Split-Path -Leaf $absolute) -like 'SAVR-Support-*') {
                Remove-Item -LiteralPath $absolute -Recurse -Force
            }
        }
        if ($locked) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
    }
}
