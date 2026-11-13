Write-Host "Cleaning up serial port processes..." -ForegroundColor Yellow
$procs = Get-Process -Name "python", "esptool" -ErrorAction SilentlyContinue
if ($procs) {
    foreach ($p in $procs) {
        Write-Host "Killing process: $($p.Name) ($($p.Id))" -ForegroundColor Red
        Stop-Process -InputObject $p -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 2
}
Write-Host "Starting flash..." -ForegroundColor Cyan
& "C:\Espressif\python_env\idf5.5_py3.10_env\Scripts\python.exe" flash.py all