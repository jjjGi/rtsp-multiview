# 가상 카메라와 RTSP 서버를 모두 끈다.
. "$PSScriptRoot\common.ps1"
1..4 | ForEach-Object { Stop-Camera $_ }
Get-Process mediamtx -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host "stopped"
