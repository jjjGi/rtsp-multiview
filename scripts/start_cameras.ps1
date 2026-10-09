# RTSP 서버(MediaMTX)와 가상 카메라 4대를 띄운다.
. "$PSScriptRoot\common.ps1"

if (-not (Get-Process mediamtx -ErrorAction SilentlyContinue)) {
    Start-Process -FilePath $MediaMtx -WorkingDirectory (Split-Path $MediaMtx) -WindowStyle Hidden | Out-Null
    Start-Sleep -Seconds 1
}
1..4 | ForEach-Object { Stop-Camera $_; Start-Camera $_ | Out-Null }
Write-Host "cameras: rtsp://127.0.0.1:8554/cam1 ~ cam4"
