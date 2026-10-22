#pragma once

// =============================================================================
//  TargetDetector.h —— schneider0807.dll 客户接口
//


#include <QObject>
#include <QtGlobal>
#include <opencv2/opencv.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace std;
using namespace cv;
#ifndef BUILD_STATIC
# if defined(SCHNEIDER0807_LIB)
#  define SCHNEIDER0807_EXPORT Q_DECL_EXPORT
# else
#  define SCHNEIDER0807_EXPORT Q_DECL_IMPORT
# endif
#else
# define SCHNEIDER0807_EXPORT
#endif

#ifdef TD_OWNER

# include "TargetDetectorParams.h"
#else
struct DetectorParams;
#endif


// =============================================================================
//  客户可调的 10 个参数（DetectOptions）
//  ---------------------------------------------------------------------------
//  * 默认值 = 当前现场调优值，客户按自己的工位修改即可；
//  * setOptions() 会把越界值夹回合法范围（不抛异常），应用后可用 options() 读回；
//  * 其余 29 项识别参数固化在 DLL 内（TargetDetectorParams.h，不随客户发布）。
// =============================================================================
struct DetectOptions {
	// ---- 预处理 ----
	// 腐蚀核边长，单位像素；必须是奇数，DLL 会夹到 1..21。
	// 作用：去掉亮斑边缘的毛刺/粘连，让近圆轮廓更干净。
	// 1 = 不腐蚀（小点保边）；3 适合 5~15px 的小标记点；5 抗噪更强。
	int    erodeKernel       = 3;

	// 是否在预处理阶段先把 BGR 转成灰度。
	// DLL 内部本来就会按 BGR2GRAY 转灰度，所以 true/false 的识别结果一致；
	// 保留这个开关是为了和上位机的参数口径一致。
	bool   grayscale         = true;

	// ---- 阈值 / 形状 ----
	// 亚像素边界使用的灰度等值线阈值，0..255。
	//   0  = 用局部大津法自动求阈值；
	//   >0 = 固定阈值：在候选周围取未腐蚀灰度图的白斑边界，沿径向找 gray==T 的
	//        亚像素交点，再拟合椭圆。适合光照稳定、希望边界固定在某个灰度上的场景。
	// ⚠ 它只影响“亚像素边界”，不改变预处理的二值化（后者是 DLL 内部固定参数）。
	int    boundaryThreshold = 60;

	// 候选轮廓的长短轴比上限（长/短），1.0..10.0。
	// 越小越严格：明显椭圆的轮廓会被丢掉；圆度差的工件可适当放宽到 2.2~2.5。
	double maxAspect         = 1.9;

	// 最小外接圆内“亮像素”占比下限，0.0..1.0。
	// 用来排除空心环、月牙、噪点等；越大越严格。
	double minDiscFill       = 0.7;

	// ---- 图像增强（黑白场拉伸）----
	// 开关；关闭时不做 LUT，直接把原图送进识别。
	bool   enhanceEnable     = true;
	// 黑场 / 白场，0..255。
	// out = (in - black) * 255 / (white - black)，逐像素、夹到 0..255。
	// 当前 180/210 适合偏亮的反光件；原图对比度已经很好时建议关闭增强，
	// 或把 black 调低、white 调高。white <= black 时增强自动失效。
	int    enhanceBlack      = 180;
	int    enhanceWhite      = 210;

	// ---- 结果过滤（面积，平方像素）----
	// 面积按“灰度等值线面积 > 拟合椭圆面积 > 圆面积”的顺序取。
	// 0 表示该方向不限制：min 去掉小噪声，max 去掉大反光/粘连块。
	// 不同拍摄距离下靶标面积差别很大，换镜头/换工位要重新标定。
	float  minResultArea     = 20.0f;
	float  maxResultArea     = 800.0f;
};

// =============================================================================
//  第三个识别函数的返回类型
// =============================================================================
struct DetectResult {
	// 普通标记点中心。顺序沿用 DLL 的“从上到下、从左到右”。
	// 标记点没有 ID；M1..Mn 只是按这个 vector 的下标生成的显示序号。
	std::vector<cv::Point2f> markers;

	// 编码片：ID -> 中心。std::map 的 key 是 ID，天然按 ID 升序、ID 唯一。
	std::map<int, cv::Point2f> codedTargets;
};

// =============================================================================
//  内部结果类型
//  ---------------------------------------------------------------------------
//  客户的 3 个识别函数只返回 cv::Point2f 的 vector / map，通常不需要
//  直接用下面的结构体。它们保留给上位机做椭圆绘制、质量显示和参数调试。
// =============================================================================

// 编码片的 ID + 中心。detectTargets（裸指针接口，上位机内部用）用它。
struct TargetInfo {
	int id;     // 靶标 ID；未识别返回 -1
	float x;    // 识别中心 x 坐标（去畸变图像坐标，像素）
	float y;    // 识别中心 y 坐标（去畸变图像坐标，像素）
};

// 单个识别结果的完整信息（中心、椭圆、亚像素轮廓、质量）。
struct DetectionResult {
	int id = -1;                // 编码片 ID；普通标记点恒为 -1
	cv::Point2f center;         // 亚像素中心（去畸变图像坐标）

	// ---- 轮廓几何 ----
	// 倾斜时圆在像面上成椭圆，所以优先用 ellipse；退化时用 center + radius 按圆处理。
	bool hasEllipse = false;    // true -> 使用 ellipse；false -> 使用 center + radius
	cv::RotatedRect ellipse;    // 亚像素椭圆，整幅图坐标
	float radius = 0.f;         // 亚像素半径（兜底，像素）

	// 亚像素边界折线（整幅图坐标，首尾不重复的闭合环）。
	// 非空表示结果走的是“灰度等值线”路径：每个点由灰度在阈值 T 处的线性插值
	// 细化到亚像素；UI 可以直接把这圈点连成平滑曲线。
	// 空表示用 ellipse/radius 画（默认 Zernike 路径）。
	std::vector<cv::Point2f> contourPoints;

	// ---- 亚像素质量 ----
	// 精化失败时中心会退回整数轮廓拟合值（精度约 0.3px），只看 center 分辨不出来，
	// 必须靠这三个字段区分。
	bool  subpixel = false;     // true = 亚像素点最小二乘拟合；false = 退回整数轮廓
	// 拟合残差 RMS（像素）。注意它不是标定精度，只反映参与拟合的点离拟合椭圆的散布。
	// 选点带内均匀分布时天然有 band/√3 ≈ 0.7px 的地板值，所以偏大不等于精度差。
	// 适合做相对比较；绝对精度需要固定靶标连拍的重复性测试。
	float rms = 0.f;
	int   nPoints = 0;          // 参与拟合的亚像素点数
};

// 亚像素精化的质量信息，与 DLL 内部 refineCenters 的输出一一对应。
struct RefineQuality {
	bool  subpixel = false;
	float rms = 0.f;
	int   nPoints = 0;
};

class SCHNEIDER0807_EXPORT TargetDetector : public QObject
{
	Q_OBJECT

public:
	// parent 可为 nullptr。实例内部缓存去畸变映射表，建议复用一个实例。
	TargetDetector(QObject *parent);
	~TargetDetector();

	// =========================================================================
	//  一、相机参数
	// =========================================================================
	// 设置相机内参 / 畸变：
	//   cameraMatrix：3x3 CV_64F；
	//   distCoeffs  ：1x5 或 5x1 CV_64F，顺序 k1,k2,p1,p2,k3。
	// * 相机与本 DLL 不同时，必须调用；
	// * 输入图已经去畸变时，传单位阵 + 全 0；
	// * 修改后会清空内部去畸变映射表，下次识别重建。
	void setCameraParameters(const cv::Mat& cameraMatrix, const cv::Mat& distCoeffs);

	// 使用 DLL 内置的默认相机参数（当前 photoneo 彩色相机，
	// 与上位机 cameras.ini 当前生效档一致）。
	// 客户相机不同时请用上面的 setCameraParameters(K, D)。
	void setDefaultCameraParameters();

	// =========================================================================
	//  二、图像读取（支持中文 / Unicode 路径）
	// =========================================================================
	// 读取图像并解码为 8UC3 BGR；失败返回空 Mat。
	// cv::imread 只接受窄字符串，在中文 Windows 上直接传路径可能打不开；
	// 这个函数先按二进制读入内存再 imdecode，路径与系统代码页无关。
	static cv::Mat readImage(const std::wstring& path);

	// 返回路径所在目录，带末尾分隔符（'\' 或 '/'）；没有分隔符时返回空串。
	static std::wstring directoryOf(const std::wstring& path);

	// 由 exe 路径得到默认示例图片路径：<exe目录>\images\sample.png。
	// 示例程序不带参数时用它；普通客户程序可以直接 readImage(自己的路径)。
	static std::wstring defaultSampleImagePath(const std::wstring& exePath);

	// =========================================================================
	//  三、客户可调的参数
	// =========================================================================
	// setOptions 会把越界值夹回合法范围（不抛异常），应用后可用 options() 读回。
	// 参数是“快照”语义：每次识别开始时取一次，识别途中修改只影响下一次。
	void setOptions(const DetectOptions& options);
	DetectOptions options() const;

	// =========================================================================
	//  四、三个识别函数（输入一张图，输出去畸变坐标下的中心）
	// =========================================================================
	// 输入要求：8UC1 灰度 或 8UC3 BGR；内部自动做黑白场拉伸、灰度化、去畸变。
	// 输出上限：每个函数内部最多 100 个结果。
	//
	// ① 只返回普通标记点。
	//    会剔除落在编码片 4.2R 区域内的中心圆；为了判断区域，内部会先跑一次
	//    编码片识别，所以耗时与 detectAllPoints 接近。
	std::vector<cv::Point2f> detectMarkerPoints(cv::Mat& image);

	// ② 只返回编码片：ID -> 中心。解不出 ID 的编码片不会出现。
	std::map<int, cv::Point2f> detectCodedTargetPoints(cv::Mat& image);

	// ③ 一次返回标记点 + 编码片（内部只跑一遍，推荐优先用这个）。
	DetectResult detectAllPoints(cv::Mat& image);

	// =========================================================================
	//  五、质量分级（按钮触发）
	// =========================================================================
	// 对“最近一次 detectMarkerPoints / detectCodedTargetPoints / detectAllPoints”
	// 的完整结果做质量分级，剔除低质量点后返回；不会重新识别。
	// 判差规则（固化在 DLL 内）：
	//   ① subpixel == false（亚像素精化失败）；
	//   ② 拟合椭圆短/长轴 < 0.70；
	//   ③ 残差超限：等值线路径 rms/R > 2%；Zernike 路径 rms > 1.5×band/√3。
	// 上位机【删除较差的点】按钮对应这个函数；还没识别过时返回空结果。
	DetectResult filterLowQuality();

#ifdef TD_OWNER
	// =========================================================================
	//  六、上位机 / 项目所有者接口（客户头文件里不声明）
	// =========================================================================
	// 这些接口保留给上位机 0807testDetect：它按值持有 TargetDetector，
	// 并通过参数栏调整完整 33 项 DetectorParams。客户不需要也不应该调用。
	// 客户头文件不定义 TD_OWNER，因此看不到下面这些方法。
	// =========================================================================

	// 裸指针 + 输出数组，最接近 C 的接口；只返回成功解码出 ID 的编码片。
	int detectTargets(const unsigned char* imgData, int width, int height, int channels,
		TargetInfo* targets, int maxTargets);

	// 完整编码片 / 标记点结果（含椭圆、亚像素轮廓、质量）。
	std::vector<DetectionResult> detectTarget(cv::Mat& image);
	std::vector<DetectionResult> detectMarkers(cv::Mat& image);

	// 中间过程可视化开关（OTSU/腐蚀/轮廓/各候选 debug 图）。默认 false。
	// 客户接口里不可见；只有上位机/项目所有者调试时使用。
	void setDebugView(bool on) { m_debugView = on; }

	// 返回与识别结果同一（去畸变后）坐标系的图像，供 UI 叠加显示。
	cv::Mat undistortImage(const cv::Mat& src);

	// 完整 33 项参数（上位机参数栏使用）。
	DetectorParams params() const;
	void setParams(const DetectorParams& p);
	static DetectorParams defaultParams() { return DetectorParams(); }
#endif

private:
	// -------------------------------------------------------------------------
	//  相机与去畸变
	// -------------------------------------------------------------------------
	cv::Mat K;
	cv::Mat distCoeffs;
	bool m_debugView = false;

	// 上位机完整参数。用 unique_ptr 是为了让客户头文件里 DetectorParams 可以只做
	// 前向声明；对象布局与 DLL 一致（指针大小不随结构体变化）。
	std::unique_ptr<DetectorParams> m_params;
	DetectOptions m_options;
	mutable std::mutex m_paramsMutex;

	// 最近一次“客户接口”识别的完整结果与质量标记，供 filterLowQuality() 使用。
	mutable std::mutex m_resultMutex;
	std::vector<DetectionResult> m_lastCoded;
	std::vector<DetectionResult> m_lastMarkers;
	std::vector<char> m_lastCodedLow;
	std::vector<char> m_lastMarkerLow;

	DetectorParams paramsSnapshot() const;
	DetectOptions optionsSnapshot() const;

	// 去畸变映射表缓存：cv::undistort 每次都会重算整张 remap 表（大图很贵），
	// 这里按图像尺寸缓存一次，之后只做 remap。setCameraParameters 会使其失效。
	cv::Mat m_undistMap1;
	cv::Mat m_undistMap2;
	cv::Size m_undistMapSize;
	std::mutex m_undistMutex;
	void ensureUndistortMap(cv::Size sz);
	void undistortCached(const cv::Mat& src, cv::Mat& dst);

	// -------------------------------------------------------------------------
	//  客户接口的内部实现
	// -------------------------------------------------------------------------
	DetectorParams paramsFromOptions(const DetectOptions& o) const;
	static cv::Mat preprocessInput(const cv::Mat& src, const DetectOptions& o);
	std::vector<DetectionResult> detectCodedImpl(const cv::Mat& undistorted, const DetectorParams& P);
	std::vector<DetectionResult> detectMarkerImpl(const cv::Mat& undistorted, const DetectorParams& P);
	DetectResult runOptionsPipeline(cv::Mat& image, bool needCoded, bool needMarkers);
	void cacheLastResults(const std::vector<DetectionResult>& coded,
		const std::vector<DetectionResult>& markers, const DetectorParams& P);

	// -------------------------------------------------------------------------
	//  识别流程内部实现（上位机/客户接口共用）
	// -------------------------------------------------------------------------
	// details 非空时，顺带输出每个靶标的亚像素轮廓（拟合椭圆/半径）。
	// P 由入口函数取一次快照后一路按值传下去，一次识别全程用同一套参数。
	int detectTargetsImpl(const cv::Mat& img, TargetInfo* targets, int maxTargets,
		std::vector<DetectionResult>* details, const DetectorParams& P);

	// 靶标 ID 和编码映射
	std::map<int, uint16_t> idToCode;

	// 辅助函数
	std::vector<uint16_t> adjustCode(uint16_t code);
	bool detectBoundary(const cv::Mat& t, const cv::Point2f& center, float a,
		int angle_start, int angle_end, int& boundary_angle, const DetectorParams& P);

	// gray_code = 保留编码环的二值图（含中心圆盘 + 跑道编码块），供 decodeSingleTarget 采样；
	// gray_clean = 预筛后只剩中心圆盘的二值图，供候选检测。
	void preprocessImage(const cv::Mat& img, cv::Mat& gray_clean, cv::Mat& gray_code,
		cv::Mat& gray_blur,
		std::vector<std::vector<cv::Point> >& contours, const DetectorParams& P);

	// ellipses：与 centers/radii 一一对应的粗椭圆（整数轮廓 fitEllipse）。
	void extractInitialCircles(const std::vector<std::vector<cv::Point> >& contours,
		const cv::Mat& gray_blur,
		std::vector<cv::Point2f>& centers,
		std::vector<float>& radii,
		std::vector<cv::RotatedRect>& ellipses, const DetectorParams& P,
		bool requireCodingRing = true);

	void validateCircles(const cv::Mat& gray_clean,
		const std::vector<cv::Point2f>& centers,
		const std::vector<float>& radii,
		const std::vector<cv::RotatedRect>& ellipses,
		std::vector<cv::Point2f>& validCenters,
		std::vector<float>& validRadii,
		std::vector<cv::RotatedRect>& validEllipses);

	// refinedContours：与其余输出一一对应的“阈值边界亚像素折线”；
	// 空 vector 表示该候选没有用等值线路径（默认 Zernike，按椭圆画）。
	void refineCenters(const cv::Mat& gray_blur,
		const std::vector<cv::Point2f>& centers,
		const std::vector<float>& radii,
		const std::vector<cv::RotatedRect>& ellipses,
		std::vector<cv::Point2f>& refinedCenters,
		std::vector<float>& refinedRadii,
		std::vector<cv::RotatedRect>& refinedEllipses,
		std::vector<RefineQuality>& quality,
		std::vector<std::vector<cv::Point2f> >& refinedContours, const DetectorParams& P);

	bool decodeSingleTarget(const cv::Mat& gray_clean,
		const cv::Point2f& center, float radius,
		const cv::RotatedRect& refEllipse,
		int& matchedID, std::string& matchedBits,
		cv::Mat& debugImage,
		cv::RotatedRect& outEllipse,
		bool& ellipseFound, const DetectorParams& P);

	// 检查圆环上白色像素比例是否大于阈值（用于预筛选）。
	bool checkRingWhiteRatio(const Mat& roi, const Point2f& centerInROI, float radius,
		float ratioThreshold = 1.0f / 3.0f);
};
