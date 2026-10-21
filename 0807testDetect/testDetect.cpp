
// windows.h 必须先于 testDetect.h(后者间接引入 OpenCV):
// NOMINMAX 防止 min/max 宏破坏 std::min/std::max,
// 并避免 winnt.h 与 OpenCV 各自定义的 ACCESS_MASK 冲突。
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "testDetect.h"

#include <algorithm>
#include <cmath>
#include <exception>

#include <QCoreApplication>
#include <QDir>
#include <QFile>

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTextStream>
#include <QThread>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {


// 结果标签 HTML。marker=true 显示 M1..Mn(标记点无编码 ID), 否则显示 ID:n。
// fontPx 为屏幕像素: 标签图元忽略视图变换, 字号不随缩放变化。
// 开头的 <br> 让文字落在锚点下方, 避免遮住轮廓。
QString idLabelHtml(int id, bool highlight, double fontPx, bool marker = false)
{
    const QString bg = highlight ? QStringLiteral("rgba(216,40,40,235)")
                                 : QStringLiteral("rgba(0,0,0,190)");
    const QString fg = highlight ? QStringLiteral("#ffffff")
                                 : QStringLiteral("#3aff8a");
    const QString tag = marker ? QStringLiteral("M%1").arg(id)
                               : QStringLiteral("ID:%1").arg(id);
    return QStringLiteral("<br><span style=\"background-color:%1;color:%2;"
                          "font-size:%3px;font-weight:bold;font-family:Consolas,monospace;"
                          "padding:2px 6px;\">%4</span>")
        .arg(bg, fg, QString::number(fontPx, 'f', 0), tag);
}

// 结果列表里"质量较差"分界线的标记角色(行项目上存 bool)。
// 与 Qt::UserRole(marker 下标)分开, 避免破坏"行号 == m_markers 下标"。
const int kFirstPoorRole = Qt::UserRole + 2;

// 在"第一条质量差的结果"上方画一条黄色分界线, 把好点/差点分成两栏。
// 只是绘制, 不新增列表行 —— 行号与 m_markers 下标的一一对应关系保持不变。
class QualitySeparatorDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);   // 先画正常行(含勾选框)
        if (!index.data(kFirstPoorRole).toBool())
            return;

        painter->save();
        QPen pen(QColor(255, 210, 40, 210));
        pen.setWidth(2);
        painter->setPen(pen);
        const QRect r = option.rect;
        painter->drawLine(r.left() + 6, r.top() + 1, r.right() - 6, r.top() + 1);
        painter->restore();
    }
};

// 亚像素轮廓路径。用圆弧段而非折线, 放大后不会出现棱角。
// 变换按 OpenCV RotatedRect 约定构造: QTransform(m11, m12, m21, m22, dx, dy)。
QPainterPath contourPath(const cv::RotatedRect& e)
{
    QPainterPath unit;
    unit.addEllipse(QPointF(0.0, 0.0), 1.0, 1.0);

    // 与 cv::ellipse 方向一致, 不会镜像或差 90°。
    const double th = e.angle * CV_PI / 180.0;
    const double ct = std::cos(th), st = std::sin(th);
    const double a = e.size.width * 0.5, b = e.size.height * 0.5;
    return QTransform(ct * a, st * a, -st * b, ct * b, e.center.x, e.center.y).map(unit);
}

QPainterPath contourPath(const cv::Point2f& c, double r)
{
    QPainterPath p;
    p.addEllipse(QPointF(c.x, c.y), r, r);
    return p;
}

// 闭合亚像素折线 -> 平滑曲线(Catmull-Rom 转三次贝塞尔)。
// 曲线严格经过每个边界点: 点本身已经是 DLL 沿灰度等值线插值出来的亚像素位置,
// 这里只负责把相邻点之间连得平滑, 不会把曲线拉离用户设定的边界。
QPainterPath smoothClosedPath(const std::vector<cv::Point2f>& pts)
{
    QPainterPath path;
    const int n = (int)pts.size();
    if (n < 3)
        return path;

    path.moveTo(pts[0].x, pts[0].y);
    for (int i = 0; i < n; ++i) {
        const cv::Point2f& p0 = pts[(i + n - 1) % n];
        const cv::Point2f& p1 = pts[i];
        const cv::Point2f& p2 = pts[(i + 1) % n];
        const cv::Point2f& p3 = pts[(i + 2) % n];
        const QPointF c1(p1.x + (p2.x - p0.x) / 6.0, p1.y + (p2.y - p0.y) / 6.0);
        const QPointF c2(p2.x - (p3.x - p1.x) / 6.0, p2.y - (p3.y - p1.y) / 6.0);
        path.cubicTo(c1, c2, QPointF(p2.x, p2.y));
    }
    path.closeSubpath();
    return path;
}

// 闭合折线面积(鞋带公式), 单位平方像素。
// 阈值等值线结果用它作为列表/导出/删除模板里的"面积", 与实际画出的轮廓一致。
double contourAreaOf(const std::vector<cv::Point2f>& pts)
{
    const int n = (int)pts.size();
    if (n < 3)
        return 0.0;
    double a = 0.0;
    for (int i = 0; i < n; ++i) {
        const cv::Point2f& p = pts[i];
        const cv::Point2f& q = pts[(i + 1) % n];
        a += (double)p.x * q.y - (double)q.x * p.y;
    }
    return std::fabs(a) * 0.5;
}

// 「编码片区域」= 编码带外沿 × 中心圆半径。中心圆之外的编码带里不会存在真正的
// 普通标记点, 所以标记点落在区域内一律丢弃, 由编码片取代。
const double kCodedRegionScale = 4.2;

// 删除黑名单的判定半径(px): 同一目标重复定位的偏差远小于它。
const double kDeletedPointTol = 5.0;

// 点是否落在 RotatedRect 按 scale 放大后的椭圆内。
// 旋转按 RotatedRect 的角度约定: 把点转到椭圆局部坐标即旋转 -angle。
bool pointInScaledEllipse(const cv::RotatedRect& e, double scale, const QPointF& p)
{
    if (e.size.width <= 0.f || e.size.height <= 0.f)
        return false;
    const double a = e.size.width * 0.5 * scale;
    const double b = e.size.height * 0.5 * scale;
    const double th = e.angle * CV_PI / 180.0;
    const double ct = std::cos(th), st = std::sin(th);
    const double dx = p.x() - e.center.x;
    const double dy = p.y() - e.center.y;
    const double xr = dx * ct + dy * st;
    const double yr = -dx * st + dy * ct;
    return (xr * xr) / (a * a) + (yr * yr) / (b * b) <= 1.0;
}

double dist2(const QPointF& a, const QPointF& b)
{
    const double dx = a.x() - b.x();
    const double dy = a.y() - b.y();
    return dx * dx + dy * dy;
}

// 与结果列表/导出里的面积口径完全一致:
//   阈值等值线结果 -> 实际绘制的闭合折线面积;
//   否则有拟合椭圆用长短半轴, 再否则用半径(至少 1px)。
double resultArea(const DetectionResult& r)
{
    if (r.contourPoints.size() >= 3)
        return contourAreaOf(r.contourPoints);
    if (r.hasEllipse && r.ellipse.size.width > 0 && r.ellipse.size.height > 0)
        return CV_PI * (r.ellipse.size.width * 0.5) * (r.ellipse.size.height * 0.5);
    const double rr = qMax((double)r.radius, 1.0);
    return CV_PI * rr * rr;
}

// 质量分级: 只影响显示(黄色轮廓 + 列表沉底), 不丢弃结果。
//   ① 非亚像素                  -> 差
//   ② 长短轴比 < 0.70            -> 差(椭度太大, 形状可信度低)
//   ③ 残差超限                  -> 差
// 两条精化路径的 rms 口径不同, 不能用一个绝对阈值:
//   - 等值线路径: rms 是"边界点相对拟合椭圆的径向残差", 用 rms/R 衡量, >2% 判差;
//   - Zernike 路径: rms 自带选点带的固有残差(band/√3), 先除以它归一化, >1.5 倍判差。
bool isLowQualityFit(const DetectionResult& r, const DetectorParams& p,
                     double major, double aspect)
{
    if (!r.subpixel)
        return true;
    if (aspect < 0.70)
        return true;

    const double R = qMax(major, 1.0);
    if (!r.contourPoints.empty())                       // 阈值等值线路径
        return (double)r.rms > 0.02 * R;

    const double band = qMax((double)p.refineBandMin, (double)p.refineBandRatio * R);
    const double expected = band / std::sqrt(3.0);      // 带内均匀分布的理论残差地板
    return expected > 0.0 && (double)r.rms > 1.5 * expected;
}

// 宿主侧参数不在 DetectorParams 里(那是 DLL 的按值结构, 改布局要同批重编 lib/dll),
// 但和识别参数放在同一个 [detectParams] 分组, 参数文件仍是"一个文件搞定"。
void writeHostParams(const HostParams& h, const QString& path)
{
    QSettings s(path, QSettings::IniFormat);
    s.beginGroup(QStringLiteral("detectParams"));
    s.setValue(QStringLiteral("minResultArea"), h.minResultArea);
    s.setValue(QStringLiteral("maxResultArea"), h.maxResultArea);
    s.setValue(QStringLiteral("enhanceEnable"), h.enhanceEnable ? 1 : 0);
    s.setValue(QStringLiteral("enhanceBlack"), h.enhanceBlack);
    s.setValue(QStringLiteral("enhanceWhite"), h.enhanceWhite);
    s.setValue(QStringLiteral("grayscale"), h.grayscale ? 1 : 0);
    s.endGroup();
    s.sync();
}

// 缺键的项保持 h 的现值(调用方先用默认值打底), 兼容旧版 settings.ini/参数文件。
bool readHostParams(HostParams& h, const QString& path)
{
    QSettings s(path, QSettings::IniFormat);
    if (s.status() != QSettings::NoError)
        return false;
    s.beginGroup(QStringLiteral("detectParams"));
    bool any = false;
    auto readInt = [&](const QString& key, int lo, int hi, int& out) {
        if (!s.contains(key))
            return;
        bool ok = false;
        const int v = s.value(key).toInt(&ok);
        if (ok) {
            out = qBound(lo, v, hi);
            any = true;
        }
    };
    readInt(QStringLiteral("minResultArea"), 0, 1000000, h.minResultArea);
    readInt(QStringLiteral("maxResultArea"), 0, 1000000, h.maxResultArea);
    readInt(QStringLiteral("enhanceBlack"), 0, 254, h.enhanceBlack);
    readInt(QStringLiteral("enhanceWhite"), 1, 255, h.enhanceWhite);
    if (s.contains(QStringLiteral("enhanceEnable"))) {
        h.enhanceEnable = s.value(QStringLiteral("enhanceEnable")).toInt() != 0;
        any = true;
    }
    if (s.contains(QStringLiteral("grayscale"))) {
        h.grayscale = s.value(QStringLiteral("grayscale")).toInt() != 0;
        any = true;
    }
    s.endGroup();
    return any;
}

// 多边形面积(鞋带公式), 单位: 平方像素。
double polygonArea(const QPolygonF& poly)
{
    const int n = poly.size();
    if (n < 3) return 0.0;
    double a = 0.0;
    for (int i = 0; i < n; ++i) {
        const QPointF& p = poly[i];
        const QPointF& q = poly[(i + 1) % n];
        a += p.x() * q.y() - q.x() * p.y();
    }
    return std::fabs(a) * 0.5;
}

// ---------------------------------------------------------------- 相机标定工具

// 从任意文本里提取所有浮点数: 兼容 "[[fx,0,cx],[0,fy,cy],[0,0,1]]"、
// 逗号/空格/换行分隔的列表, 也兼容从 MATLAB/Python 复制出来的矩阵。
QVector<double> parseNumberList(const QString& text)
{
    static const QRegularExpression re(
        QStringLiteral(R"([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)"));
    QVector<double> out;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        bool ok = false;
        const double v = m.captured(0).toDouble(&ok);
        if (ok && std::isfinite(v))
            out.push_back(v);
    }
    return out;
}

// OpenCV initUndistortRectifyMap 接受 4/5/8/12/14 个畸变系数。
bool validDistortionCount(int n)
{
    return n == 4 || n == 5 || n == 8 || n == 12 || n == 14;
}

QString formatK(const double k[9])
{
    QStringList rows;
    for (int r = 0; r < 3; ++r) {
        QStringList cols;
        for (int c = 0; c < 3; ++c)
            cols << QString::number(k[r * 3 + c], 'g', 12);
        rows << cols.join(QLatin1Char(' '));
    }
    return rows.join(QLatin1Char('\n'));
}

QString formatD(const QVector<double>& d)
{
    QStringList vals;
    for (double v : d)
        vals << QString::number(v, 'g', 12);
    return vals.join(QStringLiteral(", "));
}

// 解析 + 校验。空畸变按“无畸变”(5 个 0)处理。
bool parseCameraText(const QString& kText, const QString& dText,
    double kOut[9], QVector<double>& dOut, QString& err)
{
    const QVector<double> kn = parseNumberList(kText);
    if (kn.size() == 4) {
        // 常见简写: fx, fy, cx, cy
        kOut[0] = kn[0]; kOut[1] = 0.0;   kOut[2] = kn[2];
        kOut[3] = 0.0;   kOut[4] = kn[1]; kOut[5] = kn[3];
        kOut[6] = 0.0;   kOut[7] = 0.0;   kOut[8] = 1.0;
    }
    else if (kn.size() == 9) {
        for (int i = 0; i < 9; ++i)
            kOut[i] = kn[i];
    }
    else {
        err = QStringLiteral("内参需要 9 个数（3×3），或 4 个数（fx, fy, cx, cy）；"
                             "当前 %1 个。").arg(kn.size());
        return false;
    }

    if (std::fabs(kOut[6]) > 1e-6 || std::fabs(kOut[7]) > 1e-6 ||
        std::fabs(kOut[8] - 1.0) > 1e-6) {
        err = QStringLiteral("内参最后一行应为 0 0 1。");
        return false;
    }
    if (std::fabs(kOut[0]) < 1e-12 || std::fabs(kOut[4]) < 1e-12) {
        err = QStringLiteral("内参的 fx / fy 不能为 0。");
        return false;
    }

    dOut = parseNumberList(dText);
    if (dOut.isEmpty())
        dOut = QVector<double>{ 0, 0, 0, 0, 0 };
    else if (!validDistortionCount(dOut.size())) {
        err = QStringLiteral("畸变系数需要 4/5/8/12/14 个数（当前 %1 个）。")
                  .arg(dOut.size());
        return false;
    }
    return true;
}

// 与 TargetDetector.cpp 构造函数里的出厂内参保持一致。
CameraProfile factoryCameraProfile()
{
    CameraProfile p;
    p.name = QStringLiteral("出厂默认");
    const double k[9] = {
        6633.54, 0.0, 1544.94,
        0.0, 6633.48, 1091.96,
        0.0, 0.0, 1.0
    };
    for (int i = 0; i < 9; ++i)
        p.K[i] = k[i];
    p.D = QVector<double>{ 0.047895, -0.226498, 0.000347139, 0.000879732, 0.0 };
    return p;
}

} // namespace


testDetect::testDetect(QWidget* parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);
    initCameraDefaults();
    buildUi();
    loadActiveCameraProfile();
}

testDetect::~testDetect()
{
    // 等待识别线程结束: 回调目标是 this, 析构后回调即悬空指针。
    if (m_thread && m_thread->isRunning())
        m_thread->wait();

    // 先销毁面板: 面板内的 qvtkWidget 持有 PCL 创建的 VTK 对象, DLL 不在时会悬空。
    if (m_mmPanel && m_mmDestroy) {
        m_mmDestroy(m_mmPanel);
        m_mmPanel = nullptr;
    }

    // 撤销记录里的图元已移出 scene, 不会被 scene 析构回收, 需手动释放。
    clearUndoStack();

    // ⚠ 刻意不调用 FreeLibrary(m_mmModule):
    // 它会连锁卸载 PCL/VTK 依赖, 而 VTK 不支持进程内卸载 —— vtkObjectFactory 注册表
    // 会残留指向已卸载模块的指针, 表现为 vtkCommonCore 内 0xC0000005(报错行却在
    // FreeLibrary 上, 极易误判)。进程即将退出, 交给系统回收即可。
    // 详见 MergeMarksBridge.h。
}


// ---------------------------------------------------------------- 界面搭建

void testDetect::buildUi()
{
    setWindowTitle(QStringLiteral("标记点工具"));
    // 再小右侧参数栏的标签会被截断, 图像视图也失去意义。
    setMinimumSize(1024, 680);
    resize(1280, 820);

    QWidget* central = centralWidget();
    QVBoxLayout* root = new QVBoxLayout(central);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(0);

    // 两页: 靶标识别(默认) / 点云拼接(懒加载, 见 onTabChanged)。
    m_tabs = new QTabWidget(central);
    root->addWidget(m_tabs);

    QWidget* tabDetect = new QWidget(m_tabs);
    QVBoxLayout* root1 = new QVBoxLayout(tabDetect);
    root1->setContentsMargins(6, 8, 6, 6);
    root1->setSpacing(6);

    // —— 顶部按钮条 ——
    QHBoxLayout* bar = new QHBoxLayout();
    m_btnImport = new QPushButton(QStringLiteral("导入图片"), tabDetect);
    m_btnCamera = new QPushButton(QStringLiteral("相机参数…"), tabDetect);
    m_btnCamera->setToolTip(QStringLiteral(
        "设置靶标识别页的相机内参与畸变参数（命名保存、支持复制粘贴）。\n"
        "仅影响「靶标识别」页，不会作用于「点云拼接」页。"));
    m_btnDetect = new QPushButton(QStringLiteral("开始识别编码片"), tabDetect);
    // 复用同一套预处理与亚像素精化, 但不要求编码环、不解码。
    m_btnDetectMarkers = new QPushButton(QStringLiteral("识别标记点"), tabDetect);
    m_btnDetectMarkers->setToolTip(QStringLiteral(
        "识别普通圆形标记点（实心亮圆盘，即编码片中心圆）。\n"
        "算法与「开始识别编码片」相同，但不要求外围有编码环，也不解码 ID。\n"
        "落在已识别编码片区域内的标记点会被排除；已识别的编码片保留。"));
    // 一次跑完两类: 编码片优先, 标记点不含编码片区域内的。
    m_btnDetectAll = new QPushButton(QStringLiteral("识别所有标记点（包含编码片）"), tabDetect);
    m_btnDetectAll->setToolTip(QStringLiteral(
        "一次完成「开始识别编码片」+「识别标记点」。\n"
        "结果列表里编码片显示 ID:n，普通标记点显示 M1..Mn。"));
    m_btnRoi = new QPushButton(QStringLiteral("开始画框"), tabDetect);
    m_btnRoi->setCheckable(true);
    m_btnClear = new QPushButton(QStringLiteral("清空选择"), tabDetect);
    m_btnFit = new QPushButton(QStringLiteral("适应窗口"), tabDetect);
    // 与 F11 同一路径; 勾选态 = 当前是否为无边框全屏。
    m_btnFullScreen = new QPushButton(QStringLiteral("全屏 F11"), tabDetect);
    m_btnFullScreen->setCheckable(true);
    bar->addWidget(m_btnImport);
    bar->addWidget(m_btnCamera);
    bar->addWidget(m_btnDetect);
    bar->addWidget(m_btnDetectMarkers);
    bar->addWidget(m_btnDetectAll);
    bar->addWidget(m_btnRoi);
    bar->addSpacing(24);
    bar->addWidget(m_btnClear);
    bar->addWidget(m_btnFit);
    bar->addStretch(1);
    bar->addWidget(m_btnFullScreen);
    root1->addLayout(bar);

    // —— 左: 图像视图 ——
    m_scene = new QGraphicsScene(this);
    m_view = new QGraphicsView(m_scene, central);
    m_view->setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    m_view->setDragMode(QGraphicsView::ScrollHandDrag);   // 左键拖拽平移
    // 背景色必须在代码里设: 显式背景刷会压过 QSS(QSS 的 background 对其无效)。
    m_view->setBackgroundBrush(QBrush(QColor(0x1a, 0x20, 0x28)));
    m_view->setAlignment(Qt::AlignCenter);
    m_view->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);   // 滚轮以光标为锚点
    m_view->viewport()->installEventFilter(this);   // resize / 滚轮缩放 / 双击 / 悬停取灰度
    // 不按键也要收到 MouseMove: 灰度化时状态栏要实时显示光标像素的灰度值。
    m_view->viewport()->setMouseTracking(true);

    // —— 右: 识别结果列表 ——
    m_list = new QListWidget(central);
    m_list->setMinimumWidth(300);
    m_list->setAlternatingRowColors(true);
    m_list->setUniformItemSizes(true);
    // 质量好/差两栏之间的黄色分界线(纯绘制, 不占列表行)。
    m_list->setItemDelegate(new QualitySeparatorDelegate(m_list));
    // 等宽字体: 坐标是变长小数, 比例字体下小数点对不齐。
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPixelSize(13);
    m_list->setFont(mono);
    // 勾选框代表 ROI 顶点, 不用行选中(两层高亮会混淆)。
    // 点行任意位置等同于点勾选框, 见 onItemClicked。
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    // 必须用事件过滤器记录按下时的勾选态: 点在勾选框上时 delegate 会消费
    // MouseButtonPress, QListWidget::itemPressed 不会触发。
    m_list->viewport()->installEventFilter(this);

    m_listTitle = new QLabel(QStringLiteral("识别结果"), central);
    m_listTitle->setObjectName(QStringLiteral("sectionTitle"));   // 供 QSS 设强调色

    // 标题行右侧: 按面积批量删除误识别的小圈, 无结果时禁用。
    m_btnRemoveNoise = new QPushButton(QStringLiteral("删除干扰点"), central);
    m_btnRemoveNoise->setToolTip(QStringLiteral(
        "选一个正确识别的点作为模板, 删除与它轮廓面积相差较大的结果。\n"
        "点击后在弹窗中选择模板并调整允许偏差。"));
    m_btnRemoveNoise->setEnabled(false);

    // 与【删除干扰点】并排: 一键删掉全部黄色轮廓的质量差点。
    m_btnRemovePoor = new QPushButton(QStringLiteral("删除较差的点"), central);
    m_btnRemovePoor->setToolTip(QStringLiteral(
        "一次删除所有质量差的结果（图上画黄色轮廓的点）：\n"
        "包括拟合残差超限、椭度太大、以及非亚像素的项。\n"
        "没有质量差的结果时按钮为灰色。删除后可用【撤销】恢复。"));
    m_btnRemovePoor->setEnabled(false);

    QHBoxLayout* listHeader = new QHBoxLayout();
    listHeader->setContentsMargins(0, 0, 0, 0);
    listHeader->setSpacing(6);
    listHeader->addWidget(m_listTitle);
    listHeader->addStretch(1);
    listHeader->addWidget(m_btnRemoveNoise);
    listHeader->addWidget(m_btnRemovePoor);   // 紧挨着【删除干扰点】

    QWidget* resultPane = new QWidget(central);
    QVBoxLayout* rl = new QVBoxLayout(resultPane);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(4);
    rl->addLayout(listHeader);
    rl->addWidget(m_list, 1);

    // 撤销/导出放在列表下方, 避免标题行在最小宽度下过挤。
    m_btnUndo = new QPushButton(QStringLiteral("撤销"), central);
    m_btnUndo->setToolTip(QStringLiteral("撤销上一次删除操作。"));
    m_btnUndo->setEnabled(false);
    m_btnExport = new QPushButton(QStringLiteral("导出"), central);
    m_btnExport->setToolTip(QStringLiteral(
        "把当前剩余结果导出为 txt(ID、X/Y 坐标、面积、拟合质量等)。"));
    m_btnExport->setEnabled(false);

    QHBoxLayout* listActions = new QHBoxLayout();
    listActions->setContentsMargins(0, 0, 0, 0);
    listActions->setSpacing(6);
    listActions->addWidget(m_btnUndo);
    listActions->addWidget(m_btnExport);
    listActions->addStretch(1);
    rl->addLayout(listActions);

    // —— 右下: 参数设置 ——
    QWidget* paramPane = new QWidget(central);
    buildParamPane(paramPane);

    // 右侧竖向分割: 结果列表与参数栏高度需求互相挤, 由用户拖动比例。
    // childrenCollapsible=false 防止把某块拖成 0 高度后无法恢复。
    QSplitter* rightSplit = new QSplitter(Qt::Vertical, central);
    rightSplit->setChildrenCollapsible(false);
    rightSplit->addWidget(resultPane);
    rightSplit->addWidget(paramPane);
    rightSplit->setStretchFactor(0, 2);
    rightSplit->setStretchFactor(1, 3);
    rightSplit->setSizes({ 340, 480 });
    rightSplit->setMinimumWidth(340);

    QSplitter* split = new QSplitter(Qt::Horizontal, tabDetect);
    split->addWidget(m_view);
    split->addWidget(rightSplit);
    split->setChildrenCollapsible(false);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    split->setSizes({ 900, 360 });
    m_view->setMinimumWidth(360);
    root1->addWidget(split, 1);

    m_tabs->addTab(tabDetect, QStringLiteral("靶标识别"));

    // —— 第二页: 点云拼接(MergeMarks) ——
    // 先放占位控件, 首次切到本页时才 LoadLibrary(见 onTabChanged)。
    m_mergeMarksPage = new QWidget(m_tabs);
    QVBoxLayout* root2 = new QVBoxLayout(m_mergeMarksPage);
    root2->setContentsMargins(6, 6, 6, 6);
    m_mmPlaceholder = new QLabel(m_mergeMarksPage);
    m_mmPlaceholder->setAlignment(Qt::AlignCenter);
    m_mmPlaceholder->setWordWrap(true);
    m_mmPlaceholder->setText(QStringLiteral(
        "点云拼接模块尚未加载。\n\n"
        "切到本页会自动加载 MergeMarksLib.dll 及其 PCL/VTK 运行时(约 268 MB), 首次约需数秒。"));
    root2->addWidget(m_mmPlaceholder);
    m_tabs->addTab(m_mergeMarksPage, QStringLiteral("点云拼接"));

    connect(m_tabs, &QTabWidget::currentChanged, this, &testDetect::onTabChanged);

    connect(m_btnImport, &QPushButton::clicked, this, &testDetect::onImportImage);
    connect(m_btnCamera, &QPushButton::clicked, this, &testDetect::onCameraParams);
    connect(m_btnDetect, &QPushButton::clicked, this, &testDetect::onStartDetect);
    connect(m_btnDetectMarkers, &QPushButton::clicked, this, &testDetect::onStartDetectMarkers);
    connect(m_btnDetectAll, &QPushButton::clicked, this, &testDetect::onStartDetectAll);
    connect(m_btnRoi, &QPushButton::toggled, this, &testDetect::onToggleRoiMode);
    connect(m_btnClear, &QPushButton::clicked, this, &testDetect::onClearSelection);
    connect(m_btnFit, &QPushButton::clicked, this, &testDetect::onFitView);
    connect(m_list, &QListWidget::itemChanged, this, &testDetect::onItemChanged);
    connect(m_list, &QListWidget::itemClicked, this, &testDetect::onItemClicked);
    connect(m_btnRemoveNoise, &QPushButton::clicked, this, &testDetect::onRemoveNoise);
    connect(m_btnRemovePoor, &QPushButton::clicked, this, &testDetect::onRemovePoor);
    connect(m_btnUndo, &QPushButton::clicked, this, &testDetect::onUndo);
    connect(m_btnExport, &QPushButton::clicked, this, &testDetect::onExportResults);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::customContextMenuRequested,
        this, &testDetect::onListContextMenu);

    // —— 全屏 ——
    connect(m_btnFullScreen, &QPushButton::clicked, this, &testDetect::onToggleFullScreen);

    // 用 QShortcut: 它在焦点控件之前处理, 焦点在数字框里时 F11 仍有效。
    QShortcut* scFull = new QShortcut(QKeySequence(Qt::Key_F11), this);
    scFull->setContext(Qt::WindowShortcut);
    connect(scFull, &QShortcut::activated, this, &testDetect::onToggleFullScreen);

    // Esc 仅全屏时启用, 平时留给控件(如数字框撤销输入)。
    m_shortcutEsc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    m_shortcutEsc->setContext(Qt::WindowShortcut);
    m_shortcutEsc->setEnabled(false);
    connect(m_shortcutEsc, &QShortcut::activated, this, &testDetect::onToggleFullScreen);

    m_btnDetect->setEnabled(false);
    m_btnDetectMarkers->setEnabled(false);
    m_btnDetectAll->setEnabled(false);
    m_btnRoi->setEnabled(false);
    m_btnClear->setEnabled(false);

    // 状态栏右侧: 灰度读数(灰度化时才有内容) + 常驻缩放比例。
    m_pixelLabel = new QLabel(central);
    m_pixelLabel->setMinimumWidth(330);   // 预留宽度, 避免读数变化时状态栏左右跳
    statusBar()->addPermanentWidget(m_pixelLabel);
    m_zoomLabel = new QLabel(central);
    statusBar()->addPermanentWidget(m_zoomLabel);

    statusBar()->showMessage(QStringLiteral("请先导入图片"));
}


// ---------------------------------------------------------------- 相机标定

void testDetect::initCameraDefaults()
{
    m_activeCamera = factoryCameraProfile();
}

void testDetect::updateCameraButtonTooltip()
{
    if (!m_btnCamera)
        return;
    m_btnCamera->setToolTip(QStringLiteral(
        "设置靶标识别页的相机内参与畸变参数（命名保存、支持复制粘贴）。\n"
        "仅影响「靶标识别」页，不会作用于「点云拼接」页。\n"
        "当前生效：%1").arg(m_activeCamera.name));
}

QString testDetect::cameraConfigPath() const
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/cameras.ini");
}

bool testDetect::loadCameraProfiles(QVector<CameraProfile>& profiles, QString& activeName) const
{
    profiles.clear();
    activeName.clear();

    QSettings s(cameraConfigPath(), QSettings::IniFormat);
    s.setIniCodec("UTF-8");
    if (s.status() != QSettings::NoError)
        return false;

    activeName = s.value(QStringLiteral("activeCamera")).toString();
    const int n = s.beginReadArray(QStringLiteral("cameraProfiles"));
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        CameraProfile p;
        p.name = s.value(QStringLiteral("name")).toString().trimmed();
        const QVector<double> kn = parseNumberList(s.value(QStringLiteral("K")).toString());
        const QVector<double> dn = parseNumberList(s.value(QStringLiteral("D")).toString());
        if (p.name.isEmpty() || kn.size() != 9 || !validDistortionCount(dn.size()))
            continue;
        for (int j = 0; j < 9; ++j)
            p.K[j] = kn[j];
        p.D = dn;
        profiles.push_back(p);
    }
    s.endArray();
    return !profiles.isEmpty();
}

void testDetect::saveCameraProfiles(const QVector<CameraProfile>& profiles, const QString& activeName)
{
    QSettings s(cameraConfigPath(), QSettings::IniFormat);
    s.setIniCodec("UTF-8");
    s.beginWriteArray(QStringLiteral("cameraProfiles"), profiles.size());
    for (int i = 0; i < profiles.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue(QStringLiteral("name"), profiles[i].name);
        QStringList kv;
        for (double v : profiles[i].K)
            kv << QString::number(v, 'g', 17);
        s.setValue(QStringLiteral("K"), kv.join(QLatin1Char(',')));
        QStringList dv;
        for (double v : profiles[i].D)
            dv << QString::number(v, 'g', 17);
        s.setValue(QStringLiteral("D"), dv.join(QLatin1Char(',')));
    }
    s.endArray();
    s.setValue(QStringLiteral("activeCamera"), activeName);
    s.sync();
    if (s.status() != QSettings::NoError) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
            QStringLiteral("无法写入相机配置文件：\n%1\n请检查目录写权限。")
                .arg(cameraConfigPath()));
    }
}

void testDetect::applyCameraProfile(const CameraProfile& p)
{
    cv::Mat K = (cv::Mat_<double>(3, 3) <<
        p.K[0], p.K[1], p.K[2],
        p.K[3], p.K[4], p.K[5],
        p.K[6], p.K[7], p.K[8]);
    cv::Mat D(1, p.D.size(), CV_64F);
    for (int i = 0; i < p.D.size(); ++i)
        D.at<double>(0, i) = p.D[i];

    // 只写入靶标识别器; MergeMarks 面板(第二页)完全独立, 从不接收 K/D。
    m_detector.setCameraParameters(K, D);
    m_activeCamera = p;
    updateCameraButtonTooltip();

    if (!m_hasImage) {
        statusBar()->showMessage(QStringLiteral("相机参数已应用：%1").arg(p.name));
        return;
    }

    // 坐标系已经改变: 旧结果和旧显示图都作废, 重新去畸变(含黑白场增强)并提示重新识别。
    clearResults();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const cv::Mat disp = buildDisplayImage();
    QApplication::restoreOverrideCursor();
    showImage(disp);
    m_btnRoi->setChecked(false);
    statusBar()->showMessage(QStringLiteral("相机参数已应用：%1 —— 请重新识别").arg(p.name));
}

void testDetect::loadActiveCameraProfile()
{
    QVector<CameraProfile> profiles;
    QString activeName;
    loadCameraProfiles(profiles, activeName);

    if (!activeName.isEmpty()) {
        for (const CameraProfile& p : profiles) {
            if (p.name == activeName) {
                applyCameraProfile(p);
                // 启动时没有图片, 恢复"请先导入图片"的提示。
                statusBar()->showMessage(QStringLiteral("请先导入图片"));
                return;
            }
        }
    }
    updateCameraButtonTooltip();
}

void testDetect::onCameraParams()
{
    if (m_busy)
        return;
    // 相机参数只服务于「靶标识别」页; 「点云拼接」页有自己的数据链路,
    // 不读取内参/畸变。按钮本身也只存在于第一页, 这里是第二道保险。
    if (m_tabs && m_tabs->currentIndex() != 0)
        return;

    QVector<CameraProfile> profiles;
    QString activeName;
    loadCameraProfiles(profiles, activeName);

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("相机内参与畸变参数"));
    dlg.setMinimumWidth(600);

    QVBoxLayout* lay = new QVBoxLayout(&dlg);
    lay->setSpacing(8);

    QLabel* intro = new QLabel(QStringLiteral(
        "从标定软件复制内参和畸变系数，粘贴到下面即可。\n"
        "内参可粘贴 3×3 矩阵，或 fx, fy, cx, cy 四个数。\n"
        "畸变顺序：k1, k2, p1, p2, k3[, k4, k5, k6 ...]，与 OpenCV calibrateCamera 一致。\n"
        "配置按名称保存，下次启动自动使用最后应用的那一份。\n"
        "作用范围：仅「靶标识别」页的识别与叠加显示；「点云拼接」页不使用这些参数。"), &dlg);
    intro->setWordWrap(true);
    lay->addWidget(intro);

    lay->addWidget(new QLabel(QStringLiteral("当前生效：%1").arg(m_activeCamera.name), &dlg));

    // 已保存配置
    QHBoxLayout* profRow = new QHBoxLayout();
    profRow->addWidget(new QLabel(QStringLiteral("已保存配置："), &dlg));
    QComboBox* combo = new QComboBox(&dlg);
    combo->addItem(QStringLiteral("（新建配置）"));
    for (const CameraProfile& p : profiles)
        combo->addItem(p.name);
    int activeIdx = 0;
    for (int i = 0; i < profiles.size(); ++i) {
        if (profiles[i].name == activeName) {
            activeIdx = i + 1;
            break;
        }
    }
    combo->setCurrentIndex(activeIdx);
    profRow->addWidget(combo, 1);
    QPushButton* btnLoad = new QPushButton(QStringLiteral("载入"), &dlg);
    QPushButton* btnDel = new QPushButton(QStringLiteral("删除"), &dlg);
    profRow->addWidget(btnLoad);
    profRow->addWidget(btnDel);
    lay->addLayout(profRow);

    // 配置名称
    QHBoxLayout* nameRow = new QHBoxLayout();
    nameRow->addWidget(new QLabel(QStringLiteral("配置名称："), &dlg));
    QLineEdit* nameEdit = new QLineEdit(&dlg);
    nameEdit->setPlaceholderText(QStringLiteral("例如：2026-10-20 大华 2MP"));
    nameEdit->setText(activeName.isEmpty()
        ? QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd"))
              + QStringLiteral(" 相机")
        : m_activeCamera.name);
    nameRow->addWidget(nameEdit, 1);
    lay->addLayout(nameRow);

    // 内参
    lay->addWidget(new QLabel(QStringLiteral(
        "内参矩阵 K（3×3，或 fx, fy, cx, cy；可粘贴带括号/逗号的文本）："), &dlg));
    QPlainTextEdit* kEdit = new QPlainTextEdit(&dlg);
    kEdit->setFixedHeight(78);
    kEdit->setPlaceholderText(QStringLiteral(
        "6633.54 0 1544.94\n0 6633.48 1091.96\n0 0 1"));
    kEdit->setPlainText(formatK(m_activeCamera.K));
    lay->addWidget(kEdit);

    QHBoxLayout* kRow = new QHBoxLayout();
    QPushButton* kPaste = new QPushButton(QStringLiteral("从剪贴板粘贴"), &dlg);
    QPushButton* kCopy = new QPushButton(QStringLiteral("复制"), &dlg);
    kRow->addWidget(kPaste);
    kRow->addWidget(kCopy);
    kRow->addStretch(1);
    lay->addLayout(kRow);

    // 畸变
    lay->addWidget(new QLabel(QStringLiteral(
        "畸变系数 D（k1, k2, p1, p2, k3 ...，留空 = 无畸变）："), &dlg));
    QLineEdit* dEdit = new QLineEdit(&dlg);
    dEdit->setPlaceholderText(QStringLiteral(
        "0.047895, -0.226498, 0.000347139, 0.000879732, 0"));
    dEdit->setText(formatD(m_activeCamera.D));
    lay->addWidget(dEdit);

    QHBoxLayout* dRow = new QHBoxLayout();
    QPushButton* dPaste = new QPushButton(QStringLiteral("从剪贴板粘贴"), &dlg);
    QPushButton* dCopy = new QPushButton(QStringLiteral("复制"), &dlg);
    dRow->addWidget(dPaste);
    dRow->addWidget(dCopy);
    dRow->addStretch(1);
    lay->addLayout(dRow);

    QLabel* status = new QLabel(&dlg);
    status->setWordWrap(true);
    lay->addWidget(status);

    QDialogButtonBox* bb = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    if (QPushButton* b = bb->button(QDialogButtonBox::Ok))
        b->setText(QStringLiteral("应用并保存"));
    if (QPushButton* b = bb->button(QDialogButtonBox::Cancel))
        b->setText(QStringLiteral("取消"));
    lay->addWidget(bb);

    // 实时校验: 格式不对时禁用"应用并保存"。
    auto validate = [&]() -> bool {
        double k[9] = { 0 };
        QVector<double> d;
        QString err;
        const bool ok = parseCameraText(kEdit->toPlainText(), dEdit->text(), k, d, err);
        status->setText(ok
            ? QStringLiteral("格式正确：fx=%1, fy=%2, cx=%3, cy=%4，畸变 %5 项")
                  .arg(k[0], 0, 'g', 8).arg(k[4], 0, 'g', 8)
                  .arg(k[2], 0, 'g', 8).arg(k[5], 0, 'g', 8)
                  .arg(d.size())
            : err);
        status->setStyleSheet(ok ? QStringLiteral("color:#3aff8a;")
                                 : QStringLiteral("color:#ffc35c;"));
        if (QPushButton* b = bb->button(QDialogButtonBox::Ok))
            b->setEnabled(ok);
        return ok;
    };

    connect(kEdit, &QPlainTextEdit::textChanged, &dlg, [&]() { validate(); });
    connect(dEdit, &QLineEdit::textChanged, &dlg, [&](const QString&) { validate(); });

    connect(kPaste, &QPushButton::clicked, &dlg, [&]() {
        kEdit->setPlainText(QApplication::clipboard()->text());
    });
    connect(kCopy, &QPushButton::clicked, &dlg, [&]() {
        QApplication::clipboard()->setText(kEdit->toPlainText());
    });
    connect(dPaste, &QPushButton::clicked, &dlg, [&]() {
        dEdit->setText(QApplication::clipboard()->text().trimmed());
    });
    connect(dCopy, &QPushButton::clicked, &dlg, [&]() {
        QApplication::clipboard()->setText(dEdit->text());
    });

    connect(btnLoad, &QPushButton::clicked, &dlg, [&]() {
        const int idx = combo->currentIndex() - 1;
        if (idx < 0 || idx >= profiles.size())
            return;
        const CameraProfile& p = profiles[idx];
        nameEdit->setText(p.name);
        kEdit->setPlainText(formatK(p.K));
        dEdit->setText(formatD(p.D));
    });

    connect(btnDel, &QPushButton::clicked, &dlg, [&]() {
        const int idx = combo->currentIndex() - 1;
        if (idx < 0 || idx >= profiles.size())
            return;
        if (profiles[idx].name == m_activeCamera.name) {
            QMessageBox::information(&dlg, QStringLiteral("无法删除"),
                QStringLiteral("这是当前生效的配置。请先应用另一份配置，再删除它。"));
            return;
        }
        if (QMessageBox::question(&dlg, QStringLiteral("删除配置"),
                QStringLiteral("确定删除配置“%1”吗？").arg(profiles[idx].name))
            != QMessageBox::Yes)
            return;
        profiles.remove(idx);
        saveCameraProfiles(profiles, activeName);
        combo->clear();
        combo->addItem(QStringLiteral("（新建配置）"));
        for (const CameraProfile& p : profiles)
            combo->addItem(p.name);
        combo->setCurrentIndex(0);
    });

    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(bb, &QDialogButtonBox::accepted, &dlg, [&]() {
        double k[9] = { 0 };
        QVector<double> d;
        QString err;
        if (!parseCameraText(kEdit->toPlainText(), dEdit->text(), k, d, err)) {
            QMessageBox::warning(&dlg, QStringLiteral("格式错误"), err);
            return;
        }
        const QString name = nameEdit->text().trimmed();
        if (name.isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("缺少名称"),
                QStringLiteral("请给这份标定起一个名字，例如“2026-10-20 大华 2MP”。"));
            return;
        }

        CameraProfile p;
        p.name = name;
        for (int i = 0; i < 9; ++i)
            p.K[i] = k[i];
        p.D = d;

        // 同名更新，否则追加。
        bool found = false;
        for (CameraProfile& existing : profiles) {
            if (existing.name == name) {
                existing = p;
                found = true;
                break;
            }
        }
        if (!found)
            profiles.push_back(p);

        saveCameraProfiles(profiles, name);
        applyCameraProfile(p);
        dlg.accept();
    });

    validate();
    dlg.exec();
}


// ---------------------------------------------------------------- 参数设置栏
// UI 映射 DetectorParams(TargetDetector.h)。标签用短名(≤6 字), 详细说明放 tooltip,
// 否则 340px 宽会换行、行高失控。
// 生效时机: 编辑 → 【应用参数】→ setParams + 写 settings.ini。
// 不做实时生效: 单次识别需数秒, 拖动数字框会卡界面, 也说不清结果对应哪套参数。

namespace {

// 提示色, 与 theme.qss 的次要文字色/警示色一致(动态状态只能代码设色)。
const char* const kHintColorIdle = "#8b9bb0";
const char* const kHintColorDirty = "#ffc35c";

// 建卡片式分组(QGroupBox 的底色/圆角由 theme.qss 提供), 返回内部表单布局。
QGroupBox* makeParamGroup(QWidget* parent, const QString& title, QFormLayout** formOut)
{
    QGroupBox* g = new QGroupBox(title, parent);
    QFormLayout* f = new QFormLayout(g);
    f->setContentsMargins(8, 6, 8, 6);
    f->setSpacing(4);
    f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // 字段保持自然宽度: 窄栏里拉伸填满会让数字框显得松散。
    f->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    f->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    if (formOut)
        *formOut = f;
    return g;
}

// 三种参数控件统一取值为 double, 上层无需区分类型。
double widgetValue(const QWidget* w)
{
    if (const QCheckBox* cb = qobject_cast<const QCheckBox*>(w))
        return cb->isChecked() ? 1.0 : 0.0;
    if (const QDoubleSpinBox* dsb = qobject_cast<const QDoubleSpinBox*>(w))
        return dsb->value();
    if (const QSpinBox* sb = qobject_cast<const QSpinBox*>(w))
        return (double)sb->value();
    return 0.0;
}

void setWidgetValue(QWidget* w, double v)
{
    if (QCheckBox* cb = qobject_cast<QCheckBox*>(w))
        cb->setChecked(v != 0.0);
    else if (QDoubleSpinBox* dsb = qobject_cast<QDoubleSpinBox*>(w))
        dsb->setValue(v);
    else if (QSpinBox* sb = qobject_cast<QSpinBox*>(w))
        sb->setValue((int)qRound(v));
}

// ---------------------------------------------------------------- 参数权限
// 默认用户只看到 kBasic 中的 9 项, 其余需密码解锁。
// 按 key 判断而非控件指针, 三个 addXxxField 无需改动;
// 新增参数默认归高级, 要开放给默认用户就把 key 加进 kBasic。
bool isBasicParamKey(const QString& key)
{
    static const QSet<QString> kBasic = {
        QStringLiteral("useOtsu"),            // 大津法
        QStringLiteral("fixedThreshold"),     // 固定阈值
        QStringLiteral("erodeKernel"),        // 腐蚀核
        QStringLiteral("bigBlobAreaRatio"),   // 大块白斑
        QStringLiteral("brightThr"),          // 亮像素阈值
        QStringLiteral("minRingDirHits"),     // 方向命中数
        QStringLiteral("minApproxVertices"),  // 最少顶点数
        QStringLiteral("minCandidateRadius"), // 候选最小半径
        QStringLiteral("boundaryThreshold")   // 边界阈值(灰度边界, 0 = 关闭)
    };
    return kBasic.contains(key);
}

// 密码校验: 只保存 FNV-1a 64 散列, 源码无明文。
// ⚠ 这不是安全边界, 仅防止客户误改内部参数; 换密码时重算此常量。
bool verifyAdvancedPassword(const QString& s)
{
    quint64 h = 0xcbf29ce484222325ULL;
    const QByteArray bytes = s.toUtf8();
    for (char c : bytes) {
        h ^= static_cast<quint8>(c);
        h *= 0x100000001b3ULL;
    }
    return h == 0x79e7d2dc263df9b8ULL;   // 高级参数密码的散列(明文只在项目所有者手里)
}

} // namespace

// 三个 addXxxField 共用同一套逻辑: 建控件、写 tooltip、注册到 m_fields;
// read/write 通过成员指针生成, 参数表里不必写字段名。
QCheckBox* testDetect::addBoolField(QFormLayout* form, const QString& key, const QString& label,
    const QString& tip, bool DetectorParams::* member)
{
    QCheckBox* cb = new QCheckBox(form->parentWidget());
    cb->setToolTip(tip);
    form->addRow(label, cb);
    connect(cb, &QCheckBox::toggled, this, &testDetect::onParamEdited);

    ParamField f;
    f.key = key;
    f.w = cb;
    f.group = qobject_cast<QGroupBox*>(form->parentWidget());
    f.basic = isBasicParamKey(key);
    f.read = [member](const DetectorParams& p) -> double { return (p.*member) ? 1.0 : 0.0; };
    f.write = [member](DetectorParams& p, double v) { p.*member = (v != 0.0); };
    m_fields.push_back(f);
    return cb;
}

QSpinBox* testDetect::addIntField(QFormLayout* form, const QString& key, const QString& label,
    const QString& tip, int lo, int hi, int DetectorParams::* member)
{
    QSpinBox* sb = new QSpinBox(form->parentWidget());
    sb->setRange(lo, hi);
    sb->setAccelerated(true);            // 滚轮/长按加速
    sb->setKeyboardTracking(false);      // 输入落定才发信号, 避免每敲一键就算改动
    sb->setFixedWidth(96);
    sb->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sb->setToolTip(tip);
    form->addRow(label, sb);
    connect(sb, QOverload<int>::of(&QSpinBox::valueChanged), this, &testDetect::onParamEdited);

    ParamField f;
    f.key = key;
    f.w = sb;
    f.group = qobject_cast<QGroupBox*>(form->parentWidget());
    f.basic = isBasicParamKey(key);
    f.read = [member](const DetectorParams& p) -> double { return (double)(p.*member); };
    f.write = [member](DetectorParams& p, double v) { p.*member = (int)qRound(v); };
    m_fields.push_back(f);
    return sb;
}

QDoubleSpinBox* testDetect::addDoubleField(QFormLayout* form, const QString& key,
    const QString& label, const QString& tip, double lo, double hi, int decimals,
    double DetectorParams::* member)
{
    QDoubleSpinBox* dsb = new QDoubleSpinBox(form->parentWidget());
    dsb->setRange(lo, hi);
    dsb->setDecimals(decimals);
    // 步长取 max(最小有效位, 值域/200): 小量级可细调, 大范围不至于按到手软。
    dsb->setSingleStep(qMax(std::pow(10.0, -decimals), (hi - lo) / 200.0));
    dsb->setAccelerated(true);
    dsb->setKeyboardTracking(false);
    dsb->setFixedWidth(96);
    dsb->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    dsb->setToolTip(tip);
    form->addRow(label, dsb);
    connect(dsb, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
        this, &testDetect::onParamEdited);

    ParamField f;
    f.key = key;
    f.w = dsb;
    f.group = qobject_cast<QGroupBox*>(form->parentWidget());
    f.basic = isBasicParamKey(key);
    f.read = [member](const DetectorParams& p) -> double { return (double)(p.*member); };
    f.write = [member](DetectorParams& p, double v) { p.*member = v; };
    m_fields.push_back(f);
    return dsb;
}

void testDetect::buildParamPane(QWidget* paramPane)
{
    m_paramPane = paramPane;

    QVBoxLayout* v = new QVBoxLayout(paramPane);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    QLabel* title = new QLabel(QStringLiteral("参数设置"), paramPane);
    title->setObjectName(QStringLiteral("sectionTitle"));
    v->addWidget(title);

    // 参数总高超过一屏: 用滚动区, 窗口变矮时出现滚动条而不是压扁行高。
    m_paramScroll = new QScrollArea(paramPane);
    m_paramScroll->setWidgetResizable(true);
    m_paramScroll->setFrameShape(QFrame::NoFrame);
    m_paramScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 视口透明: 视口是独立子控件, 给 QScrollArea 写 background 改不到它。
    if (m_paramScroll->viewport())
        m_paramScroll->viewport()->setAutoFillBackground(false);

    QWidget* inner = new QWidget;
    inner->setObjectName(QStringLiteral("paramPane"));   // 供 QSS 的 ID 选择器设透明背景
    QVBoxLayout* il = new QVBoxLayout(inner);
    il->setContentsMargins(2, 2, 2, 2);
    il->setSpacing(8);

    QFormLayout* fPre = nullptr;
    QFormLayout* fGray = nullptr;
    QFormLayout* fCirc = nullptr;
    QFormLayout* fSub = nullptr;
    QFormLayout* fDec = nullptr;
    QFormLayout* fEnh = nullptr;
    QFormLayout* fFilter = nullptr;
    il->addWidget(makeParamGroup(inner, QStringLiteral("预处理"), &fPre));
    // 灰度与阈值: 唯一一个始终可见的宿主侧分组(灰度化 + 边界阈值)。
    m_groupGray = makeParamGroup(inner, QStringLiteral("灰度与阈值"), &fGray);
    il->addWidget(m_groupGray);
    il->addWidget(makeParamGroup(inner, QStringLiteral("轮廓筛选"), &fCirc));
    m_groupSubpixel = makeParamGroup(inner, QStringLiteral("亚像素精化"), &fSub);
    il->addWidget(m_groupSubpixel);
    // 这两组(结果过滤里的项也是)全部是高级参数, 锁定时整组隐藏。
    m_groupDecode = makeParamGroup(inner, QStringLiteral("解码与去重"), &fDec);
    il->addWidget(m_groupDecode);
    // 宿主侧参数: 不进 DetectorParams/DLL, 单独两组放在最后。
    m_groupEnhance = makeParamGroup(inner, QStringLiteral("图像增强"), &fEnh);
    il->addWidget(m_groupEnhance);
    m_groupResultFilter = makeParamGroup(inner, QStringLiteral("结果过滤"), &fFilter);
    il->addWidget(m_groupResultFilter);

    // ---- 预处理 ----
    m_chkOtsu = addBoolField(fPre, QStringLiteral("useOtsu"), QStringLiteral("大津法"),
        QStringLiteral("是否用大津法自动求二值化阈值(默认开)。\n"
                       "取消后改用下面的「固定阈值」, 适合整体偏暗/偏亮、\n"
                       "大津法切不干净导致靶标与背景连成一片的图。\n"
                       "判断切得对不对: 中心圆盘应是一整块白, 背景基本全黑。\n"
                       "二值化一变, 测出的半径 R 也会变, 下游所有 ×R 采样位置跟着移动。"),
        &DetectorParams::useOtsu);
    m_spinFixedThr = addIntField(fPre, QStringLiteral("fixedThreshold"), QStringLiteral("固定阈值"),
        QStringLiteral("二值化灰度阈值, 仅当「大津法」取消勾选时生效(灰显时改了也没用)。\n"
                       "像素灰度 > 该值记为白: 调低 → 更多像素算白(前景变多, 易粘连);\n"
                       "调高 → 更少(靶标可能被切碎)。典型范围 60~180。\n"
                       "方向: 调到中心圆盘是完整白块、背景基本全黑为止。"),
        1, 254, &DetectorParams::fixedThreshold);
    addIntField(fPre, QStringLiteral("erodeKernel"), QStringLiteral("腐蚀核"),
        QStringLiteral("腐蚀操作的结构元边长(奇数, 默认 5; 偶数会自动夹成奇数, 范围 1~21)。\n"
                       "调大 → 白色区域被啃得更瘦, 细窄的反光条纹/毛刺先消失, 误检减少;\n"
                       "但过大也会把编码块啃断、把中心盘啃小(半径 R 变小)。\n"
                       "小目标请调小: 半径 ≤8px 的白点经 3x3 腐蚀后轮廓会比真实边缘小一圈,\n"
                       "亚像素拟合也会跟着缩进去；调到 1 表示不腐蚀, 轮廓能更贴合白点。\n"
                       "代价是反光条纹/相邻白斑更容易连成一片, 误检可能增多, 按图取舍。"),
        1, 21, &DetectorParams::erodeKernel);
    addDoubleField(fPre, QStringLiteral("bigBlobAreaRatio"), QStringLiteral("大块白斑"),
        QStringLiteral("剔除大面积白色连通域的面积占比阈值(默认 0.02 = 全图 2%)。\n"
                       "连通域面积 ≥ 该比例 × 全图面积就整个删掉(只删它自己的像素)。\n"
                       "工件反光加工面在二值化后常整片连成一大块, 先删掉可明显提速。\n"
                       "调小 → 更激进(删得更多); 调大 → 更保守; 0 = 完全不删。\n"
                       "靶标与反光面粘连、被一起删掉时, 先填 0 验证是不是这个原因。"),
        0.0, 0.5, 3, &DetectorParams::bigBlobAreaRatio);

    // ---- 灰度与阈值 ----
    // 灰度化是宿主侧开关: 勾选后显示图和识别输入都换成单通道灰度图。
    m_chkGrayscale = new QCheckBox(m_groupGray);
    m_chkGrayscale->setToolTip(QStringLiteral(
        "勾选后, 显示图和送进识别器的输入都先转成单通道灰度图(BGR2GRAY),\n"
        "后续预处理、二值化、轮廓与亚像素拟合全部在这张灰度图上进行。\n"
        "不勾选时与以前完全一致(识别器内部本来也会转灰度, 只是显示仍是彩色)。\n"
        "灰度化打开后, 鼠标在图像上移动时状态栏右侧会实时显示该像素的灰度值,\n"
        "并可对照「边界阈值」判断该点算边界还是背景, 方便先读数再定阈值。\n"
        "改完记得点「应用参数」; 它不改变任何像素坐标, 识别结果叠加不变。"));
    fGray->addRow(QStringLiteral("灰度化"), m_chkGrayscale);
    connect(m_chkGrayscale, &QCheckBox::toggled, this, &testDetect::onParamEdited);

    // 边界阈值: DLL 侧的 DetectorParams 字段(0 = 关闭, 用局部大津法)。
    addIntField(fGray, QStringLiteral("boundaryThreshold"), QStringLiteral("边界阈值"),
        QStringLiteral("拟合轮廓要包含的灰度边界(0~255, 0 = 关闭)。\n"
                       "0: 保持原行为, 在候选周围用局部大津法自动找白斑边界。\n"
                       ">0: 灰度 > 该值的像素算目标(边界), 用它重新取未腐蚀灰度图上的\n"
                       "白斑轮廓; 拟合出的椭圆必须包住这条灰度等值线 —— 亚像素椭圆比它\n"
                       "小时按面积比放大, 因此最终轮廓会向外扩到这条边界。\n"
                       "用法: 先勾选「灰度化」, 把鼠标移到目标边缘上读出灰度值(状态栏),\n"
                       "在背景灰度与目标灰度之间取一个值填进来, 再点「应用参数」并重新识别。\n"
                       "调低 → 边界外扩(轮廓变大), 过低会把周围反光/粘连算进来;\n"
                       "调高 → 边界内缩(轮廓变小), 过高会切掉目标边缘。\n"
                       "它只影响轮廓拟合的边界, 不改变预处理里的「大津法/固定阈值」二值化。"),
        0, 255, &DetectorParams::boundaryThreshold);

    // ---- 轮廓筛选 ----
    addDoubleField(fCirc, QStringLiteral("maxAspect"), QStringLiteral("长短轴比"),
        QStringLiteral("候选轮廓拟合椭圆的长短轴比上限(默认 1.9)。中心圆盘接近正圆 ≈1,\n"
                       "细长的跑道形编码块比值更大。调小(1.3~1.6) → 候选更少更干净,\n"
                       "但大倾角靶标可能被误杀; 调大 → 收进更多圆润的编码块\n"
                       "(靠后面的编码带判据和解码再筛)。误检多时优先调小这一项。"),
        1.0, 10.0, 2, &DetectorParams::maxAspect);
    addDoubleField(fCirc, QStringLiteral("minDiscFill"), QStringLiteral("圆盘亮占比"),
        QStringLiteral("候选轮廓最小外接圆内“亮”像素的占比下限(默认 0.7)。\n"
                       "实心中心圆盘的外接圆几乎全白(≈1); 编码块/弧段的外接圆\n"
                       "远大于它的白色实体, 占比明显偏低。\n"
                       "调高(0.8~0.9) → 更严, 专治把弧形反光/编码块当中心; 调太低等于不筛。\n"
                       "它用的是未二值化的灰度图 + 「亮像素阈值」判定。"),
        0.0, 1.0, 2, &DetectorParams::minDiscFill);
    addIntField(fCirc, QStringLiteral("brightThr"), QStringLiteral("亮像素阈值"),
        QStringLiteral("判断像素“亮”的灰度阈值(默认 120), 被三处复用: 圆盘亮占比、\n"
                       "编码带环绕判定(在原始灰度图上采样)、去重时的黑白评分。\n"
                       "偏暗的图调低(80~100), 过曝/反光强调高(150~180)。\n"
                       "注意它不是二值化阈值 —— 二值化由「大津法/固定阈值」决定。"),
        0, 255, &DetectorParams::brightThr);
    addDoubleField(fCirc, QStringLiteral("minRing"), QStringLiteral("编码带内沿"),
        QStringLiteral("编码环跑道块所在径向带的内沿, 单位是中心圆盘半径 R 的倍数\n"
                       "(默认 2.3)。用比例而不是绝对像素, 相机远近变化时自动等比。\n"
                       "调到 1 附近会从圆盘边缘开始采, 那里本身就是亮的, 24 个方向\n"
                       "会全部命中, 这一关就形同虚设。"),
        1.0, 10.0, 2, &DetectorParams::minRing);
    addDoubleField(fCirc, QStringLiteral("maxRing"), QStringLiteral("编码带外沿"),
        QStringLiteral("编码环径向带的外沿 ×R(默认 4.2)。\n"
                       "注意它和「解码与去重」组里的编码带外沿(默认 4.1)不是同一个数:\n"
                       "这里判“是否被编码带环绕”, 那里是解码时的采样范围, 别改成一样。\n"
                       "调太大会采到旁边靶标或背景反光, 产生误检。"),
        1.0, 10.0, 2, &DetectorParams::maxRing);
    addIntField(fCirc, QStringLiteral("minRingDirHits"), QStringLiteral("方向命中数"),
        QStringLiteral("环上共采样 24 个方向, 至少这么多个方向看到编码块, 才认为该候选\n"
                       "被编码带环绕(默认 4)。调高(8~12) → 只认真被编码环包围的圆盘,\n"
                       "误检明显减少; 调低(2~3) → 编码块缺失/遮挡多时用, 但会放进假候选。\n"
                       "这是“漏检 vs 误检”的主要旋钮之一。"),
        0, 24, &DetectorParams::minRingDirHits);
    addDoubleField(fCirc, QStringLiteral("approxEpsRatio"), QStringLiteral("逼近精度"),
        QStringLiteral("多边形逼近的精度, 单位是轮廓周长的倍数(默认 0.03)。\n"
                       "只用来数顶点个数(配合「最少顶点数」), 不参与椭圆拟合。\n"
                       "调大 → 逼近更粗、顶点更少, 相当于把「最少顶点数」这道门槛收紧。"),
        0.0005, 0.5, 4, &DetectorParams::approxEpsRatio);
    addIntField(fCirc, QStringLiteral("minApproxVertices"), QStringLiteral("最少顶点数"),
        QStringLiteral("多边形逼近后顶点数少于该值的轮廓整个丢弃(默认 5)。\n"
                       "小靶标(远距离)顶点本来就少, 识别不到远处靶标时可以调低到 3~4; \n"
                       "调高可挡掉二值化毛刺形成的碎轮廓。"),
        3, 1000, &DetectorParams::minApproxVertices);
    addDoubleField(fCirc, QStringLiteral("minCandidateRadius"), QStringLiteral("候选最小半径"),
        QStringLiteral("候选的最小半径(像素, 默认 1.0)。拟合椭圆短半轴或最小外接圆\n"
                       "半径低于它就直接丢弃, 用来挡掉二值化毛刺。\n"
                       "常见量级 0.5~3, 不要填太大, 否则远距离小靶标会被整批丢掉。"),
        0.0, 200.0, 2, &DetectorParams::minCandidateRadius);

    // ---- 亚像素精化 ----
    m_chkSubpixel = addBoolField(fSub, QStringLiteral("subpixelEnable"), QStringLiteral("启用亚像素"),
        QStringLiteral("是否做 Zernike 亚像素精化(默认开)。\n"
                       "取消后中心直接用整数轮廓的拟合结果, 精度掉到 ~0.3px 量级,\n"
                       "结果列表里会全部标成橙色的「(!)非亚像素」——\n"
                       "可以用它对照“精化到底带来了多少提升”, 也可以用来提速。\n"
                       "取消后本组其余参数灰显, 算法也不再读取。"),
        &DetectorParams::subpixelEnable);
    addDoubleField(fSub, QStringLiteral("refineBandRatio"), QStringLiteral("选点带宽比"),
        QStringLiteral("挑选亚像素边缘点的带宽半宽, 单位是半径 R 的倍数(默认 0.18)。\n"
                       "实际半宽 = max(「带宽下限px」, 该值 × R), 圆的有效带宽是它的 2 倍。\n"
                       "它同时决定椭圆拟合的归一化带宽(再被下面两个椭圆带宽参数夹住)。\n"
                       "调小 → 只取最贴近边缘的点, 拟合更干净; 调太小会点数不足而精化失败\n"
                       "(结果变橙色)。这是最该先试的一个参数。"),
        0.01, 1.0, 3, &DetectorParams::refineBandRatio);
    addDoubleField(fSub, QStringLiteral("refineBandMin"), QStringLiteral("带宽下限px"),
        QStringLiteral("上面那个带宽的下限(像素, 默认 0.8)。靶标很小(R 只有几个像素)时\n"
                       "0.18R 会小于 1 个像素, 靠这个下限保证仍然能取到一条可拟合的带。\n"
                       "小靶标结果全是橙色时可降到 0.5。"),
        0.0, 200.0, 2, &DetectorParams::refineBandMin);
    addDoubleField(fSub, QStringLiteral("roiScale"), QStringLiteral("ROI 半径倍数"),
        QStringLiteral("Zernike 扫描区域的半径 ÷ R(默认 1.5), 实际半边还会再加 8px。\n"
                       "必须盖得住“选点带最远处”(约 1.2R)再加 7×7 模板的边距。\n"
                       "调小可以更快, 但低于约 1.25 就开始漏边缘点(点数下降、中心偏移);\n"
                       "若调大了「椭圆带宽上限」, 这里也要相应调大。"),
        1.0, 8.0, 2, &DetectorParams::roiScale);
    addDoubleField(fSub, QStringLiteral("zernikeKMin"), QStringLiteral("阶跃强度初筛"),
        QStringLiteral("Zernike 边缘点的阶跃强度初筛下限(默认 5.0), 是第一道、最便宜的门槛。\n"
                       "低于它的像素直接不算边缘点。调高 → 只保留对比强烈的边缘, 速度更快,\n"
                       "但弱边界的靶标可能整个从候选里消失(不是变橙, 是根本没进来);\n"
                       "调低(2~3) → 弱对比/模糊图能用, 代价是变慢、易被反光边缘带偏。"),
        0.0, 10000.0, 1, &DetectorParams::zernikeKMin);
    m_chkOtsuK = addBoolField(fSub, QStringLiteral("useOtsuK"), QStringLiteral("k 阈值用大津法"),
        QStringLiteral("是否用大津法自适应地在本候选附近的边缘点上求阶跃强度阈值(默认开)。\n"
                       "注意是“局部”统计: 全图统计会被反光面的海量强边缘带偏, 反而把\n"
                       "真靶标的弱边缘一起拒掉。取消后改用下面的「固定 k 阈值」。\n"
                       "它是第二道门槛: 「阶跃强度初筛」在前、更粗, 这一道决定谁真正参与拟合。"),
        &DetectorParams::useOtsuK);
    m_spinFixedKt = addDoubleField(fSub, QStringLiteral("fixedKt"), QStringLiteral("固定 k 阈值"),
        QStringLiteral("阶跃强度阈值, 仅当「k 阈值用大津法」取消勾选时生效(灰显时改了也没用)。\n"
                       "参考值: 大津法在这批样本上算出来通常在 20~60; 手动可先试 10~30。\n"
                       "调低 → 更多边缘点参与拟合(可能更准, 也可能被噪点拉偏)。\n"
                       "注意它与「阶跃强度初筛」是两级门槛, 两道都调低才是真正放宽。"),
        0.0, 10000.0, 1, &DetectorParams::fixedKt);
    addIntField(fSub, QStringLiteral("minFitPoints"), QStringLiteral("最少拟合点数"),
        QStringLiteral("参与最小二乘拟合的最少亚像素点数(默认 6), 圆拟合和椭圆拟合都用它。\n"
                       "少于它就判定该次精化失败: 圆失败退回整数轮廓(变橙); 椭圆失败则只留\n"
                       "圆的结果 —— 倾斜靶标会因此走“纯圆”解码路径, 容易解不出 ID。\n"
                       "点太少时拟合没有意义, 不要调到 3 以下。"),
        3, 100000, &DetectorParams::minFitPoints);
    addDoubleField(fSub, QStringLiteral("ellipseBandMin"), QStringLiteral("椭圆带宽下限"),
        QStringLiteral("椭圆精化的归一化带宽下限(默认 0.04)。椭圆取点的归一化半径带是\n"
                       "[1-band, 1+band], band 由「选点带宽比」算出后被这两个界限夹住。\n"
                       "带太窄会挑不出点, 不要低于 0.03。"),
        0.005, 0.5, 3, &DetectorParams::ellipseBandMin);
    addDoubleField(fSub, QStringLiteral("ellipseBandMax"), QStringLiteral("椭圆带宽上限"),
        QStringLiteral("椭圆精化的归一化带宽上限(默认 0.20)。带太宽会把非边缘点混进来。\n"
                       "大倾角靶标可以提到 0.25~0.30 让椭圆更容易拟合成功;\n"
                       "注意最远点会到 (1+band)R, 调大后「ROI 半径倍数」也要跟着调大。"),
        0.01, 1.0, 3, &DetectorParams::ellipseBandMax);
    addDoubleField(fSub, QStringLiteral("ellipseBandShrink"), QStringLiteral("二次收缩比"),
        QStringLiteral("第二次重拟合时带宽的收缩比(默认 0.6), 用来剔掉毛刺。\n"
                       "调小 → 更狠地剔离群点, 但可能点数不足; 调大(1.0) = 不做二次精化。\n"
                       "拟合用的带和算残差 RMS 用的带都是收缩后的同一条带 —— \n"
                       "改成别的数, 前后的 RMS 就不能直接比了。"),
        0.05, 1.0, 2, &DetectorParams::ellipseBandShrink);
    addDoubleField(fSub, QStringLiteral("minAxisRatio"), QStringLiteral("短长轴比下限"),
        QStringLiteral("拟合椭圆的短轴/长轴下限(默认 0.2)。比值更小的“椭圆”多半是\n"
                       "拟合到了一条弧段或一串噪点, 直接判不可信。\n"
                       "调高(0.3~0.5) → 更严, 但大倾角靶标可能判椭圆精化失败;\n"
                       "椭圆失败会让该靶标退回“纯圆”解码, 倾斜时容易解不出 ID。"),
        0.0, 1.0, 2, &DetectorParams::minAxisRatio);

    // ---- 解码与去重 ----
    addDoubleField(fDec, QStringLiteral("codeRingInner"), QStringLiteral("编码带内沿"),
        QStringLiteral("解码采样时编码环径向带的内沿 ×R(默认 2.3)。\n"
                       "它与「轮廓筛选」组里的编码带内沿是两处独立的数, 改一处不影响另一处。\n"
                       "调到 1.x 会从圆盘边缘开始采, 边缘本身就是亮的, 容易把所有位读成 1。"),
        1.0, 10.0, 2, &DetectorParams::codeRingInner);
    addDoubleField(fDec, QStringLiteral("codeRingOuter"), QStringLiteral("编码带外沿"),
        QStringLiteral("解码采样时编码环径向带的外沿 ×R(默认 4.1)。\n"
                       "与「轮廓筛选」组里的编码带外沿(默认 4.2)不是同一个数, 不要改成一样。\n"
                       "解码 ROI 是 4.5R 见方, 超过 4.5 的采样点落在 ROI 外会被忽略, 等于没效果。"),
        1.0, 10.0, 2, &DetectorParams::codeRingOuter);
    addIntField(fDec, QStringLiteral("codeBrightThr"), QStringLiteral("编码块亮阈值"),
        QStringLiteral("判定编码块采样的“亮”阈值(默认 200)。\n"
                       "注意: 当前实现里解码用的是二值图(只有 0/255), 所以取 1~254 之间\n"
                       "任何值效果都完全相同 —— 这个参数现在几乎不起作用, 解不出 ID 时\n"
                       "不要在这上面调, 优先看「拉正二值化阈值」和两个位判定参数。\n"
                       "只有设成 0(几乎全算亮)或 255(全算暗)才会改变行为, 通常都是错的。"),
        0, 255, &DetectorParams::codeBrightThr);
    addIntField(fDec, QStringLiteral("rectifyBinThr"), QStringLiteral("拉正二值化阈值"),
        QStringLiteral("倾斜靶标做仿射拉正后重新二值化的阈值(默认 127)。\n"
                       "仿射插值会把 0/255 变成中间灰度, 不重新二值化就找不到\n"
                       "精确的“0→255 上升沿”, 结果是边界检测失败、整颗靶标的位全为 0 —— \n"
                       "解不出 ID 时这个值最值得先试(100~170 扫描)。\n"
                       "只有短/长轴 < 0.965 的倾斜靶标才拉正, 正视图靶标不使用这个值。"),
        1, 254, &DetectorParams::rectifyBinThr);
    addIntField(fDec, QStringLiteral("bitSubHitThr"), QStringLiteral("子块命中点数"),
        QStringLiteral("每个 8° 子块里亮点数“严格大于”该值才算这个子块“亮”\n"
                       "(默认 4, 共 8 个采样角, 有效范围只有 0~7)。\n"
                       "调低 → 更容易把一个块读成亮(1); 调高 → 更容易读成暗(0)。"),
        0, 7, &DetectorParams::bitSubHitThr);
    addIntField(fDec, QStringLiteral("bitBlockThr"), QStringLiteral("位判定块数"),
        QStringLiteral("每个 24° 编码位分成 3 个子块, 至少这么多块亮才把该位读成 1\n"
                       "(默认 2 = 多数表决)。改成 3 → 必须全亮才算 1, 解码更严但容易漏位;\n"
                       "改成 1 → 只要一块亮就算 1, 更容易把黑块读成白。"),
        0, 3, &DetectorParams::bitBlockThr);
    addDoubleField(fDec, QStringLiteral("minGapRadii"), QStringLiteral("去重中心距倍数"),
        QStringLiteral("两个候选中心的距离小于“该值 × 两者中较小的半径”时视为同一物理靶标,\n"
                       "按白占比与白色面积取舍(默认 10.0)。\n"
                       "出现“同一靶标被识别成两个 ID”时调大(12~15); \n"
                       "相邻很近的靶标被误合并时调小(5~8)。"),
        1.0, 1000.0, 1, &DetectorParams::minGapRadii);

    // ---- 图像增强(宿主侧) ----
    // 同样不注册进 m_fields; 整组显隐由 applyParamVisibility 控制。
    m_chkEnhance = new QCheckBox(m_groupEnhance);
    m_chkEnhance->setToolTip(QStringLiteral(
        "检测前按黑白场把图像拉伸成高对比版本: 目标白底更白、环境暗噪声更黑。\n"
        "只影响喂给识别器的输入和界面显示, 不改磁盘上的原图;\n"
        "它是逐像素点运算, 不移动任何坐标, 识别结果仍能正确叠加。\n"
        "默认关闭 —— 关闭时识别结果与以前完全一致。"));
    fEnh->addRow(QStringLiteral("启用增强"), m_chkEnhance);
    connect(m_chkEnhance, &QCheckBox::toggled, this, &testDetect::onParamEdited);

    m_spinEnhanceBlack = new QSpinBox(m_groupEnhance);
    m_spinEnhanceBlack->setRange(0, 254);
    m_spinEnhanceBlack->setAccelerated(true);
    m_spinEnhanceBlack->setKeyboardTracking(false);
    m_spinEnhanceBlack->setFixedWidth(96);
    m_spinEnhanceBlack->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_spinEnhanceBlack->setToolTip(QStringLiteral(
        "黑场(灰度 0~254, 默认 0 = 不压): 低于它的像素直接变成纯黑。\n"
        "用途: 背景纹理/反光条纹的灰度明显低于目标白底时, 把黑场提到两者之间,\n"
        "环境干扰先被压成一片黑, 轮廓筛选更容易分干净。\n"
        "例如背景约 60、目标白底约 230, 可先试黑场 80~100。\n"
        "必须小于白场; 设得过高会把目标暗部一起压掉。"));
    fEnh->addRow(QStringLiteral("黑场(灰度)"), m_spinEnhanceBlack);
    connect(m_spinEnhanceBlack, QOverload<int>::of(&QSpinBox::valueChanged),
        this, &testDetect::onParamEdited);

    m_spinEnhanceWhite = new QSpinBox(m_groupEnhance);
    m_spinEnhanceWhite->setRange(1, 255);
    m_spinEnhanceWhite->setAccelerated(true);
    m_spinEnhanceWhite->setKeyboardTracking(false);
    m_spinEnhanceWhite->setFixedWidth(96);
    m_spinEnhanceWhite->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_spinEnhanceWhite->setToolTip(QStringLiteral(
        "白场(灰度 1~255, 默认 255 = 不拉): 高于它的像素直接变成纯白。\n"
        "用途: 目标白底不够纯、边缘发灰时, 把白场降到白底灰度附近,\n"
        "中心圆盘会变成干净的实心白块。例如白底约 230, 可先试白场 200~220。\n"
        "注意: 白场过低会让白斑变大/粘连, 反而影响半径与面积; 必须大于黑场。"));
    fEnh->addRow(QStringLiteral("白场(灰度)"), m_spinEnhanceWhite);
    connect(m_spinEnhanceWhite, QOverload<int>::of(&QSpinBox::valueChanged),
        this, &testDetect::onParamEdited);

    // ---- 结果过滤(宿主侧) ----
    // 不注册进 m_fields: 它不写 DetectorParams, 否则 collectParams/载入参数文件
    // 会把它当成 DLL 参数来回倒。显隐由整组控制, 脏标记由 onParamEdited 处理。
    m_spinMinArea = new QSpinBox(m_groupResultFilter);
    m_spinMinArea->setRange(0, 1000000);
    m_spinMinArea->setAccelerated(true);
    m_spinMinArea->setKeyboardTracking(false);   // 输入落定才发信号
    m_spinMinArea->setFixedWidth(96);
    m_spinMinArea->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_spinMinArea->setToolTip(QStringLiteral(
        "识别结果面积小于该值(平方像素)时, 在进入结果列表前直接丢弃。\n"
        "作用对象是编码片和标记点两类结果, 相当于自动做一遍「删除干扰点」,\n"
        "省得每次手动删小噪点。0 = 不过滤。\n"
        "默认 100: 典型干扰轮廓(毛刺/反光点)面积常在 10~50,\n"
        "而正常靶标中心圆随相机距离不同通常在几百到几千。\n"
        "判断依据就是结果列表里显示的「面积」列, 可以先不过滤跑一次看看分布再定。\n"
        "注意: 调得过大, 远距离的小靶标也会被一起滤掉。"));
    fFilter->addRow(QStringLiteral("最小面积(px^2)"), m_spinMinArea);
    connect(m_spinMinArea, QOverload<int>::of(&QSpinBox::valueChanged),
        this, &testDetect::onParamEdited);

    m_spinMaxArea = new QSpinBox(m_groupResultFilter);
    m_spinMaxArea->setRange(0, 1000000);
    m_spinMaxArea->setAccelerated(true);
    m_spinMaxArea->setKeyboardTracking(false);
    m_spinMaxArea->setFixedWidth(96);
    m_spinMaxArea->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_spinMaxArea->setToolTip(QStringLiteral(
        "识别结果面积大于该值(平方像素)时, 在进入结果列表前直接丢弃。0 = 不设上限。\n"
        "默认 10000: 正常靶标中心圆通常几百到几千; 超过一万的多是\n"
        "大面积反光/工件面被当成圆盘, 或几个白斑粘连成的大块。\n"
        "它与「最小面积」一起构成一个面积窗口: 小于下限或大于上限的结果都不显示。\n"
        "注意: 上限必须大于下限, 且 0 表示不设上限(不是“全删”)。"));
    fFilter->addRow(QStringLiteral("最大面积(px^2)"), m_spinMaxArea);
    connect(m_spinMaxArea, QOverload<int>::of(&QSpinBox::valueChanged),
        this, &testDetect::onParamEdited);

    loadHostParamsToWidgets();     // 初始化两组宿主侧控件的值

    il->addStretch(1);
    m_paramScroll->setWidget(inner);
    v->addWidget(m_paramScroll, 1);

    // ---- 按钮(2×2: 340px 宽一行放不下 4 个中文按钮) ----
    // 按钮固定在滚动区外: 【应用参数】是主操作, 不应随参数一起滚出屏幕。
    QGridLayout* btns = new QGridLayout();
    btns->setContentsMargins(0, 0, 0, 0);
    btns->setSpacing(6);
    m_btnApply = new QPushButton(QStringLiteral("应用参数"), paramPane);
    m_btnResetParams = new QPushButton(QStringLiteral("恢复默认"), paramPane);
    m_btnSaveParams = new QPushButton(QStringLiteral("保存参数…"), paramPane);
    m_btnLoadParams = new QPushButton(QStringLiteral("载入参数…"), paramPane);
    btns->addWidget(m_btnApply, 0, 0);
    btns->addWidget(m_btnResetParams, 0, 1);
    btns->addWidget(m_btnSaveParams, 1, 0);
    btns->addWidget(m_btnLoadParams, 1, 1);
    // 高级参数入口: 锁定时只显示此按钮, 密码正确才展开全部参数。
    m_btnUnlock = new QPushButton(QStringLiteral("高级参数…"), paramPane);
    m_btnUnlock->setToolTip(QStringLiteral("输入密码后显示全部参数（33 项识别参数 + 图像增强/结果过滤）。\n"
                                           "默认用户只需调上面那几项常用参数。"));
    btns->addWidget(m_btnUnlock, 2, 0, 1, 2);
    v->addLayout(btns);

    m_paramHint = new QLabel(paramPane);
    m_paramHint->setWordWrap(true);
    v->addWidget(m_paramHint);

    connect(m_btnApply, &QPushButton::clicked, this, &testDetect::onApplyParams);
    connect(m_btnResetParams, &QPushButton::clicked, this, &testDetect::onResetParams);
    connect(m_btnSaveParams, &QPushButton::clicked, this, &testDetect::onSaveParamsFile);
    connect(m_btnLoadParams, &QPushButton::clicked, this, &testDetect::onLoadParamsFile);
    connect(m_btnUnlock, &QPushButton::clicked, this, &testDetect::onUnlockAdvanced);

    // 初值: settings.ini 优先, 否则出厂默认; 两条路都要同步到识别器。
    if (!loadParamsIni()) {
        const DetectorParams def = TargetDetector::defaultParams();
        loadParamsToWidgets(def);
        m_detector.setParams(def);
    }
    setParamsDirty(false);
    updateParamDependents();
    applyParamVisibility();      // 启动即锁定
}

DetectorParams testDetect::collectParams() const
{
    // 默认值打底, 防止某个控件缺失时把该项写成 0。
    DetectorParams p = TargetDetector::defaultParams();
    for (const ParamField& f : m_fields) {
        if (!f.w)
            continue;
        f.write(p, widgetValue(f.w));
    }
    return p;
}

void testDetect::loadParamsToWidgets(const DetectorParams& p)
{
    m_updatingParams = true;      // setValue/setChecked 会发信号, 抑制"被改动"判定
    for (const ParamField& f : m_fields) {
        if (f.w)
            setWidgetValue(f.w, f.read(p));
    }
    m_updatingParams = false;
}

// 宿主侧参数: 控件 -> 参数集。缺控件时沿用已生效值。
HostParams testDetect::collectHostParams() const
{
    HostParams h = m_host;
    if (m_chkEnhance)
        h.enhanceEnable = m_chkEnhance->isChecked();
    if (m_spinEnhanceBlack)
        h.enhanceBlack = m_spinEnhanceBlack->value();
    if (m_spinEnhanceWhite)
        h.enhanceWhite = m_spinEnhanceWhite->value();
    if (m_chkGrayscale)
        h.grayscale = m_chkGrayscale->isChecked();
    if (m_spinMinArea)
        h.minResultArea = m_spinMinArea->value();
    if (m_spinMaxArea)
        h.maxResultArea = m_spinMaxArea->value();
    // 白场必须大于黑场, 否则整张图没有中间调; 夹到黑场 +1 并回填控件。
    if (h.enhanceWhite <= h.enhanceBlack)
        h.enhanceWhite = qMin(255, h.enhanceBlack + 1);
    return h;
}

// 宿主侧参数: 参数集 -> 控件。两组的显隐仍由 applyParamVisibility 控制。
void testDetect::loadHostParamsToWidgets()
{
    m_updatingParams = true;
    if (m_chkEnhance)
        m_chkEnhance->setChecked(m_host.enhanceEnable);
    if (m_spinEnhanceBlack)
        m_spinEnhanceBlack->setValue(m_host.enhanceBlack);
    if (m_spinEnhanceWhite)
        m_spinEnhanceWhite->setValue(m_host.enhanceWhite);
    if (m_chkGrayscale)
        m_chkGrayscale->setChecked(m_host.grayscale);
    if (m_spinMinArea)
        m_spinMinArea->setValue(m_host.minResultArea);
    if (m_spinMaxArea)
        m_spinMaxArea->setValue(m_host.maxResultArea);
    m_updatingParams = false;
    updateParamDependents();
}

void testDetect::onParamEdited()
{
    if (m_updatingParams)
        return;
    // 记录改动是否包含常用项: 收起高级参数时只丢弃高级项上的未应用改动。
    if (QWidget* w = qobject_cast<QWidget*>(sender())) {
        for (const ParamField& f : m_fields) {
            if (f.w == w) {
                if (f.basic)
                    m_basicDirty = true;
                break;
            }
        }
    }
    setParamsDirty(true);
    updateParamDependents();      // 总开关变化后同步联动项的启用状态
}

void testDetect::setParamsDirty(bool dirty)
{
    m_paramsDirty = dirty;
    if (!dirty)
        m_basicDirty = false;     // 已应用/已回滚: 常用项的未应用改动也一并清零
    if (m_paramHint) {
        m_paramHint->setText(dirty
            ? QStringLiteral("参数已改动，点「应用参数」生效")
            : QStringLiteral("参数已生效；改动后需点顶部任一识别按钮重新识别"));
        // 动态状态无法用 QSS 选择器表达, 只能在代码里设色。
        m_paramHint->setStyleSheet(QStringLiteral("color:%1;")
            .arg(QLatin1String(dirty ? kHintColorDirty : kHintColorIdle)));
    }
    if (m_btnApply)
        m_btnApply->setEnabled(dirty);
}

void testDetect::updateParamDependents()
{
    // 总开关关掉后, 被它压住的项不再被算法读取, 置灰以免用户白调。
    if (m_chkOtsu && m_spinFixedThr)
        m_spinFixedThr->setEnabled(!m_chkOtsu->isChecked());
    if (m_chkOtsuK && m_spinFixedKt)
        m_spinFixedKt->setEnabled(!m_chkOtsuK->isChecked());
    if (m_chkSubpixel && m_groupSubpixel) {
        const bool on = m_chkSubpixel->isChecked();
        for (const ParamField& f : m_fields) {
            if (f.group == m_groupSubpixel && f.w != m_chkSubpixel)
                f.w->setEnabled(on);
        }
    }
    // 未启用图像增强时, 黑白场不参与任何计算, 置灰避免白调。
    if (m_chkEnhance) {
        const bool on = m_chkEnhance->isChecked();
        if (m_spinEnhanceBlack)
            m_spinEnhanceBlack->setEnabled(on);
        if (m_spinEnhanceWhite)
            m_spinEnhanceWhite->setEnabled(on);
    }
}

// ---------------------------------------------------------------- 参数权限
// 锁定不只是隐藏控件, 还要在数据层挡住三条旁路:
//   ① 载入参数文件时忽略高级键; ② 恢复默认只重置可见项;
//   ③ 收起时丢弃高级项上未应用的改动, 避免之后被"应用参数"带进去。
void testDetect::setAdvancedUnlocked(bool on)
{
    if (m_advancedUnlocked == on)
        return;
    m_advancedUnlocked = on;

    if (!on) {
        // 收起 = 放弃未应用的高级改动, 恢复为识别器当前生效值。
        const DetectorParams cur = m_detector.params();
        m_updatingParams = true;
        for (const ParamField& f : m_fields) {
            if (!f.basic && f.w)
                setWidgetValue(f.w, f.read(cur));
        }
        m_updatingParams = false;
        // 宿主侧两组(图像增强/结果过滤)也不在 m_fields 里, 单独回滚到已生效值。
        loadHostParamsToWidgets();

        // 只统计常用项上的未应用改动。
        setParamsDirty(m_basicDirty);
    }

    if (m_btnUnlock) {
        m_btnUnlock->setText(on ? QStringLiteral("收起高级参数")
                                : QStringLiteral("高级参数…"));
    }
    applyParamVisibility();
    updateParamDependents();     // 解锁后的亚像素组要重新计算联动禁用

    statusBar()->showMessage(on
        ? QStringLiteral("高级参数已解锁 —— 修改后记得点「应用参数」")
        : QStringLiteral("高级参数已收起（未应用的改动已放弃）"));
}

void testDetect::applyParamVisibility()
{
    // 这两组全部是高级参数, 整组显隐。
    if (m_groupSubpixel)
        m_groupSubpixel->setVisible(m_advancedUnlocked);
    if (m_groupDecode)
        m_groupDecode->setVisible(m_advancedUnlocked);
    if (m_groupEnhance)
        m_groupEnhance->setVisible(m_advancedUnlocked);
    if (m_groupResultFilter)
        m_groupResultFilter->setVisible(m_advancedUnlocked);

    // 字段与其 QFormLayout 标签一起显隐; 空行会被布局跳过。
    for (const ParamField& f : m_fields) {
        if (!f.w)
            continue;
        const bool vis = f.basic || m_advancedUnlocked;
        f.w->setVisible(vis);
        if (f.group) {
            if (QFormLayout* form = qobject_cast<QFormLayout*>(f.group->layout())) {
                if (QWidget* lbl = form->labelForField(f.w))
                    lbl->setVisible(vis);
            }
        }
    }
}

void testDetect::onUnlockAdvanced()
{
    // 已解锁时点按钮只是收起, 不再要密码。
    if (m_advancedUnlocked) {
        setAdvancedUnlocked(false);
        return;
    }

    bool ok = false;
    const QString pwd = QInputDialog::getText(this, QStringLiteral("高级参数"),
        QStringLiteral("请输入高级参数密码："), QLineEdit::Password,
        QString(), &ok);
    if (!ok)
        return;
    if (!verifyAdvancedPassword(pwd)) {
        QMessageBox::warning(this, QStringLiteral("密码错误"),
            QStringLiteral("密码不正确，高级参数未解锁。"));
        return;
    }
    setAdvancedUnlocked(true);
}

void testDetect::onApplyParams()
{
    const DetectorParams wanted = collectParams();
    m_detector.setParams(wanted);            // DLL 侧还会再夹取一次范围
    // 回读实际生效值, 保证界面与算法一致(例如腐蚀核填偶数会被夹成奇数)。
    loadParamsToWidgets(m_detector.params());
    // 宿主侧参数(图像增强/面积窗口): 与 DLL 参数一起在【应用参数】时生效。
    m_host = collectHostParams();
    loadHostParamsToWidgets();          // 回填夹取后的值(白场必须大于黑场)
    refreshDisplayImage();              // 立刻看到黑白场效果, 已有结果叠加保留
    updateParamDependents();
    saveParamsIni();
    setParamsDirty(false);

    // 现有结果由旧参数产生, 明确提示重新识别。
    statusBar()->showMessage(m_markers.isEmpty()
        ? QStringLiteral("参数已应用")
        : QStringLiteral("参数已应用 —— 点顶部任一识别按钮用新参数重新识别"));
}

void testDetect::onResetParams()
{
    DetectorParams def = TargetDetector::defaultParams();
    // 锁定时只重置常用项, 否则可绕过密码修改高级参数。
    if (!m_advancedUnlocked) {
        const DetectorParams cur = m_detector.params();
        for (const ParamField& f : m_fields) {
            if (!f.basic)
                f.write(def, f.read(cur));
        }
    }
    else {
        // 宿主侧两组同样是高级项: 解锁时才跟着恢复默认, 锁定时保持不动。
        m_host = HostParams();
    }
    // 「灰度化」在始终可见的分组里, 属于常用项: 锁定时也一起恢复默认。
    m_host.grayscale = false;
    loadParamsToWidgets(def);
    loadHostParamsToWidgets();     // 控件回填(锁定时只是回到已生效值)
    m_detector.setParams(def);
    refreshDisplayImage();         // 增强恢复默认后, 显示图也要还原
    updateParamDependents();
    saveParamsIni();          // 默认值同样落盘, 否则下次启动会读回旧参数
    setParamsDirty(false);
    statusBar()->showMessage(m_advancedUnlocked
        ? QStringLiteral("参数已恢复默认并应用")
        : QStringLiteral("常用参数已恢复默认并应用（高级参数保持不变）"));
}

// ---------------------------------------------------------------- 参数持久化

QString testDetect::paramsIniPath() const
{
    // 放在 exe 同目录, 跟随程序, 不写注册表。
    return QCoreApplication::applicationDirPath() + QStringLiteral("/settings.ini");
}

void testDetect::writeParamsToSettings(const DetectorParams& p, const QVector<ParamField>& fields,
    const QString& group, const QString& filePath)
{
    QSettings s(filePath, QSettings::IniFormat);
    s.beginGroup(group);
    for (const ParamField& f : fields) {
        // 17 位有效数字: QVariant 默认 6 位会把 0.03001 之类的小量级抹平。
        s.setValue(f.key, QString::number(f.read(p), 'g', 17));
    }
    s.endGroup();
    s.sync();
}

bool testDetect::readParamsFromSettings(DetectorParams& p, const QVector<ParamField>& fields,
    const QString& group, const QString& filePath)
{
    QSettings s(filePath, QSettings::IniFormat);
    if (s.status() != QSettings::NoError)
        return false;
    s.beginGroup(group);
    const QStringList keys = s.childKeys();
    s.endGroup();
    if (keys.isEmpty())
        return false;

    s.beginGroup(group);
    for (const ParamField& f : fields) {
        if (!keys.contains(f.key))
            continue;                 // 缺键保留 p 的现值(调用方已用默认值打底)
        bool ok = false;
        const double v = s.value(f.key).toString().toDouble(&ok);
        if (ok)
            f.write(p, v);
    }
    s.endGroup();
    return true;
}

void testDetect::saveParamsIni()
{
    writeParamsToSettings(m_detector.params(), m_fields,
        QStringLiteral("detectParams"), paramsIniPath());
    writeHostParams(m_host, paramsIniPath());
}

bool testDetect::loadParamsIni()
{
    DetectorParams p = TargetDetector::defaultParams();
    if (!readParamsFromSettings(p, m_fields, QStringLiteral("detectParams"), paramsIniPath()))
        return false;

    // 宿主侧参数: 缺键时用出厂默认(兼容旧版 settings.ini)。
    m_host = HostParams();
    readHostParams(m_host, paramsIniPath());
    loadHostParamsToWidgets();

    loadParamsToWidgets(p);
    m_detector.setParams(p);
    loadParamsToWidgets(m_detector.params());   // 夹取后的真实值回填控件
    return true;
}

void testDetect::onSaveParamsFile()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存参数"),
        QStringLiteral("detectParams.ini"), QStringLiteral("参数文件 (*.ini)"));
    if (path.isEmpty())
        return;
    // 保存控件中的当前值(含未应用改动), 所见即所存。
    writeParamsToSettings(collectParams(), m_fields, QStringLiteral("detectParams"), path);
    writeHostParams(collectHostParams(), path);
    statusBar()->showMessage(QStringLiteral("参数已保存到 %1").arg(path));
}

void testDetect::onLoadParamsFile()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("载入参数"),
        QString(), QStringLiteral("参数文件 (*.ini);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;

    DetectorParams p = TargetDetector::defaultParams();
    if (!readParamsFromSettings(p, m_fields, QStringLiteral("detectParams"), path)) {
        QMessageBox::warning(this, QStringLiteral("载入失败"),
            QStringLiteral("这个文件里没有可用的参数(期望 [detectParams] 分组):\n%1").arg(path));
        return;
    }
    // 锁定时忽略文件中的高级键, 沿用当前生效值, 防止绕过密码。
    if (!m_advancedUnlocked) {
        const DetectorParams cur = m_detector.params();
        for (const ParamField& f : m_fields) {
            if (!f.basic)
                f.write(p, f.read(cur));
        }
    }
    else {
        // 宿主侧两组是高级项: 解锁时从文件读, 文件没有的键回出厂默认。
        m_host = HostParams();
        readHostParams(m_host, path);
        loadHostParamsToWidgets();
    }
    if (!m_advancedUnlocked) {
        // 灰度化在始终可见的分组里: 锁定时也从文件读, 其余宿主侧高级项不动。
        HostParams hp = m_host;
        if (readHostParams(hp, path))
            m_host.grayscale = hp.grayscale;
        loadHostParamsToWidgets();
    }
    loadParamsToWidgets(p);
    m_detector.setParams(p);
    loadParamsToWidgets(m_detector.params());
    refreshDisplayImage();
    updateParamDependents();
    saveParamsIni();              // 载入即视为当前配置, 同步写入 settings.ini
    setParamsDirty(false);
    const QString name = QFileInfo(path).fileName();
    if (m_markers.isEmpty()) {
        statusBar()->showMessage(m_advancedUnlocked
            ? QStringLiteral("参数已载入：%1").arg(name)
            : QStringLiteral("参数已载入：%1（高级参数保持不变）").arg(name));
    }
    else {
        statusBar()->showMessage(QStringLiteral("参数已载入：%1 —— 点顶部任一识别按钮重新识别").arg(name));
    }
}

// ---------------------------------------------------------------- 全屏

void testDetect::onToggleFullScreen()
{
    const bool goFull = !isFullScreen();
    if (goFull)
        showFullScreen();
    else
        showMaximized();      // 退出全屏回到最大化(本程序默认最大化)

    if (m_btnFullScreen)
        m_btnFullScreen->setChecked(goFull);
    if (m_shortcutEsc)
        m_shortcutEsc->setEnabled(goFull);   // 非全屏时把 Esc 还给控件

    statusBar()->showMessage(goFull
        ? QStringLiteral("全屏显示中 —— 按 F11 或 Esc 退出")
        : QStringLiteral("已退出全屏"), 4000);
}

// 用户用系统按钮/双击标题栏改窗口状态时, 让按钮勾选态跟着走
void testDetect::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        const bool full = isFullScreen();
        if (m_btnFullScreen)
            m_btnFullScreen->setChecked(full);
        if (m_shortcutEsc)
            m_shortcutEsc->setEnabled(full);
    }
}


// ---------------------------------------------------------------- 导入图片

void testDetect::onImportImage()
{
    const QString path = QFileDialog::getOpenFileName(this,
        QStringLiteral("选择图片"), QString(),
        QStringLiteral("图片 (*.png *.bmp *.jpg *.jpeg *.tif *.tiff);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;

    cv::Mat raw = cv::imread(path.toLocal8Bit().toStdString(), cv::IMREAD_COLOR);
    if (raw.empty()) {
        QMessageBox::warning(this, QStringLiteral("导入失败"),
            QStringLiteral("无法读取图片:\n%1").arg(path));
        return;
    }

    m_srcMat = raw;
    m_hasImage = true;

    // 识别坐标位于去畸变坐标系, 显示图必须同源, 否则叠加会整体偏移。
    // 显示图 = 去畸变(识别输入), 灰度化时与识别器看到的完全是同一张灰度图。
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const cv::Mat disp = buildDisplayImage();
    QApplication::restoreOverrideCursor();

    clearResults();
    showImage(disp);

    m_btnRoi->setChecked(false);
    setBusy(false);

    statusBar()->showMessage(QStringLiteral("已载入 %1   (%2 x %3)")
        .arg(QFileInfo(path).fileName()).arg(raw.cols).arg(raw.rows));
}

// 黑白场线性拉伸: 低于 black 的压成 0, 高于 white 的拉到 255, 中间线性。
// 结果是一张 256 项 LUT 的点运算, 不改变任何像素坐标, 所以识别结果仍能对得上。
// 未启用/参数退化(0..255)时原样返回(浅拷贝, 不复制像素)。
cv::Mat testDetect::enhanceImageForDetect(const cv::Mat& src) const
{
    if (src.empty() || !m_host.enhanceEnable)
        return src;
    const int lo = m_host.enhanceBlack;
    const int hi = m_host.enhanceWhite;
    if (hi <= lo || (lo <= 0 && hi >= 255))
        return src;

    cv::Mat lut(1, 256, CV_8U);
    const double scale = 255.0 / (hi - lo);
    for (int i = 0; i < 256; ++i) {
        const int v = (int)std::lround((i - lo) * scale);
        lut.at<uchar>(0, i) = (uchar)qBound(0, v, 255);
    }
    cv::Mat out;
    cv::LUT(src, lut, out);
    return out;
}

// 识别输入: 先按黑白场增强, 勾选「灰度化」时再转成单通道灰度。
// 转换权重与 DLL 内部 cvtColor(BGR2GRAY) 完全一致, 所以"宿主先转"和
// "DLL 自己转"得到的是同一张灰度图, 检测结果不受影响。
// 两个开关都没开时返回的是浅拷贝(与旧路径一致)。
cv::Mat testDetect::prepareDetectInput(const cv::Mat& src) const
{
    cv::Mat m = enhanceImageForDetect(src);
    if (!m_host.grayscale || m.empty() || m.channels() != 3)
        return m;

    cv::Mat g;
    cv::cvtColor(m, g, cv::COLOR_BGR2GRAY);
    return g;
}

// 显示图 = 去畸变(识别输入)。识别器内部对同一份输入做同样的去畸变,
// 所以灰度化时 m_viewMat 与它真正处理的灰度图逐像素一致 —— 状态栏读数可信。
cv::Mat testDetect::buildDisplayImage()
{
    if (m_srcMat.empty())
        return cv::Mat();
    return m_detector.undistortImage(prepareDetectInput(m_srcMat));
}

// 只替换显示 pixmap, 不动结果图元/列表 —— 用于【应用参数】后立刻看到黑白场/灰度效果。
void testDetect::refreshDisplayImage()
{
    if (!m_hasImage || !m_pixmapItem)
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const cv::Mat disp = buildDisplayImage();
    QApplication::restoreOverrideCursor();
    if (disp.empty())
        return;

    m_viewMat = disp;
    cv::Mat rgb;
    if (disp.channels() == 1)
        cv::cvtColor(disp, rgb, cv::COLOR_GRAY2RGB);
    else
        cv::cvtColor(disp, rgb, cv::COLOR_BGR2RGB);
    const QImage img(rgb.data, rgb.cols, rgb.rows, (int)rgb.step, QImage::Format_RGB888);
    m_pixmapItem->setPixmap(QPixmap::fromImage(img.copy()));
}

void testDetect::showImage(const cv::Mat& bgr)
{
    m_scene->clear();               // 连同图元一起销毁
    m_markers.clear();
    m_pixmapItem = nullptr;
    m_roiItem = nullptr;

    // 保存当前显示图: 灰度化时鼠标悬停从这里读灰度值(1 通道), 坐标与 scene 一致。
    m_viewMat = bgr;
    clearPixelInfo();

    cv::Mat rgb;
    if (bgr.channels() == 1)
        cv::cvtColor(bgr, rgb, cv::COLOR_GRAY2RGB);
    else
        cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    // QImage 不接管数据, 转 QPixmap 前必须深拷贝。
    const QImage img(rgb.data, rgb.cols, rgb.rows, (int)rgb.step, QImage::Format_RGB888);
    m_pixmapItem = m_scene->addPixmap(QPixmap::fromImage(img.copy()));
    m_pixmapItem->setZValue(0);

    m_scene->setSceneRect(m_pixmapItem->boundingRect());

    // 图元尺寸随图像分辨率缩放。
    const int shortSide = qMin(bgr.cols, bgr.rows);
    m_markRadius = qBound(5.0, shortSide / 180.0, 48.0);
    m_penW = qBound(1.0, shortSide / 900.0, 8.0);
    m_labelFontPx = 13.0;         // 屏幕像素, 不随缩放变化

    // ROI 多边形垫在标记点下面, 至少 3 点才显示。
    m_roiItem = m_scene->addPolygon(QPolygonF(),
        QPen(QColor(0, 220, 90), m_penW * 2.0), QBrush(QColor(0, 220, 90, 60)));
    m_roiItem->setZValue(1);
    m_roiItem->setVisible(false);

    m_autoFit = true;
    onFitView();
}


// ---------------------------------------------------------------- 识别入口
//
// 三个按钮共用一套合并规则: **只重算被点的那一类**, 另一类保留;
// 「编码片区域」= 编码带外沿 4.2R, 区域内的普通标记点一律由编码片取代。
// 这样先识别哪一类都不会互相清空, 而编码片永远优先。

void testDetect::onStartDetect()
{
    if (!m_hasImage || m_busy)
        return;

    // 编码片优先: 全图重新识别编码片, 旧编码片整类替换;
    // 新编码片区域(4.2R)内的标记点删除并由编码片取代, 区域外的标记点保留。
    setBusy(true);
    statusBar()->showMessage(QStringLiteral("识别编码片中…"));

    // 识别输入 = 已生效的黑白场增强 + 可选的灰度化; 都未启用时是浅拷贝。
    // 在主线程算好再交给工作线程, 参数取的是点击这一刻的快照。
    cv::Mat input = prepareDetectInput(m_srcMat);
    m_thread = QThread::create([this, input]() mutable {
        std::vector<DetectionResult> results;
        try {
            results = m_detector.detectTarget(input);
        }
        catch (const std::exception& e) {
            // 异常不能穿出线程函数(会 std::terminate), 也不能让界面停在 busy。
            const QString msg = QString::fromLocal8Bit(e.what());
            QMetaObject::invokeMethod(this, [this, msg]() {
                setBusy(false);
                statusBar()->showMessage(QStringLiteral("编码片识别失败"));
                QMessageBox::warning(this, QStringLiteral("识别失败"), msg);
            }, Qt::QueuedConnection);
            return;
        }
        // 切回 GUI 线程再更新界面
        QMetaObject::invokeMethod(this, [this, results]() { finishCodedDetect(results); },
                                  Qt::QueuedConnection);
    });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->start();
}

// 只重算标记点: 已识别的编码片保留, 落在编码片区域内的新标记点丢弃。
// 标记点没有编码 ID(DLL 返回 -1), 界面序号在 finalizeResultMerge 里按列表顺序生成。
void testDetect::onStartDetectMarkers()
{
    if (!m_hasImage || m_busy)
        return;

    setBusy(true);
    statusBar()->showMessage(QStringLiteral("标记点识别中…"));

    // 识别输入 = 已生效的黑白场增强 + 可选的灰度化; 都未启用时是浅拷贝。
    // 在主线程算好再交给工作线程, 参数取的是点击这一刻的快照。
    cv::Mat input = prepareDetectInput(m_srcMat);
    m_thread = QThread::create([this, input]() mutable {
        std::vector<DetectionResult> results;
        try {
            results = m_detector.detectMarkers(input);
        }
        catch (const std::exception& e) {
            const QString msg = QString::fromLocal8Bit(e.what());
            QMetaObject::invokeMethod(this, [this, msg]() {
                setBusy(false);
                statusBar()->showMessage(QStringLiteral("标记点识别失败"));
                QMessageBox::warning(this, QStringLiteral("识别失败"), msg);
            }, Qt::QueuedConnection);
            return;
        }
        QMetaObject::invokeMethod(this, [this, results]() { finishMarkerDetect(results); },
                                  Qt::QueuedConnection);
    });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->start();
}

// 一次跑完两类: 先编码片, 再标记点(标记点按本次编码片区域排除)。
void testDetect::onStartDetectAll()
{
    if (!m_hasImage || m_busy)
        return;

    setBusy(true);
    statusBar()->showMessage(QStringLiteral("识别所有标记点（含编码片）中…"));

    // 识别输入 = 已生效的黑白场增强 + 可选的灰度化; 都未启用时是浅拷贝。
    // 在主线程算好再交给工作线程, 参数取的是点击这一刻的快照。
    cv::Mat input = prepareDetectInput(m_srcMat);
    m_thread = QThread::create([this, input]() mutable {
        std::vector<DetectionResult> coded, markers;
        try {
            coded = m_detector.detectTarget(input);
            markers = m_detector.detectMarkers(input);
        }
        catch (const std::exception& e) {
            const QString msg = QString::fromLocal8Bit(e.what());
            QMetaObject::invokeMethod(this, [this, msg]() {
                setBusy(false);
                statusBar()->showMessage(QStringLiteral("识别失败"));
                QMessageBox::warning(this, QStringLiteral("识别失败"), msg);
            }, Qt::QueuedConnection);
            return;
        }
        QMetaObject::invokeMethod(this,
            [this, coded, markers]() { finishDetectAll(coded, markers); },
            Qt::QueuedConnection);
    });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->start();
}

// 编码片结果落盘: 旧编码片整类替换; 旧标记点只保留不在新编码片区域内的。
void testDetect::finishCodedDetect(const std::vector<DetectionResult>& results)
{
    // 区域用原始识别结果(不去黑名单): 某一行随后被手动删掉, 它的 4.2R 区域仍然有效。
    m_codedRegions = results;

    clearUndoStack();

    QVector<Marker> keptMarkers;
    for (const Marker& mk : m_markers) {
        if (mk.isMarker && !isInCodedRegion(mk.pos)) {
            keptMarkers.push_back(mk);   // 区域外的标记点保留, 图元留在 scene 里
            continue;
        }
        // 旧编码片 / 被编码片取代的标记点: 销毁图元(析构时自动从 scene 摘除)。
        delete mk.contour;
        delete mk.dot;
        delete mk.label;
    }
    m_markers.clear();

    for (const DetectionResult& r : results) {
        if (isResultAreaFiltered(resultArea(r)))   // 面积窗口外的干扰: 进列表前就丢掉
            continue;
        const QPointF p(r.center.x, r.center.y);
        if (isDeletedPoint(p, false))   // 手动删掉的编码片不再加回
            continue;
        m_markers.push_back(createMarker(r, false));
    }
    // 编码片排在前面, 标记点排在后面。
    for (const Marker& mk : keptMarkers)
        m_markers.push_back(mk);

    finalizeResultMerge();
}

// 标记点结果落盘: 已识别的编码片原样保留; 旧标记点整类替换。
void testDetect::finishMarkerDetect(const std::vector<DetectionResult>& results)
{
    clearUndoStack();

    QVector<Marker> keptCoded;
    for (const Marker& mk : m_markers) {
        if (!mk.isMarker) {
            keptCoded.push_back(mk);
            continue;
        }
        delete mk.contour;
        delete mk.dot;
        delete mk.label;
    }
    m_markers.clear();

    for (const Marker& mk : keptCoded)
        m_markers.push_back(mk);

    for (const DetectionResult& r : results) {
        if (isResultAreaFiltered(resultArea(r)))   // 面积窗口外的干扰: 进列表前就丢掉
            continue;
        const QPointF p(r.center.x, r.center.y);
        if (isInCodedRegion(p))          // 编码片优先: 区域内的标记点不显示
            continue;
        if (isDeletedPoint(p, true))     // 手动删掉的干扰点不再加回
            continue;
        m_markers.push_back(createMarker(r, true));
    }

    finalizeResultMerge();
}

// 一次识别两类: 两类都整类替换。
void testDetect::finishDetectAll(const std::vector<DetectionResult>& coded,
                                 const std::vector<DetectionResult>& markers)
{
    m_codedRegions = coded;

    clearUndoStack();

    for (const Marker& mk : m_markers) {
        delete mk.contour;
        delete mk.dot;
        delete mk.label;
    }
    m_markers.clear();

    for (const DetectionResult& r : coded) {
        if (isResultAreaFiltered(resultArea(r)))
            continue;
        const QPointF p(r.center.x, r.center.y);
        if (isDeletedPoint(p, false))
            continue;
        m_markers.push_back(createMarker(r, false));
    }
    for (const DetectionResult& r : markers) {
        if (isResultAreaFiltered(resultArea(r)))
            continue;
        const QPointF p(r.center.x, r.center.y);
        if (isInCodedRegion(p) || isDeletedPoint(p, true))
            continue;
        m_markers.push_back(createMarker(r, true));
    }

    finalizeResultMerge();
}

// 从一条识别结果建图元。标记点的 id 先置 0, 编号在 finalizeResultMerge 里生成。
testDetect::Marker testDetect::createMarker(const DetectionResult& r, bool isMarker)
{
    Marker mk;
    mk.id = isMarker ? 0 : r.id;
    mk.isMarker = isMarker;
    mk.pos = QPointF(r.center.x, r.center.y);
    mk.subpixel = r.subpixel;
    mk.rms = r.rms;
    mk.nPoints = r.nPoints;
    // 长短轴/长短轴比仍取拟合椭圆(椭圆由等值线点拟合而来, 两者同源);
    // 面积由 resultArea 按实际绘制的轮廓算(等值线优先)。
    if (r.hasEllipse && r.ellipse.size.width > 0 && r.ellipse.size.height > 0) {
        const double w = r.ellipse.size.width;
        const double h = r.ellipse.size.height;
        mk.major = qMax(w, h) * 0.5;
        mk.aspect = (mk.major > 0.0) ? qMin(w, h) / qMax(w, h) : 1.0;
    }
    else {
        mk.major = qMax((double)r.radius, 1.0);
        mk.aspect = 1.0;
    }
    mk.area = resultArea(r);
    // 质量分级: 残差/椭度/是否亚像素三项合并判定, 差的画黄色并沉到列表底部。
    mk.lowQuality = isLowQualityFit(r, m_detector.params(), mk.major, mk.aspect);

    // 轮廓: 有阈值等值线就画那一圈平滑曲线(严格经过亚像素边界点);
    // 否则退回拟合椭圆, 再退化按圆。
    mk.contour = new QGraphicsPathItem();
    if (r.contourPoints.size() >= 8)
        mk.contour->setPath(smoothClosedPath(r.contourPoints));
    else if (r.hasEllipse)
        mk.contour->setPath(contourPath(r.ellipse));
    else
        mk.contour->setPath(contourPath(r.center, qMax((double)r.radius, 1.0)));
    m_scene->addItem(mk.contour);
    mk.contour->setZValue(2);

    // 中心点: 固定屏幕尺寸的小圆点, 不遮挡轮廓, 也不随视图缩放变粗。
    // ItemIgnoresTransformations 让 rect 直接按设备像素解释, 所以放大多少倍
    // 都只是中心处一个几像素的小点 —— 位置仍是亚像素中心。
    const double dotR = 2.0;
    mk.dot = new QGraphicsEllipseItem(-dotR, -dotR, dotR * 2.0, dotR * 2.0);
    mk.dot->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    mk.dot->setPos(mk.pos);
    m_scene->addItem(mk.dot);
    mk.dot->setZValue(3);

    mk.label = new QGraphicsTextItem();
    // 忽略视图变换: 标签保持固定屏幕字号, 放大时不遮挡轮廓。
    mk.label->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    mk.label->setHtml(idLabelHtml(mk.id, false, m_labelFontPx, mk.isMarker));
    mk.label->setPos(mk.pos);   // HTML 的 <br> 让文字落在锚点下方
    m_scene->addItem(mk.label);
    mk.label->setZValue(3);

    return mk;
}

// 合并完成后的统一收尾: 重排标记点编号 -> 重建列表行 -> 刷新统计。
void testDetect::finalizeResultMerge()
{
    // 质量好的点排前面, 质量差的沉到列表底部。stable_sort 保留同组内的原顺序
    // (编码片在前、标记点在后), 所以列表仍是"好的编码片 → 好的标记点 →
    //  差的编码片 → 差的标记点"; 行号 == m_markers 下标的不变量不受影响。
    std::stable_sort(m_markers.begin(), m_markers.end(),
        [](const Marker& a, const Marker& b) { return !a.lowQuality && b.lowQuality; });

    // 标记点按最终列表顺序从 1 连续编号; 编码片保留自己的 ID。
    int seq = 0;
    for (Marker& mk : m_markers) {
        if (mk.isMarker)
            mk.id = ++seq;
        if (mk.label)
            mk.label->setHtml(idLabelHtml(mk.id, false, m_labelFontPx, mk.isMarker));
    }

    // 结果整体重排后, 旧的勾选/ROI 已经没有意义。
    m_roiPolygon = QPolygonF();
    m_roiIds.clear();
    if (m_roiItem) {
        m_roiItem->setPolygon(QPolygonF());
        m_roiItem->setVisible(false);
    }

    // 分界线画在第一条质量差的结果上方(排序后差的结果都在末尾)。
    int firstPoor = -1;
    for (int i = 0; i < m_markers.size(); ++i) {
        if (m_markers[i].lowQuality) { firstPoor = i; break; }
    }

    m_updatingList = true;
    m_list->clear();
    m_pressedItem = nullptr;          // 列表项已销毁, 不能再留按下记录
    for (int i = 0; i < m_markers.size(); ++i) {
        const Marker& mk = m_markers[i];
        const QString text = markerRowText(mk);
        QListWidgetItem* item = new QListWidgetItem(text, m_list);
        item->setToolTip(text);             // 列宽不足被省略时, 悬停可看全
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        item->setData(Qt::UserRole, i);     // 行号 == m_markers 下标
        item->setData(kFirstPoorRole, i == firstPoor);
        if (mk.lowQuality)
            item->setForeground(QBrush(QColor(255, 210, 40)));   // 质量差: 黄色(与轮廓同色)
    }
    m_updatingList = false;

    updateResultTitle();
    updateResultButtons();
    applyHighlights();      // 同时把结果统计写进状态栏
    // 识别跑完但一个结果都没有时, updateRoiStatus 会回落到"点识别按钮"的提示;
    // 这里覆盖成明确的"未识别到", 让用户分得清"还没识别"和"识别了但没结果"。
    if (m_markers.isEmpty())
        statusBar()->showMessage(resultSummary());
    setBusy(false);
}

// 结果列表的一行文字。坐标保留 3 位小数(亚像素结果, 不能取整)。
// RMS 是拟合残差, 用于相对比较, 不是标定精度; 非亚像素结果明确标注。
QString testDetect::markerRowText(const Marker& mk)
{
    QString text = (mk.isMarker
        ? QStringLiteral("  M%1   (%2, %3)   ")
        : QStringLiteral("  ID: %1   (%2, %3)   "))
        .arg(mk.id)
        .arg(mk.pos.x(), 0, 'f', 3)
        .arg(mk.pos.y(), 0, 'f', 3);
    // 面积紧跟坐标: 列表宽度有限, 放在质量信息后面容易被省略号截掉。
    text += QStringLiteral("面积 %1平方像素   ").arg(mk.area, 0, 'f', 1);
    // "质量差"放在 rms 前面: 列表右侧常被省略, 放末尾会看不到。
    if (mk.lowQuality && mk.subpixel)
        text += QStringLiteral("质量差   ");
    if (mk.subpixel)
        text += QStringLiteral("±%1px / %2pt").arg(mk.rms, 0, 'f', 3).arg(mk.nPoints);
    else
        text += QStringLiteral("(!)非亚像素");
    return text;
}

// 状态栏统计: 单一类型只显示该类型, 混合时两类都显示。
QString testDetect::resultSummary() const
{
    if (m_markers.isEmpty())
        return QStringLiteral("未识别到任何结果");

    int nCoded = 0, nMark = 0, nSub = 0, nPoor = 0;
    double rmsSum = 0.0, rmsMax = 0.0;
    for (const Marker& mk : m_markers) {
        if (mk.isMarker) ++nMark; else ++nCoded;
        if (mk.lowQuality) ++nPoor;
        if (!mk.subpixel) continue;
        ++nSub;
        rmsSum += mk.rms;
        rmsMax = qMax(rmsMax, (double)mk.rms);
    }

    QString s;
    if (nCoded > 0 && nMark > 0)
        s = QStringLiteral("编码片 %1 个，标记点 %2 个").arg(nCoded).arg(nMark);
    else if (nCoded > 0)
        s = QStringLiteral("编码片 %1 个").arg(nCoded);
    else
        s = QStringLiteral("标记点 %1 个").arg(nMark);

    s += QStringLiteral("  |  亚像素 %1 个").arg(nSub);
    if (nSub > 0)
        s += QStringLiteral("  残差 RMS 均值 %1px / 最大 %2px")
            .arg(rmsSum / nSub, 0, 'f', 3).arg(rmsMax, 0, 'f', 3);
    if (nSub < m_markers.size())
        s += QStringLiteral("  (!) %1 个退回整数轮廓, 见黄虚线").arg(m_markers.size() - nSub);
    if (nPoor > 0)
        s += QStringLiteral("  质量差 %1 个(黄色, 已排在最后)").arg(nPoor);
    return s;
}

// 面积窗口: 小于下限或大于上限都算被过滤。上限 0 = 不设上限(不是"全删")。
bool testDetect::isResultAreaFiltered(double area) const
{
    if (area < (double)m_host.minResultArea)
        return true;
    if (m_host.maxResultArea > 0 && area > (double)m_host.maxResultArea)
        return true;
    return false;
}

// 编码片区域: 最近一次编码片识别的原始结果(含已被手动删掉的行), 按 4.2R 放宽。
bool testDetect::isInCodedRegion(const QPointF& pos) const
{
    for (const DetectionResult& r : m_codedRegions) {
        if (r.hasEllipse && r.ellipse.size.width > 0 && r.ellipse.size.height > 0) {
            if (pointInScaledEllipse(r.ellipse, kCodedRegionScale, pos))
                return true;
        }
        else {
            const double rr = qMax((double)r.radius, 1.0) * kCodedRegionScale;
            if (dist2(pos, QPointF(r.center.x, r.center.y)) <= rr * rr)
                return true;
        }
    }
    return false;
}

bool testDetect::isDeletedPoint(const QPointF& pos, bool isMarker) const
{
    const double tol2 = kDeletedPointTol * kDeletedPointTol;
    for (const DeletedPoint& d : m_deletedPoints) {
        if (d.isMarker == isMarker && dist2(d.pos, pos) <= tol2)
            return true;
    }
    return false;
}

void testDetect::rememberDeletedPoint(const QPointF& pos, bool isMarker)
{
    if (isDeletedPoint(pos, isMarker))
        return;
    DeletedPoint d;
    d.pos = pos;
    d.isMarker = isMarker;
    m_deletedPoints.push_back(d);
}

void testDetect::forgetDeletedPoint(const QPointF& pos, bool isMarker)
{
    const double tol2 = kDeletedPointTol * kDeletedPointTol;
    for (int i = 0; i < m_deletedPoints.size(); ++i) {
        if (m_deletedPoints[i].isMarker == isMarker
            && dist2(m_deletedPoints[i].pos, pos) <= tol2) {
            m_deletedPoints.remove(i);
            return;
        }
    }
}

void testDetect::updateResultTitle()
{
    if (!m_listTitle)
        return;
    int nCoded = 0, nMark = 0;
    for (const Marker& mk : m_markers) {
        if (mk.isMarker) ++nMark; else ++nCoded;
    }
    if (nCoded > 0 && nMark > 0)
        m_listTitle->setText(QStringLiteral("识别结果 (编码片 %1 · 标记点 %2)")
            .arg(nCoded).arg(nMark));
    else if (nCoded > 0)
        m_listTitle->setText(QStringLiteral("识别结果 (编码片 %1)").arg(nCoded));
    else if (nMark > 0)
        m_listTitle->setText(QStringLiteral("标记点结果 (%1)").arg(nMark));
    else
        m_listTitle->setText(QStringLiteral("识别结果"));
}

// 行号与 m_markers 下标一一对应。从大到小删除, 前面行的下标不会失效。
// 被删的图元只从 scene 摘除、列表项只 takeItem, 都留在撤销记录里。
int testDetect::removeMarkerRows(QVector<int> rows)
{
    if (rows.isEmpty() || m_markers.isEmpty())
        return 0;

    std::sort(rows.begin(), rows.end(), std::greater<int>());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

    UndoOp op;
    int removed = 0;
    m_updatingList = true;
    for (int row : rows) {
        if (row < 0 || row >= m_markers.size() || row >= m_list->count())
            continue;

        RemovedRow rec;
        rec.index = row;
        rec.marker = m_markers[row];
        rec.item = m_list->takeItem(row);

        // 记住删除位置: 再点一次同类识别时, 坐标相差 ≤5px 的结果不再加回。
        rememberDeletedPoint(rec.marker.pos, rec.marker.isMarker);

        // 摘除但不销毁: 撤销时 addItem 原样加回。
        if (rec.marker.contour)
            m_scene->removeItem(rec.marker.contour);
        if (rec.marker.dot)
            m_scene->removeItem(rec.marker.dot);
        if (rec.marker.label)
            m_scene->removeItem(rec.marker.label);

        op.rows.push_back(rec);
        m_markers.remove(row);

        // 保持"列表行号 == m_markers 下标"的不变量。
        for (int r = row; r < m_list->count(); ++r)
            m_list->item(r)->setData(Qt::UserRole, r);
        ++removed;
    }
    m_updatingList = false;

    if (!op.rows.isEmpty()) {
        m_undoStack.push_back(op);
        // 限制撤销深度; 超出时销毁最旧的记录(图元不在 scene 里, 必须手动删)。
        while (m_undoStack.size() > 20) {
            UndoOp old = m_undoStack.takeFirst();
            for (const RemovedRow& rec : old.rows) {
                delete rec.marker.contour;
                delete rec.marker.dot;
                delete rec.marker.label;
                delete rec.item;
            }
        }
    }

    m_pressedItem = nullptr;
    if (m_markers.isEmpty() && m_roiMode)
        m_btnRoi->setChecked(false);
    updateQualitySeparator();
    updateResultTitle();
    updateResultButtons();
    applyHighlights();
    return removed;
}

void testDetect::clearUndoStack()
{
    for (const UndoOp& op : m_undoStack) {
        for (const RemovedRow& rec : op.rows) {
            delete rec.marker.contour;
            delete rec.marker.dot;
            delete rec.marker.label;
            delete rec.item;
        }
    }
    m_undoStack.clear();
}

// 删除/撤销后 m_markers 变了, 重新标出"第一条较差结果"的那一行。
// 行号与 marker 下标仍一一对应, 这里只更新 Qt::UserRole+2 这个绘制标记。
void testDetect::updateQualitySeparator()
{
    int firstPoor = -1;
    for (int i = 0; i < m_markers.size(); ++i) {
        if (m_markers[i].lowQuality) { firstPoor = i; break; }
    }
    m_updatingList = true;
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem* item = m_list->item(row);
        const int idx = item->data(Qt::UserRole).toInt();
        item->setData(kFirstPoorRole, idx == firstPoor);
    }
    m_updatingList = false;
}

void testDetect::updateResultButtons()
{
    const bool has = !m_markers.isEmpty() && !m_busy;
    if (m_btnRemoveNoise)
        m_btnRemoveNoise->setEnabled(has);
    if (m_btnUndo)
        m_btnUndo->setEnabled(!m_undoStack.isEmpty() && !m_busy);
    if (m_btnExport)
        m_btnExport->setEnabled(has);
    if (m_btnRemovePoor) {
        bool hasPoor = false;
        for (const Marker& mk : m_markers) {
            if (mk.lowQuality) { hasPoor = true; break; }
        }
        m_btnRemovePoor->setEnabled(hasPoor && !m_busy);
    }
    if (m_btnRoi)
        m_btnRoi->setEnabled(has);
    if (m_btnClear)
        m_btnClear->setEnabled(has);
}

void testDetect::onUndo()
{
    if (m_busy || m_undoStack.isEmpty())
        return;

    UndoOp op = m_undoStack.takeLast();
    // 删除时是从大到小处理的, 恢复必须从小到大插回, 记录里的下标才仍然有效。
    std::sort(op.rows.begin(), op.rows.end(),
        [](const RemovedRow& a, const RemovedRow& b) { return a.index < b.index; });

    m_updatingList = true;
    for (const RemovedRow& rec : op.rows) {
        const int idx = qBound(0, rec.index, m_markers.size());
        m_markers.insert(idx, rec.marker);
        m_list->insertItem(idx, rec.item);
        if (rec.marker.contour)
            m_scene->addItem(rec.marker.contour);
        if (rec.marker.dot)
            m_scene->addItem(rec.marker.dot);
        if (rec.marker.label)
            m_scene->addItem(rec.marker.label);
        // 撤销删除 = 这个点重新算数, 从黑名单里拿掉, 否则重新识别时它反而不会回来。
        forgetDeletedPoint(rec.marker.pos, rec.marker.isMarker);
    }
    for (int r = 0; r < m_list->count(); ++r)
        m_list->item(r)->setData(Qt::UserRole, r);
    m_updatingList = false;

    m_pressedItem = nullptr;
    updateQualitySeparator();
    updateResultTitle();
    updateResultButtons();
    applyHighlights();
    statusBar()->showMessage(QStringLiteral("已撤销：恢复 %1 个结果").arg(op.rows.size()));
}

void testDetect::onExportResults()
{
    if (m_markers.isEmpty())
        return;

    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出识别结果"),
        QStringLiteral("detected_points.txt"),
        QStringLiteral("文本文件 (*.txt);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".txt");

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
            QStringLiteral("无法写入文件：\n%1\n\n%2").arg(path, f.errorString()));
        return;
    }

    // UTF-8 BOM: 中文系统上的记事本/Excel 能正确识别编码。
    f.write("\xEF\xBB\xBF");
    QTextStream out(&f);
    out.setCodec("UTF-8");

    out << QStringLiteral("# 靶标识别结果\n");
    out << QStringLiteral("# 导出时间: ")
        << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << '\n';
    out << QStringLiteral("# 坐标单位: 像素(去畸变图像坐标系)\n");
    out << QStringLiteral("# 类型\tID\tX\tY\t面积(平方像素)\t长短轴比\t亚像素\tRMS(px)\t点数\n");

    for (const Marker& mk : m_markers) {
        out << (mk.isMarker ? QStringLiteral("标记点") : QStringLiteral("编码片")) << '\t'
            << mk.id << '\t'
            << QString::number(mk.pos.x(), 'f', 3) << '\t'
            << QString::number(mk.pos.y(), 'f', 3) << '\t'
            << QString::number(mk.area, 'f', 1) << '\t'
            << QString::number(mk.aspect, 'f', 2) << '\t'
            << (mk.subpixel ? QStringLiteral("是") : QStringLiteral("否")) << '\t'
            << (mk.subpixel ? QString::number(mk.rms, 'f', 3) : QStringLiteral("-")) << '\t'
            << (mk.subpixel ? QString::number(mk.nPoints) : QStringLiteral("-")) << '\n';
    }
    out.flush();
    f.close();

    if (f.error() != QFileDevice::NoError) {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
            QStringLiteral("写入文件时出错：\n%1\n\n%2").arg(path, f.errorString()));
        return;
    }
    statusBar()->showMessage(QStringLiteral("已导出 %1 个结果：%2")
        .arg(m_markers.size()).arg(path));
}

void testDetect::onListContextMenu(const QPoint& pos)
{
    if (m_busy)
        return;
    QListWidgetItem* item = m_list->itemAt(pos);
    if (!item)
        return;

    QMenu menu(this);
    QAction* actRemove = menu.addAction(QStringLiteral("删除该结果"));
    if (menu.exec(m_list->viewport()->mapToGlobal(pos)) != actRemove)
        return;

    const int row = m_list->row(item);
    if (removeMarkerRows(QVector<int>{ row }) > 0)
        statusBar()->showMessage(QStringLiteral("已删除 1 个识别结果"));
}

void testDetect::onRemoveNoise()
{
    if (m_busy || m_markers.isEmpty())
        return;
    if (m_markers.size() < 2) {
        statusBar()->showMessage(QStringLiteral("至少需要 2 个识别结果才能按模板筛选"));
        return;
    }

    // ---- 模板选择 + 偏差设置(实时预览保留/删除数量) ----
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("删除干扰点 · 模板匹配"));

    QVBoxLayout* lay = new QVBoxLayout(&dlg);
    lay->setSpacing(8);

    QLabel* tip = new QLabel(QStringLiteral(
        "选择一个识别正确、有代表性的结果作为模板。\n"
        "轮廓面积与模板接近的结果保留，相差较大的删除（只看面积，不再看长短轴比）。"), &dlg);
    tip->setWordWrap(true);
    lay->addWidget(tip);

    QComboBox* combo = new QComboBox(&dlg);
    combo->setMinimumWidth(460);
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(24);
    for (int i = 0; i < m_markers.size(); ++i) {
        const Marker& mk = m_markers[i];
        const QString head = mk.isMarker
            ? QStringLiteral("M%1").arg(mk.id)
            : QStringLiteral("ID:%1").arg(mk.id);
        combo->addItem(QStringLiteral("%1   (%2, %3)   面积 %4")
            .arg(head)
            .arg(mk.pos.x(), 0, 'f', 3)
            .arg(mk.pos.y(), 0, 'f', 3)
            .arg(mk.area, 0, 'f', 1));
    }
    lay->addWidget(combo);

    QHBoxLayout* tolRow = new QHBoxLayout();
    tolRow->addWidget(new QLabel(QStringLiteral("允许偏差："), &dlg));
    QDoubleSpinBox* tol = new QDoubleSpinBox(&dlg);
    tol->setRange(5.0, 95.0);
    tol->setDecimals(0);
    tol->setSingleStep(5.0);
    tol->setValue(50.0);
    tol->setSuffix(QStringLiteral(" %"));
    tolRow->addWidget(tol);
    tolRow->addStretch(1);
    lay->addLayout(tolRow);

    QLabel* preview = new QLabel(&dlg);
    preview->setWordWrap(true);
    lay->addWidget(preview);

    QDialogButtonBox* bb = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    if (QPushButton* b = bb->button(QDialogButtonBox::Ok))
        b->setText(QStringLiteral("删除"));
    if (QPushButton* b = bb->button(QDialogButtonBox::Cancel))
        b->setText(QStringLiteral("取消"));
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    // 只按轮廓面积筛: 面积在模板 ±t 范围内才保留。
    // (倾斜/椭圆度不再参与判定 —— 形状差异交由长短轴相关参数和人工判断。)
    auto collectKill = [this, combo, tol]() -> QVector<int> {
        const int ti = combo->currentIndex();
        if (ti < 0 || ti >= m_markers.size())
            return {};
        const Marker& tpl = m_markers[ti];
        const double t = tol->value() / 100.0;
        auto close = [t](double v, double ref) {
            if (ref <= 0.0)
                return true;
            return v >= ref * (1.0 - t) && v <= ref * (1.0 + t);
        };
        QVector<int> kill;
        for (int i = 0; i < m_markers.size(); ++i) {
            if (i == ti)
                continue;
            if (!close(m_markers[i].area, tpl.area))
                kill.push_back(i);
        }
        return kill;
    };

    auto refreshPreview = [this, combo, tol, preview, collectKill]() {
        const int ti = combo->currentIndex();
        if (ti < 0 || ti >= m_markers.size())
            return;
        const Marker& tpl = m_markers[ti];
        const QVector<int> kill = collectKill();
        preview->setText(QStringLiteral(
            "模板：面积 %1，允许偏差 ±%2%\n"
            "保留 %3 个，删除 %4 个")
            .arg(tpl.area, 0, 'f', 1)
            .arg(tol->value(), 0, 'f', 0)
            .arg(m_markers.size() - kill.size())
            .arg(kill.size()));
    };
    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
        &dlg, [refreshPreview](int) { refreshPreview(); });
    connect(tol, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
        &dlg, [refreshPreview](double) { refreshPreview(); });
    refreshPreview();

    if (dlg.exec() != QDialog::Accepted)
        return;

    const QVector<int> kill = collectKill();
    if (kill.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("没有与模板差异过大的结果"));
        return;
    }
    const int n = removeMarkerRows(kill);
    statusBar()->showMessage(QStringLiteral("已按模板删除 %1 个干扰点").arg(n));
}

// 一键删除所有质量差的点(黄色轮廓): 残差超限 / 椭度太大 / 非亚像素。
// 走 removeMarkerRows() 同一条路: 记入删除黑名单、进撤销栈、重排列表行号。
void testDetect::onRemovePoor()
{
    if (m_busy || m_markers.isEmpty())
        return;

    QVector<int> rows;
    for (int i = 0; i < m_markers.size(); ++i) {
        if (m_markers[i].lowQuality)
            rows.push_back(i);
    }
    if (rows.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("没有质量差的点"));
        return;
    }

    const int n = removeMarkerRows(rows);
    statusBar()->showMessage(QStringLiteral("已删除 %1 个质量差的点（可撤销）").arg(n));
}


// ---------------------------------------------------------------- 画框 / 高亮

void testDetect::onToggleRoiMode(bool on)
{
    m_roiMode = on;
    m_btnRoi->setText(on ? QStringLiteral("结束画框") : QStringLiteral("开始画框"));

    // 退出画框只收多边形, 不清空勾选(勾选兼作浏览高亮)。
    applyHighlights();      // 内部会刷新状态栏, 提示语必须在其后

    statusBar()->showMessage(on
        ? QStringLiteral("画框模式: 已勾选的点会连成 ROI 区域")
        : QStringLiteral("已退出画框模式"));
}

void testDetect::onItemChanged(QListWidgetItem* item)
{
    Q_UNUSED(item);
    if (m_updatingList)
        return;
    applyHighlights();
}

void testDetect::onItemClicked(QListWidgetItem* item)
{
    if (m_updatingList)
        return;

    // 点勾选框时 Qt 已在 release 切换过(状态与按下时不同), 不能再翻一次;
    // 点行其他位置时 Qt 不处理, 这里补一次, 于是整行可点。
    const bool qtAlreadyToggled = (item == m_pressedItem) && (item->checkState() != m_pressedCheckState);
    m_pressedItem = nullptr;
    if (qtAlreadyToggled)
        return;

    item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
}

void testDetect::onClearSelection()
{
    m_updatingList = true;
    for (int i = 0; i < m_list->count(); ++i)
        m_list->item(i)->setCheckState(Qt::Unchecked);
    m_updatingList = false;
    applyHighlights();
}

void testDetect::applyHighlights()
{
    QVector<QPointF> selected;
    m_roiIds.clear();

    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem* item = m_list->item(row);
        const int idx = item->data(Qt::UserRole).toInt();
        if (idx < 0 || idx >= m_markers.size())
            continue;

        Marker& mk = m_markers[idx];
        // 勾选即高亮, 不要求先进入画框模式。
        const bool on = (item->checkState() == Qt::Checked);

        // 轮廓配色: 选中红色 / 正常青绿 / 非亚像素橙色虚线。
        // 用 cosmetic 画笔: 线宽是屏幕像素, 不随视图缩放变粗 —— 放大看轮廓包络时
        // 不会被一条几像素宽的线盖住真实边缘。
        auto screenPen = [](const QColor& c, qreal w, Qt::PenStyle style = Qt::SolidLine) {
            QPen p(c);
            p.setWidthF(w);
            p.setCosmetic(true);
            p.setStyle(style);
            return p;
        };
        QPen contourPen;
        if (on)
            contourPen = screenPen(QColor(255, 60, 60), 2.0);          // 勾选: 红色置顶
        else if (mk.lowQuality)
            contourPen = screenPen(QColor(255, 210, 40), 1.5,
                                   mk.subpixel ? Qt::SolidLine : Qt::DashLine);  // 质量差: 黄色
        else if (mk.subpixel)
            contourPen = screenPen(QColor(0, 255, 170), 1.0);          // 好: 青绿
        else
            contourPen = screenPen(QColor(255, 170, 60), 1.0, Qt::DashLine);
        mk.contour->setPen(contourPen);

        // 中心点固定为屏幕像素尺寸(ItemIgnoresTransformations), 放大后仍是准确的小点:
        // 半径 2px, 勾选时 2.5px。外圈一道深色描边保证白底上也看得见。
        const double dotR = on ? 2.5 : 2.0;
        mk.dot->setRect(-dotR, -dotR, dotR * 2.0, dotR * 2.0);
        mk.dot->setBrush(on ? QBrush(QColor(255, 60, 60, 235))
                            : QBrush(QColor(255, 220, 60, 220)));
        QPen dotPen(QColor(0, 0, 0, 170));
        dotPen.setWidthF(1.0);
        dotPen.setCosmetic(true);
        mk.dot->setPen(dotPen);

        mk.label->setHtml(idLabelHtml(mk.id, on, m_labelFontPx, mk.isMarker));

        if (on) {
            selected.push_back(mk.pos);
            m_roiIds.push_back(mk.id);
            mk.contour->setZValue(6);   // 高亮项置顶
            mk.dot->setZValue(7);
            mk.label->setZValue(8);
        }
        else {
            mk.contour->setZValue(2);
            mk.dot->setZValue(3);
            mk.label->setZValue(4);
        }
    }

    // 多边形始终按勾选点计算, 仅在画框模式下绘制。
    m_roiPolygon = buildRoiPolygon(selected);
    if (m_roiItem) {
        m_roiItem->setPolygon(m_roiPolygon);
        m_roiItem->setVisible(m_roiMode && m_roiPolygon.size() >= 3);
    }
    updateRoiStatus();
}

// 以质心为极点按极角排序, 连出来的多边形一定不自交, 与勾选先后无关
QPolygonF testDetect::buildRoiPolygon(const QVector<QPointF>& pts)
{
    if (pts.size() < 3)
        return QPolygonF();

    QPointF c(0.0, 0.0);
    for (const QPointF& p : pts)
        c += p;
    c /= (double)pts.size();

    QVector<QPointF> sorted = pts;
    std::sort(sorted.begin(), sorted.end(), [&c](const QPointF& a, const QPointF& b) {
        return std::atan2(a.y() - c.y(), a.x() - c.x())
             < std::atan2(b.y() - c.y(), b.x() - c.x());
    });
    return QPolygonF(sorted);
}

void testDetect::updateRoiStatus()
{
    const int n = m_roiIds.size();
    if (n == 0) {
        if (!m_hasImage)
            statusBar()->showMessage(QStringLiteral("请先导入图片"));
        else if (m_markers.isEmpty())
            statusBar()->showMessage(QStringLiteral(
                "点「开始识别编码片」、「识别标记点」或「识别所有标记点」运行检测"));
        else
            statusBar()->showMessage(resultSummary());
        return;
    }

    QString msg = QStringLiteral("已选 %1 个点").arg(n);
    if (m_roiPolygon.size() >= 3) {
        if (m_roiMode)
            msg += QStringLiteral("   →   ROI 面积 %1 平方像素").arg(polygonArea(m_roiPolygon), 0, 'f', 0);
        else
            msg += QStringLiteral("   （点「开始画框」把它连成 ROI）");
    }
    else {
        msg += QStringLiteral("   （再选 %1 个才能连成 ROI）").arg(3 - n);
    }
    statusBar()->showMessage(msg);
}


// ---------------------------------------------------------------- 视图 / 杂项

void testDetect::onFitView()
{
    if (!m_pixmapItem)
        return;
    // fitInView 期间临时改回 NoAnchor, 避免 AnchorUnderMouse 把适应窗口带偏。
    const QGraphicsView::ViewportAnchor saved = m_view->transformationAnchor();
    m_view->setTransformationAnchor(QGraphicsView::NoAnchor);
    m_view->fitInView(m_pixmapItem, Qt::KeepAspectRatio);
    m_view->setTransformationAnchor(saved);

    m_autoFit = true;
    updateZoomLabel();
}

void testDetect::updateZoomLabel()
{
    if (!m_zoomLabel)
        return;
    m_zoomLabel->setText(QStringLiteral("缩放 %1%   ")
        .arg(m_view->transform().m11() * 100.0, 0, 'f', 1));
}

// 灰度化模式下状态栏的像素读数: 坐标 + 灰度值 + 与边界阈值的关系。
// 读的是 m_viewMat(显示图, 与识别输入同源), scene 坐标原点即图像左上角,
// 所以视图缩放/平移后 mapToScene 仍能给出正确的像素下标。
void testDetect::updatePixelInfo(const QPoint& viewportPos)
{
    if (!m_pixelLabel)
        return;
    if (!m_hasImage || !m_host.grayscale || m_viewMat.empty()) {
        m_pixelLabel->clear();
        return;
    }

    const QPointF sp = m_view->mapToScene(viewportPos);
    const int x = (int)std::floor(sp.x());
    const int y = (int)std::floor(sp.y());
    if (x < 0 || y < 0 || x >= m_viewMat.cols || y >= m_viewMat.rows) {
        m_pixelLabel->clear();
        return;
    }

    int gray = 0;
    if (m_viewMat.channels() == 1) {
        gray = m_viewMat.at<uchar>(y, x);
    }
    else {
        // 兜底: 与 cv::cvtColor(BGR2GRAY) 用同一组权重。
        const cv::Vec3b& p = m_viewMat.at<cv::Vec3b>(y, x);
        gray = cvRound(0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2]);
    }

    QString text = QStringLiteral("像素 (%1, %2)  灰度 %3").arg(x).arg(y).arg(gray);
    const int thr = m_detector.params().boundaryThreshold;
    if (thr > 0) {
        text += (gray > thr)
            ? QStringLiteral("   > 边界阈值 %1（边界）").arg(thr)
            : QStringLiteral("   <= 边界阈值 %1（背景）").arg(thr);
    }
    m_pixelLabel->setText(text);
}

void testDetect::clearPixelInfo()
{
    if (m_pixelLabel)
        m_pixelLabel->clear();
}

bool testDetect::eventFilter(QObject* watched, QEvent* event)
{
    // 记录按下瞬间的勾选态, 供 onItemClicked 判断 Qt 是否已切换。
    if (m_list && watched == m_list->viewport()) {
        if (event->type() == QEvent::MouseButtonPress) {
            QMouseEvent* me = static_cast<QMouseEvent*>(event);
            m_pressedItem = m_list->itemAt(me->pos());
            m_pressedCheckState = m_pressedItem ? m_pressedItem->checkState() : Qt::Unchecked;
        }
    }
    else if (m_view && watched == m_view->viewport()) {
        switch (event->type()) {
        case QEvent::Wheel: {
            QWheelEvent* we = static_cast<QWheelEvent*>(event);
            const int dy = we->angleDelta().y();
            if (dy == 0)
                break;
            // 1.0015^120 ≈ 1.20: 滚一格约 20%; 指数缩放保证放大/缩小严格互逆。
            const double factor = std::pow(1.0015, double(dy));
            const double cur = m_view->transform().m11();
            // 限制缩放范围, 避免滚飞后找不回图。
            if ((factor > 1.0 && cur < 400.0) || (factor < 1.0 && cur > 0.002)) {
                m_view->scale(factor, factor);   // 以光标为中心
                m_autoFit = false;               // 手动缩放后不再自动适应窗口
                updateZoomLabel();
            }
            return true;    // 消费滚轮, 避免视图滚动
        }

        case QEvent::Resize:
            // ⚠ 必须判空: 无图时视口也会发 Resize, 而 fitInView(item) 内部不判空, 会崩。
            if (m_autoFit && m_pixmapItem)
                m_view->fitInView(m_pixmapItem, Qt::KeepAspectRatio);
            break;

        case QEvent::MouseButtonDblClick:
            if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
                onFitView();    // 双击恢复适应窗口
            break;

        case QEvent::MouseMove:
            // 灰度化时实时显示光标所在像素的灰度值(坐标换算在 updatePixelInfo 里)。
            updatePixelInfo(static_cast<QMouseEvent*>(event)->pos());
            break;

        case QEvent::Leave:
            clearPixelInfo();
            break;

        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void testDetect::clearResults()
{
    // 换图/换相机参数后坐标与目标都变了, 删除黑名单和编码片区域一并作废。
    m_deletedPoints.clear();
    m_codedRegions.clear();

    clearUndoStack();
    for (const Marker& mk : m_markers) {
        delete mk.contour;  // 析构时自动从 scene 摘除
        delete mk.dot;
        delete mk.label;
    }
    m_markers.clear();

    m_updatingList = true;
    m_list->clear();
    m_updatingList = false;
    m_pressedItem = nullptr;      // 列表项已销毁, 不能再留按下记录

    m_roiPolygon = QPolygonF();
    m_roiIds.clear();
    if (m_roiItem) {
        m_roiItem->setPolygon(QPolygonF());
        m_roiItem->setVisible(false);
    }
    updateResultButtons();
    if (m_listTitle)
        m_listTitle->setText(QStringLiteral("识别结果"));
}

void testDetect::setBusy(bool busy)
{
    m_busy = busy;
    m_btnImport->setEnabled(!busy);
    m_btnCamera->setEnabled(!busy);
    m_btnDetect->setEnabled(!busy && m_hasImage);
    m_btnDetectMarkers->setEnabled(!busy && m_hasImage);
    m_btnDetectAll->setEnabled(!busy && m_hasImage);
    m_btnFit->setEnabled(!busy);
    updateResultButtons();
    // 识别期间禁用整栏: 参数本身有快照保护, 但禁掉可避免"本次用的哪套参数"说不清。
    if (m_paramPane)
        m_paramPane->setEnabled(!busy);
}


// ------------------------------------------------- 第二页「点云拼接」(MergeMarks)

// MergeMarksLib 的 PCL/VTK 运行时目录, 前插到 PATH(先命中先赢):
//   1) <exe>/runtime —— 发布包自带; 客户机器上只有这一份, 因此必须排第一;
//   2) 本机绝对路径 —— 开发机直接运行 x64\{Debug,Release} 时使用。
// 不存在的目录塞进 PATH 无害, 所以不做存在性判断。
static const wchar_t* kMergeMarksRuntimeDirsDev =
    L"D:\\1yjy\\software\\newcomputer\\VTK\\VTK8.1.0\\install\\bin;"
    L"D:\\1yjy\\software\\newcomputer\\PCL 1.9.1\\bin;"
    L"D:\\1yjy\\software\\newcomputer\\PCL 1.9.1\\3rdParty\\FLANN\\bin;"
    L"D:\\1yjy\\software\\newcomputer\\PCL 1.9.1\\3rdParty\\Qhull\\bin;"
    L"D:\\1yjy\\software\\newcomputer\\PCL 1.9.1\\3rdParty\\VTK\\bin;"
    L"D:\\1yjy\\software\\newcomputer\\PCL 1.9.1\\3rdParty\\OpenNI2\\Redist";

// 组装要前插到 PATH 的目录串: runtime 优先, 其后是本机路径。
static QString mergeMarksRuntimeSearchPath()
{
    QStringList dirs;
    dirs << QDir::toNativeSeparators(
        QCoreApplication::applicationDirPath() + QStringLiteral("/runtime"));

    const QString dev = QString::fromWCharArray(kMergeMarksRuntimeDirsDev);
    dirs += dev.split(QLatin1Char(';'), Qt::SkipEmptyParts);

    return dirs.join(QLatin1Char(';'));
}

void testDetect::onTabChanged(int index)
{
    // 仅第二页需要; 已加载或已失败则跳过。
    if (index != 1 || m_mmPanel || m_mmLoadTried)
        return;

    // 先刷新提示再阻塞加载, 否则界面看起来像卡死。
    m_mmPlaceholder->setText(QStringLiteral(
        "正在加载点云拼接模块…\n(首次需要加载 MergeMarksLib 及其 PCL/VTK 运行时)"));
    statusBar()->showMessage(QStringLiteral("正在加载点云拼接模块…"));
    QApplication::processEvents();

    ensureMergeMarksLoaded();
}

void testDetect::ensureMergeMarksLoaded()
{
    m_mmLoadTried = true;

    const QString appDir = QCoreApplication::applicationDirPath();
    const QString dllPath = appDir + QStringLiteral("/MergeMarksLib.dll");
    if (!QFile::exists(dllPath)) {
        showMergeMarksError(QStringLiteral(
            "找不到 MergeMarksLib.dll。\n\n期望位置：\n%1\n\n"
            "请先编译 MergeMarks1215.sln 里的 MergeMarksLib 工程，"
            "再把生成的 dll 放到该位置。").arg(dllPath));
        return;
    }

    // ⚠ 必须在 LoadLibrary 之前把运行时目录加入 PATH: 加载器此时就要解析依赖,
    // 而 DLL 自身尚未加载, 无法代劳。不用 SetDefaultDllDirectories, 以免破坏
    // Qt DLL 的搜索路径。
    {
        const QString prefixes = mergeMarksRuntimeSearchPath();
        std::wstring path(reinterpret_cast<const wchar_t*>(prefixes.utf16()));
        if (!path.empty() && path.back() != L';')
            path += L';';
        wchar_t old[32767] = { 0 };
        if (::GetEnvironmentVariableW(L"PATH", old, 32766) > 0)
            path += old;
        ::SetEnvironmentVariableW(L"PATH", path.c_str());
    }

    HMODULE mod = ::LoadLibraryW(reinterpret_cast<const wchar_t*>(dllPath.utf16()));
    if (!mod) {
        const DWORD err = ::GetLastError();
        QString dirs = mergeMarksRuntimeSearchPath();
        dirs.replace(QLatin1Char(';'), QLatin1Char('\n'));
        showMergeMarksError(QStringLiteral(
            "加载 MergeMarksLib.dll 失败（Win32 错误码 %1）。\n\n"
            "多半是它依赖的 PCL/VTK 运行时 dll 没找到。请确认下面这些目录存在且完整：\n%2")
            .arg(err).arg(dirs));
        return;
    }
    m_mmModule = mod;

    auto create = reinterpret_cast<FnMergeMarksCreatePanel>(
        ::GetProcAddress(mod, "MergeMarks_CreatePanel"));
    m_mmDestroy = reinterpret_cast<FnMergeMarksDestroyPanel>(
        ::GetProcAddress(mod, "MergeMarks_DestroyPanel"));
    if (!create || !m_mmDestroy) {
        // 同样不卸载: FreeLibrary 会连锁卸载 PCL/VTK, 而 VTK 不支持进程内卸载。
        m_mmModule = nullptr;
        m_mmDestroy = nullptr;
        showMergeMarksError(QStringLiteral(
            "MergeMarksLib.dll 里没找到导出函数 MergeMarks_CreatePanel / MergeMarks_DestroyPanel，"
            "可能拷错了 dll。"));
        return;
    }

    m_mmPanel = create(m_mergeMarksPage);
    if (!m_mmPanel) {
        showMergeMarksError(QStringLiteral("MergeMarksLib.dll 已加载，但创建面板失败。"));
        return;
    }

    // 用真实面板替换占位提示。
    qobject_cast<QVBoxLayout*>(m_mergeMarksPage->layout())->addWidget(
        static_cast<QWidget*>(m_mmPanel), 1);
    m_mmPlaceholder->hide();
    statusBar()->showMessage(QStringLiteral("点云拼接模块已加载"));
}

void testDetect::showMergeMarksError(const QString& msg)
{
    m_mmPlaceholder->setText(msg);
    m_mmPlaceholder->show();
    statusBar()->showMessage(QStringLiteral("点云拼接模块加载失败，详见该页说明"));
}
