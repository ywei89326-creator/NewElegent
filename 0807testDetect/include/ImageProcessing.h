#pragma once

#include "ImageProcessingLib_global.h"
#include <opencv2/opencv.hpp>
#include <vector>

class IMAGEPROCESSINGLIB_EXPORT ImageProcessing
{
public:
    ImageProcessing();
    ~ImageProcessing();


    // Zernike矩亚像素边缘检测 
	static void zernikeEdgeDetection(const cv::Mat& gray,std::vector<cv::Point2f>& edgePoints,std::vector<float>& edgeK,double kMin = 0.0);

    // 大津法阈值计算 
    static float otsuThreshold(const std::vector<float>& data);

    // 最小二乘圆拟合 
    static void fitCircle(const std::vector<cv::Point2f>& points,
        cv::Point2f& center, float& radius);

    // Zernike圆精化
    static bool refineCircleWithZernike(const std::vector<cv::Point2f>& allEdgePts,const std::vector<float>& allEdgeK,float kt,cv::Point2f approxCenter, float approxRadius,float margin,cv::Point2f& refinedCenter, float& refinedRadius);

    // Zernike椭圆精化: 在 approx 椭圆周围的椭圆环带内挑选亚像素边缘点, 最小二乘拟合椭圆
    // (倾斜时圆在像面投成椭圆, 用亚像素轮廓拟合椭圆比只拟合圆/只fit整数轮廓更准)。
    // approx 通常来自整数轮廓 cv::fitEllipse; marginPx 为带半宽(像素)。
    // 成功返回 true, refined 为精化后的亚像素椭圆。
    static bool refineEllipseWithZernike(const std::vector<cv::Point2f>& allEdgePts,
        const std::vector<float>& allEdgeK, float kt,
        const cv::RotatedRect& approx,
        float marginPx,
        cv::RotatedRect& refined);
};
