#pragma once

// 与 MergeMarksLib.dll 的 C 接口约定。
// 纯声明: 不含 PCL/VTK 头, 也不链接 .lib; 宿主按需 LoadLibrary + GetProcAddress,
// 不切到「点云拼接」页就不会加载相关内容。
// 签名必须与 MergeMarksLib/MergeMarksLib.h 保持一致。

// ⚠ LoadLibrary 之前必须把 PCL/VTK 运行时目录加入进程 PATH:
//   依赖在加载时即解析, 而 DLL 自身尚未加载, 无法代劳。见 ensureMergeMarksLoaded()。

// ⚠ 不要用 FreeLibrary 卸载: 会连锁卸载 PCL/VTK, 而 VTK 不支持进程内卸载
//   (vtkObjectFactory 注册表残留悬空指针, 可能在 vtkCommonCore 内崩溃)。
//   进程退出交给系统回收即可。详见 testDetect.cpp 析构函数。

// 创建面板; 返回可直接 addWidget 的 QWidget*, 失败返回 nullptr。
typedef void* (*FnMergeMarksCreatePanel)(void* parentWidget);

// 销毁 CreatePanel 返回的面板。
typedef void  (*FnMergeMarksDestroyPanel)(void* panel);
