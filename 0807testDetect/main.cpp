#include "testDetect.h"
#include <QtWidgets/QApplication>

#include <QStyleFactory>
#include <QPalette>
#include <QFont>
#include <QFile>

// 深色主题, 三部分:
//   1) Fusion 样式 —— 原生样式对 palette/QSS 响应不一致, 且会用浅色绘制部分控件;
//   2) QPalette —— 覆盖 QSS 够不到的原生绘制(禁用文字、tooltip、高亮);
//   3) theme.qss —— 主要观感。
// ⚠ 必须在 show() 之前调用: 点云面板是懒加载, 创建时会自动套用已安装的全局样式表,
// 因此两个页签共用同一套主题。
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

    // 基础字体只在这里设, 不放 theme.qss: QSS 的 font 会压过代码里的 setFont(),
    // 破坏结果列表/日志框的等宽对齐。Segoe UI 无中文字形, 显式给出回退链。
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
    // 三栏并排需要足够宽度, 启动即最大化; F11 进入无边框全屏, Esc/F11 返回。
    w.showMaximized();

    return a.exec();
}
