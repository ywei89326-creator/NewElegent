#pragma once

// 与 MergeMarksLib.dll 的接口约定。
//
// 这是一个**纯声明**头文件: 不含任何 PCL/VTK 头, 也不需要链接 MergeMarksLib.lib。
// 宿主用 LoadLibrary + GetProcAddress 按需加载, 所以本工程的编译/链接里
// 完全不出现 PCL/VTK 的包含路径 —— 不切到「点云拼接」页就一个字节都不加载。
//
// 与 MergeMarksLib/MergeMarksLib.h 里的签名必须保持一致。
//
// ⚠ 调用 LoadLibrary 之前, 宿主必须先把 PCL/VTK 的运行时目录加进进程 PATH ——
//   因为 MergeMarksLib.dll 依赖它们, 加载它的那一刻就要解析。这件事没法由那个 DLL
//   代劳(它那时候还没被加载起来), 所以放在宿主这边做, 见 ensureMergeMarksLoaded()。

// ⚠ 加载之后**不要**用 FreeLibrary 卸载本模块。
//   它依赖的是一整批 DLL(pcl_*_debug.dll / vtk*_d.dll / vtkGUISupportQt-8.1_d.dll ...),
//   FreeLibrary 会让引用计数归零从而连锁卸载它们, 而 VTK 不支持在进程内被卸载: 各 VTK DLL
//   里的 vtkObjectFactory 都注册在 vtkCommonCore 的全局表里, 卸载顺序一旦错开, 那张表里
//   就留下悬空指针, vtkCommonCore 自己卸载时访问它即 0xC0000005(异常报在 vtkCommonCore
//   里、行号却落在 FreeLibrary 那一句上, 极易误判)。进程退出时交给系统回收即可, 没有收益损失。
//   详见 testDetect.cpp 析构函数与 MergeMarksLib.cpp 里 MergeMarks_DestroyPanel 的说明。

// 创建 MergeMarks 面板。返回可直接 addWidget 进布局的 QWidget*; 失败返回 nullptr。
typedef void* (*FnMergeMarksCreatePanel)(void* parentWidget);

// 销毁 MergeMarks_CreatePanel 返回的面板。
typedef void  (*FnMergeMarksDestroyPanel)(void* panel);
