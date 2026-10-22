#pragma once

// =============================================================================
// 公共头（预编译头）：示例只依赖 OpenCV + 靶标识别接口。
// 文件名按项目所有者要求使用 wyf.h / wyf.cpp。
// =============================================================================

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

// 唯一的接口头文件。⚠ 不要自己再声明里面的类/结构体。
#include "TargetDetector.h"
