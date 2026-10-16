#pragma once

#include <QString>
#include <QStringList>
#include <QTextFormat>

class QObject;
class QTextDocument;

// Syntax highlighting via KDE's KSyntaxHighlighting when yadiff is built with it;
// otherwise every function is a harmless no-op and available() is false.
namespace Syntax {

// Blocks whose QTextBlockFormat has this property set to true are left unhighlighted and
// are seen by the highlighter as empty lines (yadiff's fold, filler and comment rows).
constexpr int kPlainBlockProperty = QTextFormat::UserProperty + 100;

bool available();

// Language names suitable for a picker, sorted case-insensitively.
QStringList languages();

// Best language for a file name ("C++", "Python", ...), or empty when unknown.
QString detect(const QString &fileName);

// Attaches (or retargets) a highlighter for `language` on `doc`. Pass the previous
// return value as `existing` to reuse it. Returns nullptr for an empty language.
QObject *apply(QObject *existing, QTextDocument *doc, const QString &language, bool darkTheme);

// Temporarily detach/reattach while a document is rebuilt, to avoid re-highlighting per edit.
void suspend(QObject *highlighter);
void resume(QObject *highlighter, QTextDocument *doc);

} // namespace Syntax
