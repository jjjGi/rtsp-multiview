# 가상 카메라 공통 함수. 실제 카메라 대신 FFmpeg 테스트 영상을 RTSP로 송출한다.
$Root = Split-Path -Parent $PSScriptRoot
$FFmpeg = Join-Path $Root 'third_party\ffmpeg\bin\ffmpeg.exe'
$MediaMtx = Join-Path $Root 'third_party\mediamtx\mediamtx.exe'
$FilterDir = Join-Path $Root 'scripts\filters'

# 카메라마다 다른 테스트 패턴. 화면 왼쪽 위에 카메라 이름과 현재 시각을 찍어 실시간임을 보여 준다.
$Sources = @{
    1 = 'testsrc2=size=640x360:rate=25'
    2 = 'smptehdbars=size=640x360:rate=25'
    3 = 'mandelbrot=size=640x360:rate=25'
    4 = 'life=size=640x360:rate=25:mold=10:ratio=0.1:death_color=#202060:life_color=#40c0ff'
}

function Start-Camera([int]$n) {
    New-Item -ItemType Directory -Force $FilterDir | Out-Null
    $filterFile = Join-Path $FilterDir "cam$n.txt"
    # 필터 문자열은 파일로 넘겨 PowerShell 따옴표 처리를 피한다 (ffmpeg -/vf 파일 문법)
    $filter = "drawtext=fontfile='C\:/Windows/Fonts/consola.ttf':text='CAM$n  %{localtime}':x=16:y=14:fontsize=26:fontcolor=white:box=1:boxcolor=black@0.6:boxborderw=6"
    [IO.File]::WriteAllText($filterFile, $filter)
    $args = @('-hide_banner', '-loglevel', 'error', '-re', '-f', 'lavfi', '-i', $Sources[$n],
              '-/vf', $filterFile, '-c:v', 'libopenh264', '-b:v', '1M', '-g', '25',
              '-f', 'rtsp', '-rtsp_transport', 'tcp', "rtsp://127.0.0.1:8554/cam$n")
    Start-Process -FilePath $FFmpeg -ArgumentList $args -WindowStyle Hidden -PassThru
}

function Stop-Camera([int]$n) {
    Get-CimInstance Win32_Process -Filter "Name='ffmpeg.exe'" |
        Where-Object { $_.CommandLine -like "*8554/cam$n*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
}
