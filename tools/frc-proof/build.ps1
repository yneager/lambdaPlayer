$ErrorActionPreference = "Continue"
$bt = "C:\Program Files\Microsoft Visual Studio\18\BuildTools"
$envDump = cmd /c "`"$bt\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>nul && set"
foreach ($line in $envDump) { if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] } }
$repo = Split-Path (Split-Path $PSScriptRoot)
$amf = "$repo\third_party\amf"
Push-Location $PSScriptRoot
cl /nologo /EHsc /O2 /std:c++20 /utf-8 /DUNICODE /D_UNICODE /I"$amf" frcproof.cpp "$amf\public\common\AMFFactory.cpp" "$amf\public\common\Thread.cpp" "$amf\public\common\AMFSTL.cpp" "$amf\public\common\TraceAdapter.cpp" "$amf\public\common\Windows\ThreadWindows.cpp" /Fe:frcproof.exe /link opengl32.lib gdi32.lib user32.lib d3d11.lib dxgi.lib winmm.lib ole32.lib 2>&1 | Select-String -Pattern 'error|warning C4' | Select-Object -First 20
Pop-Location

