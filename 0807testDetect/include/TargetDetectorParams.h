#pragma once

// =============================================================================
// 上位机 / DLL 内部使用的完整识别参数（33 项）。
//
// ⚠ 这个头文件**不发给客户**：
//   - 客户只通过 TargetDetector.h 里的 DetectOptions 调整 10 个重要参数；
//   - 其余参数在这里固化，只有项目所有者改这里并重编 DLL 才会变化。
//
// 每个字段的默认值都逐个等于参数化之前写死在 TargetDetector.cpp 里的常量，
// 所以不调用 setParams() 时行为与从前完全一致。
//
// 注意有几组"数值相近但语义不同"的参数不能合并：
//   - maxRing(4.2, 轮廓筛选的编码带外沿) 与 codeRingOuter(4.1, 解码采样的编码带外沿)
//   - refineBandRatio(0.18, Zernike 选点带半宽) 与 codeBrightAtAngle 里的径向步长 0.18
// 单位说明: "×R" 表示相对中心圆盘半径 R 的倍数。
// =============================================================================
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
