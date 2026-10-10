# 장애 재현 테스트: 가상 카메라에 장애 3종을 일으키고, probe.exe 로그로 감지·복구 시간을 잰다.
#   cut          송출 프로세스를 10초 동안 종료했다가 다시 켠다 (카메라 전원 차단)
#   freeze       송출 프로세스를 8초 동안 일시 정지한다 (연결은 열린 채 응답 없음)
#   video_stop   영상은 7초 뒤 끊기고 오디오만 계속 온다 (영상만 멈춘 카메라)
# 사용: .\tests\failure_tests.ps1        (먼저 build.bat 실행)
. "$PSScriptRoot\..\scripts\common.ps1"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Nt {
    [DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr h);
    [DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr h);
}
'@

$Probe = Join-Path $Root 'build\probe.exe'
$OutDir = Join-Path $Root 'logs\tests'
New-Item -ItemType Directory -Force $OutDir | Out-Null

function Stamp { Get-Date -Format 'HH:mm:ss.fff' }
function ToMs([string]$t) { [TimeSpan]::Parse($t).TotalMilliseconds }

# 로그 한 줄: "HH:mm:ss.fff  [이름]  내용"
function Read-Log([string]$path) {
    Get-Content $path | Where-Object { $_ -match '^(\d\d:\d\d:\d\d\.\d{3})  \[(.+?)\]  (.*)$' } | ForEach-Object {
        [pscustomobject]@{ T = ToMs $Matches[1]; Src = $Matches[2]; Msg = $Matches[3] }
    }
}

function Start-ProbeRun([string]$name, [string]$url, [int]$seconds) {
    $log = Join-Path $OutDir "$name.log"
    Remove-Item $log -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $Probe -ArgumentList $name, $url, $seconds -RedirectStandardOutput $log `
        -WindowStyle Hidden -PassThru
    return @{ Proc = $p; Log = $log }
}

function Start-AvCamera([int]$n, [int]$videoSeconds) {
    # 영상은 videoSeconds 뒤 끝나고, 오디오(사인파)는 계속 송출된다.
    $args = @('-hide_banner', '-loglevel', 'error',
              '-re', '-f', 'lavfi', '-i', "testsrc2=size=640x360:rate=25:duration=$videoSeconds",
              '-re', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
              '-map', '0:v', '-map', '1:a', '-c:v', 'libopenh264', '-b:v', '1M', '-g', '25', '-c:a', 'aac',
              # 기본값이면 영상이 끊긴 뒤 오디오도 최대 10초 동안 붙잡아 둔다. 오디오만 계속 흐르도록 0.1초로 줄인다.
              '-max_interleave_delta', '100000',
              '-f', 'rtsp', '-rtsp_transport', 'tcp', "rtsp://127.0.0.1:8554/cam$n")
    Start-Process -FilePath $FFmpeg -ArgumentList $args -WindowStyle Hidden -PassThru
}

function Get-Camera([int]$n) {
    Get-CimInstance Win32_Process -Filter "Name='ffmpeg.exe'" | Where-Object { $_.CommandLine -like "*8554/cam$n*" } |
        Select-Object -First 1
}

if (-not (Get-Process mediamtx -ErrorAction SilentlyContinue)) {
    Start-Process -FilePath $MediaMtx -WorkingDirectory (Split-Path $MediaMtx) -WindowStyle Hidden | Out-Null
    Start-Sleep -Seconds 1
}

$results = @()

# ---- 1. cut ---------------------------------------------------------------
Stop-Camera 2; Start-Camera 2 | Out-Null; Start-Sleep -Seconds 2
$run = Start-ProbeRun 'cut' 'rtsp://127.0.0.1:8554/cam2' 32
Start-Sleep -Seconds 6
$down = Stamp; Stop-Camera 2
Start-Sleep -Seconds 10
$back = Stamp; Start-Camera 2 | Out-Null
$run.Proc.WaitForExit(); Stop-Camera 2
$l = Read-Log $run.Log
$tDown = ToMs $down; $tBack = ToMs $back
$lost = $l | Where-Object { $_.Msg -like 'lost:*' -and $_.T -ge $tDown } | Select-Object -First 1
$conn = $l | Where-Object { $_.Msg -like 'connecting*' -and $_.T -ge $tBack } | Select-Object -First 1
$live = $l | Where-Object { $_.Msg -like 'LIVE*' -and $_.T -ge $tBack } | Select-Object -First 1
# 영상이 나온 세션의 접속 시작 (그 앞 시도는 송출기가 아직 준비되지 않아 실패했을 수 있다)
$lastConn = $l | Where-Object { $_.Msg -like 'connecting*' -and $live -and $_.T -le $live.T } | Select-Object -Last 1
$sum = ($l | Where-Object { $_.Msg -like 'SUMMARY*' }).Msg
$ok = $lost -and $live -and $sum -match 'state=LIVE' -and $sum -match 'reconnects=1'
$results += [pscustomobject]@{
    Test = 'cut'; Pass = [bool]$ok
    Detail = if ($ok) {
        'down->lost {0:N0} ms | back->next connect {1:N0} ms (backoff wait) | successful connect->first frame {2:N0} ms | back->LIVE {3:N0} ms' -f `
            ($lost.T - $tDown), ($conn.T - $tBack), ($live.T - $lastConn.T), ($live.T - $tBack)
    } else { "log: $($run.Log)" }
}

# ---- 2. freeze ------------------------------------------------------------
Stop-Camera 3; Start-Camera 3 | Out-Null; Start-Sleep -Seconds 2
$run = Start-ProbeRun 'freeze' 'rtsp://127.0.0.1:8554/cam3' 30
Start-Sleep -Seconds 6
$proc = Get-Process -Id (Get-Camera 3).ProcessId
$frz = Stamp; [Nt]::NtSuspendProcess($proc.Handle) | Out-Null
Start-Sleep -Seconds 8
$res = Stamp; [Nt]::NtResumeProcess($proc.Handle) | Out-Null
$run.Proc.WaitForExit(); Stop-Camera 3
$l = Read-Log $run.Log
$tFrz = ToMs $frz; $tRes = ToMs $res
$lost = $l | Where-Object { $_.Msg -like 'lost: stalled*' -and $_.T -ge $tFrz } | Select-Object -First 1
$sum = ($l | Where-Object { $_.Msg -like 'SUMMARY*' }).Msg
$ok = $lost -and $sum -match 'state=LIVE' -and $sum -notmatch 'reconnects=0'
$gap = if ($lost -and $lost.Msg -match 'for (\d+) ms') { [int]$Matches[1] } else { 0 }
$live = $l | Where-Object { $_.Msg -like 'LIVE*' -and $lost -and $_.T -ge $lost.T } | Select-Object -First 1
$results += [pscustomobject]@{
    Test = 'freeze'; Pass = [bool]$ok
    Detail = if ($ok) {
        'freeze->last frame {0:N0} ms (frames already in flight) | last frame->lost {1:N0} ms | resume->LIVE {2:N0} ms' -f `
            ($lost.T - $gap - $tFrz), $gap, ($live.T - $tRes)
    } else { "log: $($run.Log)" }
}

# ---- 3. video_stop (audio continues) -------------------------------------
Stop-Camera 5
$pub = Start-AvCamera 5 7
$start = Stamp
Start-Sleep -Milliseconds 1500
$run = Start-ProbeRun 'video_stop' 'rtsp://127.0.0.1:8554/cam5' 12
$run.Proc.WaitForExit()
$audioAlive = -not $pub.HasExited
Stop-Camera 5
$l = Read-Log $run.Log
$live = $l | Where-Object { $_.Msg -like 'LIVE*' } | Select-Object -First 1
$lost = $l | Where-Object { $_.Msg -like 'lost: stalled: no video frame*' } | Select-Object -First 1
$gap = if ($lost -and $lost.Msg -match 'for (\d+) ms') { [int]$Matches[1] } else { 0 }
$other = if ($lost -and $lost.Msg -match '\((\d+) other packets') { [int]$Matches[1] } else { 0 }
$ok = $live -and $lost -and $audioAlive -and $gap -lt 3600 -and $other -gt 0
$results += [pscustomobject]@{
    Test = 'video_stop'; Pass = [bool]$ok
    Detail = if ($ok) { 'last video frame->lost {0:N0} ms while {1} audio packets kept arriving' -f $gap, $other } else { "log: $($run.Log) audioAlive=$audioAlive" }
}

$results | Format-Table -AutoSize -Wrap | Out-String -Width 220 | Tee-Object -FilePath (Join-Path $OutDir 'results.txt')
if ($results.Pass -contains $false) { exit 1 }
