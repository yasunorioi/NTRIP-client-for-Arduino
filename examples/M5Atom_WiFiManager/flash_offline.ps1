# flash_offline.ps1 -- WiFiManager 例をオフラインで書き込む (機種非依存)
# ビルド済み firmware.factory.bin を 0x0 に一発で焼く。ネット不要。
# .pio/build 下のビルドフォルダを自動検出、chip は esptool 自動判定。
# 使い方 (このスクリプトがある example ディレクトリで実行):
#   .\flash_offline.ps1                 # COM 自動検出して書き込み
#   .\flash_offline.ps1 -Port COM5      # ポート指定
#   .\flash_offline.ps1 -Baud 460800    # 速度up(Atomは既定115200が無難)
#   .\flash_offline.ps1 -Port COM5 -NoReset  # G0手動DLモード時(自動リセット無効)

param(
    [string]$Port = "",
    [int]$Baud = 115200,  # M5Atom は 115200 が鉄板。S3/Stackは460800等も可
    [switch]$NoReset      # GPIO0 を手で GND に落として自分でDLモードに入れた時に使う
)

$ErrorActionPreference = "Stop"
# esptool の進捗バー(░ 等の罫線文字)を旧コンソール(CP932)に出す際の
# UnicodeEncodeError を防ぐため、子プロセスの stdout/stderr を UTF-8 に固定する。
$env:PYTHONIOENCODING = "utf-8"
$py = "C:\Users\kita_\.platformio\penv\Scripts\python.exe"

# --- ビルドフォルダ自動検出 (.pio/build/<env>/firmware.factory.bin) ---
$buildRoot = Join-Path $PSScriptRoot ".pio\build"
if (-not (Test-Path $buildRoot)) { Write-Error "未ビルド: $buildRoot が無い。先に pio run して"; exit 1 }
$bin = Get-ChildItem $buildRoot -Recurse -Filter firmware.factory.bin | Select-Object -First 1
if (-not $bin) { Write-Error "firmware.factory.bin が見つからない。先に pio run して"; exit 1 }
Write-Host "対象: $($bin.FullName)" -ForegroundColor DarkGray

# --- COM 自動検出 (USBシリアル / S3ネイティブUSB(CDC) 両対応) ---
if (-not $Port) {
    $cands = Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.Name -match '\(COM(\d+)\)' -and
                       $_.Name -match 'CH9102|CH340|CP210|Silicon Labs|USB-SERIAL|USB Serial|UART|JTAG' }
    if ($cands.Count -eq 0) {
        Write-Host "USBシリアルが見つからない。全COMを表示するので -Port で指定して:" -ForegroundColor Yellow
        Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match '\(COM\d+\)' } | ForEach-Object { "  $($_.Name)" }
        exit 1
    }
    if ($cands.Count -gt 1) {
        Write-Host "USBシリアルが複数。-Port で指定して:" -ForegroundColor Yellow
        $cands | ForEach-Object { "  $($_.Name)" }
        exit 1
    }
    $null = $cands[0].Name -match '\((COM\d+)\)'
    $Port = $Matches[1]
    Write-Host "検出: $($cands[0].Name)" -ForegroundColor Green
}

$before = if ($NoReset) { "no-reset" } else { "default-reset" }
Write-Host "書き込み: $Port @ $Baud baud -> 0x0 (--before $before, chip=auto)" -ForegroundColor Cyan
& $py -m esptool --port $Port --baud $Baud --before $before --after hard-reset write-flash 0x0 $bin.FullName
if ($LASTEXITCODE -ne 0) {
    Write-Host "失敗。順に試す:" -ForegroundColor Yellow
    Write-Host "  1) 速度を落とす      : .\flash_offline.ps1 -Port $Port -Baud 115200"
    Write-Host "  2) GPIO0手動DLモード : GPIO0(G0)をGNDに接続 → 再給電(USB挿し直し) → 下記"
    Write-Host "                         .\flash_offline.ps1 -Port $Port -Baud 115200 -NoReset"
    Write-Host "                         (書き込み開始後はG0を離してOK)"
    exit $LASTEXITCODE
}
Write-Host "完了。シリアルモニタ: $py -m serial.tools.miniterm $Port 115200" -ForegroundColor Green
