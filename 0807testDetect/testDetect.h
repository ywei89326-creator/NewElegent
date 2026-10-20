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
    void onStartDetect();          // 识别编码片(原有行为)
    void onStartDetectMarkers();   // 识别普通标记点(编码片中心圆那种实心亮圆盘)
    void onToggleRoiMode(bool on);
    void onItemChanged(QListWidgetItem* item);
    void onItemClicked(QListWidgetItem* item);
    void onListContextMenu(const QPoint& pos);   // 右键删除单个结果
    void onRemoveNoise();                        // 按轮廓面积批量删除干扰点
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
        QGraphicsPathItem* contour = nullptr;   // 亚像素拟合轮廓(倾斜时是椭圆)
        QGraphicsEllipseItem* dot = nullptr;    // 亚像素中心点
        QGraphicsTextItem* label = nullptr;
    };

    void buildUi();
    void showImage(const cv::Mat& bgr);
    void clearResults();
    void finishDetect(const std::vector<DetectionResult>& results);
    void applyHighlights();
    void updateRoiStatus();
    void updateResultTitle();                             // 标题里的结果计数
    void updateZoomLabel();
    void setBusy(bool busy);
    // 按列表行号删除结果(行号与 m_markers 下标一一对应), 同步删除图元并重排索引。
    int removeMarkerRows(QVector<int> rows);

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
    void applyParamsToDetector();                         // 控件 -> setParams -> 落盘
    void setParamsDirty(bool dirty);                      // 更新"未应用"提示
    void updateParamDependents();                         // 参数间的联动禁用
    void setAdvancedUnlocked(bool on);                    // 切换高级参数权限
    void applyParamVisibility();                          // 按权限显示/隐藏参数行与分组

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
    cv::Mat m_srcMat;                // 导入的原始图(未去畸变), 识别时喂给它
    bool m_hasImage = false;
    bool m_roiMode = false;          // 「开始画框」是否打开
    bool m_busy = false;
    bool m_markerMode = false;       // 最近一次识别是否为「识别标记点」(结果列表/状态栏据此换文案)
    bool m_updatingList = false;     // 程序化改列表选中状态时抑制 itemSelectionChanged
    bool m_autoFit = true;           // 用户手动缩放后置 false, 窗口 resize 就不再抢回适应窗口

    QGraphicsScene* m_scene = nullptr;
    QGraphicsView* m_view = nullptr;
    QGraphicsPixmapItem* m_pixmapItem = nullptr;
    QGraphicsPolygonItem* m_roiItem = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_listTitle = nullptr;
    QPushButton* m_btnRemoveNoise = nullptr;   // 【删除干扰点】按面积阈值批量删除
    QLabel* m_zoomLabel = nullptr;   // 状态栏右侧常驻的缩放比例

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
    QPushButton* m_btnDetect = nullptr;
    QPushButton* m_btnDetectMarkers = nullptr;
    QPushButton* m_btnRoi = nullptr;
    QPushButton* m_btnFit = nullptr;
    QPushButton* m_btnClear = nullptr;

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
