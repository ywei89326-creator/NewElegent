#pragma once

#include <QtWidgets/QMainWindow>
#include <QGraphicsScene>
#include <QPointer>
#include <QPolygonF>
#include <QThread>
#include <QVector>

#include <functional>

#include <opencv2/opencv.hpp>

#include "include/TargetDetector.h"
// 注意: 不要再 include TargetDetectorLib 的 TargetDetector1215.h ——
// 它在全局命名空间重复定义了 TargetInfo / DetectionResult / TargetDetector,
// 与上面 schneider0807 的同名(且 Layout 不同), 同时包含会 C2011 重定义。
#include "MergeMarksBridge.h"   // 纯声明, 不含 PCL/VTK
#include "ui_testDetect.h"

class QCheckBox;
class QDoubleSpinBox;
class QGraphicsEllipseItem;
class QGraphicsPathItem;
class QGraphicsPixmapItem;
class QGraphicsPolygonItem;
class QGraphicsTextItem;
class QGraphicsView;
class QFormLayout;
class QGroupBox;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QLabel;
class QScrollArea;
class QShortcut;
class QSpinBox;
class QTabWidget;

// 一个参数对应的控件与读写方式。
// 所有参数共用同一套操作(读写控件、存/读 ini、恢复默认), 因此收敛成 m_fields 表;
// 新增参数只需在 buildParamPane 加一行 addXxxField()。
struct ParamField {
    QString key;                                          // ini 键名(纯 ASCII)
    QWidget* w = nullptr;                                 // QCheckBox / QSpinBox / QDoubleSpinBox
    QGroupBox* group = nullptr;                           // 它属于哪个分组(整组联动禁用要用)
    bool basic = false;                                   // true = 默认用户可见; false = 需密码解锁
    std::function<double(const DetectorParams&)> read;    // 参数集 -> 数值(填控件用)
    std::function<void(DetectorParams&, double)> write;   // 数值 -> 参数集
};

// 一份命名保存的相机标定: 3x3 内参 + 畸变系数。
// D 的长度允许 4/5/8/12/14, 顺序与 OpenCV calibrateCamera 一致。
struct CameraProfile {
    QString name;
    double K[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    QVector<double> D = { 0, 0, 0, 0, 0 };
};

// 宿主侧参数: 不放进 DetectorParams。
// DetectorParams 是按值嵌在 TargetDetector 里的结构, 加字段会改变对象布局,
// 必须同批重编两份头文件 + schneider0807.lib + dll; 这里的规则都只在
// 进入 DLL 之前/之后做, 与识别算法本身无关。
struct HostParams {
    // 结果过滤: 面积窗口(平方像素), 0 = 该方向不限。
    int  minResultArea = 100;
    int  maxResultArea = 10000;

    // 检测前图像增强: 黑白场线性拉伸。
    // 低于黑场的像素压成 0, 高于白场的拉到 255, 中间线性拉伸 —— 目标白底更白、
    // 环境暗噪声更黑, 让后续二值化/轮廓筛选更容易分干净。
    // 只在喂给识别器的输入上做(以及显示图), 不改磁盘上的原图。
    bool enhanceEnable = false;
    int  enhanceBlack = 0;
    int  enhanceWhite = 255;

    // 灰度化: 勾选后显示图、识别输入都用单通道灰度图(CV_8UC1)。
    // 转换在宿主侧完成(BGR2GRAY, 与 DLL 内部的 cvtColor 同一组权重), DLL 直接
    // 拿灰度图跑预处理/拟合; 不勾选时行为与以前完全一致。纯点运算, 不改坐标。
    bool grayscale = false;

    // ---- 质量分级(只影响黄色标记与列表排序, 不丢弃结果) ----
    // 长短轴比(短/长)低于它 -> 质量差。
    double poorAspect = 0.70;
    // 等值线路径: rms / 半径 超过它(百分比) -> 质量差。
    double poorIsoRmsPercent = 2.0;
    // Zernike 路径: rms 超过 (选点带宽/√3) 的倍数 -> 质量差。
    double poorZernikeRmsFactor = 1.5;
};

// 主窗口: 导入图片 → 识别 → 结果列表与叠加显示; 勾选结果可连成 ROI。
// 右下角为识别参数面板(见 buildParamPane); 第二页为懒加载的点云拼接。
class testDetect : public QMainWindow
{
    Q_OBJECT

public:
    testDetect(QWidget* parent = nullptr);
    ~testDetect() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;

private slots:
    void onImportImage();
    void onCameraParams();         // 相机内参/畸变设置(命名保存、复制粘贴)
    void onStartDetect();          // 识别编码片(编码片优先, 区域内标记点被取代)
    void onStartDetectMarkers();   // 识别普通标记点(编码片区域内的结果被排除)
    void onStartDetectAll();       // 识别所有标记点(包含编码片): 一次跑完两类
    void onToggleRoiMode(bool on);
    void onItemChanged(QListWidgetItem* item);
    void onItemClicked(QListWidgetItem* item);
    void onListContextMenu(const QPoint& pos);   // 右键删除单个结果
    void onRemoveNoise();                        // 按轮廓面积批量删除干扰点
    void onRemovePoor();                         // 一键删除所有质量差(黄色轮廓)的点
    void onUndo();                               // 撤销上一次删除
    void onExportResults();                      // 导出剩余结果到 txt
    void onClearSelection();
    void onFitView();
    void onTabChanged(int index);
    void onParamEdited();          // 任一参数控件被改动 -> 标记"未应用"
    void onApplyParams();          // 【应用参数】
    void onResetParams();          // 【恢复默认】
    void onSaveParamsFile();       // 【保存参数…】
    void onLoadParamsFile();       // 【载入参数…】
    void onUnlockAdvanced();       // 【高级参数…】输入密码解锁 / 收起
    void onToggleFullScreen();     // F11 / 【全屏】

private:
    // 一个识别结果对应的图元
    struct Marker {
        int id = -1;
        bool isMarker = false;                  // true = 标记点模式(标签显示 M1..Mn, id 是序号)
        QPointF pos;
        double area = 0.0;                      // 轮廓面积(拟合椭圆/圆), 平方像素
        double major = 0.0;                     // 长半轴(px)
        double aspect = 1.0;                    // 短轴/长轴, 圆为 1
        bool subpixel = true;                   // false = 中心退回了整数轮廓, 不是亚像素的
        bool lowQuality = false;                // 拟合质量差(残差大 / 椭度大 / 非亚像素):
                                                // 轮廓画黄色, 结果列表排在最后
        bool isocontour = false;                // 轮廓来自阈值等值线(质量分级用它的残差口径)
        float rms = 0.f;                        // 拟合残差(仅供参考, 非标定精度)
        int nPoints = 0;                        // 参与拟合的点数
        QGraphicsPathItem* contour = nullptr;   // 亚像素拟合轮廓(倾斜时是椭圆)
        QGraphicsEllipseItem* dot = nullptr;    // 亚像素中心点
        QGraphicsTextItem* label = nullptr;
    };

    // 撤销记录: 删除时把图元从 scene 摘除但不销毁, 列表项也 takeItem 保留,
    // 撤销时按原下标插回即可, 无需重建图形。
    struct RemovedRow {
        int index = -1;
        Marker marker;
        QListWidgetItem* item = nullptr;
    };
    struct UndoOp {
        QVector<RemovedRow> rows;
    };

    // 手动删除的点(坐标黑名单)。重新识别同一类型时, 与其中一项中心距 ≤ 5px 的
    // 新结果不再加回列表 —— 用户删掉的干扰点不会因为再点一次识别就复活。
    // 编码片与标记点分开记: 删掉的标记点不能压制后来在同一位置识别出的编码片。
    struct DeletedPoint {
        QPointF pos;
        bool isMarker = false;
    };

    void buildUi();
    void showImage(const cv::Mat& bgr);
    void refreshDisplayImage();                          // 只换显示图, 保留结果叠加
    // 按 m_host 的黑白场把图像拉伸成高对比版本; 未启用时原样返回(浅拷贝)。
    cv::Mat enhanceImageForDetect(const cv::Mat& src) const;
    // 识别输入: 先按黑白场增强, 勾选「灰度化」时再转成单通道灰度。
    cv::Mat prepareDetectInput(const cv::Mat& src) const;
    // 显示图 = 去畸变(prepareDetectInput(原图))。与识别器内部看到的图同源,
    // 所以灰度化时鼠标悬停读到的就是识别器实际使用的灰度值。
    cv::Mat buildDisplayImage();
    // 灰度化模式下, 在状态栏右侧显示鼠标所在像素的坐标/灰度/与边界阈值的关系。
    void updatePixelInfo(const QPoint& viewportPos);
    void clearPixelInfo();
    void clearResults();
    void applyHighlights();
    void updateRoiStatus();
    void updateResultTitle();                             // 标题里的结果计数
    void updateZoomLabel();
    void setBusy(bool busy);
    // 按列表行号删除结果(行号与 m_markers 下标一一对应), 记录撤销信息并重排索引。
    int removeMarkerRows(QVector<int> rows);
    void clearUndoStack();                                // 丢弃撤销记录并销毁被摘除的图元
    void updateResultButtons();                           // 撤销/导出/删除等结果相关按钮的可用状态
    void updateQualitySeparator();                        // 重算"质量好/差"分界线画在哪一行
    void reclassifyResults();                             // 按当前质量阈值重算好坏并重排列表(不重新识别)

    // ---- 编码片/标记点混合结果 ----
    // 三个识别入口最终都汇到这里: 只替换被重算的那一类, 另一类按编码片区域/删除黑名单合并。
    void finishCodedDetect(const std::vector<DetectionResult>& results);
    void finishMarkerDetect(const std::vector<DetectionResult>& results);
    void finishDetectAll(const std::vector<DetectionResult>& coded,
                         const std::vector<DetectionResult>& markers);
    Marker createMarker(const DetectionResult& r, bool isMarker);
    void finalizeResultMerge();                           // 重排编号/重建列表行/刷新统计
    static QString markerRowText(const Marker& mk);       // 结果列表一行文字
    QString resultSummary() const;                        // "编码片 x 个，标记点 y 个 | ..."
    bool isResultAreaFiltered(double area) const;         // 面积不在 [最小, 最大] 内, 直接丢弃
    bool isInCodedRegion(const QPointF& pos) const;       // 落在最近一次编码片区域 4.2R 内
    bool isDeletedPoint(const QPointF& pos, bool isMarker) const;
    void rememberDeletedPoint(const QPointF& pos, bool isMarker);
    void forgetDeletedPoint(const QPointF& pos, bool isMarker);

    // ---- 参数设置栏 ----
    void buildParamPane(QWidget* paramPane);
    // 建参数控件并注册到 m_fields; 返回控件供少数需要联动禁用的地方使用。
    QCheckBox* addBoolField(QFormLayout* form, const QString& key, const QString& label,
        const QString& tip, bool DetectorParams::* member);
    QSpinBox* addIntField(QFormLayout* form, const QString& key, const QString& label,
        const QString& tip, int lo, int hi, int DetectorParams::* member);
    QDoubleSpinBox* addDoubleField(QFormLayout* form, const QString& key, const QString& label,
        const QString& tip, double lo, double hi, int decimals,
        double DetectorParams::* member);

    DetectorParams collectParams() const;                 // 控件 -> 参数集
    void loadParamsToWidgets(const DetectorParams& p);    // 参数集 -> 控件
    HostParams collectHostParams() const;                 // 控件 -> 宿主侧参数(含夹取)
    void loadHostParamsToWidgets();                       // 宿主侧参数 -> 控件
    void applyParamsToDetector();                         // 控件 -> setParams -> 落盘
    void setParamsDirty(bool dirty);                      // 更新"未应用"提示
    void updateParamDependents();                         // 参数间的联动禁用
    void setAdvancedUnlocked(bool on);                    // 切换高级参数权限
    void applyParamVisibility();                          // 按权限显示/隐藏参数行与分组

    // ---- 相机标定 ----
    void initCameraDefaults();                            // 出厂默认(与 DLL 构造函数一致)
    void loadActiveCameraProfile();                       // 启动时载入上次使用的配置
    QString cameraConfigPath() const;                     // <exe>/cameras.ini
    bool loadCameraProfiles(QVector<CameraProfile>& profiles, QString& activeName) const;
    void saveCameraProfiles(const QVector<CameraProfile>& profiles, const QString& activeName);
    void applyCameraProfile(const CameraProfile& p);      // 应用到识别器并刷新显示
    void updateCameraButtonTooltip();

    QString paramsIniPath() const;
    void saveParamsIni();
    bool loadParamsIni();                                 // 返回 false = 没有可用的 ini
    static void writeParamsToSettings(const DetectorParams& p, const QVector<ParamField>& fields,
        const QString& group, const QString& filePath);
    static bool readParamsFromSettings(DetectorParams& p, const QVector<ParamField>& fields,
        const QString& group, const QString& filePath);

    // ---- 第二页「点云拼接」(MergeMarks) ----
    // 懒加载: 第一次切到那一页才 LoadLibrary, 平时一个字节都不加载。
    void ensureMergeMarksLoaded();
    void showMergeMarksError(const QString& msg);

    // 以选中点质心为中心按极角排序 —— 保证无论勾选先后, 都连成不自交的多边形
    static QPolygonF buildRoiPolygon(const QVector<QPointF>& pts);

    Ui::testDetectClass ui;

    // 复用一个 detector: 去畸变映射表缓存在它里面, 导入时算一次识别时就不用再算
    TargetDetector m_detector{ nullptr };
    CameraProfile m_activeCamera;    // 当前生效的标定(名称 + K + D)
    cv::Mat m_srcMat;                // 导入的原始图(未去畸变), 识别时喂给它
    // 当前显示图(已去畸变 + 增强, 灰度化时是 CV_8UC1)。鼠标悬停读灰度值用它,
    // 坐标与 m_pixmapItem 的 scene 坐标一致(原点 = 图像左上角)。
    cv::Mat m_viewMat;
    bool m_hasImage = false;
    bool m_roiMode = false;          // 「开始画框」是否打开
    bool m_busy = false;
    bool m_updatingList = false;     // 程序化改列表选中状态时抑制 itemSelectionChanged
    bool m_autoFit = true;           // 用户手动缩放后置 false, 窗口 resize 就不再抢回适应窗口

    QGraphicsScene* m_scene = nullptr;
    QGraphicsView* m_view = nullptr;
    QGraphicsPixmapItem* m_pixmapItem = nullptr;
    QGraphicsPolygonItem* m_roiItem = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_listTitle = nullptr;
    QPushButton* m_btnRemoveNoise = nullptr;   // 【删除干扰点】模板匹配批量删除
    QPushButton* m_btnRemovePoor = nullptr;    // 【删除较差的点】删除全部黄色轮廓结果
    QPushButton* m_btnUndo = nullptr;          // 【撤销】恢复上一次删除
    QPushButton* m_btnExport = nullptr;        // 【导出】当前剩余结果 -> txt
    QVector<UndoOp> m_undoStack;               // 删除操作的撤销记录(仅本次识别有效)
    QLabel* m_zoomLabel = nullptr;   // 状态栏右侧常驻的缩放比例
    QLabel* m_pixelLabel = nullptr;  // 状态栏右侧: 灰度化时鼠标所在像素的灰度/边界判定

    // ---- 参数设置栏 ----
    QVector<ParamField> m_fields;
    QWidget* m_paramPane = nullptr;        // 参数区整块(setBusy 时整体禁用)
    QScrollArea* m_paramScroll = nullptr;
    QLabel* m_paramHint = nullptr;         // "有未应用的改动" 提示行
    QPushButton* m_btnApply = nullptr;
    QPushButton* m_btnResetParams = nullptr;
    QPushButton* m_btnSaveParams = nullptr;
    QPushButton* m_btnLoadParams = nullptr;
    QPushButton* m_btnUnlock = nullptr;    // 【高级参数…】默认用户看不到高级项
    QPushButton* m_btnFullScreen = nullptr;
    QShortcut* m_shortcutEsc = nullptr;    // 只在全屏时启用, 避免抢走控件的 Esc
    bool m_paramsDirty = false;
    bool m_basicDirty = false;             // 未应用的改动里是否含"默认用户可见"的项
    bool m_updatingParams = false;         // 程序化填控件时抑制"被改动"判定
    // 高级参数解锁状态。刻意不持久化: 每次启动都锁着, 避免客户机器上"一次输入永久可见"。
    bool m_advancedUnlocked = false;

    // 联动禁用所需的控件(见 updateParamDependents)。
    QGroupBox* m_groupSubpixel = nullptr;
    QGroupBox* m_groupDecode = nullptr;

    // ---- 宿主侧参数(不进 DetectorParams/DLL), 两组都只在高级参数解锁后可见 ----
    // 图像增强: 检测前黑场/白场拉伸。
    QGroupBox* m_groupEnhance = nullptr;
    QCheckBox* m_chkEnhance = nullptr;
    QSpinBox* m_spinEnhanceBlack = nullptr;
    QSpinBox* m_spinEnhanceWhite = nullptr;
    // 灰度与阈值: 灰度化开关(宿主侧) + 边界阈值(DetectorParams, DLL 侧)。
    // 完整识别链路不使用密码, 整组始终可见。
    QGroupBox* m_groupGray = nullptr;
    QCheckBox* m_chkGrayscale = nullptr;
    // 质量分级: 三项宿主侧阈值, 决定哪些点画黄色/沉底。始终可见。
    QGroupBox* m_groupQuality = nullptr;
    QDoubleSpinBox* m_spinPoorAspect = nullptr;
    QDoubleSpinBox* m_spinPoorIsoRms = nullptr;
    QDoubleSpinBox* m_spinPoorZernike = nullptr;
    // 结果过滤: 面积窗口, 进列表前丢弃。
    QGroupBox* m_groupResultFilter = nullptr;
    QSpinBox* m_spinMinArea = nullptr;
    QSpinBox* m_spinMaxArea = nullptr;

    HostParams m_host;                         // 已生效的宿主侧参数
    QCheckBox* m_chkSubpixel = nullptr;
    QCheckBox* m_chkOtsu = nullptr;
    QWidget* m_spinFixedThr = nullptr;
    QCheckBox* m_chkOtsuK = nullptr;
    QWidget* m_spinFixedKt = nullptr;

    // 标签页 + 第二页(点云拼接)
    QTabWidget* m_tabs = nullptr;
    QWidget* m_mergeMarksPage = nullptr;
    QLabel* m_mmPlaceholder = nullptr;
    void* m_mmPanel = nullptr;       // MergeMarks_CreatePanel 返回的 QWidget*
    void* m_mmModule = nullptr;      // MergeMarksLib.dll 的 HMODULE
    FnMergeMarksDestroyPanel m_mmDestroy = nullptr;
    bool m_mmLoadTried = false;      // 失败过就不再反复重试
    QPushButton* m_btnImport = nullptr;
    QPushButton* m_btnCamera = nullptr;    // 【相机参数…】
    QPushButton* m_btnDetect = nullptr;
    QPushButton* m_btnDetectMarkers = nullptr;
    QPushButton* m_btnDetectAll = nullptr;     // 【识别所有标记点（包含编码片）】
    QPushButton* m_btnRoi = nullptr;
    QPushButton* m_btnFit = nullptr;
    QPushButton* m_btnClear = nullptr;

    // 最近一次编码片识别得到的**原始**结果(不去黑名单), 用来界定 4.2R 编码片区域。
    // 即使某一行随后被手动删掉, 它的区域仍然有效 —— 否则那个位置会冒出标记点。
    std::vector<DetectionResult> m_codedRegions;
    QVector<DeletedPoint> m_deletedPoints;     // 手动删除的坐标黑名单(当前图片内有效)

    QVector<Marker> m_markers;
    QPolygonF m_roiPolygon;          // 当前 ROI 多边形(图像像素坐标)
    QVector<int> m_roiIds;           // 参与 ROI 的编码 id

    // 图元尺寸随图像大小缩放, 保证不同分辨率下观感一致
    double m_markRadius = 8.0;
    double m_penW = 2.0;
    double m_labelFontPx = 20.0;     // ID 标签字号(图像像素)

    // 按下瞬间的勾选状态, 用于判断 Qt 是否已自行切换(见 onItemClicked)。
    QListWidgetItem* m_pressedItem = nullptr;
    Qt::CheckState m_pressedCheckState = Qt::Unchecked;

    // 线程结束后会 deleteLater 自身, 必须用 QPointer 防止悬空。
    QPointer<QThread> m_thread;
};
