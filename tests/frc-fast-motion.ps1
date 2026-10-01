param([string]$Bench, [string]$Root, [string]$Model, [string]$Source)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force -Path $Root | Out-Null
$inputImage = [Drawing.Image]::FromFile($Source)
$canvas = New-Object Drawing.Bitmap(6000,3400)
$graphics = [Drawing.Graphics]::FromImage($canvas)
$graphics.DrawImage($inputImage,0,0,6000,3400)
$graphics.Dispose()
$inputImage.Dispose()
try {
    foreach ($pan in @(@{name='fast-pan';dx=384;dy=128}, @{name='very-fast-pan';dx=640;dy=192})) {
        $folder = Join-Path $Root $pan.name
        New-Item -ItemType Directory -Force -Path $folder | Out-Null
        foreach ($frame in @(@{name='A';t=0}, @{name='B';t=1}, @{name='truth';t=0.5})) {
            $rect = New-Object Drawing.Rectangle( (900+[int]($pan.dx*$frame.t)), (400+[int]($pan.dy*$frame.t)),3840,2160)
            $crop = $canvas.Clone($rect, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
            $crop.Save((Join-Path $folder ($frame.name+'.png')), [Drawing.Imaging.ImageFormat]::Png)
            $crop.Dispose()
        }
        foreach ($mode in @('rife-reuse','rife')) {
            foreach ($sample in @(@{fps=48;t='0.5'}, @{fps=60;t='0.4'})) {
                $arguments = @('interp',$mode,(Join-Path $folder 'A.png'),(Join-Path $folder 'B.png'),
                    (Join-Path $folder "$mode-$($sample.fps).png"),'--model',$Model,
                    '--analysis','540','--fps',"$($sample.fps)",'--time',$sample.t,'--repeat','2')
                if ($sample.fps -eq 48) { $arguments += @('--truth',(Join-Path $folder 'truth.png')) }
                $output = & $Bench @arguments 2>&1 | Out-String
                if ($LASTEXITCODE -ne 0) { throw "Motion benchmark failed: $output" }
                if (($mode -eq 'rife-reuse' -and $output -notmatch 'fast pairs: static 0\s+cuts 0') -or $output -notmatch 'stage network') {
                    throw "Fast movement was held as a scene cut: $($pan.name), $mode, $($sample.fps): $output"
                }
                if ($sample.fps -eq 48) {
                    if ($output -notmatch 'result ([\d.]+) dB.*repeat A ([\d.]+) dB') { throw "Missing quality result: $output" }
                    if ([double]$Matches[1] -lt [double]$Matches[2]+2) { throw "Generated motion failed to improve on the held original: $output" }
                }
                Write-Output "PASS: $($pan.name), $mode, 4K 24->$($sample.fps), motion interpolated without false cuts"
            }
        }
    }
} finally { $canvas.Dispose() }
