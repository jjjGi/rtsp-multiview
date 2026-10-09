# 장애 재현: 카메라 하나를 N초 동안 끊었다가 다시 켠다.
# 예) .\scripts\cut_camera.ps1 -Cam 2 -Seconds 10
param([int]$Cam = 2, [int]$Seconds = 10)
. "$PSScriptRoot\common.ps1"

Write-Host "$(Get-Date -Format HH:mm:ss.fff)  CAM$Cam down for $Seconds s"
Stop-Camera $Cam
Start-Sleep -Seconds $Seconds
Start-Camera $Cam | Out-Null
Write-Host "$(Get-Date -Format HH:mm:ss.fff)  CAM$Cam back"
