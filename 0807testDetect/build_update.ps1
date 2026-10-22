# ============================================================================
# 打包"增量更新包": 只含 3 个会被你的源码改动影响的文件, 客户覆盖即可。
#
#   0807testDetect.exe    宿主(改界面/参数栏/主题 -> 重编它)
#   schneider0807.dll     靶标识别算法
#   MergeMarksLib.dll     点云拼接面板
#
# 其余 170 多个文件(Qt / OpenCV / PCL / VTK / VC++ 运行库 / Qt 插件)只有在**升级这些
# 第三方库**时才需要重新发 —— 那时用 build_release.ps1 出整包, 不是这个脚本。
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File 0807testDetect\build_update.ps1
#   (加 -Zip 顺带压成 <OutDir>\..\update_<日期时间>.zip)
#
# ⚠ 为什么这 3 个要一起发, 别图省事只发一个:
#
#   * schneider0807.dll 与 0807testDetect.exe 是通过 **对象布局** 耦合的 —— 宿主把
#     TargetDetector 当值成员持有(m_detector), 两边对 sizeof 必须一致。只要
#     include/TargetDetector.h 动过(例如 DetectorParams 又加了一个字段), 两者就必须
#     同批更新; 只换 dll 会让宿主按旧的偏移去读写对象内部, 是**写坏内存**, 不是报错。
#   * 同理 MergeMarksBridge.h 里的 C ABI 签名若改了, 宿主和 MergeMarksLib.dll 也要同批。
#   * 三个文件加起来才 2 MB 左右, 一起发没有成本, 却能免掉上面这类最难查的故障。
#
# 客户侧的替换方式: 先关掉程序, 把这 3 个文件覆盖到程序目录, 再打开。
# 同目录的 settings.ini(他调好的识别参数)不会被覆盖, 因为本脚本根本不碰它。
# ============================================================================

[CmdletBinding()]
param(
    # 输出目录。空 = <仓库>\dist\update
    [string]$OutDir = '',
    [switch]$Zip,
    # 跳过"源码比产物新"的防呆检查(不建议; 除非你确信产物就是最新的)
    [switch]$SkipStaleCheck
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrEmpty($OutDir)) {
    $OutDir = Join-Path (Split-Path $PSScriptRoot -Parent) 'dist\update'
}

$root   = Split-Path $PSScriptRoot -Parent                                   # 0807testDetect\
$mmRoot = 'D:\1yjy\software\c++sourse\MergeMarks1215'
$schRoot = 'D:\1yjy\software\c++sourse\schneider0807'

# 产物 / 它们各自的源码目录(用于"改了没重编"检查)
$targets = @(
    [pscustomobject]@{
        Name  = '0807testDetect.exe'
        Prod  = Join-Path $root 'x64\Release\0807testDetect.exe'
        Src   = @((Join-Path $root '0807testDetect\MergeMarksBridge.h'),
                  (Join-Path $root '0807testDetect\main.cpp'),
                  (Join-Path $root '0807testDetect\testDetect.cpp'),
                  (Join-Path $root '0807testDetect\testDetect.h'),
                  (Join-Path $root '0807testDetect\testDetect.ui'),
                  (Join-Path $root '0807testDetect\testDetect.qrc'),
                  (Join-Path $root '0807testDetect\theme.qss'),
                  (Join-Path $root '0807testDetect\include\TargetDetector.h'),
                  (Join-Path $root '0807testDetect\include\TargetDetectorParams.h'),
                  # 这三份 .vcxproj / .props 改了也属于"宿主需要重编"
                  (Join-Path $root '0807testDetect\0807testDetect.vcxproj'))
        Note  = '宿主界面/参数栏'
    }
    [pscustomobject]@{
        Name  = 'schneider0807.dll'
        Prod  = Join-Path $schRoot 'x64\Release\schneider0807.dll'
        Src   = @((Join-Path $schRoot 'schneider0807\TargetDetector.cpp'),
                  (Join-Path $schRoot 'schneider0807\TargetDetector.h'),
                  (Join-Path $schRoot 'schneider0807\TargetDetectorParams.h'))
        Note  = '靶标识别算法'
    }
    [pscustomobject]@{
        Name  = 'MergeMarksLib.dll'
        Prod  = Join-Path $mmRoot 'x64\Release\MergeMarksLib.dll'
        Src   = @((Join-Path $mmRoot 'MergeMarks1215\MergeMarks1215.cpp'),
                  (Join-Path $mmRoot 'MergeMarks1215\MergeMarks1215.h'),
                  (Join-Path $mmRoot 'MergeMarks1215\MergeMarks1215.ui'),
                  (Join-Path $mmRoot 'MergeMarks1215\MergeMarks1215.qrc'),
                  (Join-Path $mmRoot 'MergeMarksLib\MergeMarksLib.cpp'),
                  (Join-Path $mmRoot 'MergeMarksLib\MergeMarksLib.h'))
        Note  = '点云拼接面板'
    }
)

Write-Host "== 打包增量更新 ==" -ForegroundColor Cyan

# ---- 0) 两份 TargetDetector.h 必须完全一致 --------------------------------
# 宿主按值持有 TargetDetector(m_detector), 它的成员布局必须和 DLL 里的一致。改了一份没同步
# 另一份, 两边就会按不同的偏移读写同一个对象 —— 那是**写坏内存**, 不报错、只是偶尔崩或算出怪结果。
# 这是排查代价最高的一类故障, 所以打包前直接比对文件内容拦下来。
$headerPairs = @(
    [pscustomobject]@{
        Name = 'TargetDetector.h'
        Host = Join-Path $root '0807testDetect\include\TargetDetector.h'
        Dll  = Join-Path $schRoot 'schneider0807\TargetDetector.h'
    }
    [pscustomobject]@{
        # 上位机定义了 TD_OWNER，会编译进完整的 DetectorParams；两边布局也必须一致
        Name = 'TargetDetectorParams.h'
        Host = Join-Path $root '0807testDetect\include\TargetDetectorParams.h'
        Dll  = Join-Path $schRoot 'schneider0807\TargetDetectorParams.h'
    }
)
foreach ($h in $headerPairs) {
    if (-not (Test-Path -LiteralPath $h.Host) -or -not (Test-Path -LiteralPath $h.Dll)) {
        throw "找不到 $($h.Name) 的两份副本:`n  $($h.Host)`n  $($h.Dll)"
    }
    if ((Get-FileHash -LiteralPath $h.Host).Hash -ne (Get-FileHash -LiteralPath $h.Dll).Hash) {
        Write-Host "!! 两份 $($h.Name) 内容不一致, 绝不要这样发出去:" -ForegroundColor Red
        Write-Host "     宿主: $($h.Host)" -ForegroundColor Red
        Write-Host "     DLL : $($h.Dll)" -ForegroundColor Red
        Write-Host "   宿主按值持有 TargetDetector, 两边布局必须一致; 请把改动同步到另一份并重编两者。" -ForegroundColor Yellow
        exit 1
    }
    Write-Host "  两份 $($h.Name) 一致" -ForegroundColor Green
}

# ---- 1) 产物是否齐全 ------------------------------------------------------
foreach ($t in $targets) {
    if (-not (Test-Path -LiteralPath $t.Prod)) {
        throw "缺少 Release 产物: $($t.Prod)`n请先编译 0807testDetect.sln / MergeMarks1215.sln / schneider0807.sln 的 Release|x64(见 CLAUDE.md 的 Release package 一节)"
    }
}

# ---- 2) 防呆: 源码比产物新 = 你改了但没重编, 发出去的是旧版 -----------------
# 这是这套流程最容易犯、又最难发现的错(客户那边表现成"改了没效果")。
if (-not $SkipStaleCheck) {
    $stale = @()
    foreach ($t in $targets) {
        $prodTime = (Get-Item -LiteralPath $t.Prod).LastWriteTimeUtc
        $newest = Get-ChildItem -LiteralPath $t.Src -File -ErrorAction SilentlyContinue |
                  Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
        if ($newest -and $newest.LastWriteTimeUtc -gt $prodTime) {
            $stale += "  $($t.Name) [$($t.Note)] 比源码 '$($newest.Name)' 旧"
        }
    }
    if ($stale.Count -gt 0) {
        Write-Host "!! 有产物比源码旧, 说明改动还没编进 Release(或编的是 Debug):" -ForegroundColor Red
        $stale | ForEach-Object { Write-Host $_ -ForegroundColor Red }
        Write-Host "   请先重编对应工程的 Release|x64 再打包; 确认无误可加 -SkipStaleCheck 强行继续。" -ForegroundColor Yellow
        exit 1
    }
    Write-Host "  防呆检查通过: 三个产物都不旧于各自源码" -ForegroundColor Green
}

# ---- 3) 拷贝 --------------------------------------------------------------
if (Test-Path -LiteralPath $OutDir) { Remove-Item -LiteralPath $OutDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

foreach ($t in $targets) {
    Copy-Item -LiteralPath $t.Prod -Destination $OutDir -Force
    $kb = [math]::Round((Get-Item -LiteralPath $t.Prod).Length / 1KB, 0)
    Write-Host ("  {0,-22} {1,8} KB   {2}" -f $t.Name, $kb, $t.Note)
}

$stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
$readme = @"
标记点工具 — 更新包 ($stamp)
========================================

里面有 3 个文件:
  0807testDetect.exe     主程序
  schneider0807.dll      靶标识别算法
  MergeMarksLib.dll      点云拼接面板

怎么替换
--------
1. 先**完全关闭**主程序(窗口关掉; 如果任务栏上还有, 右键退出)。
2. 把这 3 个文件复制到程序所在目录(就是有 0807testDetect.exe 的那个文件夹),
   提示"是否替换"选**全部替换**。
3. 重新打开 0807testDetect.exe。

注意
----
* 这三个要**一起替换**, 不要只换其中一两个 —— 主程序和两个 dll 之间有严格的
  内部约定, 混用不同批次会导致程序崩溃或算出错误结果。
* 同目录的 settings.ini(你调好的识别参数)不会被这次替换动到。
* 不需要替换 runtime\、platforms\、imageformats\ 以及那些 Qt / OpenCV 的 dll,
  它们没有变化, 保持原样即可。
"@
Set-Content -LiteralPath (Join-Path $OutDir '更新说明.txt') -Value $readme -Encoding UTF8

$bytes = (Get-ChildItem -LiteralPath $OutDir -File | Measure-Object -Property Length -Sum).Sum
Write-Host ("  合计 {0:N2} MB" -f ($bytes / 1MB)) -ForegroundColor Green

if ($Zip) {
    $zipPath = Join-Path (Split-Path $OutDir -Parent) ('update_' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '.zip')
    Compress-Archive -Path (Join-Path $OutDir '*') -DestinationPath $zipPath -CompressionLevel Optimal
    Write-Host ("  压缩包: {0} ({1:N2} MB)" -f $zipPath, ((Get-Item -LiteralPath $zipPath).Length / 1MB)) -ForegroundColor Green
}

Write-Host "完成。把 $OutDir 里的文件(或压缩包)发给客户覆盖即可。" -ForegroundColor Cyan
