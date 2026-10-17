#pragma once

#include <QDialog>
#include <QStringList>

class QButtonGroup;
class QLineEdit;
class QStackedWidget;

// File > New comparison: picks what to compare and returns the yadiff arguments for it
// (the comparison opens in a new window).
class NewComparisonDialog : public QDialog
{
    Q_OBJECT
public:
    explicit NewComparisonDialog(const QString &currentRepo, QWidget *parent = nullptr);

    QStringList arguments() const; // valid after accept()

private:
    void validateAndAccept();

    QButtonGroup *m_kind = nullptr;
    QStackedWidget *m_pages = nullptr;
    QLineEdit *m_repo = nullptr;
    QLineEdit *m_revRepo = nullptr, *m_rev1 = nullptr, *m_rev2 = nullptr;
    QLineEdit *m_leftDir = nullptr, *m_rightDir = nullptr;
    QLineEdit *m_leftFile = nullptr, *m_rightFile = nullptr;
    QLineEdit *m_prUrl = nullptr;
    QStringList m_args;
};
