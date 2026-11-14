// Genera los iconos del programa a partir de assets/logo.svg (solo hace falta al cambiar el logo; los
// resultados van en el repo, el programa no necesita Qt SVG para ejecutarse).
//   make_icons assets/logo.svg assets/icons
// Escribe app-<N>.png (16..512) y app.ico (16, 24, 32, 48, 64, 128, 256; entradas PNG, Windows Vista+).
#include <QBuffer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QtEndian>

#include <cstdio>

static QImage render(QSvgRenderer &svg, int n) {
    QImage img(n, n, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    svg.render(&p, QRectF(0, 0, n, n));
    return img;
}

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    if (argc != 3) {
        std::fprintf(stderr, "uso: make_icons logo.svg dir_salida\n");
        return 2;
    }
    QSvgRenderer svg(QString::fromLocal8Bit(argv[1]));
    if (!svg.isValid()) {
        std::fprintf(stderr, "SVG no valido: %s\n", argv[1]);
        return 1;
    }
    const QString out = QString::fromLocal8Bit(argv[2]);
    for (int n : {16, 24, 32, 48, 64, 128, 256, 512})
        if (!render(svg, n).save(QStringLiteral("%1/app-%2.png").arg(out).arg(n))) return 1;

    // ICO con imagenes PNG dentro
    const int sizes[] = {16, 24, 32, 48, 64, 128, 256};
    const int count = int(std::size(sizes));
    QList<QByteArray> pngs;
    for (int n : sizes) {
        QByteArray b;
        QBuffer buf(&b);
        buf.open(QIODevice::WriteOnly);
        render(svg, n).save(&buf, "PNG");
        pngs << b;
    }
    QByteArray ico;
    auto u16 = [&](quint16 v) { char c[2]; qToLittleEndian(v, c); ico.append(c, 2); };
    auto u32 = [&](quint32 v) { char c[4]; qToLittleEndian(v, c); ico.append(c, 4); };
    u16(0); u16(1); u16(quint16(count));
    quint32 off = 6 + 16 * count;
    for (int i = 0; i < count; i++) {
        ico.append(char(sizes[i] >= 256 ? 0 : sizes[i]));
        ico.append(char(sizes[i] >= 256 ? 0 : sizes[i]));
        ico.append(char(0)); ico.append(char(0));
        u16(1); u16(32);
        u32(quint32(pngs[i].size())); u32(off);
        off += quint32(pngs[i].size());
    }
    for (const auto &b : pngs) ico.append(b);
    QFile f(out + "/app.ico");
    if (!f.open(QIODevice::WriteOnly) || f.write(ico) != ico.size()) return 1;
    std::printf("OK: %s/app-*.png, app.ico\n", argv[2]);
    return 0;
}
