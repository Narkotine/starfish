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

$vs = [System.Windows.Forms.SystemInformation]::VirtualScreen

$dir = Split-Path -Parent $OutputPath
if ($dir -and -not (Test-Path $dir)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}

$bmp = New-Object System.Drawing.Bitmap($vs.Width, $vs.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($vs.X, $vs.Y, 0, 0, $bmp.Size)
$bmp.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()

Write-Output "saved $OutputPath ($($vs.Width)x$($vs.Height) at $($vs.X),$($vs.Y))"
