
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
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
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


// fontPx 是**屏幕像素**字号 —— 标签图元带 ItemIgnoresTransformations(见 finishDetect)
// 尺寸不随视图缩放变化, 所以放大看亚像素轮廓时标签不会膨胀成一块招牌挡住画面。
// 开头的 <br> 把文字压到锚点下方一行, 免得正好盖住要观察的轮廓
// marker=true 时显示 M1/M2...(标记点没有编码 ID, id 只是本次结果里的序号), 否则沿用 ID:n
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

// 亚像素轮廓路径。故意**不采样成多边形** —— 折线在放大几十倍后能看出棱角, 而 QPainterPath
// 的圆弧段做仿射变换后仍是平滑曲线, 放到多大都不会有折线感。
// (圆是用 4 段三次贝塞尔近似的, 半径 11px 时最大径向误差约 0.003px, 远小于这里的亚像素量级。)
QPainterPath contourPath(const cv::RotatedRect& e)
{
    QPainterPath unit;
    unit.addEllipse(QPointF(0.0, 0.0), 1.0, 1.0);

    // 按 OpenCV RotatedRect 的约定: 点 = R(θ) · S(a, b) · (cos t, sin t),
    // 与 DLL 内部 cv::ellipse 画出来的方向一致, 不会镜像或差 90°。
    // QTransform 的构造参数顺序是 (m11, m12, m21, m22, dx, dy)。
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

// 多边形面积(鞋带公式), 单位是图像像素
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
    // 识别线程还在跑的话等它结束: 回调是打到 this 上的, 对象析构后再回调就是悬空指针
    if (m_thread && m_thread->isRunning())
        m_thread->wait();

    // 第二页: 销毁面板(它背后是 MergeMarks1215 那个 QMainWindow)。
    // 必须在 DLL 卸载之前 —— 面板里的 qvtkWidget 持有 PCL 建出来的 VTK 对象,
    // DLL 一走这些就全成了悬空指针。
    if (m_mmPanel && m_mmDestroy) {
        m_mmDestroy(m_mmPanel);
        m_mmPanel = nullptr;
    }

 
    //
    // FreeLibrary(MergeMarksLib.dll) 会让它的引用计数归零, 加载器随即连锁卸载它那一大批
    // 依赖(pcl_*_debug.dll、vtk*_d.dll、vtkGUISupportQt-8.1_d.dll ...), 而 VTK 并不支持
    // 在进程内被卸载: 各个 VTK DLL 里的 vtkObjectFactory 都注册在 vtkCommonCore 的全局
    // 注册表里, 一旦卸载顺序错开, 那个注册表里就留下指向已卸载模块的悬空指针, 等
    // vtkCommonCore 自己卸载时再去访问它, 就是 0xC0000005 —— 异常表现为"报在 FreeLibrary
    // 这一行、位置在 vtkCommonCore-8.1_d.dll 里", 但根因是卸载 VTK 这件事本身不安全。
    //
    // 而且退出时卸载它没有任何收益: 进程马上就结束了, 省不下内存。
    // m_mmModule 成员保留, 只是不再主动释放; 若将来确实需要中途卸载,
    // 得先确认那一批 VTK/PCL 依赖没有任何别的引用者, 否则同样会崩。
    // if (m_mmModule) {
    //     ::FreeLibrary(static_cast<HMODULE>(m_mmModule));
    //     m_mmModule = nullptr;
    // }
}


// ---------------------------------------------------------------- 界面搭建

void testDetect::buildUi()
{
    setWindowTitle(QStringLiteral("标记点工具"));
    // showMaximized() 之外还给一个最小尺寸: 再小右侧的参数栏就会窄到标签被截断,
    // 图像视图也失去意义。resize() 的值是"用户取消最大化后的还原尺寸"。
    setMinimumSize(1024, 680);
    resize(1280, 820);

    QWidget* central = centralWidget();
    QVBoxLayout* root = new QVBoxLayout(central);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(0);

    // 两个功能各占一个标签页: 第一页 = 靶标识别(原本的界面原封不动搬进来),
    // 第二页 = 点云拼接(MergeMarks)。第二页是**懒加载**, 见 onTabChanged。
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
    // 标记点识别复用同一套预处理 + 亚像素精化, 只是不要求外围有编码环、也不解码。
    m_btnDetectMarkers = new QPushButton(QStringLiteral("识别标记点"), tabDetect);
    m_btnDetectMarkers->setToolTip(QStringLiteral(
        "识别普通圆形标记点（实心亮圆盘，即编码片中心圆）。\n"
        "算法与「开始识别编码片」相同，但不要求外围有编码环，也不解码 ID。"));
    m_btnRoi = new QPushButton(QStringLiteral("开始画框"), tabDetect);
    m_btnRoi->setCheckable(true);
    m_btnClear = new QPushButton(QStringLiteral("清空选择"), tabDetect);
    m_btnFit = new QPushButton(QStringLiteral("适应窗口"), tabDetect);
    // 全屏按钮放在最右: 与 F11 同一条路径。勾选态 = 当前是否处于无边框全屏。
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

    // —— 左: 最终识别结果画面 ——
    m_scene = new QGraphicsScene(this);
    m_view = new QGraphicsView(m_scene, central);
    m_view->setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    m_view->setDragMode(QGraphicsView::ScrollHandDrag);   // 左键拖拽平移
    // 视口底色与主题一致(theme.qss 里的视口色 #1a2028)。必须在这里用代码设:
    // QGraphicsView 的显式背景刷会压过样式表, QSS 里写 background 是无效的。
    m_view->setBackgroundBrush(QBrush(QColor(0x1a, 0x20, 0x28)));
    m_view->setAlignment(Qt::AlignCenter);
    // 滚轮缩放时以鼠标所指的位置为锚点, 而不是以视图中心
    m_view->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    m_view->viewport()->installEventFilter(this);   // resize / 滚轮缩放 / 双击

    // —— 右: 识别结果列表 ——
    m_list = new QListWidget(central);
    m_list->setMinimumWidth(300);
    m_list->setAlternatingRowColors(true);
    m_list->setUniformItemSizes(true);
    // 等宽字体: 坐标是变长小数, 比例字体下小数点对不齐, 一列数字看着是歪的
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPixelSize(13);
    m_list->setFont(mono);
    // 用勾选框决定 ROI 顶点, 不用行选中: 行选中会自带一层高亮, 和"已选中要连框"混在一起看不清。
    // 行不参与选中, 但**点行任意位置都等同于点勾选框**(见 onItemClicked), 不必去精准点那个小方框。
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    // 必须用事件过滤器记录按下瞬间的勾选状态, 不能用 QListWidget::itemPressed:
    // 点在勾选框上时 QStyledItemDelegate::editorEvent 会消费掉 MouseButtonPress,
    // QAbstractItemView::mousePressEvent 提前 return, 那句 emit pressed 根本到不了。
    m_list->viewport()->installEventFilter(this);

    m_listTitle = new QLabel(QStringLiteral("识别结果"), central);
    // 起个名字, 让 theme.qss 能把它做成强调色小标题(宿主控件都没设 objectName)
    m_listTitle->setObjectName(QStringLiteral("sectionTitle"));

    QWidget* resultPane = new QWidget(central);
    QVBoxLayout* rl = new QVBoxLayout(resultPane);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(4);
    rl->addWidget(m_listTitle);
    rl->addWidget(m_list, 1);

    // —— 右下: 参数设置 ——
    QWidget* paramPane = new QWidget(central);
    buildParamPane(paramPane);

    // 右侧竖向分成「识别结果」和「参数设置」两块。用垂直 splitter 而不是固定布局:
    // 结果条数 0~30+ 与参数栏高度需求互相挤, 让用户自己拖比例最省事;
    // childrenCollapsible=false 避免一不小心把某一块拖成 0 高度(参数就再也点不到了)。
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
    split->setChildrenCollapsible(false);   // 图像视图/右侧栏都不许被拖没
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    split->setSizes({ 900, 360 });
    m_view->setMinimumWidth(360);           // 同理: 图像区也要留得住
    root1->addWidget(split, 1);

    m_tabs->addTab(tabDetect, QStringLiteral("靶标识别"));

    // —— 第二页: 点云拼接(MergeMarks) ——
    // 占位控件先摆上, 真正的内容等第一次切到本页时再 LoadLibrary 出来(见 onTabChanged)。
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

    // 用 QShortcut 而不是重写 keyPressEvent: 快捷键在焦点控件之前处理, 所以焦点停在
    // 某个数字框里时 F11 依然能到窗口(否则会被控件先吃掉, 时灵时不灵)。
    QShortcut* scFull = new QShortcut(QKeySequence(Qt::Key_F11), this);
    scFull->setContext(Qt::WindowShortcut);
    connect(scFull, &QShortcut::activated, this, &testDetect::onToggleFullScreen);

    // Esc 只在全屏时启用: 平时把 Esc 留给控件自己(例如数字框撤销输入)。
    m_shortcutEsc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    m_shortcutEsc->setContext(Qt::WindowShortcut);
    m_shortcutEsc->setEnabled(false);
    connect(m_shortcutEsc, &QShortcut::activated, this, &testDetect::onToggleFullScreen);

    m_btnDetect->setEnabled(false);
    m_btnDetectMarkers->setEnabled(false);
    m_btnRoi->setEnabled(false);
    m_btnClear->setEnabled(false);

    // 状态栏右侧常驻显示当前缩放倍数
    m_zoomLabel = new QLabel(central);
    statusBar()->addPermanentWidget(m_zoomLabel);

    statusBar()->showMessage(QStringLiteral("请先导入图片"));
}


// ---------------------------------------------------------------- 参数设置栏
//
// 这一栏对应 schneider0807 里的 DetectorParams(TargetDetector.h)。文字说明写在
// 工具提示里而非标签上: 标签短(≤6 字)才能在 340px 宽的栏里排下"标签 + 数字框"两列,
// 否则标签换行会把行高撑得七零八落。
//
// 生效时机: 改控件 → 点【应用参数】→ 一次性 setParams 给识别器 + 落盘 settings.ini。
// 之所以不做"边改边生效", 是因为识别一张 1200 万像素的图要若干秒, 拖动数字框时
// 反复触发会把界面拖住; 而显式一步也让"现在屏幕上这些结果到底是哪套参数跑的"不含糊。

namespace {

// 两个提示色, 与 theme.qss 里的次要文字色/警示色保持一致(样式表够不到动态状态, 只能代码里给)
const char* const kHintColorIdle = "#8b9bb0";
const char* const kHintColorDirty = "#ffc35c";

// 建一个卡片式分组(QGroupBox 的底色/圆角由 theme.qss 统一给), 返回内部表单布局。
QGroupBox* makeParamGroup(QWidget* parent, const QString& title, QFormLayout** formOut)
{
    QGroupBox* g = new QGroupBox(title, parent);
    QFormLayout* f = new QFormLayout(g);
    f->setContentsMargins(8, 6, 8, 6);
    f->setSpacing(4);
    f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // 字段保持自然宽度并左对齐: 默认的 ExpandingFieldsGrow 会把数字框拉满整行,
    // 窄栏里一行行看着像在"撑", 固定宽度反而更整齐也更好扫读。
    f->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    f->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    if (formOut)
        *formOut = f;
    return g;
}

// 三种参数控件归一到 double, 于是 m_fields 上的所有操作都不必再区分控件类型。
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

} // namespace

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
    sb->setAccelerated(true);            // 长按/滚轮可以加速
    sb->setKeyboardTracking(false);      // 只在输入落定时发信号, 不要每敲一个键都算"改过了"
    sb->setFixedWidth(96);
    sb->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sb->setToolTip(tip);
    form->addRow(label, sb);
    connect(sb, QOverload<int>::of(&QSpinBox::valueChanged), this, &testDetect::onParamEdited);

    ParamField f;
    f.key = key;
    f.w = sb;
    f.group = qobject_cast<QGroupBox*>(form->parentWidget());
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
    // 步长: 取"最小有效位"和"值域 1/200"里较大的那个 —— 小数位多的参数(如 0.0005)
    // 用最小有效位刚好一格一格走, 而 0~10000 这种大范围则按 50 一格, 不然要按到手软。
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

    // 三十来个参数在 340px 宽的栏里必然超出一屏高度, 所以套一个滚动区:
    // 这样窗口变矮时是"出现滚动条", 而不是把每一行压扁。
    m_paramScroll = new QScrollArea(paramPane);
    m_paramScroll->setWidgetResizable(true);
    m_paramScroll->setFrameShape(QFrame::NoFrame);
    m_paramScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 视口不自己填背景, 让它透出页签底色。视口是滚动区的独立子控件, QSS 里给
    // QScrollArea 写 background 改不到它 —— 同 CLAUDE.md 记的"部分颜色只能在代码里改"。
    if (m_paramScroll->viewport())
        m_paramScroll->viewport()->setAutoFillBackground(false);

    QWidget* inner = new QWidget;
    inner->setObjectName(QStringLiteral("paramPane"));   // 透明背景靠 QSS 的 ID 选择器给
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
    il->addWidget(makeParamGroup(inner, QStringLiteral("解码与去重"), &fDec));

    // ---- 预处理 ----
    m_chkOtsu = addBoolField(fPre, QStringLiteral("useOtsu"), QStringLiteral("大津法"),
        QStringLiteral("是否用大津法自动求二值化阈值(默认开)。\n"
                       "取消后改用下面的「固定阈值」, 适合整体偏暗/偏亮、\n"
                       "大津法切不干净导致靶标与背景连成一片的图。"),
        &DetectorParams::useOtsu);
    m_spinFixedThr = addIntField(fPre, QStringLiteral("fixedThreshold"), QStringLiteral("固定阈值"),
        QStringLiteral("二值化灰度阈值, 仅当「大津法」取消勾选时生效。\n"
                       "调低 → 更多像素算作白(前景变多); 调高 → 更少。\n"
                       "典型范围 60~180。"),
        1, 254, &DetectorParams::fixedThreshold);
    addIntField(fPre, QStringLiteral("erodeKernel"), QStringLiteral("腐蚀核"),
        QStringLiteral("腐蚀操作的结构元边长(奇数, 默认 5)。\n"
                       "调大 → 白色区域被啃得更瘦, 细窄的反光条纹/毛刺会先消失,\n"
                       "但过大也会把靶标编码块啃断。"),
        3, 21, &DetectorParams::erodeKernel);
    addDoubleField(fPre, QStringLiteral("bigBlobAreaRatio"), QStringLiteral("大块白斑"),
        QStringLiteral("剔除大面积白色连通域的面积占比阈值(默认 0.02 = 全图 2%)。\n"
                       "工件反光加工面在二值化后常整片连成一大块, 用这个把它先删掉,\n"
                       "后面的轮廓/Zernike 只面对真靶标, 明显提速。填 0 = 不删任何大块。"),
        0.0, 0.5, 3, &DetectorParams::bigBlobAreaRatio);

    // ---- 轮廓筛选 ----
    addDoubleField(fCirc, QStringLiteral("maxAspect"), QStringLiteral("长短轴比"),
        QStringLiteral("候选轮廓的长短轴比上限(默认 1.9)。中心圆盘接近正圆 ≈1,\n"
                       "细长的跑道形编码块比值更大。调小 → 候选更少更干净,\n"
                       "调大 → 收进更多圆润的编码块(靠后面的解码和去重再筛掉)。"),
        1.0, 10.0, 2, &DetectorParams::maxAspect);
    addDoubleField(fCirc, QStringLiteral("minDiscFill"), QStringLiteral("圆盘亮占比"),
        QStringLiteral("候选轮廓最小外接圆内“亮”像素的占比下限(默认 0.7)。\n"
                       "实心中心圆盘的外接圆几乎全白(≈1); 编码块/弧段的外接圆\n"
                       "远大于它的白色实体, 占比明显偏低。调高 → 更严。"),
        0.0, 1.0, 2, &DetectorParams::minDiscFill);
    addIntField(fCirc, QStringLiteral("brightThr"), QStringLiteral("亮像素阈值"),
        QStringLiteral("判断像素“亮”的灰度阈值(默认 120), 同时用于圆盘亮占比\n"
                       "和去重时的黑白评分。偏暗的图调低一些。"),
        0, 255, &DetectorParams::brightThr);
    addDoubleField(fCirc, QStringLiteral("minRing"), QStringLiteral("编码带内沿"),
        QStringLiteral("编码环跑道块所在径向带的内沿, 单位是中心圆盘半径 R 的倍数\n"
                       "(默认 2.3)。用比例而不是绝对像素, 相机远近变化时自动等比。"),
        1.0, 10.0, 2, &DetectorParams::minRing);
    addDoubleField(fCirc, QStringLiteral("maxRing"), QStringLiteral("编码带外沿"),
        QStringLiteral("编码环径向带的外沿 ×R(默认 4.2)。\n"
                       "注意它和「解码与去重」组里的编码带外沿(默认 4.1)不是同一个数:\n"
                       "这里判“是否被编码带环绕”, 那里是解码时的采样范围, 别改成一样。"),
        1.0, 10.0, 2, &DetectorParams::maxRing);
    addIntField(fCirc, QStringLiteral("minRingDirHits"), QStringLiteral("方向命中数"),
        QStringLiteral("环上共采样 24 个方向, 至少这么多个方向看到编码块, 才认为该候选\n"
                       "被编码带环绕(默认 4)。调高 → 只认真被编码环包围的圆盘, 更严; \n"
                       "编码块缺失/遮挡多时可适当调低。"),
        0, 24, &DetectorParams::minRingDirHits);
    addDoubleField(fCirc, QStringLiteral("approxEpsRatio"), QStringLiteral("逼近精度"),
        QStringLiteral("多边形逼近的精度, 单位是轮廓周长的倍数(默认 0.03)。\n"
                       "只用来数顶点个数, 不参与椭圆拟合。调大 → 逼近更粗, 顶点更少。"),
        0.0005, 0.5, 4, &DetectorParams::approxEpsRatio);
    addIntField(fCirc, QStringLiteral("minApproxVertices"), QStringLiteral("最少顶点数"),
        QStringLiteral("多边形逼近后顶点数少于该值的轮廓整个丢弃(默认 5)。\n"
                       "小靶标(远距离)顶点本来就少, 识别不到远处靶标时可以调低。"),
        3, 1000, &DetectorParams::minApproxVertices);
    addDoubleField(fCirc, QStringLiteral("minCandidateRadius"), QStringLiteral("候选最小半径"),
        QStringLiteral("候选的最小半径(像素, 默认 1.0)。低于它的轮廓和外接圆直接丢弃,\n"
                       "用来挡掉二值化毛刺。常见量级 0.5~3, 不要填太大,\n"
                       "否则远距离小靶标会被整批丢掉。"),
        0.0, 200.0, 2, &DetectorParams::minCandidateRadius);

    // ---- 亚像素精化 ----
    m_chkSubpixel = addBoolField(fSub, QStringLiteral("subpixelEnable"), QStringLiteral("启用亚像素"),
        QStringLiteral("是否做 Zernike 亚像素精化(默认开)。\n"
                       "取消后中心直接用整数轮廓的拟合结果, 精度掉到 ~0.3px 量级,\n"
                       "结果列表里会全部标成橙色的「(!)非亚像素」——\n"
                       "可以用它对照“精化到底带来了多少提升”。"),
        &DetectorParams::subpixelEnable);
    addDoubleField(fSub, QStringLiteral("refineBandRatio"), QStringLiteral("选点带宽比"),
        QStringLiteral("挑选亚像素边缘点的带宽半宽, 单位是半径 R 的倍数(默认 0.18)。\n"
                       "这是最该先试的一个参数: 调小 → 只取最贴近边缘的点, 拟合更干净;\n"
                       "调太小可能点不够而精化失败(退回整数轮廓)。"),
        0.01, 1.0, 3, &DetectorParams::refineBandRatio);
    addDoubleField(fSub, QStringLiteral("refineBandMin"), QStringLiteral("带宽下限px"),
        QStringLiteral("上面那个带宽的下限(像素, 默认 0.8)。靶标很小(R 只有几个像素)时\n"
                       "0.18R 会小于 1 个像素, 靠这个下限保证仍然能取到一条可拟合的带。"),
        0.0, 200.0, 2, &DetectorParams::refineBandMin);
    addDoubleField(fSub, QStringLiteral("roiScale"), QStringLiteral("ROI 半径倍数"),
        QStringLiteral("Zernike 扫描区域的半径 ÷ R(默认 1.5)。\n"
                       "必须盖得住“选点带最远处”(约 1.2R)再加 7×7 模板的边距。\n"
                       "调小可以更快, 但盖不住就会漏掉本该参与拟合的边缘点。"),
        1.0, 8.0, 2, &DetectorParams::roiScale);
    addDoubleField(fSub, QStringLiteral("zernikeKMin"), QStringLiteral("阶跃强度初筛"),
        QStringLiteral("Zernike 边缘点的阶跃强度初筛下限(默认 5.0)。\n"
                       "低于它的像素直接不算边缘点。调高 → 只保留对比强烈的边缘,\n"
                       "弱边界的靶标可能整个被跳过。"),
        0.0, 10000.0, 1, &DetectorParams::zernikeKMin);
    m_chkOtsuK = addBoolField(fSub, QStringLiteral("useOtsuK"), QStringLiteral("k 阈值用大津法"),
        QStringLiteral("是否用大津法自适应地在本候选附近的边缘点上求阶跃强度阈值(默认开)。\n"
                       "注意是“局部”统计: 全图统计会被反光面的海量强边缘带偏, 反而把\n"
                       "真靶标的弱边缘一起拒掉。取消后改用下面的「固定 k 阈值」。"),
        &DetectorParams::useOtsuK);
    m_spinFixedKt = addDoubleField(fSub, QStringLiteral("fixedKt"), QStringLiteral("固定 k 阈值"),
        QStringLiteral("阶跃强度阈值, 仅当「k 阈值用大津法」取消勾选时生效。\n"
                       "参考值: 大津法在这批样本上算出来通常在 20~60。\n"
                       "注意它与上面的「阶跃强度初筛」是两级门槛, 初筛在前、更粗。"),
        0.0, 10000.0, 1, &DetectorParams::fixedKt);
    addIntField(fSub, QStringLiteral("minFitPoints"), QStringLiteral("最少拟合点数"),
        QStringLiteral("参与最小二乘拟合的最少亚像素点数(默认 6)。少于它就判定精化失败,\n"
                       "退回整数轮廓。点太少时拟合出来的圆/椭圆没有意义, 所以有这道闸。"),
        3, 100000, &DetectorParams::minFitPoints);
    addDoubleField(fSub, QStringLiteral("ellipseBandMin"), QStringLiteral("椭圆带宽下限"),
        QStringLiteral("椭圆精化的归一化带宽下限(默认 0.04)。带太窄会挑不出点。"),
        0.005, 0.5, 3, &DetectorParams::ellipseBandMin);
    addDoubleField(fSub, QStringLiteral("ellipseBandMax"), QStringLiteral("椭圆带宽上限"),
        QStringLiteral("椭圆精化的归一化带宽上限(默认 0.20)。带太宽会把非边缘点混进来。"),
        0.01, 1.0, 3, &DetectorParams::ellipseBandMax);
    addDoubleField(fSub, QStringLiteral("ellipseBandShrink"), QStringLiteral("二次收缩比"),
        QStringLiteral("第二次重拟合时带宽的收缩比(默认 0.6), 用来剔掉毛刺。\n"
                       "拟合用的带和算残差 RMS 用的带必须是同一个值 —— \n"
                       "改成别的数, 前后的 RMS 就不能直接比了。"),
        0.05, 1.0, 2, &DetectorParams::ellipseBandShrink);
    addDoubleField(fSub, QStringLiteral("minAxisRatio"), QStringLiteral("短长轴比下限"),
        QStringLiteral("拟合椭圆的短轴/长轴下限(默认 0.2)。比值更小的“椭圆”多半是\n"
                       "拟合到了一条弧段或一串噪点, 直接判不可信。"),
        0.0, 1.0, 2, &DetectorParams::minAxisRatio);

    // ---- 解码与去重 ----
    addDoubleField(fDec, QStringLiteral("codeRingInner"), QStringLiteral("编码带内沿"),
        QStringLiteral("解码采样时编码环径向带的内沿 ×R(默认 2.3)。"),
        1.0, 10.0, 2, &DetectorParams::codeRingInner);
    addDoubleField(fDec, QStringLiteral("codeRingOuter"), QStringLiteral("编码带外沿"),
        QStringLiteral("解码采样时编码环径向带的外沿 ×R(默认 4.1)。\n"
                       "与「轮廓筛选」组里的编码带外沿(默认 4.2)不是同一个数, 不要改成一样。"),
        1.0, 10.0, 2, &DetectorParams::codeRingOuter);
    addIntField(fDec, QStringLiteral("codeBrightThr"), QStringLiteral("编码块亮阈值"),
        QStringLiteral("判定编码块的灰度阈值(默认 200)。比「亮像素阈值」更严, 因为\n"
                       "编码块是纯白跑道块, 用高阈值能少受反光和灰边影响。"),
        0, 255, &DetectorParams::codeBrightThr);
    addIntField(fDec, QStringLiteral("rectifyBinThr"), QStringLiteral("拉正二值化阈值"),
        QStringLiteral("倾斜靶标做仿射拉正后重新二值化的阈值(默认 127)。\n"
                       "仿射插值会把 0/255 变成中间灰度, 不重新二值化就找不到\n"
                       "精确的“0→255 上升沿”, 结果是边界检测失败、整颗靶标的位全为 0 —— \n"
                       "解不出 ID 时这个值最值得先试。"),
        1, 254, &DetectorParams::rectifyBinThr);
    addIntField(fDec, QStringLiteral("bitSubHitThr"), QStringLiteral("子块命中点数"),
        QStringLiteral("每个 8° 子块里亮点数超过该值才算这个子块“亮”(默认 4, 共 8 个采样角)。\n"
                       "调低 → 更容易把一个块读成亮(1); 调高 → 更容易读成暗(0)。"),
        0, 7, &DetectorParams::bitSubHitThr);
    addIntField(fDec, QStringLiteral("bitBlockThr"), QStringLiteral("位判定块数"),
        QStringLiteral("每个 24° 编码位分成 3 个子块, 至少这么多块亮才把该位读成 1\n"
                       "(默认 2)。改成 3 → 必须全亮才算 1, 解码更严但更容易漏位。"),
        0, 3, &DetectorParams::bitBlockThr);
    addDoubleField(fDec, QStringLiteral("minGapRadii"), QStringLiteral("去重中心距倍数"),
        QStringLiteral("两个候选中心的距离小于该值×半径时视为同一物理靶标, 按黑白占比\n"
                       "与白色面积取舍(默认 10.0)。出现“同一靶标被识别成两个 ID”时\n"
                       "适当调大。"),
        1.0, 1000.0, 1, &DetectorParams::minGapRadii);

    // 分组之下留一点空白, 让最后一个卡片不贴边
    il->addStretch(1);
    m_paramScroll->setWidget(inner);
    v->addWidget(m_paramScroll, 1);

    // ---- 按钮(2×2: 340px 宽的一栏里一行放不下 4 个中文按钮) ----
    // 按钮和提示刻意放在滚动区**外面**, 固定在参数栏底部。【应用参数】是这一栏的主操作,
    // 若跟着参数一起滚, 默认视图下正好落在屏幕外, 很容易让人以为压根没这个按钮。
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
    v->addLayout(btns);

    m_paramHint = new QLabel(paramPane);
    m_paramHint->setWordWrap(true);
    v->addWidget(m_paramHint);

    connect(m_btnApply, &QPushButton::clicked, this, &testDetect::onApplyParams);
    connect(m_btnResetParams, &QPushButton::clicked, this, &testDetect::onResetParams);
    connect(m_btnSaveParams, &QPushButton::clicked, this, &testDetect::onSaveParamsFile);
    connect(m_btnLoadParams, &QPushButton::clicked, this, &testDetect::onLoadParamsFile);

    // 初值: 优先上次保存的 ini; 没有(或读不出来)就用出厂默认。
    // 两条路都要把参数真正灌进识别器 —— 光填控件不改识别器, 界面上显示的和实际用的是两回事。
    if (!loadParamsIni()) {
        const DetectorParams def = TargetDetector::defaultParams();
        loadParamsToWidgets(def);
        m_detector.setParams(def);
    }
    setParamsDirty(false);
    updateParamDependents();
}

DetectorParams testDetect::collectParams() const
{
    // 以默认值打底再逐项覆盖: 万一某个控件没建起来, 也不会把那一项变成 0。
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
    m_updatingParams = true;      // 下面的 setValue/setChecked 会发信号, 别当成用户改动
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
    setParamsDirty(true);
    updateParamDependents();      // 总开关一改, 被它压住的那几行要跟着变灰/变亮
}

void testDetect::setParamsDirty(bool dirty)
{
    m_paramsDirty = dirty;
    if (m_paramHint) {
        m_paramHint->setText(dirty
            ? QStringLiteral("参数已改动，点「应用参数」生效")
            : QStringLiteral("参数已生效；改动后需点「开始识别编码片」或「识别标记点」重新识别"));
        // QSS 里没法表达"这个 label 现在处于未应用状态", 动态状态只能在代码里上色
        m_paramHint->setStyleSheet(QStringLiteral("color:%1;")
            .arg(QLatin1String(dirty ? kHintColorDirty : kHintColorIdle)));
    }
    if (m_btnApply)
        m_btnApply->setEnabled(dirty);
}

void testDetect::updateParamDependents()
{
    // 关掉总开关之后, 被它压住的那几项就不再被算法读取了 —— 明确置灰,
    // 免得用户对着一个不起作用的数字反复调(这是参数栏最容易让人误判的地方)。
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

void testDetect::onApplyParams()
{
    const DetectorParams wanted = collectParams();
    m_detector.setParams(wanted);            // DLL 侧还会再夹一次范围(第二道防线)
    // 回读识别器里真正生效的值再填回控件: setParams 可能夹取过(比如腐蚀核填了偶数),
    // 这样界面显示的和实际用的一定一致, 不会出现"界面上是 4、算法里是 5"。
    loadParamsToWidgets(m_detector.params());
    updateParamDependents();
    saveParamsIni();
    setParamsDirty(false);

    // 已经显示出来的结果是旧参数跑出来的, 必须说清楚, 否则很容易以为"改了没用"
    statusBar()->showMessage(m_markers.isEmpty()
        ? QStringLiteral("参数已应用")
        : QStringLiteral("参数已应用 —— 点「开始识别编码片」或「识别标记点」用新参数重新识别"));
}

void testDetect::onResetParams()
{
    const DetectorParams def = TargetDetector::defaultParams();
    loadParamsToWidgets(def);
    m_detector.setParams(def);
    updateParamDependents();
    saveParamsIni();          // 默认值也落盘, 否则下次启动又会读回旧的那套
    setParamsDirty(false);
    statusBar()->showMessage(QStringLiteral("参数已恢复默认并应用"));
}

// ---------------------------------------------------------------- 参数持久化

QString testDetect::paramsIniPath() const
{
    // 放在 exe 同目录: 这套参数是"这台机器上这个识别流程"的配置, 跟着程序走最直观,
    // 也免得写注册表。x64\Debug 是可写的。
    return QCoreApplication::applicationDirPath() + QStringLiteral("/settings.ini");
}

void testDetect::writeParamsToSettings(const DetectorParams& p, const QVector<ParamField>& fields,
    const QString& group, const QString& filePath)
{
    QSettings s(filePath, QSettings::IniFormat);
    s.beginGroup(group);
    for (const ParamField& f : fields) {
        // 用 17 位有效数字的字符串存 double: QVariant 默认的 double→文本转换只有 6 位
        // 有效数字, 像 0.0005 这种小量级的参数会被截断到 0.0005 尚可, 但 0.03001
        // 之类就会被抹平, 存回去的值和前一次应用的就不再是同一个数。
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
            continue;                 // ini 里缺的键保持 p 现有的值(调用方已用默认值打底)
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
    // 存的是"当前控件里这套"而不是"已生效那套": 用户的直觉是"我现在看到的这份存下来"
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
    loadParamsToWidgets(p);
    m_detector.setParams(p);
    loadParamsToWidgets(m_detector.params());
    updateParamDependents();
    saveParamsIni();              // 载入即视为当前配置, 一并写进自动记住的那份
    setParamsDirty(false);
    statusBar()->showMessage(m_markers.isEmpty()
        ? QStringLiteral("参数已载入：%1").arg(QFileInfo(path).fileName())
        : QStringLiteral("参数已载入：%1 —— 点「开始识别编码片」或「识别标记点」重新识别").arg(QFileInfo(path).fileName()));
}

// ---------------------------------------------------------------- 全屏

void testDetect::onToggleFullScreen()
{
    const bool goFull = !isFullScreen();
    if (goFull)
        showFullScreen();
    else
        showMaximized();      // 退出全屏退回最大化, 而不是普通窗口: 本程序默认就是最大化形态

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

    // 识别的 (x,y) 是去畸变坐标系下的, 显示图必须用同一坐标系, 否则叠加会整体偏移
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
    m_scene->clear();               // 连同所有图元一起销毁, 下面重新建
    m_markers.clear();
    m_pixmapItem = nullptr;
    m_roiItem = nullptr;

    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    // QImage 不接管数据, 交给 QPixmap 前必须深拷贝一份
    const QImage img(rgb.data, rgb.cols, rgb.rows, (int)rgb.step, QImage::Format_RGB888);
    m_pixmapItem = m_scene->addPixmap(QPixmap::fromImage(img.copy()));
    m_pixmapItem->setZValue(0);

    m_scene->setSceneRect(m_pixmapItem->boundingRect());

    // 图元尺寸跟着图像大小走, 高分辨率图上不会细成一根线
    const int shortSide = qMin(bgr.cols, bgr.rows);
    m_markRadius = qBound(5.0, shortSide / 180.0, 48.0);
    m_penW = qBound(1.0, shortSide / 900.0, 8.0);
    // 标签字号是**屏幕像素**, 固定值: 标签图元忽略视图变换, 尺寸不随缩放变化
    m_labelFontPx = 13.0;

    // ROI 多边形先建好(垫在圆点下面), 有 3 个点以上才显示
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

    m_markerMode = false;       // 编码片模式(结果列表/标签显示 ID:n)
    clearResults();
    setBusy(true);
    statusBar()->showMessage(QStringLiteral("识别中…"));

    cv::Mat input = m_srcMat;   // 浅拷贝, 与主线程共享像素, 线程里只读
    m_thread = QThread::create([this, input]() mutable {
        const std::vector<DetectionResult> results = m_detector.detectTarget(input);
        // 回到 GUI 线程再碰界面
        QMetaObject::invokeMethod(this, [this, results]() { finishDetect(results); },
                                  Qt::QueuedConnection);
    });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->start();
}

// 标记点识别: 与 onStartDetect 完全相同的线程/回包方式, 只是调用另一个 DLL 入口。
// detectMarkers 不做解码, 所以结果里 id 恒为 -1; 序号由 finishDetect 按结果下标生成。
void testDetect::onStartDetectMarkers()
{
    if (!m_hasImage || m_busy)
        return;

    m_markerMode = true;
    clearResults();
    setBusy(true);
    statusBar()->showMessage(QStringLiteral("标记点识别中…"));

    cv::Mat input = m_srcMat;   // 浅拷贝, 与主线程共享像素, 线程里只读
    m_thread = QThread::create([this, input]() mutable {
        std::vector<DetectionResult> results;
        try {
            results = m_detector.detectMarkers(input);
        }
        catch (const std::exception& e) {
            // 异常不能穿出 QThread 的线程函数(会 std::terminate), 也不能让界面永远 busy
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
        // 标记点没有编码 ID(DLL 返回 id=-1), 这里用 1 起的序号做界面标识
        mk.id = m_markerMode ? (int)i + 1 : (int)r.id;
        mk.isMarker = m_markerMode;
        mk.pos = QPointF(r.center.x, r.center.y);
        mk.subpixel = r.subpixel;

        // 真实识别到的亚像素轮廓: 优先用 Zernike 精化出的拟合椭圆, 退化时按正圆画
        mk.contour = new QGraphicsPathItem();
        if (r.hasEllipse)
            mk.contour->setPath(contourPath(r.ellipse));
        else
            mk.contour->setPath(contourPath(r.center, qMax((double)r.radius, 1.0)));
        m_scene->addItem(mk.contour);
        mk.contour->setZValue(2);

        // 中心只留一个小点标出亚像素中心, 别把轮廓盖住
        mk.dot = m_scene->addEllipse(mk.pos.x() - m_markRadius * 0.3,
            mk.pos.y() - m_markRadius * 0.3, m_markRadius * 0.6, m_markRadius * 0.6);
        mk.dot->setZValue(3);

        mk.label = new QGraphicsTextItem();
        // 忽略视图变换 → 标签始终是固定的屏幕字号, 不会随放大变成巨幅挡住轮廓
        mk.label->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
        mk.label->setHtml(idLabelHtml(mk.id, false, m_labelFontPx, mk.isMarker));
        mk.label->setPos(mk.pos);   // 文字靠 HTML 里的 <br> 落到锚点下方
        m_scene->addItem(mk.label);
        mk.label->setZValue(3);

        m_markers.push_back(mk);

        // 坐标是 Zernike 亚像素精化的结果(cv::Point2f), 按小数点后 3 位显示,
        // 不要 round 成整数 —— 那会把亚像素精度整个抹掉。
        // 后面带上拟合残差 RMS: 这才是精度的真实度量, 小数位数本身说明不了精度。
        // 亚像素精化失败退回整数轮廓的, 明确标出来, 免得把 0.3px 的东西当亚像素结果用。
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
        item->setData(Qt::UserRole, (int)i);        // 关联回 m_markers 下标
        if (!r.subpixel)
            item->setForeground(QBrush(QColor(255, 170, 60)));   // 掉档的用橙色提醒
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

    // 退出画框只收掉多边形, **不清空勾选**: 勾选现在代表"高亮这个点", 是浏览用的,
    // 不该因为切一下模式就把用户挑好的一堆点全抹掉。要清空请用「清空选择」。
    applyHighlights();      // 内部会顺带刷新状态栏, 所以下面的提示语必须放在它后面

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

    // 点在勾选框上时, Qt 已经替我们切换过(在 release 时), 所以勾选状态和按下那一刻不一样
    // → 不能再翻一次, 否则两次刚好抵消、点了没反应。
    // 点在行的其他位置时 Qt 不管, 状态没变 → 这里补上切换, 于是整行都可以点。
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
        // 勾上就高亮, **不需要先进画框模式** —— 识别完就能对着列表快速定位每个 ID 在图上哪儿
        const bool on = (item->checkState() == Qt::Checked);

        // 亚像素轮廓: 选中红色粗线, 正常青绿色细线, 精化掉档的用橙色虚线标出来 ——
        // 高亮靠轮廓颜色/线型区分, 不再用固定大小的大圆盘, 免得盖住真正的拟合结果
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
            mk.contour->setZValue(6);   // 高亮项压在其他项之上
            mk.dot->setZValue(7);
            mk.label->setZValue(8);
        }
        else {
            mk.contour->setZValue(2);
            mk.dot->setZValue(3);
            mk.label->setZValue(4);
        }
    }

    // 多边形本身始终算好(顶点就是勾选的点), 但只在画框模式下画出来:
    // 「开始画框」的语义是"我要圈一块区域出来", 平时勾选只是用来定位是哪个点。
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
    // fitInView 内部会 scale, 而滚轮缩放把锚点设成了 AnchorUnderMouse,
    // 那样"适应窗口"会被鼠标位置带偏, 所以这里临时换回 NoAnchor。
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
    // 列表的鼠标按下: 抢占记录勾选状态。过滤器在控件自己的事件处理之前跑, 所以这里一定拿得到
    // "Qt 有没有替我们翻过" 的基准值(见 onItemClicked)。
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
            // 1.0015^120 ≈ 1.20: 滚一格(120)约放大 20%。用指数而不是每次固定倍数,
            // 放大/缩小才是严格互逆的, 来回滚不会漂。
            const double factor = std::pow(1.0015, double(dy));
            const double cur = m_view->transform().m11();
            // 夹住缩放范围, 免得几下滚飞了找不回图
            if ((factor > 1.0 && cur < 400.0) || (factor < 1.0 && cur > 0.002)) {
                m_view->scale(factor, factor);   // 锚点是 AnchorUnderMouse → 以光标处为中心
                m_autoFit = false;               // 手动缩放过, 之后 resize 不再抢回适应窗口
                updateZoomLabel();
            }
            return true;    // 吃掉滚轮, 别让视图拿去上下滚动
        }

        case QEvent::Resize:
            // m_pixmapItem 必须判空: 窗口刚出来、还没导入图片时视口就会发 Resize,
            // 而 QGraphicsView::fitInView(const QGraphicsItem*, ...) 内部不判空,
            // 直接解引用 item->d_ptr, 传 nullptr 会以 0xC0000005 读地址 0x8 崩掉。
            if (m_autoFit && m_pixmapItem)
                m_view->fitInView(m_pixmapItem, Qt::KeepAspectRatio);
            break;

        case QEvent::MouseButtonDblClick:
            if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
                onFitView();    // 双击回到适应窗口
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
    // 识别进行中整栏禁用。DLL 侧本来就会给每次识别取一份参数快照(所以中途改不会崩),
    // 但那样会出现"这次识别到底用的哪套参数"说不清的情况, 直接禁掉最不容易误判。
    if (m_paramPane)
        m_paramPane->setEnabled(!busy);
}


// ------------------------------------------------- 第二页「点云拼接」(MergeMarks)

// MergeMarksLib.dll 依赖 PCL/VTK 的一批运行时 dll。查找顺序(前插进 PATH, 先命中先赢):
//
//   1) <exe目录>\runtime\  —— 发布包自带的那一份(build_release.ps1 组装)。**别人机器上只有这个**,
//      所以它必须排第一: 开发机上这两处都能用, 但发布机上本机绝对路径根本不存在。
//   2) 本机已装好的那几套绝对路径 —— 开发机直接跑 x64\Debug / x64\Release 时用, 免得每次都要
//      先组装一遍发布包。换机器/换安装位置时改下面这一串即可。
//
// 注意第 1 条是"有就优先", 不做存在性判断: 往 PATH 里塞一个不存在的目录是无害的,
// 少写一个分支反而更不容易出错。
static const wchar_t* kMergeMarksRuntimeDirsDev =
    L"D:\\1yjy\\software\\newcomputer\\VTK\\VTK8.1.0\\install\\bin;"
    L"D:\\1yjy\\software\\newcomputer\\PCL 1.9.1\\bin;"
    L"D:\\1yjy\\software\\newcomputer\\PCL 1.9.1\\3rdParty\\FLANN\\bin;"
    L"D:\\1yjy\\software\\newcomputer\\PCL 1.9.1\\3rdParty\\Qhull\\bin;"
    L"D:\\1yjy\\software\\newcomputer\\PCL 1.9.1\\3rdParty\\VTK\\bin;"
    L"D:\\1yjy\\software\\newcomputer\\PCL 1.9.1\\3rdParty\\OpenNI2\\Redist";

// 组装出最终要前插到 PATH 的那一串目录(发布包 runtime 优先, 再是本机那几套)。
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
    // 只有第二页需要它; 已经加载过或已经失败过就不再折腾
    if (index != 1 || m_mmPanel || m_mmLoadTried)
        return;

    // 加载那几十个 dll 要几秒, 先把提示画出来再阻塞, 免得看着像卡死
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

    // 关键: PCL/VTK 的运行时目录必须在 LoadLibrary 之前进 PATH ——
    // MergeMarksLib.dll 依赖它们, 加载它的那一刻加载器就要解析。
    // 这件事不能交给那个 DLL 自己做(它那时还没被加载起来)。
    // 用前插而不是 SetDefaultDllDirectories: 后者会把 PATH 从搜索路径里摘掉,
    // 有把本来依赖的 Qt dll 一起弄找不到的风险。
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
        // 这里也不卸载 mod —— 同析构函数里的理由: FreeLibrary 会连锁把 PCL/VTK 那批依赖
        // 一起卸掉, 而 VTK 不支持在进程内卸载。这条错误分支上虽然还没创建任何 VTK 对象,
        // 但卸载动作本身一样有风险, 而这里本来就是个无法继续的错误路径, 让它留在内存里即可。
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

    // 面板建好后把占位提示换掉
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
