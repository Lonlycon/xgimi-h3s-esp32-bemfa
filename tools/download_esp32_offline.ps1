<#
  下载 ESP32 Arduino core 3.3.13 的 Windows x64 离线包
  ------------------------------------------------------------
  用法（在 tools 目录下）：
    powershell -ExecutionPolicy Bypass -File .\download_esp32_offline.ps1
    powershell -ExecutionPolicy Bypass -File .\download_esp32_offline.ps1 -ListOnly

  说明：
  - 共 18 个文件，约 1.2 GB，全部来自官方 GitHub release。
  - 下载到 .\esp32-offline\，带 SHA-256 校验。
  - 索引里没有 dfu-util（只影响 DFU 模式），脚本会跳过并提示。
  - 只下载，不安装；下完告诉 DSH，由它离线安装。
#>

param(
  [switch]$ListOnly,
  [string]$Version = "3.3.13",
  [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
$indexUrl = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"

Write-Host "读取索引: $indexUrl"
$idx = Invoke-RestMethod -Uri $indexUrl -TimeoutSec 60
$pkg = $idx.packages | Where-Object { $_.name -eq "esp32" }
$plat = $pkg.platforms | Where-Object { $_.version -eq $Version }
if (-not $plat) { throw "索引里找不到 esp32 平台版本 $Version" }

$files = New-Object System.Collections.Generic.List[object]
$files.Add([pscustomobject]@{
  Name = "platform"; File = $plat.archiveFileName;
  Url = $plat.url; Sha = $plat.checksum; Size = $plat.size
})

foreach ($dep in $plat.toolsDependencies) {
  $tool = $pkg.tools | Where-Object { $_.name -eq $dep.name -and $_.version -eq $dep.version }
  if (-not $tool) {
    Write-Warning "跳过 $($dep.name) $($dep.version)：索引里没有这个工具（通常不影响编译）"
    continue
  }

  # 优先 64 位 Windows，其次 32 位
  $sys = $tool.systems | Where-Object { $_.host -eq "x86_64-mingw32" } | Select-Object -First 1
  if (-not $sys) {
    $sys = $tool.systems | Where-Object { $_.host -eq "i686-mingw32" } | Select-Object -First 1
  }
  if (-not $sys) {
    Write-Warning "跳过 $($dep.name)：没有 Windows 版本"
    continue
  }

  $files.Add([pscustomobject]@{
    Name = $dep.name; File = $sys.archiveFileName;
    Url = $sys.url; Sha = $sys.checksum; Size = $sys.size
  })
}

if ($ListOnly) {
  $files | ForEach-Object { "{0}`t{1}`t{2}" -f $_.Name, $_.File, $_.Url }
  return
}

if (-not $OutDir) { $OutDir = Join-Path $PSScriptRoot "esp32-offline" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$totalMB = ($files | Measure-Object Size -Sum).Sum / 1MB
Write-Host ("输出目录: {0}" -f $OutDir)
Write-Host ("共 {0} 个文件，约 {1:N0} MB" -f $files.Count, $totalMB)

$i = 0
foreach ($f in $files) {
  $i++
  $dest = Join-Path $OutDir $f.File

  if ((Test-Path $dest) -and ((Get-Item $dest).Length -eq $f.Size)) {
    Write-Host ("[{0}/{1}] 已存在，跳过 {2}" -f $i, $files.Count, $f.File)
    continue
  }

  Write-Host ("[{0}/{1}] 下载 {2} ({3:N0} MB)" -f $i, $files.Count, $f.File, ($f.Size / 1MB))
  & curl.exe -L --fail --retry 5 --retry-delay 2 -C - -o $dest $f.Url
  if ($LASTEXITCODE -ne 0) { throw "下载失败: $($f.Url)" }
}

Write-Host "校验 SHA-256 ..."
$bad = 0
foreach ($f in $files) {
  $dest = Join-Path $OutDir $f.File
  if (-not (Test-Path $dest)) {
    Write-Warning "缺失: $($f.File)"
    $bad++
    continue
  }
  $want = ($f.Sha -replace '^SHA-256:', '').ToLower()
  $got = (Get-FileHash -Algorithm SHA256 $dest).Hash.ToLower()
  if ($want -and $got -ne $want) {
    Write-Warning "校验失败: $($f.File)"
    $bad++
  }
}

if ($bad -eq 0) {
  Write-Host ""
  Write-Host "全部完成，校验通过。"
  Write-Host ("路径: {0}" -f $OutDir)
} else {
  Write-Host ""
  Write-Host ("有 {0} 个文件有问题，重新跑一次脚本会断点续传。" -f $bad)
}
