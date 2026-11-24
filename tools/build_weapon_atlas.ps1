$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$taskAssets = Join-Path (Split-Path -Parent $PSScriptRoot) '2_src_port\assets'
New-Item -ItemType Directory -Path $taskAssets -Force | Out-Null
# Eight 128x64 cells: dp9, rc556, kl545, shotgun, smg, sniper, melee, unknown.
$taskShapes = @(
    @{ Color='#BBC1CA'; Polygons=@('24,18 96,18 101,24 99,32 68,32 61,49 46,49 50,31 25,31','67,33 79,33 75,40 65,40') },
    @{ Color='#79DF91'; Polygons=@('8,24 29,27 39,22 83,22 83,25 118,25 118,31 82,31 73,36 61,36 64,48 51,48 47,35 32,33 13,40','60,33 73,34 68,49 59,47') },
    @{ Color='#79DF91'; Polygons=@('8,24 31,26 40,22 80,22 82,25 119,25 119,30 79,30 70,36 57,35 58,48 47,48 43,35 29,34 10,41','60,34 73,35 80,48 75,54 65,49') },
    @{ Color='#FF6767'; Polygons=@('7,26 31,29 42,23 119,23 119,29 65,30 58,36 46,37 43,48 33,46 35,35 14,42','63,29 87,29 87,35 62,35') },
    @{ Color='#79DF91'; Polygons=@('12,25 31,25 39,19 82,19 85,24 113,24 113,30 80,30 71,35 61,35 61,47 49,47 47,32 30,31 15,37','66,32 78,32 76,50 67,50') },
    @{ Color='#79DF91'; Polygons=@('7,24 33,28 46,25 90,25 91,28 120,28 120,32 76,33 62,37 57,47 47,47 46,35 28,34 10,42','55,17 80,17 80,23 55,23','66,33 72,33 72,47 65,47') },
    @{ Color='#BBC1CA'; Polygons=@('15,37 28,28 53,33 111,18 116,22 64,42 46,44 26,37 19,44') },
    @{ Color='#BBC1CA'; Polygons=@('13,25 35,28 44,23 94,23 95,26 116,26 116,31 73,32 64,38 59,48 47,48 45,35 30,34 15,41') }
)
$taskBitmap = New-Object System.Drawing.Bitmap(4096,256,[System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$taskGraphics = [System.Drawing.Graphics]::FromImage($taskBitmap)
$taskGraphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$taskGraphics.Clear([System.Drawing.Color]::Transparent)
$taskGraphics.ScaleTransform(4,4)
$taskSvg = '<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="64" viewBox="0 0 1024 64">'
try {
    for ($taskIndex=0; $taskIndex -lt $taskShapes.Count; $taskIndex++) {
        $taskShape=$taskShapes[$taskIndex]
        $taskBrush=New-Object System.Drawing.SolidBrush([System.Drawing.ColorTranslator]::FromHtml($taskShape.Color))
        foreach($taskPolygon in $taskShape.Polygons) {
            $taskPoints = @($taskPolygon.Split(' ') | ForEach-Object {
                $taskXY=$_.Split(',')
                [System.Drawing.PointF]::new([float]$taskXY[0]+128*$taskIndex,[float]$taskXY[1])
            })
            $taskGraphics.FillPolygon($taskBrush,[System.Drawing.PointF[]]$taskPoints)
            $taskSvg += '<polygon fill="'+$taskShape.Color+'" points="'+(($taskPoints | ForEach-Object { "$($_.X),$($_.Y)" }) -join ' ')+'"/>'
        }
        $taskBrush.Dispose()
    }
    $taskFinal = New-Object System.Drawing.Bitmap(1024,64,[System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $taskFinalGraphics=[System.Drawing.Graphics]::FromImage($taskFinal)
    $taskFinalGraphics.InterpolationMode=[System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $taskFinalGraphics.DrawImage($taskBitmap,0,0,1024,64)
    $taskFinal.Save((Join-Path $taskAssets 'weapons_atlas.png'),[System.Drawing.Imaging.ImageFormat]::Png)
    $taskFinalGraphics.Dispose(); $taskFinal.Dispose()
    [IO.File]::WriteAllText((Join-Path $taskAssets 'weapons_atlas.svg'),$taskSvg+'</svg>')
} finally { $taskGraphics.Dispose(); $taskBitmap.Dispose() }
Write-Output 'WEAPON ATLAS OK: 1024x64 RGBA, eight cells, editable SVG source'
