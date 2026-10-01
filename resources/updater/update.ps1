param([Parameter(Mandatory=$true)][string]$Job, [switch]$Apply)
$ErrorActionPreference = 'Stop'
$config = Get-Content -LiteralPath $Job -Raw | ConvertFrom-Json
$target = [IO.Path]::GetFullPath($config.target).TrimEnd('\')
$parent = Split-Path $target -Parent
$id = $config.id
if ($id -notmatch '^[a-f0-9]{32}$' -or !$parent -or $target -eq [IO.Path]::GetPathRoot($target)) { throw 'Invalid update target' }
$next = Join-Path $parent ".lambda-next-$id"
$backup = Join-Path $parent ".lambda-previous-$id"
$jobDir = Split-Path ([IO.Path]::GetFullPath($Job)) -Parent
$result = Join-Path $jobDir 'result.json'
$health = Join-Path $jobDir 'ready'
function SafeRemove([string]$path) {
    $full = [IO.Path]::GetFullPath($path)
    if ($full -ne $next -and $full -ne $backup) { throw 'Unsafe cleanup path' }
    if ((Split-Path $full -Parent) -ne $parent) { throw 'Cleanup outside install parent' }
    if (Test-Path -LiteralPath $full) {
        if ((Get-Item -LiteralPath $full).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Unsafe cleanup link' }
        Get-ChildItem -LiteralPath $full -Recurse -Force | ForEach-Object {
            if ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Unsafe cleanup link' }
        }
        Remove-Item -LiteralPath $full -Recurse -Force
    }
}
function Report([bool]$ok, [string]$message) {
    @{ok=$ok; tag=$config.tag; message=$message} | ConvertTo-Json | Set-Content -LiteralPath $result -Encoding UTF8
}
function Launch([bool]$checkHealth) {
    if ($checkHealth) { $env:LAMBDA_UPDATE_HEALTH_FILE = $health }
    else { Remove-Item Env:LAMBDA_UPDATE_HEALTH_FILE -ErrorAction SilentlyContinue }
    Start-Process -FilePath (Join-Path $target 'LambdaPlayer.exe') -WorkingDirectory $target -PassThru
}
if (!$Apply) {
    try {
        if (!(Test-Path -LiteralPath (Join-Path $target 'LambdaPlayer.exe'))) { throw 'Install folder is missing' }
        if ((Get-Item -LiteralPath $target).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Install folder is a link' }
        if ((Test-Path -LiteralPath $next) -or (Test-Path -LiteralPath $backup)) { throw 'Update transaction already exists' }
        New-Item -ItemType Directory -Path $next | Out-Null
        # Preserve uninstaller, portable configuration and any user-added files.
        Get-ChildItem -LiteralPath $target -Recurse -Force | ForEach-Object {
            if ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Install folder contains a link' }
        }
        Get-ChildItem -LiteralPath $target -Force | Copy-Item -Destination $next -Recurse -Force
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [IO.Compression.ZipFile]::OpenRead((Join-Path $jobDir 'update.zip'))
        try {
            $root = $next + [IO.Path]::DirectorySeparatorChar
            $total = [long]0
            $names = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
            if ($archive.Entries.Count -gt 50000) { throw 'Update has too many files' }
            foreach ($entry in $archive.Entries) {
                $name = $entry.FullName.Replace('/','\')
                if ([IO.Path]::IsPathRooted($name) -or $name.Contains(':') -or ($name.Split('\') -contains '..')) { throw 'Unsafe archive path' }
                foreach ($part in $name.TrimEnd('\').Split('\')) {
                    if (!$part -or $part.EndsWith('.') -or $part.EndsWith(' ') -or $part -match '^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\.|$)') { throw 'Unsafe Windows filename' }
                }
                $dest = [IO.Path]::GetFullPath((Join-Path $next $name))
                if (!$dest.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) -or !$names.Add($dest)) { throw 'Invalid archive entry' }
                if ((($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000 -or ($entry.ExternalAttributes -band 0x400)) { throw 'Archive contains a link' }
                $total += $entry.Length
                if ($total -gt 8GB) { throw 'Update exceeds extraction limit' }
                if ($name.EndsWith('\')) { New-Item -ItemType Directory -Force -Path $dest | Out-Null }
                else {
                    New-Item -ItemType Directory -Force -Path (Split-Path $dest -Parent) | Out-Null
                    [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dest, $true)
                }
            }
            foreach ($required in 'LambdaPlayer.exe','Qt6Core.dll','Qt6Widgets.dll','QtWebEngineProcess.exe','libmpv-2.dll','platforms\qwindows.dll') {
                if (!$names.Contains((Join-Path $next $required))) { throw "Update is missing $required" }
            }
        } finally { $archive.Dispose() }
    } catch { SafeRemove $next; throw }
    exit 0
}
$movedOld = $false
$movedNew = $false
$newProcess = $null
try {
    $oldProcess = Get-Process -Id $config.processId -ErrorAction SilentlyContinue
    if ($oldProcess -and !$oldProcess.WaitForExit(60000)) { throw 'LAMBDA did not close; update cancelled' }
    # WebEngine may release its DLLs a moment after the parent exits.
    for ($attempt=0; $attempt -lt 40; $attempt++) {
        try { [IO.Directory]::Move($target,$backup); $movedOld=$true; break }
        catch { if ($attempt -eq 39) { throw }; Start-Sleep -Milliseconds 250 }
    }
    [IO.Directory]::Move($next,$target); $movedNew=$true
    $newProcess = Launch $true
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    while (!(Test-Path -LiteralPath $health)) {
        if ($newProcess.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw 'Updated app failed its startup check' }
        Start-Sleep -Milliseconds 250
    }
    if ((Get-Content -LiteralPath $health -Raw).Trim() -ne $config.tag) { throw 'Update version does not match the release' }
    Report $true "Updated to $($config.tag)"
    # Keep Windows Installed Apps version in sync for per-user installations.
    $key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{EA0BD783-856D-479D-89AA-26B443231FEB}_is1'
    if (Test-Path $key) {
        $installed = Get-ItemProperty $key
        if ($installed.InstallLocation.TrimEnd('\') -eq $target) {
            Set-ItemProperty $key DisplayVersion ($config.tag -replace '^v','') -ErrorAction SilentlyContinue
        }
    }
    try { SafeRemove $backup } catch { } # A locked backup is recoverable; next launch remains usable.
} catch {
    $message = $_.Exception.Message
    if ($newProcess -and !$newProcess.HasExited) { $newProcess.Kill(); $newProcess.WaitForExit(10000) | Out-Null }
    if ($movedNew) {
        for ($attempt=0; $attempt -lt 40; $attempt++) {
            try { [IO.Directory]::Move($target,$next); break }
            catch { if ($attempt -eq 39) { Report $false "Rollback needs recovery from $backup : $message"; exit 1 }; Start-Sleep -Milliseconds 250 }
        }
    }
    if ($movedOld) { [IO.Directory]::Move($backup,$target) }
    Report $false "Update was cancelled; your previous version was kept. $message"
    if (!$oldProcess -or $oldProcess.HasExited) { Launch $false | Out-Null }
    try { SafeRemove $next } catch { }
    exit 1
}
