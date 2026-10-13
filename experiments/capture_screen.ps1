# capture_screen.ps1
# Screenshot helper for the StarFish desktop placement spike: captures the
# whole virtual screen while the spike window is visible.
#
#   powershell -ExecutionPolicy Bypass -File experiments/capture_screen.ps1 `
#       -OutputPath experiments/out/spike_check.png

param(
    [string]$OutputPath = "experiments/out/spike_check.png"
)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$screens = [System.Windows.Forms.Screen]::AllScreens
$x = ($screens | Measure-Object -Property Bounds.X -Minimum).Minimum
$y = ($screens | Measure-Object -Property Bounds.Y -Minimum).Minimum
$w = ($screens | Measure-Object -Property { $_.Bounds.Right } -Maximum).Maximum - $x
$h = ($screens | Measure-Object -Property { $_.Bounds.Bottom } -Maximum).Maximum - $y

$dir = Split-Path -Parent $OutputPath
if ($dir -and -not (Test-Path $dir)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}

$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($x, $y, 0, 0, $bmp.Size)
$bmp.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()

Write-Output "saved $OutputPath (${w}x${h} at ${x},${y})"
