# Build LAMBDA Player from this folder and deploy a runnable copy to _local\app.
#   .\build.ps1               build + deploy + launch
#   .\build.ps1 -NoRun        build + deploy only
#   .\build.ps1 -Reconfigure  re-run CMake configure first (after CMakeLists changes)
#   .\build.ps1 -Test         build + run the unit tests (no deploy)
param([switch]$Reconfigure, [switch]$NoRun, [switch]$Test)
$ErrorActionPreference = "Stop"
$repo  = $PSScriptRoot
$local = "$repo\_local"
$deps  = "$local\deps"
$build = "$local\build"
$out   = "$local\app"
$qt    = "C:\Qt\6.8.3\msvc2022_64"
$bt    = "C:\Program Files\Microsoft Visual Studio\18\BuildTools"

foreach ($requiredTool in @("$bt\VC\Auxiliary\Build\vcvars64.bat", "$qt\bin\windeployqt.exe")) {
    if (-not (Test-Path -LiteralPath $requiredTool)) { throw "Required build tool is missing: $requiredTool" }
}

# Double-clicking the CMD launcher does not inherit Codex's Node runtime PATH.
# Verify a cached engine directly so normal/offline builds do not require Node.
$enginePath = Join-Path $repo 'tools\extra\win32\x64\aria2c.exe'
$engineLock = Get-Content -LiteralPath (Join-Path $repo 'tools\download-engine\engine.lock.json') -Raw | ConvertFrom-Json
$expectedEngineHash = $engineLock.assets.'win32-x64'.binarySha256
$engineReady = $false
if (Test-Path -LiteralPath $enginePath) {
    $engineStream = [System.IO.File]::OpenRead($enginePath)
    $engineHasher = [System.Security.Cryptography.SHA256]::Create()
    try {
        $actualEngineHash = [BitConverter]::ToString($engineHasher.ComputeHash($engineStream)).Replace('-', '')
        $engineReady = $actualEngineHash -eq $expectedEngineHash
    } finally {
        $engineStream.Dispose()
        $engineHasher.Dispose()
    }
}
if (-not $engineReady) {
    $nodeCommand = Get-Command node -ErrorAction SilentlyContinue
    $nodePath = if ($nodeCommand) { $nodeCommand.Source } else { $null }
    if (-not $nodePath) {
        $bundledNode = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\node\bin\node.exe'
        if (Test-Path -LiteralPath $bundledNode) { $nodePath = $bundledNode }
    }
    if (-not $nodePath) { throw "The download engine is missing and Node.js is unavailable. Install Node.js and run Build and Run.cmd again." }
    & $nodePath "$repo\tools\download-engine\fetch-engine.mjs" --platform win32 --arch x64
    if ($LASTEXITCODE -ne 0) { throw "download engine setup failed" }
}

# MSVC environment (vcvars64) imported into this PowerShell session.
$ErrorActionPreference = "Continue"
$envDump = cmd /c "`"$bt\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>nul && set"
$compilerExitCode = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($compilerExitCode -ne 0) { throw "Visual Studio compiler setup failed ($compilerExitCode)." }
foreach ($line in $envDump) { if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] } }
$env:Path = "$bt\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$bt\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;$qt\bin;" + $env:Path

if ($Reconfigure -or -not (Test-Path "$build\build.ninja")) {
    cmake -S $repo -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -U LAMBDA_RELEASE_TAG -DCMAKE_PREFIX_PATH="$qt" `
        -DMPV_INCLUDE_DIR="$deps\mpv\include" -DMPV_LIBRARY="$deps\mpv-import\mpv.lib"
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
}
if ($Test) { cmake --build $build --parallel }
else { cmake --build $build --target LambdaPlayer --parallel }
if ($LASTEXITCODE -ne 0) { throw "build failed" }

if ($Test) {
    # Run with Qt's DLL directory on PATH. CTest's Windows test launcher can
    # otherwise show a modal "Qt6Test.dll was not found" dialog for each test.
    if (-not $env:QT_QPA_PLATFORM) { $env:QT_QPA_PLATFORM = "offscreen" }
    foreach ($testExe in Get-ChildItem -LiteralPath $build -Filter "tst_*.exe") {
        & $testExe.FullName -silent
        if ($LASTEXITCODE -ne 0) { throw "$($testExe.Name) failed ($LASTEXITCODE)" }
    }
    return
}

# Close only this deployed player, allowing its download engine to save and exit.
$deployedExe = Join-Path $out 'LambdaPlayer.exe'
foreach ($runningPlayer in Get-Process LambdaPlayer -ErrorAction SilentlyContinue) {
    if ($runningPlayer.Path -ne $deployedExe) { continue }
    if ($runningPlayer.HasExited) { continue }
    # The player has no tray/background mode. A stale startup with no window
    # cannot receive CloseMainWindow and otherwise permanently blocks updates.
    if ($runningPlayer.MainWindowHandle -eq 0) {
        if ($runningPlayer.WaitForExit(2000)) { continue }
        $runningPlayer.Refresh()
        if ($runningPlayer.MainWindowHandle -eq 0) {
            Write-Host "Closing stalled player process $($runningPlayer.Id)."
            Stop-Process -Id $runningPlayer.Id -Force
            $runningPlayer.WaitForExit()
            continue
        }
    }
    if (-not $runningPlayer.CloseMainWindow() -or -not $runningPlayer.WaitForExit(10000)) {
        throw "Close the running LAMBDA Player, then run Build and Run.cmd again."
    }
}
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item "$build\LambdaPlayer.exe" $out -Force
$deployedEngine = Join-Path $out 'aria2c.exe'
if (-not (Test-Path -LiteralPath $deployedEngine) -or
    [Convert]::ToBase64String([System.IO.File]::ReadAllBytes($enginePath)) -ne
    [Convert]::ToBase64String([System.IO.File]::ReadAllBytes($deployedEngine))) {
    Copy-Item -LiteralPath $enginePath -Destination $deployedEngine -Force
}
Copy-Item "$repo\licenses" $out -Recurse -Force
Copy-Item "$repo\THIRD_PARTY_NOTICES.md" $out -Force
Copy-Item "$deps\mpv\libmpv-2.dll" "$out\libmpv-2.dll" -Force
$ErrorActionPreference = "Continue"
$deployLog = Join-Path $local 'qt-deploy.log'
& "$qt\bin\windeployqt.exe" --release --no-translations --compiler-runtime "$out\LambdaPlayer.exe" *> $deployLog
$deployExitCode = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($deployExitCode -ne 0) { throw "Qt runtime deployment failed ($deployExitCode). See $deployLog" }
if (-not (Test-Path "$out\rife")) {
    Copy-Item "$deps\rife" "$out\rife" -Recurse -Force
    Copy-Item "$deps\vapoursynth" "$out\vapoursynth" -Recurse -Force
}
Copy-Item "$repo\resources\rife\rife.vpy" "$out\rife\rife.vpy" -Force
# Built-in streaming engine (_local\tools\build-streamserver.ps1).
if (Test-Path "$deps\stream-server\lambda-stream-server.exe") {
    Copy-Item "$deps\stream-server\lambda-stream-server.exe" $out -Force
}
Write-Host "Built: $out\LambdaPlayer.exe"

foreach ($runtimeFile in @('libmpv-2.dll', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll',
    'Qt6Concurrent.dll', 'Qt6WebEngineCore.dll', 'QtWebEngineProcess.exe',
    'platforms\qwindows.dll', 'resources\qtwebengine_resources.pak')) {
    if (-not (Test-Path -LiteralPath (Join-Path $out $runtimeFile))) {
        throw "The deployed player is missing $runtimeFile. See $deployLog"
    }
}

if (-not $NoRun) {
    # Shell launch detaches the GUI from the build console. Redirecting a GUI's
    # streams here can keep a calling CMD pipeline alive until the app closes.
    $logDir = Join-Path $local 'logs'
    New-Item -ItemType Directory -Force $logDir | Out-Null
    $startupLog = Join-Path $logDir ("player-" + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '.log')
    "Starting $deployedExe" | Set-Content -LiteralPath $startupLog
    $player = Start-Process -FilePath $deployedExe -WorkingDirectory $out -WindowStyle Normal -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 200
        $player.Refresh()
        if ($player.HasExited) {
            "Exited during startup: $($player.ExitCode)" | Add-Content -LiteralPath $startupLog
            throw "LAMBDA Player exited during startup ($($player.ExitCode)). See $startupLog"
        }
    } while ($player.MainWindowHandle -eq 0 -and [DateTime]::UtcNow -lt $deadline)
    if ($player.MainWindowHandle -eq 0) {
        "No window appeared within 20 seconds. Process: $($player.Id)" | Add-Content -LiteralPath $startupLog
        throw "LAMBDA Player did not open a window. See $startupLog"
    }
    "Opened window. Process: $($player.Id)" | Add-Content -LiteralPath $startupLog
    Write-Host "Opened LAMBDA Player. Startup log: $startupLog"
}
