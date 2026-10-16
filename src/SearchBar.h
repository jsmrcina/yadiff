#pragma once

#include "Search.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QToolButton;

// Find bar shown under the diff (Ctrl+F): text, scope, substring/regex, case.
class SearchBar : public QWidget
{
    Q_OBJECT
public:
    enum class Scope { Selection, File, AllChanges };

    explicit SearchBar(QWidget *parent = nullptr);

    SearchQuery query() const;
    Scope scope() const;
    void setScope(Scope s);
    void activate();          // show, focus and select the text
    void setResult(const QString &text, bool error = false);
    void setBusy(bool busy); // shows a Stop button while an "All changes" search runs

signals:
    void queryChanged();      // text or options edited
    void next();
    void previous();
    void closed();
    void stopRequested();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QLineEdit *m_edit = nullptr;
    QComboBox *m_scope = nullptr;
    QComboBox *m_mode = nullptr;
    QCheckBox *m_case = nullptr;
    QLabel *m_result = nullptr;
    QToolButton *m_stop = nullptr;
};
