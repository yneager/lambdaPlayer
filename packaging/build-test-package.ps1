param(
    [string]$AppDir = "$PSScriptRoot\..\_local\app",
    [string]$OutputDir = "$PSScriptRoot\..\dist\LAMBDA-Friend-Test",
    [string]$Compiler = "$PSScriptRoot\..\_local\tools\inno\compiler\ISCC.exe",
    [string]$BuildExe = "$PSScriptRoot\..\_local\build\LambdaPlayer.exe",
    [string]$BuildCache = "$PSScriptRoot\..\_local\build\CMakeCache.txt",
    [string]$QtDir = 'C:\Qt\6.8.3\msvc2022_64',
    [string]$UpdateBaseUrl = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$releaseLine = Get-Content $BuildCache | Where-Object { $_ -match '^LAMBDA_RELEASE_TAG:STRING=' } | Select-Object -First 1
if (!$releaseLine) { throw 'Configure the application release tag before packaging' }
$releaseTag = $releaseLine.Substring('LAMBDA_RELEASE_TAG:STRING='.Length)
if ($releaseTag -notmatch '^v?\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?$') { throw 'Invalid application release tag' }
$releaseVersion = $releaseTag -replace '^v',''
$app = (Resolve-Path $AppDir).Path
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$package = (Resolve-Path $OutputDir).Path
$payload = Join-Path $package 'LAMBDA Player'
New-Item -ItemType Directory -Force $payload | Out-Null
Copy-Item "$app\*" $payload -Recurse -Force
Copy-Item $BuildExe $payload -Force
Copy-Item "$repo\tools\extra\win32\x64\aria2c.exe" $payload -Force
Copy-Item "$repo\resources\rife\rife.vpy" "$payload\rife\rife.vpy" -Force
$deploy = Join-Path $QtDir 'bin\windeployqt.exe'
& $deploy --release --no-translations --no-compiler-runtime "$payload\LambdaPlayer.exe"
if ($LASTEXITCODE -ne 0) { throw 'Qt runtime deployment failed' }
Copy-Item (Join-Path $QtDir 'bin\Qt6Concurrent.dll') $payload -Force
foreach ($name in 'frc_bench.exe','vc_redist.x64.exe') {
    $path = Join-Path $payload $name
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
}
# App-local CRTs let the app, Qt WebEngine, and interpolation plugins run
# without a separate administrator-level redistributable installation.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
$crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory | Sort-Object FullName -Descending | Select-Object -First 1
if (!$crt) { throw 'MSVC x64 runtime folder not found' }
Copy-Item "$($crt.FullName)\*.dll" $payload -Force
Copy-Item "$repo\licenses" $payload -Recurse -Force
Copy-Item "$repo\THIRD_PARTY_NOTICES.md" $payload -Force
Copy-Item "$repo\tools\stream-server\THIRD-PARTY.txt" "$payload\licenses\stream-server-THIRD-PARTY.txt" -Force
if (Test-Path "$repo\_local\deps\stream-server\stream-server-LICENSE.txt") {
    Copy-Item "$repo\_local\deps\stream-server\stream-server-LICENSE.txt" "$payload\licenses" -Force
}
foreach ($required in 'LambdaPlayer.exe','libmpv-2.dll','Qt6Concurrent.dll','QtWebEngineProcess.exe','resources\qtwebengine_resources.pak','platforms\qwindows.dll','lambda-stream-server.exe','rife\rife.vpy','vcruntime140.dll','msvcp140.dll') {
    if (!(Test-Path -LiteralPath (Join-Path $payload $required))) { throw "Missing package dependency: $required" }
}
$archiveName = "LAMBDA-Player-Windows-x64-$releaseTag.zip"
$archivePath = Join-Path $package $archiveName
if (Test-Path -LiteralPath $archivePath) { Remove-Item -LiteralPath $archivePath }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($payload, $archivePath, [IO.Compression.CompressionLevel]::Optimal, $false)
if (!$UpdateBaseUrl) { $UpdateBaseUrl = "https://github.com/yneager/lambdaPlayer/releases/download/$releaseTag" }
if ($UpdateBaseUrl -notmatch '^https://') { throw 'Update files must be hosted over HTTPS' }
$archiveHash = (Get-FileHash -LiteralPath $archivePath).Hash.ToLowerInvariant()
$feedNotesPath = Join-Path $repo "docs/releases/$releaseTag.md"
$feedNotes = if (Test-Path -LiteralPath $feedNotesPath) { [IO.File]::ReadAllText($feedNotesPath) } else { 'One-click updates with automatic restart and recovery.' }
@{schemaVersion=1; releases=@(@{version=$releaseTag; prerelease=$releaseTag.Contains('-');
  url="$($UpdateBaseUrl.TrimEnd('/'))/$archiveName"; sha256=$archiveHash;
  size=(Get-Item -LiteralPath $archivePath).Length; notes=$feedNotes})} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $package 'update-feed.json') -Encoding utf8
& $Compiler "/DPayloadDir=$payload" "/DPackageDir=$package" "/DReleaseVersion=$releaseVersion" "$PSScriptRoot\windows-test.iss"
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed' }
Copy-Item "$PSScriptRoot\TESTING.txt" $package -Force
$files = Get-ChildItem $payload -Recurse -File | ForEach-Object {
    $hash = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
    [pscustomobject]@{file=$_.FullName.Substring($payload.Length+1); bytes=$_.Length; sha256=$hash.Hash}
}
$files | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath "$package\payload-manifest.json" -Encoding utf8
$installerName = "LAMBDA-Player-Setup-$releaseVersion-x64.exe"
$installerHash = (Get-FileHash (Join-Path $package $installerName)).Hash.ToLowerInvariant()
"$installerHash  $installerName" | Set-Content "$package\SHA256.txt" -Encoding ascii
"$archiveHash  $archiveName" | Add-Content "$package\SHA256.txt" -Encoding ascii
Write-Host "Ready to share: $package"
