<#
.SYNOPSIS
    Automated Android SDK Setup Script for Windows
    Designed for AI Agents & Developers

.DESCRIPTION
    This script automatically:
    1. Downloads the Android Command-line Tools.
    2. Sets up the correct directory structure required by sdkmanager.
    3. Configures ANDROID_HOME and PATH environment variables (User Scope).
    4. Installs platform-tools and build-tools.
    5. Accepts all licenses automatically.

.NOTES
    Run this script in PowerShell.
    No Admin privileges required for User Scope installation.
#>

$ErrorActionPreference = "Stop"

# --- Configuration ---
$InstallDir = "$env:USERPROFILE\AndroidSDK"
$CmdLineToolsUrl = "https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip"
$ZipPath = "$env:TEMP\cmdline-tools.zip"

Write-Host ">>> Starting Automated Android Environment Setup..." -ForegroundColor Cyan

# 1. Create Directory
if (-not (Test-Path $InstallDir)) {
    New-Item -ItemType Directory -Path $InstallDir | Out-Null
    Write-Host "[+] Created installation directory: $InstallDir" -ForegroundColor Green
}

# 2. Download Command Line Tools
Write-Host ">>> Downloading Android Command-line Tools..." -ForegroundColor Cyan
Invoke-WebRequest -Uri $CmdLineToolsUrl -OutFile $ZipPath
Write-Host "[+] Download complete." -ForegroundColor Green

# 3. Extract and Restructure
# The SDK expects the structure: cmdline-tools/latest/bin
Write-Host ">>> Extracting..." -ForegroundColor Cyan
Expand-Archive -Path $ZipPath -DestinationPath $InstallDir -Force

# Move to 'latest' (Critical step for sdkmanager to work)
$Source = "$InstallDir\cmdline-tools"
$Dest = "$InstallDir\cmdline-tools\latest"

if (-not (Test-Path $Dest)) {
    Write-Host ">>> Restructuring for SDK compliance..." -ForegroundColor Cyan
    # Rename extracted 'cmdline-tools' to 'latest' inside a new 'cmdline-tools' folder is tricky
    # Easier: Move content to temp, recreate structure
    $TempMove = "$InstallDir\temp_tools"
    Move-Item -Path $Source -Destination $TempMove
    
    New-Item -ItemType Directory -Path $Source | Out-Null
    Move-Item -Path $TempMove -Destination $Dest
    Write-Host "[+] Directory structure fixed: $Dest" -ForegroundColor Green
}

# 4. Set Environment Variables (User Scope)
Write-Host ">>> Configuring Environment Variables..." -ForegroundColor Cyan
[System.Environment]::SetEnvironmentVariable("ANDROID_HOME", $InstallDir, [System.EnvironmentVariableTarget]::User)

# Add to PATH
$CurrentPath = [System.Environment]::GetEnvironmentVariable("Path", [System.EnvironmentVariableTarget]::User)
$NewPaths = @(
    "$InstallDir\platform-tools",
    "$InstallDir\cmdline-tools\latest\bin"
)

foreach ($P in $NewPaths) {
    if ($CurrentPath -notlike "*$P*") {
        $CurrentPath += ";$P"
        Write-Host "[+] Adding to PATH: $P" -ForegroundColor Green
    }
}
[System.Environment]::SetEnvironmentVariable("Path", $CurrentPath, [System.EnvironmentVariableTarget]::User)

# 5. Refresh Env for current session
$env:ANDROID_HOME = $InstallDir
$env:Path += ";$InstallDir\platform-tools;$InstallDir\cmdline-tools\latest\bin"

# 6. Install Components
Write-Host ">>> Installing SDK Components (This may take a while)..." -ForegroundColor Cyan
$SdkManager = "$InstallDir\cmdline-tools\latest\bin\sdkmanager.bat"

# Auto-accept licenses using 'yes' pipe
# Install: platform-tools, build-tools (latest), and platform (API 34)
$Components = "platform-tools", "platforms;android-34", "build-tools;34.0.0"

foreach ($Comp in $Components) {
    Write-Host "   Installing: $Comp"
    cmd /c "echo y | `"$SdkManager`" --sdk_root=`"$InstallDir`" `"$Comp`"" 
}

Write-Host "`n>>> SETUP COMPLETE! <<<" -ForegroundColor Green
Write-Host "Please restart your terminal (VS Code) to apply environment variable changes." -ForegroundColor Yellow
