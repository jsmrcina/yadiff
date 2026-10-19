#include "UiLog.h"

#include "Log.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMessageBox>
#include <QStatusBar>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QToolBar>

namespace UiLog {

namespace {

QString actionName(const QAction *a)
{
    QString text = a->text();
    text.remove(u'&');
    if (text.isEmpty())
        text = a->toolTip();
    if (text.isEmpty())
        text = a->objectName();
    return text;
}

const char *iconName(QMessageBox::Icon icon)
{
    switch (icon) {
    case QMessageBox::Warning:
        return "warning";
    case QMessageBox::Critical:
        return "error";
    case QMessageBox::Question:
        return "question";
    case QMessageBox::Information:
        return "information";
    default:
        return "message";
    }
}

class Filter : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::Show:
            if (auto *box = qobject_cast<QMessageBox *>(obj))
                messageBox(box);
            else if (auto *menu = qobject_cast<QMenu *>(obj))
                once(menu, [](QMenu *m) {
                    QObject::connect(m, &QMenu::triggered, m, [](QAction *a) {
                        qCDebug(lcApp).noquote() << "menu:" << actionName(a) << (a->isCheckable() ? (a->isChecked() ? "(on)" : "(off)") : "");
                    });
                });
            else if (auto *bar = qobject_cast<QToolBar *>(obj))
                once(bar, [](QToolBar *b) {
                    QObject::connect(b, &QToolBar::actionTriggered, b, [](QAction *a) {
                        qCDebug(lcApp).noquote() << "toolbar:" << actionName(a) << (a->isCheckable() ? (a->isChecked() ? "(on)" : "(off)") : "");
                    });
                });
            else if (auto *status = qobject_cast<QStatusBar *>(obj))
                once(status, [](QStatusBar *s) {
                    QObject::connect(s, &QStatusBar::messageChanged, s, [](const QString &text) {
                        if (!text.isEmpty())
                            qCInfo(lcApp).noquote() << "status:" << text;
                    });
                });
            break;
        case QEvent::Shortcut:
            if (auto *a = qobject_cast<QAction *>(obj))
                qCDebug(lcApp).noquote() << "shortcut" << a->shortcut().toString(QKeySequence::PortableText) << ":" << actionName(a);
            break;
        case QEvent::MouseButtonRelease:
            if (!lcApp().isDebugEnabled())
                break;
            if (auto *b = qobject_cast<QAbstractButton *>(obj); b && b->isEnabled() && !b->text().isEmpty()
                && !qobject_cast<QMessageBox *>(b->window()))
                qCDebug(lcApp).noquote() << "button:" << b->text().remove(u'&') << "in" << b->window()->windowTitle();
            break;
        default:
            break;
        }
        return false;
    }

private:
    template <typename T, typename Fn>
    static void once(T *obj, Fn connect)
    {
        if (obj->property("yadiffLogged").toBool())
            return;
        obj->setProperty("yadiffLogged", true);
        connect(obj);
    }

    static void messageBox(QMessageBox *box)
    {
        if (box->property("yadiffLogged").toBool())
            return;
        box->setProperty("yadiffLogged", true);
        QString text = box->text();
        if (!box->informativeText().isEmpty())
            text += u'\n' + box->informativeText();
        if (Qt::mightBeRichText(text))
            text = QTextDocumentFragment::fromHtml(text).toPlainText();
        const bool bad = box->icon() == QMessageBox::Warning || box->icon() == QMessageBox::Critical;
        if (bad)
            qCWarning(lcApp).noquote() << QStringLiteral("%1 dialog \"%2\": %3").arg(QLatin1String(iconName(box->icon())), box->windowTitle(), text);
        else
            qCInfo(lcApp).noquote() << QStringLiteral("%1 dialog \"%2\": %3").arg(QLatin1String(iconName(box->icon())), box->windowTitle(), text);
        QObject::connect(box, &QMessageBox::finished, box, [box] {
            if (const QAbstractButton *b = box->clickedButton())
                qCInfo(lcApp).noquote() << QStringLiteral("dialog \"%1\" answered: %2").arg(box->windowTitle(), b->text().remove(u'&'));
        });
    }
};

} // namespace

void install(QApplication &app)
{
    app.installEventFilter(new Filter(&app));
}

} // namespace UiLog
