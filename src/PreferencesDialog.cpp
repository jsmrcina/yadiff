#include "PreferencesDialog.h"

#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFontDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

PreferencesDialog::PreferencesDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Preferences"));

    auto *colorsBox = new QGroupBox(tr("Diff colors"));
    auto *colorsForm = new QFormLayout(colorsBox);
    m_added = new QPushButton;
    m_removed = new QPushButton;
    m_moved = new QPushButton;
    colorsForm->addRow(tr("Added lines:"), m_added);
    colorsForm->addRow(tr("Removed lines:"), m_removed);
    colorsForm->addRow(tr("Moved lines:"), m_moved);
    connect(m_added, &QPushButton::clicked, this, [this] { pickColor(m_colors.added); });
    connect(m_removed, &QPushButton::clicked, this, [this] { pickColor(m_colors.removed); });
    connect(m_moved, &QPushButton::clicked, this, [this] { pickColor(m_colors.moved); });

    auto *textBox = new QGroupBox(tr("Text"));
    auto *textForm = new QFormLayout(textBox);
    m_fontButton = new QPushButton;
    m_tab = new QSpinBox;
    m_tab->setRange(1, 16);
    textForm->addRow(tr("Font:"), m_fontButton);
    textForm->addRow(tr("Tab width:"), m_tab);
    connect(m_fontButton, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QFont f = QFontDialog::getFont(&ok, m_font, this, tr("Diff font"), QFontDialog::MonospacedFonts);
        if (ok) {
            m_font = f;
            updatePreview();
        }
    });

    auto *previewBox = new QGroupBox(tr("Preview"));
    auto *previewLayout = new QVBoxLayout(previewBox);
    previewLayout->setSpacing(0);
    const char *samples[] = {"    int unchanged = 0;", "-   int removed = 1;", "+   int added = 2;",
                             "⇄   int moved = 3;   // moved from line 40"};
    for (int i = 0; i < 4; ++i) {
        m_preview[i] = new QLabel(QString::fromUtf8(samples[i]));
        m_preview[i]->setAutoFillBackground(true);
        m_preview[i]->setContentsMargins(6, 1, 6, 1);
        previewLayout->addWidget(m_preview[i]);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply
                                         | QDialogButtonBox::RestoreDefaults);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        apply();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &PreferencesDialog::apply);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this] {
        load(Settings::defaultColors(), QFontDatabase::systemFont(QFontDatabase::FixedFont), 4);
    });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(colorsBox);
    layout->addWidget(textBox);
    layout->addWidget(previewBox);
    layout->addWidget(buttons);

    load(Settings::colors(), Settings::font(), Settings::tabWidth());
}

void PreferencesDialog::load(const DiffColors &c, const QFont &f, int tab)
{
    m_colors = c;
    m_font = f;
    m_tab->setValue(tab);
    updatePreview();
}

void PreferencesDialog::apply()
{
    Settings::setColors(m_colors);
    Settings::setFont(m_font);
    Settings::setTabWidth(m_tab->value());
    emit applied();
}

void PreferencesDialog::pickColor(QColor &target)
{
    const QColor c = QColorDialog::getColor(target, this, tr("Choose color"));
    if (c.isValid()) {
        target = c;
        updatePreview();
    }
}

void PreferencesDialog::updateSwatch(QPushButton *b, const QColor &c)
{
    QPixmap pm(40, 16);
    pm.fill(c);
    QPainter p(&pm);
    p.setPen(palette().color(QPalette::Text));
    p.drawRect(pm.rect().adjusted(0, 0, -1, -1));
    p.end();
    b->setIcon(QIcon(pm));
    b->setIconSize(pm.size());
    b->setText(c.name());
}

void PreferencesDialog::updatePreview()
{
    updateSwatch(m_added, m_colors.added);
    updateSwatch(m_removed, m_colors.removed);
    updateSwatch(m_moved, m_colors.moved);
    m_fontButton->setText(QStringLiteral("%1, %2 pt").arg(m_font.family()).arg(m_font.pointSize()));

    const QColor bgs[] = {palette().color(QPalette::Base), m_colors.removed, m_colors.added, m_colors.moved};
    for (int i = 0; i < 4; ++i) {
        QPalette pal = m_preview[i]->palette();
        pal.setColor(QPalette::Window, bgs[i]);
        pal.setColor(QPalette::WindowText, palette().color(QPalette::Text));
        m_preview[i]->setPalette(pal);
        m_preview[i]->setFont(m_font);
    }
}
