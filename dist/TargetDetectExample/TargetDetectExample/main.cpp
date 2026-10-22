
//  编译/运行环境：
//      Visual Studio 2017 工具集 v141，x64
//      Qt 5.14.2 MSVC2017（DLL 只用到 Qt5Core）
//      OpenCV 4.3.0（opencv_world430）
//
//  ⚠ 三条硬性约束：
//     1. Debug/Release 必须与 schneider0807.dll 一致，不能混用；
//     2. 必须包含随附的 TargetDetector.h，不要自己重定义里面的结构体；
//     3. 识别是同步阻塞的，正式项目请放到工作线程；同一个实例不要并发调用。
// =============================================================================

#include "wyf.h"

namespace {

// 开放可调的 10 个参数。默认值就是当前现场调优值。
// 每个字段的含义、范围和调整方向见 TargetDetector.h 的 DetectOptions 注释。
DetectOptions makeOptions()
{
    DetectOptions o;
    o.erodeKernel       = 3;      // 腐蚀核边长(奇数 1..21)；1 = 不腐蚀
    o.grayscale         = true;   // 预处理阶段是否先转灰度
    o.boundaryThreshold = 60;     // 灰度等值线边界阈值；0 = 局部大津法自动求
    o.maxAspect         = 1.9;    // 长短轴比上限
    o.minDiscFill       = 0.7;    // 最小外接圆内亮像素占比下限

    o.enhanceEnable     = true;   // 图像增强(黑白场拉伸)
    o.enhanceBlack      = 180;    // 黑场：< 它的像素压到 0
    o.enhanceWhite      = 210;    // 白场：> 它的像素拉到 255

    o.minResultArea     = 20.0f;  // 结果过滤：最小面积(px^2)，0 = 不限
    o.maxResultArea     = 800.0f; // 结果过滤：最大面积(px^2)，0 = 不限
    return o;
}

// 只打印数量和前 5 个结果，证明数据已经拿到；完整数据在返回的 vector / map 里。
void printMarkers(const std::vector<cv::Point2f>& markers)
{
    std::cout << "  markers: " << markers.size() << "\n";
    const size_t show = std::min<size_t>(markers.size(), 5);
    for (size_t i = 0; i < show; ++i) {
        std::cout << "    M" << (i + 1)
                  << "  x=" << std::fixed << std::setprecision(3) << markers[i].x
                  << "  y=" << markers[i].y << "\n";
    }
    if (markers.size() > show)
        std::cout << "    ... (" << (markers.size() - show) << " more)\n";
}

void printCodedTargets(const std::map<int, cv::Point2f>& coded)
{
    std::cout << "  codedTargets: " << coded.size() << "\n";
    size_t shown = 0;
    for (const auto& kv : coded) {
        if (shown++ >= 5) {
            std::cout << "    ... (" << (coded.size() - 5) << " more)\n";
            break;
        }
        std::cout << "    ID=" << std::setw(4) << kv.first
                  << "  x=" << std::fixed << std::setprecision(3) << kv.second.x
                  << "  y=" << kv.second.y << "\n";
    }
}

} // namespace

// 用法:
//   直接运行即可。示例固定读取下面这个路径；
//   集成到客户项目时，把 imagePath 改成客户自己的图片路径。
int wmain()
{
    // 示例调用 sample 图像；客户集成时改成自己的路径即可。
    const std::wstring imagePath =
        L"D:\\1yjy\\software\\c++sourse\\0807testDetect\\TargetDetectExample\\images\\sample.png";

    std::cout << "============================================================\n"
              << " schneider0807.dll  target detection example\n"
              << "============================================================\n";

    // ---- 1) 读图：支持中文路径 ----
    std::cout << "[1] readImage ...\n";
    cv::Mat image = TargetDetector::readImage(imagePath);
    if (image.empty()) {
        std::cerr << "ERROR: cannot read image. Check imagePath in main.cpp.\n";
        return 2;
    }
    std::cout << "    size = " << image.cols << " x " << image.rows
              << ", channels = " << image.channels() << " (8-bit BGR)\n";

    // ---- 2) 创建识别器，使用 DLL 内置相机参数 ----
    std::cout << "[2] TargetDetector + setDefaultCameraParameters ...\n";
    TargetDetector det(nullptr);
    det.setDefaultCameraParameters();

    // ---- 3) 设置客户可调的 10 个参数 ----
    std::cout << "[3] setOptions ...\n";
    det.setOptions(makeOptions());

    // setOptions 会把越界值夹回合法范围；读回真正生效的值。
    const DetectOptions opts = det.options();
    std::cout << "    erodeKernel=" << opts.erodeKernel
              << "  grayscale=" << (opts.grayscale ? 1 : 0)
              << "  boundaryThreshold=" << opts.boundaryThreshold
              << "  maxAspect=" << opts.maxAspect
              << "  minDiscFill=" << opts.minDiscFill << "\n";
    std::cout << "    enhance=" << (opts.enhanceEnable ? 1 : 0)
              << " (" << opts.enhanceBlack << "," << opts.enhanceWhite << ")"
              << "  area=[" << opts.minResultArea << ", " << opts.maxResultArea << "]\n";

    try {
        // ---- 4) 函数 1：只要标记点 xy ----
        std::cout << "[4] detectMarkerPoints ...\n";
        const std::vector<cv::Point2f> markers = det.detectMarkerPoints(image);
        printMarkers(markers);

        // ---- 5) 函数 2：只要编码片 id -> xy ----
        std::cout << "[5] detectCodedTargetPoints ...\n";
        const std::map<int, cv::Point2f> coded = det.detectCodedTargetPoints(image);
        printCodedTargets(coded);

        // ---- 6) 函数 3：标记点 + 编码片一次返回 ----
        std::cout << "[6] detectAllPoints ...\n";
        const DetectResult all = det.detectAllPoints(image);
        printMarkers(all.markers);
        printCodedTargets(all.codedTargets);

        // ---- 7) 质量分级（按钮触发）----
        // 对“最近一次识别”的结果做质量分级，剔除低质量点。
        // 上位机里对应【删除较差的点】按钮。
        std::cout << "[7] filterLowQuality (button) ...\n";
        const DetectResult good = det.filterLowQuality();
        printMarkers(good.markers);
        printCodedTargets(good.codedTargets);
    }
    catch (const cv::Exception& e) {
        std::cerr << "OpenCV exception: " << e.what() << "\n";
        return 3;
    }
    catch (const std::exception& e) {
        std::cerr << "exception: " << e.what() << "\n";
        return 4;
    }

    std::cout << "done.\n";
    return 0;
}
