#include "Syntax.h"

#include <QTextDocument>

#ifdef YADIFF_HAVE_KSYNTAX
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

namespace {
KSyntaxHighlighting::Repository &repository()
{
    static KSyntaxHighlighting::Repository repo;
    return repo;
}
// Skips placeholder blocks so they keep their own look.
class Highlighter : public KSyntaxHighlighting::SyntaxHighlighter
{
public:
    using KSyntaxHighlighting::SyntaxHighlighter::SyntaxHighlighter;

protected:
    void highlightBlock(const QString &text) override
    {
        KSyntaxHighlighting::SyntaxHighlighter::highlightBlock(text);
        if (currentBlock().blockFormat().boolProperty(Syntax::kPlainBlockProperty))
            setFormat(0, int(text.size()), QTextCharFormat());
    }
};
} // namespace

namespace Syntax {

bool available()
{
    return true;
}

QStringList languages()
{
    QStringList names;
    for (const auto &def : repository().definitions())
        if (!def.isHidden())
            names << def.name();
    names.sort(Qt::CaseInsensitive);
    return names;
}

QString detect(const QString &fileName)
{
    const auto def = repository().definitionForFileName(fileName);
    return def.isValid() ? def.name() : QString();
}

QObject *apply(QObject *existing, QTextDocument *doc, const QString &language, bool darkTheme)
{
    auto *h = qobject_cast<KSyntaxHighlighting::SyntaxHighlighter *>(existing);
    const auto def = repository().definitionForName(language);
    if (language.isEmpty() || !def.isValid()) {
        delete h;
        return nullptr;
    }
    if (!h)
        h = new Highlighter(doc);
    else if (h->document() != doc)
        h->setDocument(doc);
    h->setTheme(repository().defaultTheme(darkTheme ? KSyntaxHighlighting::Repository::DarkTheme
                                                    : KSyntaxHighlighting::Repository::LightTheme));
    h->setDefinition(def);
    h->rehighlight();
    return h;
}

void suspend(QObject *highlighter)
{
    if (auto *h = qobject_cast<KSyntaxHighlighting::SyntaxHighlighter *>(highlighter))
        h->setDocument(nullptr);
}

void resume(QObject *highlighter, QTextDocument *doc)
{
    if (auto *h = qobject_cast<KSyntaxHighlighting::SyntaxHighlighter *>(highlighter))
        h->setDocument(doc);
}

} // namespace Syntax

#else

namespace Syntax {
bool available() { return false; }
QStringList languages() { return {}; }
QString detect(const QString &) { return {}; }
QObject *apply(QObject *existing, QTextDocument *, const QString &, bool) { delete existing; return nullptr; }
void suspend(QObject *) {}
void resume(QObject *, QTextDocument *) {}
} // namespace Syntax

#endif
