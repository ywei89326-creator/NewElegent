
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

#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
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
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
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

} // namespace


testDetect::testDetect(QWidget* parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);
    buildUi();
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
    m_btnDetect = new QPushButton(QStringLiteral("开始识别编码片"), tabDetect);
    // 复用同一套预处理与亚像素精化, 但不要求编码环、不解码。
    m_btnDetectMarkers = new QPushButton(QStringLiteral("识别标记点"), tabDetect);
    m_btnDetectMarkers->setToolTip(QStringLiteral(
        "识别普通圆形标记点（实心亮圆盘，即编码片中心圆）。\n"
        "算法与「开始识别编码片」相同，但不要求外围有编码环，也不解码 ID。"));
    m_btnRoi = new QPushButton(QStringLiteral("开始画框"), tabDetect);
    m_btnRoi->setCheckable(true);
    m_btnClear = new QPushButton(QStringLiteral("清空选择"), tabDetect);
    m_btnFit = new QPushButton(QStringLiteral("适应窗口"), tabDetect);
    // 与 F11 同一路径; 勾选态 = 当前是否为无边框全屏。
    m_btnFullScreen = new QPushButton(QStringLiteral("全屏 F11"), tabDetect);
    m_btnFullScreen->setCheckable(true);
    bar->addWidget(m_btnImport);
    bar->addWidget(m_btnDetect);
    bar->addWidget(m_btnDetectMarkers);
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
    m_view->viewport()->installEventFilter(this);   // resize / 滚轮缩放 / 双击

    // —— 右: 识别结果列表 ——
    m_list = new QListWidget(central);
    m_list->setMinimumWidth(300);
    m_list->setAlternatingRowColors(true);
    m_list->setUniformItemSizes(true);
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

    QWidget* resultPane = new QWidget(central);
    QVBoxLayout* rl = new QVBoxLayout(resultPane);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(4);
    rl->addWidget(m_listTitle);
    rl->addWidget(m_list, 1);

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
    connect(m_btnDetect, &QPushButton::clicked, this, &testDetect::onStartDetect);
    connect(m_btnDetectMarkers, &QPushButton::clicked, this, &testDetect::onStartDetectMarkers);
    connect(m_btnRoi, &QPushButton::toggled, this, &testDetect::onToggleRoiMode);
    connect(m_btnClear, &QPushButton::clicked, this, &testDetect::onClearSelection);
    connect(m_btnFit, &QPushButton::clicked, this, &testDetect::onFitView);
    connect(m_list, &QListWidget::itemChanged, this, &testDetect::onItemChanged);
    connect(m_list, &QListWidget::itemClicked, this, &testDetect::onItemClicked);

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
    m_btnRoi->setEnabled(false);
    m_btnClear->setEnabled(false);

    // 状态栏右侧常驻缩放比例。
    m_zoomLabel = new QLabel(central);
    statusBar()->addPermanentWidget(m_zoomLabel);

    statusBar()->showMessage(QStringLiteral("请先导入图片"));
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
// 默认用户只看到 kBasic 中的 8 项, 其余需密码解锁。
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
        QStringLiteral("minCandidateRadius")  // 候选最小半径
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
    QFormLayout* fCirc = nullptr;
    QFormLayout* fSub = nullptr;
    QFormLayout* fDec = nullptr;
    il->addWidget(makeParamGroup(inner, QStringLiteral("预处理"), &fPre));
    il->addWidget(makeParamGroup(inner, QStringLiteral("轮廓筛选"), &fCirc));
    m_groupSubpixel = makeParamGroup(inner, QStringLiteral("亚像素精化"), &fSub);
    il->addWidget(m_groupSubpixel);
    // 这两组全部是高级参数, 锁定时整组隐藏。
    m_groupDecode = makeParamGroup(inner, QStringLiteral("解码与去重"), &fDec);
    il->addWidget(m_groupDecode);

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
        QStringLiteral("腐蚀操作的结构元边长(奇数, 默认 5; 偶数会自动夹成奇数)。\n"
                       "调大 → 白色区域被啃得更瘦, 细窄的反光条纹/毛刺先消失, 误检减少;\n"
                       "但过大也会把编码块啃断、把中心盘啃小(半径 R 变小)。\n"
                       "远距离小靶标、编码块很细时必须调小到 3。"),
        3, 21, &DetectorParams::erodeKernel);
    addDoubleField(fPre, QStringLiteral("bigBlobAreaRatio"), QStringLiteral("大块白斑"),
        QStringLiteral("剔除大面积白色连通域的面积占比阈值(默认 0.02 = 全图 2%)。\n"
                       "连通域面积 ≥ 该比例 × 全图面积就整个删掉(只删它自己的像素)。\n"
                       "工件反光加工面在二值化后常整片连成一大块, 先删掉可明显提速。\n"
                       "调小 → 更激进(删得更多); 调大 → 更保守; 0 = 完全不删。\n"
                       "靶标与反光面粘连、被一起删掉时, 先填 0 验证是不是这个原因。"),
        0.0, 0.5, 3, &DetectorParams::bigBlobAreaRatio);

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
    m_btnUnlock->setToolTip(QStringLiteral("输入密码后显示全部 32 项参数。\n"
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
            : QStringLiteral("参数已生效；改动后需点「开始识别编码片」或「识别标记点」重新识别"));
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
    updateParamDependents();
    saveParamsIni();
    setParamsDirty(false);

    // 现有结果由旧参数产生, 明确提示重新识别。
    statusBar()->showMessage(m_markers.isEmpty()
        ? QStringLiteral("参数已应用")
        : QStringLiteral("参数已应用 —— 点「开始识别编码片」或「识别标记点」用新参数重新识别"));
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
    loadParamsToWidgets(def);
    m_detector.setParams(def);
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
}

bool testDetect::loadParamsIni()
{
    DetectorParams p = TargetDetector::defaultParams();
    if (!readParamsFromSettings(p, m_fields, QStringLiteral("detectParams"), paramsIniPath()))
        return false;
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
    loadParamsToWidgets(p);
    m_detector.setParams(p);
    loadParamsToWidgets(m_detector.params());
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
        statusBar()->showMessage(QStringLiteral("参数已载入：%1 —— 点「开始识别编码片」或「识别标记点」重新识别").arg(name));
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
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const cv::Mat disp = m_detector.undistortImage(m_srcMat);
    QApplication::restoreOverrideCursor();

    clearResults();
    showImage(disp);

    m_btnRoi->setChecked(false);
    setBusy(false);

    statusBar()->showMessage(QStringLiteral("已载入 %1   (%2 x %3)")
        .arg(QFileInfo(path).fileName()).arg(raw.cols).arg(raw.rows));
}

void testDetect::showImage(const cv::Mat& bgr)
{
    m_scene->clear();               // 连同图元一起销毁
    m_markers.clear();
    m_pixmapItem = nullptr;
    m_roiItem = nullptr;

    cv::Mat rgb;
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


// ---------------------------------------------------------------- 开始识别编码片

void testDetect::onStartDetect()
{
    if (!m_hasImage || m_busy)
        return;

    m_markerMode = false;       // 编码片模式: 标签显示 ID:n
    clearResults();
    setBusy(true);
    statusBar()->showMessage(QStringLiteral("识别中…"));

    cv::Mat input = m_srcMat;   // 浅拷贝共享像素, 工作线程只读
    m_thread = QThread::create([this, input]() mutable {
        const std::vector<DetectionResult> results = m_detector.detectTarget(input);
        // 切回 GUI 线程再更新界面
        QMetaObject::invokeMethod(this, [this, results]() { finishDetect(results); },
                                  Qt::QueuedConnection);
    });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->start();
}

// 与 onStartDetect 相同的线程模型, 调用 detectMarkers。
// 不做解码, 结果 id 恒为 -1, 界面序号由 finishDetect 生成。
void testDetect::onStartDetectMarkers()
{
    if (!m_hasImage || m_busy)
        return;

    m_markerMode = true;
    clearResults();
    setBusy(true);
    statusBar()->showMessage(QStringLiteral("标记点识别中…"));

    cv::Mat input = m_srcMat;   // 浅拷贝共享像素, 工作线程只读
    m_thread = QThread::create([this, input]() mutable {
        std::vector<DetectionResult> results;
        try {
            results = m_detector.detectMarkers(input);
        }
        catch (const std::exception& e) {
            // 异常不能穿出线程函数(会 std::terminate), 也不能让界面停在 busy。
            const QString msg = QString::fromLocal8Bit(e.what());
            QMetaObject::invokeMethod(this, [this, msg]() {
                setBusy(false);
                statusBar()->showMessage(QStringLiteral("标记点识别失败"));
                QMessageBox::warning(this, QStringLiteral("识别失败"), msg);
            }, Qt::QueuedConnection);
            return;
        }
        QMetaObject::invokeMethod(this, [this, results]() { finishDetect(results); },
                                  Qt::QueuedConnection);
    });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->start();
}

void testDetect::finishDetect(const std::vector<DetectionResult>& results)
{
    m_updatingList = true;
    for (size_t i = 0; i < results.size(); ++i) {
        const DetectionResult& r = results[i];

        Marker mk;
        // 标记点无编码 ID(DLL 返回 -1), 用 1 起的序号作为界面标识。
        mk.id = m_markerMode ? (int)i + 1 : (int)r.id;
        mk.isMarker = m_markerMode;
        mk.pos = QPointF(r.center.x, r.center.y);
        mk.subpixel = r.subpixel;

        // 优先画亚像素椭圆, 退化时按圆。
        mk.contour = new QGraphicsPathItem();
        if (r.hasEllipse)
            mk.contour->setPath(contourPath(r.ellipse));
        else
            mk.contour->setPath(contourPath(r.center, qMax((double)r.radius, 1.0)));
        m_scene->addItem(mk.contour);
        mk.contour->setZValue(2);

        // 中心小点, 不遮挡轮廓。
        mk.dot = m_scene->addEllipse(mk.pos.x() - m_markRadius * 0.3,
            mk.pos.y() - m_markRadius * 0.3, m_markRadius * 0.6, m_markRadius * 0.6);
        mk.dot->setZValue(3);

        mk.label = new QGraphicsTextItem();
        // 忽略视图变换: 标签保持固定屏幕字号, 放大时不遮挡轮廓。
        mk.label->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
        mk.label->setHtml(idLabelHtml(mk.id, false, m_labelFontPx, mk.isMarker));
        mk.label->setPos(mk.pos);   // HTML 的 <br> 让文字落在锚点下方
        m_scene->addItem(mk.label);
        mk.label->setZValue(3);

        m_markers.push_back(mk);

        // 坐标保留 3 位小数(亚像素结果, 不能取整)。
        // RMS 是拟合残差, 用于相对比较, 不是标定精度; 非亚像素结果明确标注。
        QString text = (m_markerMode
            ? QStringLiteral("  M%1   (%2, %3)   ")
            : QStringLiteral("  ID: %1   (%2, %3)   "))
            .arg(mk.id)
            .arg(mk.pos.x(), 0, 'f', 3)
            .arg(mk.pos.y(), 0, 'f', 3);
        if (r.subpixel)
            text += QStringLiteral("±%1px / %2pt").arg(r.rms, 0, 'f', 3).arg(r.nPoints);
        else
            text += QStringLiteral("(!)非亚像素");

        QListWidgetItem* item = new QListWidgetItem(text, m_list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        item->setData(Qt::UserRole, (int)i);        // 关联 m_markers 下标
        if (!r.subpixel)
            item->setForeground(QBrush(QColor(255, 170, 60)));   // 非亚像素: 橙色提醒
    }
    m_updatingList = false;

    m_listTitle->setText((m_markerMode ? QStringLiteral("标记点结果 (%1)")
                                       : QStringLiteral("识别结果 (%1)")).arg(results.size()));

    applyHighlights();
    setBusy(false);

    const QString what = m_markerMode ? QStringLiteral("标记点") : QStringLiteral("编码点");
    if (results.empty()) {
        statusBar()->showMessage(QStringLiteral("未识别到%1").arg(what));
    }
    else {
        int nSub = 0;
        double rmsSum = 0.0, rmsMax = 0.0;
        for (const DetectionResult& r : results) {
            if (!r.subpixel) continue;
            ++nSub;
            rmsSum += r.rms;
            rmsMax = qMax(rmsMax, (double)r.rms);
        }
        QString msg = QStringLiteral("识别到 %1 个%2  |  亚像素 %3 个")
            .arg(results.size()).arg(what).arg(nSub);
        if (nSub > 0)
            msg += QStringLiteral("  残差 RMS 均值 %1px / 最大 %2px")
                .arg(rmsSum / nSub, 0, 'f', 3).arg(rmsMax, 0, 'f', 3);
        if (nSub < (int)results.size())
            msg += QStringLiteral("  (!) %1 个退回整数轮廓, 见橙虚线")
                .arg((int)results.size() - nSub);
        statusBar()->showMessage(msg);
    }
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

        // 轮廓配色: 选中红色粗线 / 正常青绿 / 非亚像素橙色虚线。
        QPen contourPen;
        if (on)
            contourPen = QPen(QColor(255, 60, 60), m_penW * 2.2);
        else if (mk.subpixel)
            contourPen = QPen(QColor(0, 255, 170), m_penW * 1.3);
        else
            contourPen = QPen(QColor(255, 170, 60), m_penW * 1.3, Qt::DashLine);
        mk.contour->setPen(contourPen);

        const double r = m_markRadius * (on ? 0.5 : 0.3);
        mk.dot->setRect(mk.pos.x() - r, mk.pos.y() - r, r * 2, r * 2);
        mk.dot->setBrush(on ? QBrush(QColor(255, 60, 60, 235))
                            : QBrush(QColor(255, 220, 60, 200)));
        mk.dot->setPen(Qt::NoPen);

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
        if (m_markers.isEmpty())
            statusBar()->showMessage(m_hasImage
                ? QStringLiteral("点「开始识别编码片」或「识别标记点」运行检测")
                : QStringLiteral("请先导入图片"));
        else
            statusBar()->showMessage(QStringLiteral("识别到 %1 个%2")
                .arg(m_markers.size())
                .arg(m_markerMode ? QStringLiteral("标记点") : QStringLiteral("编码点")));
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

        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void testDetect::clearResults()
{
    for (const Marker& mk : m_markers) {
        delete mk.contour;  // 析构时自动从 scene 摘除
        delete mk.dot;
        delete mk.label;
    }
    m_markers.clear();

    m_updatingList = true;
    m_list->clear();
    m_updatingList = false;

    m_roiPolygon = QPolygonF();
    m_roiIds.clear();
    if (m_roiItem) {
        m_roiItem->setPolygon(QPolygonF());
        m_roiItem->setVisible(false);
    }
    if (m_listTitle)
        m_listTitle->setText(QStringLiteral("识别结果"));
}

void testDetect::setBusy(bool busy)
{
    m_busy = busy;
    m_btnImport->setEnabled(!busy);
    m_btnDetect->setEnabled(!busy && m_hasImage);
    m_btnDetectMarkers->setEnabled(!busy && m_hasImage);
    m_btnFit->setEnabled(!busy);
    m_btnRoi->setEnabled(!busy && !m_markers.isEmpty());
    m_btnClear->setEnabled(!busy && !m_markers.isEmpty());
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
