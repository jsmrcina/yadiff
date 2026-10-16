#include "ImageDiffView.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

QString describe(const QImage &img, qint64 bytes)
{
    if (img.isNull())
        return QObject::tr("(none)");
    return QStringLiteral("%1×%2, %3").arg(img.width()).arg(img.height()).arg(QLocale().formattedDataSize(bytes));
}

void checkerboard(QPainter &p, const QRect &r)
{
    static const QPixmap tile = [] {
        QPixmap pm(16, 16);
        pm.fill(QColor(0xcc, 0xcc, 0xcc));
        QPainter tp(&pm);
        tp.fillRect(0, 0, 8, 8, QColor(0xf2, 0xf2, 0xf2));
        tp.fillRect(8, 8, 8, 8, QColor(0xf2, 0xf2, 0xf2));
        return pm;
    }();
    p.drawTiledPixmap(r, tile);
}

} // namespace

// Paints the current mode. In "fit" mode it scales to its own size; otherwise it
// is sized to the images at 100% inside the scroll area.
class ImageDiffView::Canvas : public QWidget
{
public:
    explicit Canvas(ImageDiffView *view) : QWidget(view), m_view(view) { setMouseTracking(true); }

    bool fit = true;
    double split = 0.5; // swipe divider, 0..1
    double opacity = 0.5;

    QSize contentSize() const
    {
        const QSize a = m_view->m_old.size(), b = m_view->m_new.size();
        const QSize both(std::max(a.width(), b.width()), std::max(a.height(), b.height()));
        if (m_view->m_mode == Mode::SideBySide)
            return QSize(a.width() + b.width() + 24, both.height() + 8);
        return both + QSize(8, 8);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Base));
        const QImage &a = m_view->m_old;
        const QImage &b = m_view->m_new;
        // Smooth when shrinking; keep pixels sharp when enlarging (icons, sprites).
        const QSize largest(std::max(a.width(), b.width()), std::max(a.height(), b.height()));
        p.setRenderHint(QPainter::SmoothPixmapTransform,
                        fit && (largest.width() > width() / 2 || largest.height() > height()));

        auto fitRect = [&](const QSize &img, const QRect &area) {
            if (img.isEmpty())
                return QRect();
            QSize s = img;
            if (fit)
                s.scale(area.size(), Qt::KeepAspectRatio); // small images grow too, with crisp pixels
            return QRect(area.x() + (area.width() - s.width()) / 2, area.y() + (area.height() - s.height()) / 2, s.width(),
                         s.height());
        };
        auto drawImage = [&](const QImage &img, const QRect &r) {
            if (img.isNull() || r.isEmpty())
                return;
            checkerboard(p, r);
            p.drawImage(r, img);
            QColor frame = palette().color(QPalette::Text);
            frame.setAlpha(70);
            p.setPen(frame);
            p.drawRect(r.adjusted(0, 0, -1, -1));
        };

        const QRect area = rect().adjusted(4, 4, -4, -4);
        switch (m_view->m_mode) {
        case Mode::SideBySide: {
            const int half = (area.width() - 16) / 2;
            drawImage(a, fitRect(a.size(), QRect(area.x(), area.y(), half, area.height())));
            drawImage(b, fitRect(b.size(), QRect(area.x() + half + 16, area.y(), half, area.height())));
            break;
        }
        case Mode::Swipe:
        case Mode::Onion:
        case Mode::Difference: {
            const QSize both(std::max(a.width(), b.width()), std::max(a.height(), b.height()));
            const QRect r = fitRect(both, area);
            if (r.isEmpty())
                break;
            checkerboard(p, r);
            // Images of different sizes share the top-left corner.
            auto sub = [&](const QImage &img) {
                return QRect(r.topLeft(), QSize(int(double(img.width()) / both.width() * r.width()),
                                                int(double(img.height()) / both.height() * r.height())));
            };
            if (m_view->m_mode == Mode::Difference) {
                p.drawImage(sub(m_view->m_diff), m_view->m_diff);
            } else if (m_view->m_mode == Mode::Onion) {
                if (!a.isNull())
                    p.drawImage(sub(a), a);
                p.setOpacity(opacity);
                if (!b.isNull())
                    p.drawImage(sub(b), b);
                p.setOpacity(1);
            } else {
                const int x = r.x() + int(split * r.width());
                p.save();
                p.setClipRect(QRect(r.x(), r.y(), x - r.x(), r.height()));
                if (!a.isNull())
                    p.drawImage(sub(a), a);
                p.setClipRect(QRect(x, r.y(), r.right() - x + 1, r.height()));
                if (!b.isNull())
                    p.drawImage(sub(b), b);
                p.restore();
                p.setPen(QPen(palette().color(QPalette::Highlight), 2));
                p.drawLine(x, r.top(), x, r.bottom());
                p.setBrush(palette().color(QPalette::Highlight));
                p.drawEllipse(QPoint(x, r.center().y()), 7, 7);
            }
            break;
        }
        }
    }

    void mousePressEvent(QMouseEvent *e) override { moveSplit(e); }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (e->buttons() & Qt::LeftButton)
            moveSplit(e);
        setCursor(m_view->m_mode == Mode::Swipe ? Qt::SplitHCursor : Qt::ArrowCursor);
    }

private:
    void moveSplit(QMouseEvent *e)
    {
        if (m_view->m_mode != Mode::Swipe || width() <= 8)
            return;
        split = std::clamp((e->position().x() - 4) / double(width() - 8), 0.0, 1.0);
        update();
    }
    ImageDiffView *m_view;
};

ImageDiffView::ImageDiffView(QWidget *parent) : QWidget(parent)
{
    m_canvas = new Canvas(this);
    m_scroll = new QScrollArea;
    m_scroll->setWidget(m_canvas);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(6, 3, 6, 3);
    m_modes = new QButtonGroup(this);
    const struct {
        Mode mode;
        QString label;
        QString tip;
    } modes[] = {{Mode::SideBySide, tr("Side by side"), tr("Both versions next to each other")},
                 {Mode::Swipe, tr("Swipe"), tr("Drag the divider to wipe between the versions")},
                 {Mode::Onion, tr("Onion skin"), tr("Blend the new version over the old one")},
                 {Mode::Difference, tr("Difference"), tr("Changed pixels in red over a faded old version")}};
    for (const auto &m : modes) {
        auto *b = new QToolButton;
        b->setText(m.label);
        b->setToolTip(m.tip);
        b->setCheckable(true);
        b->setAutoRaise(true);
        m_modes->addButton(b, int(m.mode));
        bar->addWidget(b);
    }
    m_modes->button(int(Mode::SideBySide))->setChecked(true);
    connect(m_modes, &QButtonGroup::idClicked, this, [this](int id) { setMode(Mode(id)); });

    m_opacity = new QSlider(Qt::Horizontal);
    m_opacity->setRange(0, 100);
    m_opacity->setValue(50);
    m_opacity->setMaximumWidth(160);
    m_opacity->setToolTip(tr("Opacity of the new version"));
    m_opacity->setVisible(false);
    connect(m_opacity, &QSlider::valueChanged, this, [this](int v) {
        m_canvas->opacity = v / 100.0;
        m_canvas->update();
    });
    bar->addWidget(m_opacity);
    bar->addStretch(1);
    m_info = new QLabel;
    bar->addWidget(m_info);
    m_fit = new QToolButton;
    m_fit->setText(tr("Fit"));
    m_fit->setCheckable(true);
    m_fit->setChecked(true);
    m_fit->setAutoRaise(true);
    m_fit->setToolTip(tr("Scale to fit, or show at 100%"));
    connect(m_fit, &QToolButton::toggled, this, [this](bool on) {
        m_canvas->fit = on;
        m_scroll->setWidgetResizable(on);
        if (!on)
            m_canvas->resize(m_canvas->contentSize());
        m_canvas->update();
    });
    bar->addWidget(m_fit);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(m_scroll, 1);
}

void ImageDiffView::setImages(const QImage &oldImage, const QImage &newImage, qint64 oldBytes, qint64 newBytes)
{
    m_old = oldImage.convertToFormat(QImage::Format_ARGB32);
    m_new = newImage.convertToFormat(QImage::Format_ARGB32);
    m_oldBytes = oldBytes;
    m_newBytes = newBytes;
    computeDifference();
    updateInfo();
    if (!m_fit->isChecked())
        m_canvas->resize(m_canvas->contentSize());
    m_canvas->update();
}

void ImageDiffView::setMode(Mode m)
{
    m_mode = m;
    m_modes->button(int(m))->setChecked(true);
    m_opacity->setVisible(m == Mode::Onion);
    if (!m_fit->isChecked())
        m_canvas->resize(m_canvas->contentSize());
    updateInfo();
    m_canvas->update();
}

void ImageDiffView::computeDifference()
{
    // Faded grayscale old image with every differing pixel painted red.
    const int w = std::max(m_old.width(), m_new.width());
    const int h = std::max(m_old.height(), m_new.height());
    m_diffCount = 0;
    m_diff = QImage(w, h, QImage::Format_ARGB32);
    m_diff.fill(Qt::transparent);
    for (int y = 0; y < h; ++y) {
        auto *out = reinterpret_cast<QRgb *>(m_diff.scanLine(y));
        const QRgb *a = y < m_old.height() ? reinterpret_cast<const QRgb *>(m_old.constScanLine(y)) : nullptr;
        const QRgb *b = y < m_new.height() ? reinterpret_cast<const QRgb *>(m_new.constScanLine(y)) : nullptr;
        for (int x = 0; x < w; ++x) {
            const bool hasA = a && x < m_old.width();
            const bool hasB = b && x < m_new.width();
            const QRgb pa = hasA ? a[x] : 0;
            const QRgb pb = hasB ? b[x] : 0;
            if (hasA != hasB || pa != pb) {
                ++m_diffCount;
                out[x] = qRgba(230, 30, 30, 255);
            } else {
                const int g = qGray(pa);
                out[x] = qRgba(g, g, g, qAlpha(pa) / 4);
            }
        }
    }
}

void ImageDiffView::updateInfo()
{
    QString text = tr("Old: %1   New: %2").arg(describe(m_old, m_oldBytes), describe(m_new, m_newBytes));
    if (m_mode == Mode::Difference && !m_old.isNull() && !m_new.isNull()) {
        const qint64 total = qint64(m_diff.width()) * m_diff.height();
        text += tr("   %1 pixels differ (%2%)")
                    .arg(m_diffCount)
                    .arg(total ? QString::number(100.0 * m_diffCount / total, 'f', 2) : QStringLiteral("0"));
    }
    m_info->setText(text);
}
