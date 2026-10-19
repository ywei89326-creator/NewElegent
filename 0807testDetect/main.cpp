#include "testDetect.h"
#include <QtWidgets/QApplication>

#include <QStyleFactory>
#include <QPalette>
#include <QFont>
#include <QFile>

// 深色科技风主题(青色强调)。三件事, 缺一不可:
//   1) Fusion —— 系统原生样式对调色板和样式表的响应不一致, 且会把部分控件(滚动条、
//      勾选框等)按浅色画出来。换成 Fusion 后调色板与 QSS 才是同一套。
//   2) QPalette —— 覆盖样式表够不到的原生绘制部分(禁用文字、工具提示、原生高亮)。
//   3) theme.qss —— 真正的观感都在那份资源里, 见文件头部说明。
//
// ⚠ 必须在窗口 show() 之前设好: MergeMarksLib 面板是切到「点云拼接」页才创建的
// (懒加载), 届时样式表已经装在 QApplication 上, 面板控件创建时就自动套用,
// 于是两个页签是同一套主题 —— 这也是「一处定义、两个页签统一」的原理。
static void applyDarkTechTheme(QApplication& app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    // 基础色板, 取值与 theme.qss 里的一致
    const QColor window(0x12, 0x16, 0x1c);
    const QColor base(0x16, 0x1b, 0x23);
    const QColor text(0xdb, 0xe4, 0xee);
    const QColor accent(0x22, 0xd3, 0xee);
    const QColor disabled(0x55, 0x60, 0x6e);

    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, QColor(0x19, 0x1f, 0x28));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(0x1b, 0x22, 0x2b));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, QColor(0xff, 0x52, 0x52));
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, QColor(0x08, 0x16, 0x1c));
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::ToolTipBase, QColor(0x1b, 0x22, 0x2b));
    p.setColor(QPalette::ToolTipText, text);

    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x23, 0x2a, 0x34));

    app.setPalette(p);

    // 默认字体。注意基础字体只在这里设, 不放进 theme.qss: 样式表里的字体属性
    // 会盖掉代码里 setFont() 的结果, 而结果列表/日志框靠 setFont 拿等宽字体,
    // 放进去就会把数字对齐毁掉(theme.qss 头部也记了这条)。
    // Segoe UI 没有中文字形, 所以显式给出回退链, 别依赖系统的自动替换。
    QFont f = app.font();
    f.setFamilies({ QStringLiteral("Segoe UI"),
                    QStringLiteral("Microsoft YaHei UI"),
                    QStringLiteral("Microsoft YaHei") });
    f.setStyleHint(QFont::SansSerif);
    f.setPointSizeF(9.5);
    app.setFont(f);

    // 样式表(由 testDetect.qrc 打包成资源)
    QFile qss(QStringLiteral(":/testDetect/theme.qss"));
    if (qss.open(QIODevice::ReadOnly | QIODevice::Text))
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));
}

int main(int argc, char* argv[])
{
    QApplication a(argc, argv);

    applyDarkTechTheme(a);

    testDetect w;
    // 启动即最大化: 识别界面要高分辨率图 + 结果列表 + 参数栏三块并排, 窗口化起步太窄。
    // 需要无边框全屏时按 F11(或点顶部按钮条最右的「全屏 F11」), Esc/F11 退回最大化。
    w.showMaximized();

    return a.exec();
}
