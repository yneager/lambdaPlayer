param(
    [string]$AppDir = "$PSScriptRoot\..\_local\app",
    [string]$OutputDir = "$PSScriptRoot\..\dist\LAMBDA-Friend-Test",
    [string]$Compiler = "$PSScriptRoot\..\_local\tools\inno\compiler\ISCC.exe"
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$app = (Resolve-Path $AppDir).Path
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$package = (Resolve-Path $OutputDir).Path
$payload = Join-Path $package 'LAMBDA Player'
New-Item -ItemType Directory -Force $payload | Out-Null
Copy-Item "$app\*" $payload -Recurse -Force
Copy-Item "$repo\_local\build\LambdaPlayer.exe" $payload -Force
$deploy = 'C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe'
& $deploy --release --no-translations --no-compiler-runtime "$payload\LambdaPlayer.exe"
if ($LASTEXITCODE -ne 0) { throw 'Qt runtime deployment failed' }
Copy-Item 'C:\Qt\6.8.3\msvc2022_64\bin\Qt6Concurrent.dll' $payload -Force
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
Copy-Item "$repo\_local\deps\stream-server\stream-server-LICENSE.txt" "$payload\licenses" -Force
foreach ($required in 'LambdaPlayer.exe','libmpv-2.dll','Qt6Concurrent.dll','QtWebEngineProcess.exe','resources\qtwebengine_resources.pak','platforms\qwindows.dll','lambda-stream-server.exe','rife\rife.vpy','vcruntime140.dll','msvcp140.dll') {
    if (!(Test-Path -LiteralPath (Join-Path $payload $required))) { throw "Missing package dependency: $required" }
}
& $Compiler "/DPayloadDir=$payload" "/DPackageDir=$package" "$PSScriptRoot\windows-test.iss"
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed' }
Copy-Item "$PSScriptRoot\TESTING.txt" $package -Force
$files = Get-ChildItem $payload -Recurse -File | ForEach-Object {
    $hash = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
    [pscustomobject]@{file=$_.FullName.Substring($payload.Length+1); bytes=$_.Length; sha256=$hash.Hash}
}
$files | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath "$package\payload-manifest.json" -Encoding utf8
$installerName = 'LAMBDA-Player-Setup-0.2.7-test.1-x64.exe'
$installerHash = (Get-FileHash (Join-Path $package $installerName)).Hash.ToLowerInvariant()
"$installerHash  $installerName" | Set-Content "$package\SHA256.txt" -Encoding ascii
Write-Host "Ready to share: $package"
