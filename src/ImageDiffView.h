#pragma once

#include <QImage>
#include <QWidget>

class QButtonGroup;
class QLabel;
class QScrollArea;
class QSlider;
class QToolButton;

// Compares two versions of an image: side by side, swipe, onion skin or pixel difference.
class ImageDiffView : public QWidget
{
    Q_OBJECT
public:
    enum class Mode { SideBySide, Swipe, Onion, Difference };

    explicit ImageDiffView(QWidget *parent = nullptr);

    // Either image may be null (added or deleted file). Sizes are the encoded file sizes.
    void setImages(const QImage &oldImage, const QImage &newImage, qint64 oldBytes, qint64 newBytes);
    void setMode(Mode m);
    Mode mode() const { return m_mode; }

    // Exposed for tests: number of pixels that differ.
    qint64 differingPixels() const { return m_diffCount; }

private:
    class Canvas;
    void updateInfo();
    void computeDifference();

    Mode m_mode = Mode::SideBySide;
    QImage m_old, m_new, m_diff;
    qint64 m_oldBytes = 0, m_newBytes = 0, m_diffCount = 0;
    Canvas *m_canvas = nullptr;
    QScrollArea *m_scroll = nullptr;
    QLabel *m_info = nullptr;
    QSlider *m_opacity = nullptr;
    QToolButton *m_fit = nullptr;
    QButtonGroup *m_modes = nullptr;
};
