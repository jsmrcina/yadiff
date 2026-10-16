#pragma once

#include "Settings.h"

#include <QDialog>

class QLabel;
class QPushButton;
class QSpinBox;

class PreferencesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PreferencesDialog(QWidget *parent = nullptr);

signals:
    void applied();

private:
    void load(const DiffColors &c, const QFont &f, int tab);
    void apply();
    void updatePreview();
    void pickColor(QColor &target);
    void updateSwatch(QPushButton *b, const QColor &c);

    DiffColors m_colors;
    QFont m_font;
    QPushButton *m_added = nullptr;
    QPushButton *m_removed = nullptr;
    QPushButton *m_moved = nullptr;
    QPushButton *m_fontButton = nullptr;
    QSpinBox *m_tab = nullptr;
    QLabel *m_preview[4] = {};
};
