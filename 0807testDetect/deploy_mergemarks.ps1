# 把 MergeMarksLib.dll 拷到 0807testDetect.exe 旁边。
#
# 只拷**我们自己那一个 dll**(约 14MB)。它依赖的 PCL/VTK 运行时(35 个, 约 254MB)**不拷**——
# 运行时靠 testDetect.cpp 里的 kMergeMarksRuntimeDirs 指向本机已装好的 PCL/VTK 目录。
# 所以本机必须装有 PCL 1.9.1 / VTK 8.1.0, 路径与那里写的一致; 换机器改那一处即可。
#
# 可重复执行。

$ErrorActionPreference = 'Stop'

$srcDll = 'D:\1yjy\software\c++sourse\MergeMarks1215\x64\Debug\MergeMarksLib.dll'
$dstDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\x64\Debug'))

if (-not (Test-Path $srcDll)) {
    Write-Host "找不到 $srcDll" -ForegroundColor Red
    Write-Host "请先用 VS 编译 MergeMarks1215.sln 里的 MergeMarksLib 工程。" -ForegroundColor Yellow
    exit 1
}

New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
Copy-Item $srcDll (Join-Path $dstDir 'MergeMarksLib.dll') -Force

$mb = [math]::Round((Get-Item $srcDll).Length / 1MB, 1)
Write-Host "MergeMarksLib.dll ($mb MB) -> $(Join-Path $dstDir 'MergeMarksLib.dll')"
Write-Host "PCL/VTK 运行时未拷贝, 运行时按 kMergeMarksRuntimeDirs 指向本机已装的目录。"
