param([string]$Probe, [string]$Updater, [string]$Root)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$Root=[IO.Path]::GetFullPath($Root)
New-Item -ItemType Directory -Force -Path $Root | Out-Null
function Assert([bool]$value,[string]$message) { if(!$value) { throw $message } }
function Fixture([string]$name,[string]$version) {
    $test=Join-Path $Root ($name+'-'+[guid]::NewGuid().ToString('N'))
    $target=Join-Path $test 'installed'
    $job=Join-Path $test ([guid]::NewGuid().ToString('N'))
    $payload=Join-Path $test 'payload'
    New-Item -ItemType Directory -Force -Path $target,$job,$payload,(Join-Path $payload 'platforms') | Out-Null
    Copy-Item -LiteralPath $Probe -Destination (Join-Path $target 'LambdaPlayer.exe')
    Copy-Item -LiteralPath $Probe -Destination (Join-Path $payload 'LambdaPlayer.exe')
    'old' | Set-Content (Join-Path $target 'version.txt')
    'saved-user-file' | Set-Content (Join-Path $target 'saved.txt')
    'uninstaller' | Set-Content (Join-Path $target 'unins000.dat')
    $version | Set-Content (Join-Path $payload 'version.txt')
    foreach($file in 'Qt6Core.dll','Qt6Widgets.dll','QtWebEngineProcess.exe','libmpv-2.dll','platforms\qwindows.dll') {
        'fixture' | Set-Content (Join-Path $payload $file)
    }
    [IO.Compression.ZipFile]::CreateFromDirectory($payload,(Join-Path $job 'update.zip'))
    $config=Join-Path $job 'job.json'
    @{target=$target;id=(Split-Path $job -Leaf);processId=2147483647;tag='v0.2.8'} | ConvertTo-Json | Set-Content $config
    return @{target=$target;job=$job;config=$config;test=$test}
}
function Run([hashtable]$fixture,[bool]$apply) {
    $args=@('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',$Updater,'-Job',$fixture.config)
    if($apply) { $args+='-Apply' }
    & "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" @args
    return $LASTEXITCODE
}
$normal=Fixture 'success' 'new'
Assert ((Run $normal $false) -eq 0) 'Preparation failed'
Assert ((Get-Content (Join-Path $normal.target 'version.txt')).Trim() -eq 'old') 'Preparation changed the running install'
Assert ((Run $normal $true) -eq 0) 'Apply failed'
Assert ((Get-Content (Join-Path $normal.target 'version.txt')).Trim() -eq 'new') 'New version not installed'
Assert ((Get-Content (Join-Path $normal.target 'saved.txt')).Trim() -eq 'saved-user-file') 'User file lost'
Assert (Test-Path (Join-Path $normal.target 'unins000.dat')) 'Uninstaller lost'
Assert ((Get-Content (Join-Path $normal.job 'result.json') -Raw | ConvertFrom-Json).ok) 'Success result missing'
Write-Output 'PASS: prepare, atomic replacement, restart health, preserve user files and uninstaller'
$failed=Fixture 'rollback' 'bad'
Assert ((Run $failed $false) -eq 0) 'Rollback preparation failed'
Assert ((Run $failed $true) -eq 1) 'Bad startup was accepted'
Assert ((Get-Content (Join-Path $failed.target 'version.txt')).Trim() -eq 'old') 'Rollback failed'
Assert (!(Get-Content (Join-Path $failed.job 'result.json') -Raw | ConvertFrom-Json).ok) 'Failure result missing'
Write-Output 'PASS: startup failure restores previous version'
$wrong=Fixture 'wrong-version' 'wrong'
Assert ((Run $wrong $false) -eq 0) 'Version check preparation failed'
Assert ((Run $wrong $true) -eq 1) 'Wrong release version was accepted'
Assert ((Get-Content (Join-Path $wrong.target 'version.txt')).Trim() -eq 'old') 'Wrong version rollback failed'
Write-Output 'PASS: mismatched release version rolls back'
$unsafe=Fixture 'traversal' 'new'
$zip=[IO.Compression.ZipFile]::Open((Join-Path $unsafe.job 'update.zip'),[IO.Compression.ZipArchiveMode]::Update)
$entry=$zip.CreateEntry('../escaped.txt'); $writer=New-Object IO.StreamWriter($entry.Open()); $writer.Write('bad'); $writer.Dispose(); $zip.Dispose()
Assert ((Run $unsafe $false) -ne 0) 'Traversal archive accepted'
Assert (!(Test-Path (Join-Path $unsafe.test 'escaped.txt'))) 'Archive escaped extraction folder'
Assert ((Get-Content (Join-Path $unsafe.target 'version.txt')).Trim() -eq 'old') 'Unsafe archive changed install'
Write-Output 'PASS: traversal archive rejected without changing install'
$missing=Fixture 'incomplete' 'new'
$zip=[IO.Compression.ZipFile]::Open((Join-Path $missing.job 'update.zip'),[IO.Compression.ZipArchiveMode]::Update)
$zip.GetEntry('LambdaPlayer.exe').Delete(); $zip.Dispose()
Assert ((Run $missing $false) -ne 0) 'Incomplete archive accepted'
Write-Output 'PASS: incomplete package rejected'
