# ============================================================================
# 组装可分发的 Release 发布包。
#
# 产物目录结构(这就是"发出去的那一份"):
#   <OutDir>\
#     0807testDetect.exe          Release 编译的宿主
#     schneider0807.dll           Release 编译的靶标识别算法库
#     MergeMarksLib.dll           Release 编译的点云拼接面板
#     Qt5Core/Gui/Widgets.dll     Qt 运行库 + platforms\qwindows.dll + imageformats\
#     opencv_world430.dll         OpenCV 运行库
#     msvcp140.dll 等             VC++ 运行库(可再分发的那些, 不是 Debug 版)
#     runtime\                    PCL / VTK 的全部 release DLL(点云拼接页要的)
#     使用说明.txt
#
# 关键点(踩过的坑):
#   * **必须是 Release**: Debug 版依赖 MSVCP140D/VCRUNTIME140D/ucrtbased, 微软不允许再分发,
#     只在装了 VS 的机器上才有, 发出去必然跑不起来。而且 Debug 的 OpenCV/PCL 慢好几倍。
#   * Qt 的 platforms\qwindows.dll 不能漏 —— 少了它 Qt 起不来, 报
#     "This application failed to start because no Qt platform plugin could be initialized"。
#   * PCL/VTK 的 debug 版文件名带 _debug / -8.1_d, release 版是 _release / -8.1。
#     这里按后缀筛, 只拷 release 的; 混进 debug 版会连带把不可再分发的 debug CRT 拖出去。
#   * 运行时 DLL 统一放 runtime\ 子目录, 靠宿主 LoadLibrary 前把它们前插进 PATH
#     (见 testDetect.cpp 的 mergeMarksRuntimeSearchPath)。开发机上那几套绝对路径
#     会作为后备, 所以本脚本在开发机上也能照常跑。
#
# 可重复执行: 每次都先清空 <OutDir> 再重来, 不会残留上一次的文件。
# ============================================================================

[CmdletBinding()]
param(
    # 产物目录。空 = 用仓库外的 dist\0807testDetect(见下面)。
    # ⚠ 不能在 param 默认值里用 $PSScriptRoot: 参数默认值是在绑定阶段求值的, 那时它还是空的,
    #   会报 "无法绑定参数 Path, 因为该参数为空字符串"。只能在脚本体里算。
    [string]$OutDir = '',
    # 组装完顺带压成 zip(放 <OutDir>.zip), 便于发给别人。
    [switch]$Zip,
    # 打包用的 PCL/VTK 安装位置。换机器时改这里(或改 testDetect.cpp 里的后备路径)。
    [string]$PclDir = 'D:\1yjy\software\newcomputer\PCL 1.9.1',
    [string]$VtkDir = 'D:\1yjy\software\newcomputer\VTK\VTK8.1.0\install',
    [string]$QtDir  = 'D:\1yjy\software\qt\Qt5.14.2\5.14.2\msvc2017_64',
    [string]$OcvDir = 'D:\1yjy\software\opencv\build\x64\vc15\bin',
    [string]$RedistDir = 'D:\Software\VS2017\VC\Redist\MSVC\14.16.27012\x64'
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrEmpty($OutDir)) {
    $OutDir = Join-Path (Split-Path $PSScriptRoot -Parent) 'dist\0807testDetect'
}

# ---- 小工具 ---------------------------------------------------------------

$script:copied = @()   # 记录每个来源拷了多少个文件/多少字节, 末尾汇总

function Copy-OneFile([string]$src, [string]$dstDir) {
    if (-not (Test-Path -LiteralPath $src)) { throw "缺少文件: $src" }
    if (-not (Test-Path -LiteralPath $dstDir)) { New-Item -ItemType Directory -Force -Path $dstDir | Out-Null }
    Copy-Item -LiteralPath $src -Destination $dstDir -Force
    $script:copied += [pscustomobject]@{ Src = $src; Bytes = (Get-Item -LiteralPath $src).Length }
}

# 按通配符批量拷(用于成百个 PCL/VTK dll)。$exclude 用来剔掉 debug 版。
function Copy-Glob([string]$pattern, [string]$dstDir, [string[]]$exclude = @()) {
    $items = @(Get-ChildItem -Path $pattern -File -ErrorAction SilentlyContinue)
    if ($items.Count -eq 0) { throw "通配符没匹配到任何文件: $pattern" }
    foreach ($ex in $exclude) { $items = @($items | Where-Object { $_.Name -notlike $ex }) }
    if ($items.Count -eq 0) { throw "排除 debug 版之后一个都不剩: $pattern" }
    if (-not (Test-Path -LiteralPath $dstDir)) { New-Item -ItemType Directory -Force -Path $dstDir | Out-Null }
    foreach ($it in $items) {
        Copy-Item -LiteralPath $it.FullName -Destination $dstDir -Force
        $script:copied += [pscustomobject]@{ Src = $it.FullName; Bytes = $it.Length }
    }
}

# 递归拷贝一棵目录树(保留子目录结构), 用于 OpenNI2 那种带 Drivers\ 子目录的运行时。
function Copy-Tree([string]$srcDir, [string]$dstDir, [string[]]$exclude = @()) {
    if (-not (Test-Path -LiteralPath $srcDir)) { throw "缺少目录: $srcDir" }
    $files = @(Get-ChildItem -Path $srcDir -Recurse -File)
    foreach ($ex in $exclude) { $files = @($files | Where-Object { $_.Name -notlike $ex }) }
    if ($files.Count -eq 0) { throw "递归拷贝一个文件都没剩下: $srcDir" }
    foreach ($f in $files) {
        $rel = $f.FullName.Substring($srcDir.Length).TrimStart('\')
        $target = Join-Path $dstDir $rel
        $targetDir = Split-Path $target -Parent
        if (-not [string]::IsNullOrEmpty($targetDir) -and -not (Test-Path -LiteralPath $targetDir)) {
            New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
        }
        Copy-Item -LiteralPath $f.FullName -Destination $target -Force
        $script:copied += [pscustomobject]@{ Src = $f.FullName; Bytes = $f.Length }
    }
}

# ---- 0) 前置检查 ----------------------------------------------------------

$root = Split-Path $PSScriptRoot -Parent          # 0807testDetect\
$mmRoot = 'D:\1yjy\software\c++sourse\MergeMarks1215'   # 兄弟仓库(MergeMarksLib 来源)

$exeRel   = Join-Path $root 'x64\Release\0807testDetect.exe'
$schRel   = 'D:\1yjy\software\c++sourse\schneider0807\x64\Release\schneider0807.dll'
$mmlibRel = Join-Path $mmRoot 'x64\Release\MergeMarksLib.dll'

foreach ($f in @($exeRel, $schRel, $mmlibRel)) {
    if (-not (Test-Path -LiteralPath $f)) {
        throw "缺少 Release 产物: $f`n请先编译: 0807testDetect.sln 与 MergeMarks1215.sln 的 Release|x64(见 CLAUDE.md)"
    }
}

Write-Host "== 组装发布包 ==" -ForegroundColor Cyan
Write-Host "  产物目录: $OutDir"

if (Test-Path -LiteralPath $OutDir) { Remove-Item -LiteralPath $OutDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ---- 1) 我们自己的程序 ----------------------------------------------------

Copy-OneFile $exeRel   $OutDir
Copy-OneFile $schRel   $OutDir
Copy-OneFile $mmlibRel $OutDir

# ---- 2) Qt 运行库与插件 ---------------------------------------------------

foreach ($q in @('Qt5Core.dll', 'Qt5Gui.dll', 'Qt5Widgets.dll')) {
    Copy-OneFile (Join-Path $QtDir "bin\$q") $OutDir
}
# ⚠ qwindows.dll 必须有, 否则 Qt 起不来
Copy-OneFile (Join-Path $QtDir 'plugins\platforms\qwindows.dll') (Join-Path $OutDir 'platforms')
# 图片格式插件: 导入图片支持 png/bmp/jpg/tif, 其中 jpeg/tiff/ico/gif 是插件形式提供的
foreach ($p in @('qjpeg.dll', 'qtiff.dll', 'qico.dll', 'qgif.dll')) {
    $src = Join-Path $QtDir "plugins\imageformats\$p"
    if (Test-Path -LiteralPath $src) { Copy-OneFile $src (Join-Path $OutDir 'imageformats') }
}
Copy-OneFile (Join-Path $QtDir 'plugins\styles\qwindowsvistastyle.dll') (Join-Path $OutDir 'styles')

# ---- 3) OpenCV -----------------------------------------------------------

Copy-OneFile (Join-Path $OcvDir 'opencv_world430.dll') $OutDir

# ---- 4) VC++ 运行库(可再分发的那几个; 不是 Debug 版) ----------------------

foreach ($d in @('msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
                 'vcruntime140.dll', 'concrt140.dll', 'vccorlib140.dll')) {
    $src = Join-Path $RedistDir "Microsoft.VC141.CRT\$d"
    if (Test-Path -LiteralPath $src) { Copy-OneFile $src $OutDir }
}
Copy-OneFile (Join-Path $RedistDir 'Microsoft.VC141.OpenMP\vcomp140.dll') $OutDir

# ---- 5) PCL / VTK 的 release 运行时(点云拼接页) ---------------------------
# 统一放 runtime\ 子目录, 由宿主在 LoadLibrary 之前前插进 PATH。
# 只按后缀筛 release 版, 排除 pcl-1.9.1-pdb-* 这类目录和所有 _debug / _d 文件。

$rt = Join-Path $OutDir 'runtime'
# PCL: 运行时 dll 只有 *_release.dll 和 *_debug.dll 两种后缀(实测没有无后缀的), 按前者取即可。
Copy-Glob (Join-Path $PclDir 'bin\*_release.dll') $rt
# ⚠ 各家第三方库的 debug 后缀**不统一**, 这里都必须按实际命名排除, 否则会把 debug 版
#   (以及它连结的不可再分发 debug CRT) 一起发出去:
#     FLANN : release=flann.dll / flann_cpp.dll     debug=flann-gd.dll / flann_cpp-gd.dll
#     Qhull : release=qhull.dll / qhull_p / qhull_r  debug=qhull_d.dll / ...
#     VTK   : release=vtkXxx-8.1.dll                 debug=vtkXxx-8.1_d.dll
#     PCL   : release=pcl_xxx_release.dll            debug=pcl_xxx_debug.dll
Copy-Glob (Join-Path $PclDir '3rdParty\FLANN\bin\*.dll') $rt @('*-gd.dll', '*_d.dll', '*_debug.dll')
Copy-Glob (Join-Path $PclDir '3rdParty\Qhull\bin\*.dll') $rt @('*_d.dll', '*_debug.dll')
# OpenNI2 要连 OpenNI2\Drivers\ 一起带(驱动是运行时按需加载的), 只去掉 pdb / jar
Copy-Tree (Join-Path $PclDir '3rdParty\OpenNI2\Redist') $rt @('*.pdb', '*.jar')
# VTK: 只拷 release
Copy-Glob (Join-Path $VtkDir 'bin\*.dll') $rt @('*_d.dll')

# ⚠ 必须一起带上 MSVCR100.dll(VS2010 的 CRT)。
#   PCL 的 pcl_io_release.dll **硬引入** OpenNI2.dll, 而 OpenNI2.dll 是早年用 VS2010 编的,
#   只依赖 KERNEL32 + MSVCR100.dll。MSVCR100.dll 不是 Windows 自带(也不在我们上面拷的
#   VS2017 CRT 里), 干净机器上必然缺 —— 于是 OpenNI2.dll 加载失败、pcl_io_release.dll 跟着
#   失败, 宿主 LoadLibrary 报 Win32 错误 126(找不到模块), 排查起来非常绕。
#   MSVCR100.dll 属于 VS2010 可再发行运行库, 允许随程序一起分发。
#   来源只能是本机 System32(装过 VS2010 运行库才有), 拿不到就直接报错, 不要静默跳过。
$msvcr100 = Join-Path $env:SystemRoot 'System32\MSVCR100.dll'
if (-not (Test-Path -LiteralPath $msvcr100)) {
    throw "找不到 $msvcr100。点云拼接页需要它(OpenNI2.dll 依赖 VS2010 运行库)。`n请先装 Microsoft Visual C++ 2010 x64 可再发行组件包。"
}
Copy-OneFile $msvcr100 $rt

# ---- 6) 使用说明 ---------------------------------------------------------

$readme = @'
标记点工具 — 免安装使用说明
================================

运行
----
双击 0807testDetect.exe 即可。不需要安装任何东西, 整个文件夹拷到哪都能用。

  * 第一页「靶标识别」: 导入图片 → 开始识别 → 右侧看结果与亚像素参数。
    右侧下方的「参数设置」可以直接调识别算法的阈值, 改完点【应用参数】生效,
    这些参数会自动记在同目录的 settings.ini 里, 下次打开还是这套。
  * 第二页「点云拼接」: 首次切换过去要等几秒(要加载 PCL/VTK 运行时)。
    里面有「顺序导入并拼接」(选一个文件夹一次拼完) 和「单步拼接」(逐帧手动:
    ① 载入参考帧 → ② 载入下一帧 → ③ 对齐并拼入左侧 → 重复 ②③ → ④ 全局优化)。

注意
----
  * 本程序是 64 位 Windows 程序, 需要 64 位 Windows 10 或更高版本。
  * 别把 exe 单独拷出来 —— 它要和同目录的 dll 以及 runtime\ 文件夹在一起。
  * settings.ini 和导出的 ply/pcd/stl 都写在程序所在目录, 所以请把这个文件夹
    放在有写权限的位置(例如桌面或 D 盘), 不要放进 C:\Program Files。
  * 点云拼接的「输出RT」和合并导出的文件会写在当前工作目录(双击运行时就是本目录)。

目录说明
--------
  runtime\        PCL / VTK 运行库, 点云拼接页需要, 别删。
  platforms\      Qt 平台插件, 删了程序起不来。
  imageformats\   Qt 图片格式插件(jpg/tif/ico/gif), 删了对应格式的图导不进来。
'@
Set-Content -LiteralPath (Join-Path $OutDir '使用说明.txt') -Value $readme -Encoding UTF8

# ---- 7) 汇总与自检 -------------------------------------------------------

$totalBytes = ($script:copied | Measure-Object -Property Bytes -Sum).Sum
$totalFiles = $script:copied.Count
Write-Host ("  已复制 {0} 个文件, {1:N1} MB" -f $totalFiles, ($totalBytes / 1MB)) -ForegroundColor Green

# 自检: 发布包里不能出现任何 debug 版运行库/库文件(不可再分发, 且说明混进了 debug 依赖)
$bad = @(Get-ChildItem -LiteralPath $OutDir -Recurse -File | Where-Object {
    $_.Name -match '(?i)(_d|_debug)\.dll$' -or
    $_.Name -match '(?i)^(MSVCP140D|VCRUNTIME140D|ucrtbased|VCOMP140D)\.dll$'
})
if ($bad.Count -gt 0) {
    Write-Host "!! 发布包里混进了 debug 版文件, 这是不该发的:" -ForegroundColor Red
    $bad | ForEach-Object { Write-Host ("     " + $_.FullName) }
    exit 1
}
Write-Host "  自检通过: 没有任何 debug 版 dll" -ForegroundColor Green

if ($Zip) {
    $zipPath = "$OutDir.zip"
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    Write-Host "  正在压缩…"
    Compress-Archive -Path (Join-Path $OutDir '*') -DestinationPath $zipPath -CompressionLevel Optimal
    Write-Host ("  压缩包: {0} ({1:N1} MB)" -f $zipPath, ((Get-Item -LiteralPath $zipPath).Length / 1MB)) -ForegroundColor Green
}

Write-Host "完成。整个目录可直接拷给别人。" -ForegroundColor Cyan
