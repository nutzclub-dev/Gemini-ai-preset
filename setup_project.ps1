# ==============================================================================
# setup_project.ps1 - Automated Setup Tool for VS2022 Dependencies
# ==============================================================================
$ErrorActionPreference = "Stop"

Write-Host "[1/4] Downloading miniaudio single-header library..." -ForegroundColor Cyan
Invoke-WebRequest -Uri "https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h" -OutFile "miniaudio.h"

Write-Host "[2/4] Downloading SDL3 development release for Visual Studio..." -ForegroundColor Cyan
$sdlZip = "SDL3-devel-windows-VC.zip"
Invoke-WebRequest -Uri "https://github.com/libsdl-org/SDL/releases/download/preview-3.2.0/SDL3-devel-3.2.0-VC.zip" -OutFile $sdlZip
Expand-Archive -Path $sdlZip -DestinationPath "sdl3_temp" -Force
$sdlExtracted = Get-ChildItem -Path "sdl3_temp" -Directory | Select-Object -First 1
Move-Item -Path $sdlExtracted.FullName -Destination "SDL3" -Force
Remove-Item -Recurse -Force "sdl3_temp", $sdlZip

Write-Host "[3/4] Creating folder structures and default configuration..." -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path "preset" | Out-Null
New-Item -ItemType Directory -Force -Path "save" | Out-Null

if (-not (Test-Path "api.key")) {
    Set-Content -Path "api.key" -Value "YOUR_GEMINI_API_KEY`nYOUR_FALLBACK_API_KEY"
    Write-Host "Created api.key placeholder. Enter your API keys on lines 1 and 2." -ForegroundColor Yellow
}

Write-Host "[4/4] Dependency Setup Complete!" -ForegroundColor Green
Write-Host "Note: projectM-4 and curl can be installed via vcpkg:"
Write-Host "vcpkg install curl:x64-windows projectm:x64-windows" -ForegroundColor Cyan
