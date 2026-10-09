# 뷰어 창을 PNG로 저장한다 (README 스크린샷용).
param([string]$Out)
$Out = [IO.Path]::GetFullPath((Join-Path (Get-Location) $Out))  # GDI+는 PowerShell 현재 위치를 모르므로 절대 경로로
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Win {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
'@
Add-Type -AssemblyName System.Drawing
[Win]::SetProcessDPIAware() | Out-Null
$h = (Get-Process multiview -ErrorAction SilentlyContinue | Select-Object -First 1).MainWindowHandle
if (-not $h -or $h -eq [IntPtr]::Zero) { throw 'viewer window not found' }
$r = New-Object Win+RECT
[Win]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
$bmp = New-Object Drawing.Bitmap $w, $ht
$g = [Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[Win]::PrintWindow($h, $hdc, 2) | Out-Null   # PW_RENDERFULLCONTENT: 가려져 있어도 창 내용을 그대로 캡처
$g.ReleaseHdc($hdc)
$bmp.Save($Out, [Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "saved $Out"
