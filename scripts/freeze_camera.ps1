# 장애 재현 2: 연결은 열어 둔 채 카메라를 N초 동안 멈춘다 (응답 없는 카메라).
# 뷰어는 3초 동안 패킷이 없으면 끊긴 것으로 판단하고 다시 연결해야 한다.
# 예) .\scripts\freeze_camera.ps1 -Cam 3 -Seconds 8
param([int]$Cam = 3, [int]$Seconds = 8)
. "$PSScriptRoot\common.ps1"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Nt {
    [DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr h);
    [DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr h);
}
'@
$proc = Get-CimInstance Win32_Process -Filter "Name='ffmpeg.exe'" |
    Where-Object { $_.CommandLine -like "*8554/cam$Cam*" } | Select-Object -First 1
if (-not $proc) { throw "CAM$Cam publisher not running" }
$p = Get-Process -Id $proc.ProcessId
Write-Host "$(Get-Date -Format HH:mm:ss.fff)  CAM$Cam frozen for $Seconds s"
[Nt]::NtSuspendProcess($p.Handle) | Out-Null
Start-Sleep -Seconds $Seconds
[Nt]::NtResumeProcess($p.Handle) | Out-Null
Write-Host "$(Get-Date -Format HH:mm:ss.fff)  CAM$Cam resumed"
