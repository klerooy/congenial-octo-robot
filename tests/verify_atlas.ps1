$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$taskAtlasPath=Join-Path (Split-Path -Parent $PSScriptRoot) '2_src_port\assets\weapons_atlas.png'
$taskAtlas=[System.Drawing.Bitmap]::new($taskAtlasPath)
try {
    if($taskAtlas.Width -ne 1024 -or $taskAtlas.Height -ne 64){throw 'Wrong atlas dimensions'}
    if($taskAtlas.GetPixel(0,0).A -ne 0){throw 'Atlas lost transparency'}
    for($taskCell=0;$taskCell -lt 8;$taskCell++) {
        $taskInk=0
        for($taskY=0;$taskY -lt 64;$taskY+=2) {
            for($taskX=0;$taskX -lt 128;$taskX+=2) {
                if($taskAtlas.GetPixel($taskCell*128+$taskX,$taskY).A -gt 0){$taskInk++}
            }
        }
        if($taskInk -lt 30){throw "Empty atlas cell: $taskCell"}
    }
} finally {$taskAtlas.Dispose()}
Write-Output 'ATLAS CHECKS OK: PNG decoding, transparency and eight nonempty weapon cells'
