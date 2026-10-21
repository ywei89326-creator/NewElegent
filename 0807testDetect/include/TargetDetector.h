#pragma once

#include <QObject>
#include <QtGlobal>
#include <opencv2/opencv.hpp>
#include <mutex>
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

struct TargetInfo {
	int id;     // 靶标ID，未识别返回-1
	float x;    // 识别中心x坐标
	float y;    // 识别中心y坐标
};
struct DetectionResult {
	int id = -1;
	cv::Point2f center;         // 亚像素中心(去畸变后图像坐标)
	// 亚像素轮廓: 中心圆盘经 Zernike 亚像素边缘 + 最小二乘拟合的结果, 不是整数像素轮廓。
	// 倾斜时圆在像面上成椭圆, 所以优先用 ellipse; 退化时退回按正圆(radius)画。
	bool hasEllipse = false;    // true → 用 ellipse; false → 用 radius 按正圆画
	cv::RotatedRect ellipse;    // 亚像素椭圆, 整幅图坐标
	float radius = 0.f;         // 亚像素半径(兜底)
	// 亚像素边界折线(整幅图坐标, 首尾不重复的闭合环)。
	// 非空时表示该结果是按"灰度阈值边界"取的等值线: 每个点都由灰度在 T 处的
	// 线性插值细化到亚像素, UI 直接把这一圈连成平滑曲线(比椭圆更贴合实际边界)。
	// 空 = 用 ellipse/radius 画(默认 Zernike 路径)。
	std::vector<cv::Point2f> contourPoints;

	// ---- 质量信息: 判断这个中心到底是不是"真的亚像素" ----
	// 亚像素精化失败时中心会退回整数轮廓拟合值(精度掉到 ~0.3px 量级), 光看 center 分辨不出来,
	// 必须靠这三个字段区分。
	bool  subpixel = false;     // true = 中心来自亚像素点最小二乘拟合; false = 退回了整数轮廓
	// 拟合残差 RMS(像素)。**注意它不是标定过的精度**, 只反映"参与拟合的点离拟合椭圆的散布"。
	// 选点带宽是 ±band·R(实测约 ±1.2px), 点若在带内均匀分布, RMS 天然就是 band/√3 ≈ 0.7px
	// —— 实测值 0.69~0.72px 与之一致, 所以这个数偏大不等于精度差, 偏小也不等于精度好。
	// 它的用处是**相对比较**: 某次识别明显高于其它次, 说明那次选点混进了非边缘点。
	// 要确认绝对精度, 只能做重复性测试(固定靶标连拍 N 张, 看中心坐标的标准差)。
	float rms = 0.f;
	int   nPoints = 0;          // 参与拟合的亚像素点数
};

// 亚像素精化的质量信息, 与 refineCenters 的输出一一对应
struct RefineQuality {
	bool  subpixel = false;
	float rms = 0.f;
	int   nPoints = 0;
};

// ---------------------------------------------------------------------------
// 识别流程的可调参数。
//
// 每个字段的默认值都**逐个等于**参数化之前写死在 TargetDetector.cpp 里的那个常量,
// 所以不调用 setParams() 时行为与从前完全一致 —— 这是改这里的硬性约束。
// 每项字段的注释里都标注了它替换掉的那个常量名。
//
// ⚠ 注意有几组"数值相近但语义不同"的参数**不能互相合并**:
//   - maxRing(4.2, 轮廓筛选的编码带外沿) 与 codeRingOuter(4.1, 解码采样的编码带外沿)
//     值不同、用在两处, 合并会让默认结果变化。
//   - refineBandRatio(0.18, Zernike 选点带半宽) 与 codeBrightAtAngle 里的径向步长
//     系数 0.18 数值相同但语义无关, 后者没做成参数。
// 单位说明: "×R" 表示相对中心圆盘半径 R 的倍数(所以相机远近变化时自动等比例)。
// ---------------------------------------------------------------------------
struct DetectorParams {
	// ---- 预处理 / 二值化 (preprocessImage) ----
	int    erodeKernel      = 5;      // 腐蚀核边长(奇数), 原 Size(5,5)
	bool   useOtsu          = true;   // 二值化用大津法; false 时用 fixedThreshold
	int    fixedThreshold   = 128;    // 固定二值化阈值(仅 useOtsu=false 时生效)
	double bigBlobAreaRatio = 0.02;   // 剔除大面积白色连通域的面积占比阈值, 0=关闭(原 kBigBlobAreaRatio)

	// ---- 轮廓筛选 (extractInitialCircles) ----
	double maxAspect        = 1.9;    // 长短轴比上限(原 kMaxAspect)
	double minDiscFill      = 0.7;    // 最小外接圆内亮像素占比下限(原 kMinDiscFill)
	int    brightThr        = 120;    // "亮"像素灰度阈值(原 kBrightThr, 也被评分复用)
	double minRing          = 2.3;    // 编码带内沿 ×R(原 kMinRing)
	double maxRing          = 4.2;    // 编码带外沿 ×R(原 kMaxRing)
	int    minRingDirHits   = 4;      // 24 个方向里至少几个看到编码块(原 kMinRingDirHits)
	double approxEpsRatio   = 0.03;   // 多边形逼近精度 ×周长(原字面量 0.03)
	int    minApproxVertices = 5;     // 逼近后最少顶点数, 少了整个轮廓丢弃(原字面量 5)
	double minCandidateRadius = 1.0;  // 候选最小半径(px), 小于它的轮廓/外接圆直接丢(原字面量 1.0)

	// ---- 亚像素精化 (refineCenters) ----
	bool   subpixelEnable   = true;   // 关掉则不做 Zernike 精化, 中心退回整数轮廓(quality.subpixel=false)
	double refineBandRatio  = 0.18;   // 选点带半宽 ×R(原字面量 0.18)
	double refineBandMin    = 0.8;    // 选点带半宽下限(px)(原字面量 0.8)
	double roiScale         = 1.5;    // Zernike 扫描 ROI 半径 ×R(原字面量 1.5)
	double zernikeKMin      = 5.0;    // Zernike 阶跃强度初筛(原调用处传的 5.0)
	bool   useOtsuK         = true;   // 阶跃强度阈值用大津法; false 时用 fixedKt
	double fixedKt          = 30.0;   // 固定阶跃强度阈值(仅 useOtsuK=false 时生效)
	int    minFitPoints     = 6;      // 参与拟合的最少亚像素点数(原三处字面量 6)
	double ellipseBandMin   = 0.04;   // 椭圆归一化带宽下限(原 clamp 的 0.04)
	double ellipseBandMax   = 0.20;   // 椭圆归一化带宽上限(原 clamp 的 0.20)
	double ellipseBandShrink = 0.6;   // 二次重拟合时带宽收缩比(原两处字面量 0.6)
	double minAxisRatio     = 0.2;    // 拟合椭圆的短/长轴比下限(原字面量 0.2)

	// ---- 解码 / 去重 ----
	double codeRingInner    = 2.3;    // 解码采样编码带内沿 ×R(原 kCodeRingInner)
	double codeRingOuter    = 4.1;    // 解码采样编码带外沿 ×R(原 kCodeRingOuter)
	int    codeBrightThr    = 200;    // 编码块"亮"判定灰度阈值(原字面量 200)
	int    rectifyBinThr    = 127;    // 仿射拉正后重新二值化的阈值(原字面量 127)
	int    bitSubHitThr     = 4;      // 8 个采样角里亮点数须 > 该值才算命中子块(原字面量 4)
	int    bitBlockThr      = 2;      // 3 个子块里至少命中几个才算该位为 1(原字面量 2)
	double minGapRadii      = 10.0;   // 去重: 两中心距 < 该值×半径 视为同一靶标(原 kMinGapRadii)

	// ---- 灰度边界 (refineCenters → localThresholdEllipse) ----
	// >0: 用这个固定灰度阈值重新取候选周围的未腐蚀白斑轮廓(灰度 > 该值的像素算目标),
	//     拟合出的椭圆必须包含这条灰度等值线 —— 亚像素椭圆比它小时按面积比放大。
	// 0 (默认): 保持原行为, 用局部大津法自动求阈值。
	// 注意它只作用于"拟合轮廓的边界", 不改变预处理里的二值化(那是 useOtsu/fixedThreshold)。
	int    boundaryThreshold = 0;
};


class SCHNEIDER0807_EXPORT TargetDetector : public QObject
{
	Q_OBJECT

public:
	TargetDetector(QObject *parent);
	~TargetDetector();

	int detectTargets(const unsigned char* imgData, int width, int height, int channels,
		TargetInfo* targets, int maxTargets);

	void setCameraParameters(const cv::Mat& cameraMatrix, const cv::Mat& distCoeffs);
	std::vector<DetectionResult> detectTarget(cv::Mat& image);

	// 标记点识别: 与 detectTarget 共用预处理、近圆初筛和 Zernike 亚像素精化,
	// 但**去掉"外围必须有编码环"的筛选**, 也不解码、不按编码片的 minGapRadii 去重。
	// 用于识别普通圆形标记点 —— 也就是编码片的中心圆那种实心亮圆盘。
	// 返回的 DetectionResult::id 固定为 -1(标记点没有编码 ID); 顺序号由调用方按结果下标生成。
	// center/ellipse/radius/subpixel/rms/nPoints 的语义与 detectTarget 完全一致。
	std::vector<DetectionResult> detectMarkers(cv::Mat& image);

	// 中间过程可视化开关(OTSU/腐蚀/轮廓/各候选 debug 图)。
	// 默认 false: 供 UI 或产线调用时静默运行, 不弹窗、不 waitKey 阻塞。
	// 自己单独调试识别流程时 setDebugView(true) 打开。
	void setDebugView(bool on) { m_debugView = on; }

	// 返回与 detectTargets/detectTarget 同一(去畸变后)坐标系的图像, 供 UI 把识别结果叠加上去。
	// 若直接显示原图, 因畸变(k1/k2 不为 0)叠加点会整体偏移。
	cv::Mat undistortImage(const cv::Mat& src);

	// ---- 识别参数 ----
	// 取一份当前有效参数的副本(加锁, 可跨线程调用)。
	DetectorParams params() const;
	// 设置识别参数。每一项都会被夹到合法范围(越界的那一项保持原值不动, 不抛异常)。
	// 可在识别进行中调用: 正在跑的那一次识别用的是它开始时取的快照, 不受影响,
	// 下一次识别才用新参数 —— 不会出现"半新半旧"的一次识别。
	void setParams(const DetectorParams& p);
	// 出厂默认参数(= 参数化之前的硬编码常量)。
	static DetectorParams defaultParams() { return DetectorParams(); }

private:
	cv::Mat K;
	cv::Mat distCoeffs;
	bool m_debugView = false;

	// 当前参数与它的锁。识别跑在别的线程上, GUI 线程随时可能 setParams, 必须加锁。
	// 读取一律通过 paramsSnapshot() 拿副本后按值使用, 不直接读成员。
	DetectorParams m_params;
	mutable std::mutex m_paramsMutex;
	DetectorParams paramsSnapshot() const;

	// 去畸变映射表缓存: cv::undistort 每次调用都会重算整张 remap 表(大图很贵),
	// 这里按图像尺寸缓存一次, 之后只做 remap。setCameraParameters 会使其失效。
	cv::Mat m_undistMap1;
	cv::Mat m_undistMap2;
	cv::Size m_undistMapSize;
	std::mutex m_undistMutex;          // 去畸变表与图像内容无关, 只需按尺寸建一次
	void ensureUndistortMap(cv::Size sz);
	void undistortCached(const cv::Mat& src, cv::Mat& dst);

	// 内部实现识别逻辑。
	// details 非空时, 顺带把每个靶标的亚像素轮廓(拟合椭圆/半径)一并输出, 供 UI 叠加显示。
	// P 由入口函数取一次快照后一路按值传下去: 一次识别全程用同一套参数, 中途改参数不会
	// 造成"前半段用旧值、后半段用新值"。下面的私有方法同理都收 P。
	int detectTargetsImpl(const cv::Mat& img, TargetInfo* targets, int maxTargets,
		std::vector<DetectionResult>* details, const DetectorParams& P);

	// 靶标ID和编码映射
	std::map<int, uint16_t> idToCode;

	// 辅助函数
	std::vector<uint16_t> adjustCode(uint16_t code);
	bool detectBoundary(const cv::Mat& t, const cv::Point2f& center, float a,
		int angle_start, int angle_end, int& boundary_angle, const DetectorParams& P);




	// gray_code = 保留编码环的二值图(含中心圆盘+跑道编码块), 供 decodeSingleTarget 采样;
	// gray_clean = 预筛后只剩中心圆盘的二值图, 供候选检测。
	void preprocessImage(const cv::Mat& img, cv::Mat& gray_clean, cv::Mat& gray_code,
		cv::Mat& gray_blur,
		std::vector<std::vector<cv::Point> >& contours, const DetectorParams& P);

	// ellipses: 与 centers/radii 一一对应的粗椭圆(整数轮廓 fitEllipse), 倾斜时圆在像面是椭圆,
	// 供后续 refineCenters 用 Zernike 亚像素点精化出亚像素椭圆。
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

	// refinedContours: 与其余输出一一对应的"阈值边界亚像素折线";
	// 空 vector = 该候选没有用等值线路径(默认 Zernike, 按椭圆画)。
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


	// 检查圆环上白色像素比例是否大于阈值（用于预筛选）
	bool checkRingWhiteRatio(const Mat& roi, const Point2f& centerInROI, float radius, float ratioThreshold = 1.0f / 3.0f);



};



