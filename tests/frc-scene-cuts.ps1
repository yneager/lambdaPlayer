param([string]$Bench, [string]$Root, [string]$Model = '')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force -Path $Root | Out-Null
function Image([string]$file,[int]$r,[int]$g,[int]$b,[string]$pattern = "") {
    $bitmap=New-Object Drawing.Bitmap(3840,2160)
    $graphics=[Drawing.Graphics]::FromImage($bitmap)
    $graphics.Clear([Drawing.Color]::FromArgb($r,$g,$b))
    if ($pattern) {
        $brush=New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb($r+32,$g+32,$b+32))
        if($pattern -eq 'vertical'){ for($x=0;$x -lt 3840;$x+=768){$graphics.FillRectangle($brush,$x,0,384,2160)} }
        else { for($y=0;$y -lt 2160;$y+=768){$graphics.FillRectangle($brush,0,$y,3840,384)} }
        $brush.Dispose()
    }
    $graphics.Dispose()
    $bitmap.Save($file,[Drawing.Imaging.ImageFormat]::Png); $bitmap.Dispose()
}
$cases=@(
    @{name='color-cut';a=@(64,0,0);b=@(0,19,0)},
    @{name='dark-cut';a=@(20,0,0);b=@(0,6,0)},
    @{name='bright-cut';a=@(30,30,30);b=@(220,220,220)},
    @{name='same-palette-structure-cut';a=@(64,64,64);b=@(64,64,64);patternA='vertical';patternB='horizontal'}
)
$modes=@('block')
if ($Model) { $modes+=@('rife-reuse','rife') }
foreach($case in $cases) {
    $a=Join-Path $Root ($case.name+'-A.png'); $b=Join-Path $Root ($case.name+'-B.png')
    Image $a $case.a[0] $case.a[1] $case.a[2] $case.patternA
    Image $b $case.b[0] $case.b[1] $case.b[2] $case.patternB
    foreach($mode in $modes) {
        foreach($sample in @(@{fps=48;time='0.5'},@{fps=60;time='0.4'},@{fps=60;time='0.8'},@{fps=240;time='0.5'},@{fps=240;time='0.9'},@{fps=240;time='1.0'})) {
            $time=$sample.time
            $output=Join-Path $Root ($case.name+'-'+$mode+'-'+$sample.fps+'-'+$time+'.png')
            $arguments=@('interp',$mode,$a,$b,$output,'--fps',([string]$sample.fps),'--time',$time)
            if($Model -and $mode -ne 'block') { $arguments+=@('--model',$Model,'--analysis','360') }
            $log=& $Bench @arguments
            if($LASTEXITCODE -ne 0) { throw "Interpolation failed: $log" }
            $bitmap=New-Object Drawing.Bitmap($output)
            $reference=New-Object Drawing.Bitmap($(if($time -eq '1.0'){$b}else{$a}))
            if($bitmap.Width -ne 3840 -or $bitmap.Height -ne 2160) { throw 'Output resolution changed' }
            foreach($point in @(@(0,0),@(1920,1080),@(3839,2159),@(800,300),@(300,800))) {
                $pixel=$bitmap.GetPixel($point[0],$point[1])
                $expected=$reference.GetPixel($point[0],$point[1])
                if($pixel.R -ne $expected.R -or $pixel.G -ne $expected.G -or $pixel.B -ne $expected.B) {
                    throw "Scene cut was warped/blended or advanced early: $($case.name), $mode, $time ($($pixel.R),$($pixel.G),$($pixel.B))"
                }
            }
            $bitmap.Dispose(); $reference.Dispose()
        }
        Write-Output "PASS: $($case.name), $mode, 4K, preceding shot held until the exact source boundary"
    }
}
