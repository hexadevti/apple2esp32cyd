# run_desktop.ps1 — launch the desktop (SDL+ImGui) build and optionally screenshot the composited
# window for offline verification. Env vars must be set in-process (the Git Bash "VAR=val ./exe"
# prefix does NOT reach the native .exe — only $env: from PowerShell does).
#
#   pwsh tools/run_desktop.ps1 -Platform apple2                      # just run (Ctrl-C to stop)
#   pwsh tools/run_desktop.ps1 -Platform c64 -Capture cap.png -At 120 -Quit   # screenshot frame 120 then exit
param(
  [string]$Platform = 'apple2',
  [string]$Sd       = 'C:/Users/lucia/repos/emu6502/build/sdcard',
  [string]$Capture  = '',          # output PNG path (relative to build/) — empty = no capture
  [int]   $At       = 120,         # frame to capture at
  [switch]$Quit,                   # exit right after the capture
  [int]   $TimeoutMs = 25000
)

$build = 'C:\Users\lucia\repos\emu6502\build'
Set-Location $build

$env:EMU_PLATFORM = $Platform
$env:EMU_SD_DIR   = $Sd
if ($Capture) {
  $bmp = [System.IO.Path]::ChangeExtension($Capture, '.bmp')
  $env:EMU_UI_DUMP    = $bmp
  $env:EMU_UI_DUMP_AT = "$At"
  if ($Quit) { $env:EMU_UI_QUIT = '1' } else { Remove-Item Env:EMU_UI_QUIT -ErrorAction SilentlyContinue }
  if (Test-Path $bmp) { Remove-Item $bmp }
} else {
  Remove-Item Env:EMU_UI_DUMP -ErrorAction SilentlyContinue
}

$p = Start-Process -FilePath .\emu6502.exe -PassThru -NoNewWindow `
       -RedirectStandardOutput run.log -RedirectStandardError run.err.log
if (-not $p.WaitForExit($TimeoutMs)) { $p.Kill(); Write-Host "TIMEOUT (killed)" }
else { Write-Host "exited code $($p.ExitCode)" }

if ($Capture -and (Test-Path $bmp)) {
  Add-Type -AssemblyName System.Drawing
  $img = [System.Drawing.Image]::FromFile((Resolve-Path $bmp))
  $img.Save((Join-Path $build $Capture), [System.Drawing.Imaging.ImageFormat]::Png)
  $img.Dispose()
  Remove-Item $bmp
  Write-Host "capture -> $build\$Capture"
}
