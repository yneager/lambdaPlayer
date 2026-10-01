# Rebuild the Windows multi-resolution ICO from the same lambda paths as lambda.svg.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$output = Join-Path $repo 'resources\branding'
$images = @()
foreach ($size in 16,24,32,48,64,128,256) {
    $bitmap = New-Object System.Drawing.Bitmap(1024,1024)
    $g = [System.Drawing.Graphics]::FromImage($bitmap)
    $g.SmoothingMode = 'AntiAlias'
    $g.ScaleTransform(4,4)
    $shape = New-Object System.Drawing.Drawing2D.GraphicsPath
    $shape.AddArc(5,5,108,108,180,90)
    $shape.AddArc(143,5,108,108,270,90)
    $shape.AddArc(143,143,108,108,0,90)
    $shape.AddArc(5,143,108,108,90,90)
    $shape.CloseFigure()
    $rect = New-Object System.Drawing.Rectangle(5,5,246,246)
    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect,[System.Drawing.ColorTranslator]::FromHtml('#223744'),[System.Drawing.ColorTranslator]::FromHtml('#070d18'),65)
    $g.FillPath($bg,$shape)
    $border = New-Object System.Drawing.Pen([System.Drawing.ColorTranslator]::FromHtml('#4a686f'),2)
    $g.DrawPath($border,$shape)
    $mark = New-Object System.Drawing.Drawing2D.GraphicsPath
    $mark.AddBezier(90,56,103,47,113,53,121,72)
    $mark.AddLine(121,72,179,201)
    $mark.StartFigure()
    $mark.AddLine(134,101,76,201)
    $ink = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect,[System.Drawing.ColorTranslator]::FromHtml('#d5fff8'),[System.Drawing.ColorTranslator]::FromHtml('#54e8d3'),80)
    $pen = New-Object System.Drawing.Pen($ink,27)
    $pen.StartCap='Round'; $pen.EndCap='Round'; $pen.LineJoin='Round'
    $g.DrawPath($pen,$mark)
    $small = New-Object System.Drawing.Bitmap($size,$size)
    $sg = [System.Drawing.Graphics]::FromImage($small)
    $sg.InterpolationMode='HighQualityBicubic'
    $sg.DrawImage($bitmap,0,0,$size,$size)
    $stream = New-Object System.IO.MemoryStream
    $small.Save($stream,[System.Drawing.Imaging.ImageFormat]::Png)
    $images += ,($stream.ToArray())
    if ($size -eq 256) { $small.Save((Join-Path $output 'lambda.png'),[System.Drawing.Imaging.ImageFormat]::Png) }
    $stream.Dispose(); $sg.Dispose(); $small.Dispose(); $pen.Dispose(); $ink.Dispose(); $mark.Dispose(); $border.Dispose(); $bg.Dispose(); $shape.Dispose(); $g.Dispose(); $bitmap.Dispose()
}
$file = [System.IO.File]::Create((Join-Path $output 'lambda.ico'))
$writer = New-Object System.IO.BinaryWriter($file)
$writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]7)
$offset = 6 + 16 * 7
$sizes = @(16,24,32,48,64,128,256)
for ($i=0;$i -lt 7;$i++) {
    $dimension = if($sizes[$i] -eq 256){0}else{$sizes[$i]}
    $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
    $writer.Write([byte]0); $writer.Write([byte]0)
    $writer.Write([uint16]1); $writer.Write([uint16]32)
    $writer.Write([uint32]$images[$i].Length); $writer.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach($bytes in $images){$writer.Write([byte[]]$bytes)}
$writer.Dispose(); $file.Dispose()
