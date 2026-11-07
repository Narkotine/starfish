# Builds the per-user StarFish installer.
#   pwsh tools/build_installer.ps1 [-BuildDir build] [-Iscc <path>]
# Stages build\starfish.exe + its translations into build\installer\stage, runs
# windeployqt there (clean runtime, no test binaries), then ISCC.
param([string]$BuildDir = "build", [string]$Iscc = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
$exe = Join-Path $build "starfish.exe"
if (-not (Test-Path $exe)) { throw "$exe not found: build first (cmake --build $BuildDir)" }

# Single version source: project(StarFish VERSION x.y.z) in CMakeLists.txt.
$m = [regex]::Match((Get-Content (Join-Path $root "CMakeLists.txt") -Raw), 'project\(StarFish\s+VERSION\s+([0-9.]+)')
if (-not $m.Success) { throw "no project(StarFish VERSION ...) in CMakeLists.txt" }
$version = $m.Groups[1].Value

if (-not $Iscc) {
    $Iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
              "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
              "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Iscc) { throw "ISCC.exe not found (winget install JRSoftware.InnoSetup --scope user)" }
$wdq = Get-Command windeployqt -ErrorAction SilentlyContinue
if (-not $wdq) { throw "windeployqt not on PATH (add Qt's bin)" }

$out = Join-Path $build "installer"
$stage = Join-Path $out "stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory $stage -Force | Out-Null
Copy-Item $exe $stage
Get-ChildItem $build -Filter "starfish_*.qm" | Copy-Item -Destination $stage
& $wdq.Source --release --no-system-d3d-compiler --no-opengl-sw --compiler-runtime (Join-Path $stage "starfish.exe") | Out-Null
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

& $Iscc /Qp "/DAppVersion=$version" "/DStageDir=$stage" "/DOutDir=$out" (Join-Path $root "installer\starfish.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC failed" }
$setup = Join-Path $out "StarFish-$version-setup.exe"
$hash = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower()
"$hash  StarFish-$version-setup.exe" | Set-Content "$setup.sha256" -Encoding ascii
Write-Host "$setup`nsha256 $hash"
