# Assembles the portable VapourSynth + RIFE frame-interpolation runtime that
# LAMBDA Player loads through mpv's `vapoursynth` video filter.
#
# Every third-party download is pinned to a specific release/commit and verified
# with SHA-256 before it is used. Update the table below deliberately when
# bumping a dependency and record the change in THIRD_PARTY_NOTICES.md.
#
# Usage: assemble-rife-runtime.ps1 -PortableDir <dir> [-DownloadDir <dir>]
param(
    [Parameter(Mandatory = $true)][string]$PortableDir,
    [string]$DownloadDir = "deps\rife-downloads"
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")

# ---------------------------------------------------------------------------
# Pinned third-party artifacts
# ---------------------------------------------------------------------------
$PythonVersion = "3.12.10"
$VapourSynthRelease = "R80"
$RifePluginTag = "r9_mod_v33"
$RifePluginCommit = "c3ec6aabc07c8fa37a4f58d7fed9e2ad1fc1b13f"
$RifeModelDir = "rife-v4.6_ensembleFalse"
# Generic D3D11 frame generation (src/frc/rifed3d11.*) runs this flownet
# directly; same pinned commit as the plugin model above.
$FrcModelDir = "rife-v4.26_ensembleFalse"
$MiscFiltersTag = "R2"

$Artifacts = @(
    @{ Name = "python-$PythonVersion-embed-amd64.zip"
       Url  = "https://www.python.org/ftp/python/$PythonVersion/python-$PythonVersion-embed-amd64.zip"
       Sha256 = "4ACBED6DD1C744B0376E3B1CF57CE906F9DC9E95E68824584C8099A63025A3C3" },
    @{ Name = "VapourSynth64-Portable-$VapourSynthRelease.zip"
       Url  = "https://github.com/vapoursynth/vapoursynth/releases/download/$VapourSynthRelease/VapourSynth64-Portable-$VapourSynthRelease.zip"
       Sha256 = "5D927152D9DB29D104C8960BF44D0DF7777835F7741D310184BFAE6FDA2F4F22" },
    @{ Name = "librife_windows_x86-64.dll"
       Url  = "https://github.com/styler00dollar/VapourSynth-RIFE-ncnn-Vulkan/releases/download/$RifePluginTag/librife_windows_x86-64.dll"
       Sha256 = "36A25B471BE88E6F915320C818022DC8657DD9BEAC22A8C3158BD7F4260CC410" },
    @{ Name = "flownet.bin"
       Url  = "https://raw.githubusercontent.com/styler00dollar/VapourSynth-RIFE-ncnn-Vulkan/$RifePluginCommit/models/$RifeModelDir/flownet.bin"
       Sha256 = "03393CF14FE6D0AF015D24D93BAE1E5925F2828CB3B6555300A132643C617AA5" },
    @{ Name = "flownet.param"
       Url  = "https://raw.githubusercontent.com/styler00dollar/VapourSynth-RIFE-ncnn-Vulkan/$RifePluginCommit/models/$RifeModelDir/flownet.param"
       Sha256 = "AE9B08AF43FBA97E27AA2C40C04C35B1A75F637B2D2B01A941FA94361AB25CBD" },
    @{ Name = "frc-flownet.bin"
       Url  = "https://raw.githubusercontent.com/styler00dollar/VapourSynth-RIFE-ncnn-Vulkan/$RifePluginCommit/models/$FrcModelDir/flownet.bin"
       Sha256 = "94D58E30B75D7C7609CFA6F3BDAD524DEDDD14F5F75E85C36D2F827EF5C64731" },
    @{ Name = "frc-flownet.param"
       Url  = "https://raw.githubusercontent.com/styler00dollar/VapourSynth-RIFE-ncnn-Vulkan/$RifePluginCommit/models/$FrcModelDir/flownet.param"
       Sha256 = "79F16C28903F93F8308F0C4C947F8C7E0C17D99A57B85473E5F298DD578D137B" },
    @{ Name = "miscfilters-r2.7z"
       Url  = "https://github.com/vapoursynth/vs-miscfilters-obsolete/releases/download/$MiscFiltersTag/miscfilters-r2.7z"
       Sha256 = "54CF54C4D66151C01C1C663ED0D47CA99C7F1B0A94927BD99B35362C02172BD2" }
)

function Get-Verified([hashtable]$Artifact) {
    $path = Join-Path $DownloadDir $Artifact.Name
    if (-not (Test-Path $path)) {
        Write-Host "Downloading $($Artifact.Name)"
        Invoke-WebRequest -Headers @{ "User-Agent" = "LAMBDA Player-GitHub-Actions" } -Uri $Artifact.Url -OutFile $path
    }
    $hash = (Get-FileHash -Algorithm SHA256 $path).Hash
    if ($hash -ne $Artifact.Sha256) {
        Remove-Item $path -Force
        throw "SHA-256 mismatch for $($Artifact.Name): expected $($Artifact.Sha256), got $hash"
    }
    Write-Host "Verified $($Artifact.Name) ($hash)"
    return $path
}

New-Item -ItemType Directory -Force $DownloadDir | Out-Null
$files = @{}
foreach ($a in $Artifacts) { $files[$a.Name] = Get-Verified $a }

$vsRoot = Join-Path $PortableDir "vapoursynth"
$sitePackages = Join-Path $vsRoot "Lib\site-packages"
$rifeDir = Join-Path $PortableDir "rife"
$licenseDir = Join-Path $PortableDir "licenses"
$work = Join-Path $DownloadDir "work"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Force $vsRoot, $sitePackages, $rifeDir, $licenseDir, $work | Out-Null

# --- Embedded Python (layout mirrors VapourSynth's Install-Portable script) ----
Write-Host "Extracting embedded Python $PythonVersion"
Expand-Archive -LiteralPath $files["python-$PythonVersion-embed-amd64.zip"] -DestinationPath $vsRoot -Force
$pth = Get-ChildItem $vsRoot -Filter "python*._pth" | Select-Object -First 1
if (-not $pth) { throw "python._pth not found in embedded Python" }
Add-Content -Path $pth.FullName -Encoding ASCII -Value "Lib\site-packages"
Copy-Item (Join-Path $vsRoot "LICENSE.txt") (Join-Path $licenseDir "Python-LICENSE.txt") -Force

# --- VapourSynth wheel -> Lib\site-packages (equivalent of `pip install`) -----
Write-Host "Extracting VapourSynth $VapourSynthRelease"
Expand-Archive -LiteralPath $files["VapourSynth64-Portable-$VapourSynthRelease.zip"] -DestinationPath (Join-Path $work "vs") -Force
$wheel = Get-ChildItem (Join-Path $work "vs\wheel") -Filter "*.whl" | Select-Object -First 1
if (-not $wheel) { throw "VapourSynth wheel not found in portable zip" }
$wheelZip = Join-Path $work "vapoursynth-wheel.zip"
Copy-Item $wheel.FullName $wheelZip
Expand-Archive -LiteralPath $wheelZip -DestinationPath (Join-Path $work "wheel") -Force
# Skip debug symbols (~80 MB) - not needed at runtime.
Get-ChildItem (Join-Path $work "wheel") -Recurse -Filter "*.pdb" | Remove-Item -Force
Copy-Item (Join-Path $work "wheel\*") $sitePackages -Recurse -Force
$vsscript = Join-Path $sitePackages "vapoursynth\vsscript.dll"
if (-not (Test-Path $vsscript)) { throw "vsscript.dll missing after wheel extraction" }
$lgpl = Get-ChildItem $sitePackages -Recurse -Filter "COPYING.LESSER" | Select-Object -First 1
if ($lgpl) { Copy-Item $lgpl.FullName (Join-Path $licenseDir "VapourSynth-COPYING.LESSER.txt") -Force }

# --- RIFE plugin, model and scene-change plugin -------------------------------
Write-Host "Installing RIFE plugin + model"
Copy-Item $files["librife_windows_x86-64.dll"] (Join-Path $rifeDir "librife_windows_x86-64.dll") -Force
$modelDir = Join-Path $rifeDir "models\$RifeModelDir"
New-Item -ItemType Directory -Force $modelDir | Out-Null
Copy-Item $files["flownet.bin"] (Join-Path $modelDir "flownet.bin") -Force
Copy-Item $files["flownet.param"] (Join-Path $modelDir "flownet.param") -Force
$frcModelPath = Join-Path $rifeDir "models\$FrcModelDir"
New-Item -ItemType Directory -Force $frcModelPath | Out-Null
Copy-Item $files["frc-flownet.bin"] (Join-Path $frcModelPath "flownet.bin") -Force
Copy-Item $files["frc-flownet.param"] (Join-Path $frcModelPath "flownet.param") -Force

Write-Host "Installing misc.SCDetect plugin"
& 7z x $files["miscfilters-r2.7z"] "-o$(Join-Path $work 'misc')" -y | Out-Null
if ($LASTEXITCODE -ne 0) { throw "7z extraction of miscfilters failed" }
Copy-Item (Join-Path $work "misc\win64\MiscFilters.dll") (Join-Path $rifeDir "MiscFilters.dll") -Force

# --- LAMBDA Player script and license notices ------------------------------------
Copy-Item (Join-Path $RepoRoot "resources\rife\rife.vpy") (Join-Path $rifeDir "rife.vpy") -Force
Copy-Item (Join-Path $RepoRoot "licenses\*") $licenseDir -Force
Copy-Item (Join-Path $RepoRoot "THIRD_PARTY_NOTICES.md") $PortableDir -Force

# --- Sanity check --------------------------------------------------------------
$required = @(
    (Join-Path $vsRoot "python.exe"),
    (Join-Path $vsRoot "python3.dll"),
    $vsscript,
    (Join-Path $sitePackages "vapoursynth\libvapoursynth.dll"),
    (Join-Path $sitePackages "vapoursynth\vapoursynth.pyd"),
    (Join-Path $rifeDir "rife.vpy"),
    (Join-Path $rifeDir "librife_windows_x86-64.dll"),
    (Join-Path $rifeDir "MiscFilters.dll"),
    (Join-Path $modelDir "flownet.bin"),
    (Join-Path $modelDir "flownet.param"),
    (Join-Path $frcModelPath "flownet.bin"),
    (Join-Path $frcModelPath "flownet.param")
)
foreach ($r in $required) { if (-not (Test-Path $r)) { throw "Runtime file missing: $r" } }
Write-Host "RIFE runtime assembled in $PortableDir"
