// Azure DevOps pull requests, against a fake Azure DevOps server and a local bare repo.
#include "AdoAuth.h"
#include "AdoPr.h"
#include "GitRepo.h"
#include "Http.h"
#include "PrReview.h"
#include "SecretStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QtConcurrent/QtConcurrentRun>

namespace {

void git(const QString &dir, const QStringList &args, QByteArray *out = nullptr)
{
    QProcess p;
    p.setWorkingDirectory(dir);
    p.start(QStringLiteral("git"), QStringList{QStringLiteral("-c"), QStringLiteral("user.name=t"), QStringLiteral("-c"),
                                               QStringLiteral("user.email=t@example.com"), QStringLiteral("-c"),
                                               QStringLiteral("commit.gpgsign=false")}
                                       + args);
    QVERIFY(p.waitForFinished());
    QVERIFY2(p.exitCode() == 0, p.readAllStandardError().constData());
    if (out)
        *out = p.readAllStandardOutput().trimmed();
}

void write(const QString &path, const QByteArray &data)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

// A tiny HTTP/1.1 server standing in for Azure DevOps.
class FakeAdo : public QObject
{
public:
    struct Request {
        QByteArray method, path, auth, body;
    };
    QVector<Request> requests;
    QByteArray acceptedAuth; // anything else gets 203 + HTML, like the real service
    QString remoteUrl;
    QString sourceCommit, targetCommit;
    int vote = 5; // user-1's vote, as PUT to the reviewers endpoint
    QByteArray changesJson;              // the latest iteration's changes
    QHash<QByteArray, QByteArray> blobs; // blob id -> content
    quint16 port = 0;

    FakeAdo()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = m_server.nextPendingConnection())
                connect(s, &QTcpSocket::readyRead, this, [this, s] { onData(s); });
        });
        m_server.listen(QHostAddress::LocalHost);
        port = m_server.serverPort();
    }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(port); }

private:
    void onData(QTcpSocket *s)
    {
        m_buffers[s] += s->readAll();
        QByteArray &buf = m_buffers[s];
        const qsizetype headerEnd = buf.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        const QList<QByteArray> lines = buf.left(headerEnd).split('\n');
        qsizetype length = 0;
        Request r;
        const QList<QByteArray> first = lines.first().trimmed().split(' ');
        r.method = first.value(0);
        r.path = QByteArray::fromPercentEncoding(first.value(1));
        for (const QByteArray &l : lines) {
            const QByteArray lower = l.toLower();
            if (lower.startsWith("content-length:"))
                length = l.mid(15).trimmed().toLongLong();
            if (lower.startsWith("authorization:"))
                r.auth = l.mid(14).trimmed();
        }
        if (buf.size() < headerEnd + 4 + length)
            return;
        r.body = buf.mid(headerEnd + 4, length);
        buf.clear();
        requests << r;
        respond(s, r);
    }

    void respond(QTcpSocket *s, const Request &r)
    {
        int code = 200;
        QByteArray body;
        if (r.auth != acceptedAuth) {
            code = 203; // Azure DevOps' way of saying "sign in first"
            body = "<html>Sign in</html>";
        } else if (r.path.startsWith("/org/_apis/connectionData")) {
            body = R"({"authenticatedUser":{"id":"user-1","providerDisplayName":"Test User"}})";
        } else if (r.path.contains("/pullRequests/7/threads/5/comments")) {
            body = R"({"id":3,"parentCommentId":1,"content":"thanks","author":{"displayName":"Test User"},"publishedDate":"2026-01-03T00:00:00Z"})";
        } else if (r.path.contains("/pullRequests/7/threads") && r.method == "POST") {
            const QJsonObject in = QJsonDocument::fromJson(r.body).object();
            QJsonObject t{{QStringLiteral("id"), 99},
                          {QStringLiteral("comments"),
                           QJsonArray{QJsonObject{{QStringLiteral("id"), 1},
                                                  {QStringLiteral("parentCommentId"), 0},
                                                  {QStringLiteral("content"), in.value(QStringLiteral("comments")).toArray().first()
                                                                                  .toObject().value(QStringLiteral("content"))},
                                                  {QStringLiteral("author"), QJsonObject{{QStringLiteral("displayName"), QStringLiteral("Test User")}}},
                                                  {QStringLiteral("publishedDate"), QStringLiteral("2026-01-02T00:00:00Z")}}}}};
            if (in.contains(QStringLiteral("threadContext")))
                t.insert(QStringLiteral("threadContext"), in.value(QStringLiteral("threadContext")));
            body = QJsonDocument(t).toJson();
        } else if (r.path.contains("/pullRequests/7/threads")) {
            body = R"({"value":[
              {"id":5,"isDeleted":false,"threadContext":{"filePath":"/a.txt","rightFileStart":{"line":2,"offset":1},"rightFileEnd":{"line":2,"offset":5}},
               "comments":[{"id":1,"parentCommentId":0,"content":"Why TWO?","commentType":"text","author":{"displayName":"Alice"},"publishedDate":"2026-01-01T00:00:00Z"},
                           {"id":2,"parentCommentId":1,"content":"Because.","commentType":"text","author":{"displayName":"Bob"},"publishedDate":"2026-01-01T01:00:00Z"},
                           {"id":3,"parentCommentId":0,"content":"Bob changed the status","commentType":"system","author":{"displayName":"Bob"}}]},
              {"id":6,"isDeleted":false,"comments":[{"id":1,"parentCommentId":0,"content":"Overall fine","commentType":"text","author":{"displayName":"Carol"}}]},
              {"id":8,"isDeleted":false,"threadContext":{"filePath":"/a.txt","leftFileStart":{"line":1,"offset":1},"leftFileEnd":{"line":3,"offset":1}},
               "comments":[{"id":1,"parentCommentId":0,"content":"old side","author":{"displayName":"Dan"},"publishedDate":"2026-01-01T02:00:00Z"}]}]})";
        } else if (r.path.contains("/pullRequests/7/reviewers/user-1")) {
            if (r.method == "PUT")
                vote = QJsonDocument::fromJson(r.body).object().value(QStringLiteral("vote")).toInt();
            body = QJsonDocument(QJsonObject{{QStringLiteral("vote"), vote}}).toJson();
        } else if (r.path.contains("/pullRequests/7/reviewers")) {
            body = QJsonDocument(QJsonObject{{QStringLiteral("value"),
                                              QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("someone-else")},
                                                                     {QStringLiteral("vote"), -10}},
                                                         QJsonObject{{QStringLiteral("id"), QStringLiteral("USER-1")},
                                                                     {QStringLiteral("vote"), vote}}}}})
                       .toJson();
        } else if (r.path.contains("/pullRequests/7/iterations/2/changes")) {
            body = changesJson;
        } else if (r.path.contains("/pullRequests/7/iterations")) {
            body = QJsonDocument(
                       QJsonObject{{QStringLiteral("value"),
                                    QJsonArray{QJsonObject{{QStringLiteral("id"), 1},
                                                           {QStringLiteral("sourceRefCommit"), QJsonObject{{QStringLiteral("commitId"), targetCommit}}},
                                                           {QStringLiteral("commonRefCommit"), QJsonObject{{QStringLiteral("commitId"), targetCommit}}}},
                                               QJsonObject{{QStringLiteral("id"), 2},
                                                           {QStringLiteral("sourceRefCommit"), QJsonObject{{QStringLiteral("commitId"), sourceCommit}}},
                                                           {QStringLiteral("commonRefCommit"), QJsonObject{{QStringLiteral("commitId"), targetCommit}}}}}}})
                       .toJson();
        } else if (r.path.contains("/_apis/git/repositories/repo/blobs/")) {
            const QByteArray id = QString::fromLatin1(r.path).section(u'/', -1).section(u'?', 0, 0).toLatin1();
            if (blobs.contains(id)) {
                body = blobs.value(id);
            } else {
                code = 404;
                body = R"({"message":"no such blob"})";
            }
        } else if (r.path.contains("/pullRequests/7")) {
            body = QJsonDocument(QJsonObject{
                                     {QStringLiteral("title"), QStringLiteral("Make two loud")},
                                     {QStringLiteral("status"), QStringLiteral("active")},
                                     {QStringLiteral("sourceRefName"), QStringLiteral("refs/heads/feature")},
                                     {QStringLiteral("targetRefName"), QStringLiteral("refs/heads/main")},
                                     {QStringLiteral("lastMergeSourceCommit"), QJsonObject{{QStringLiteral("commitId"), sourceCommit}}},
                                     {QStringLiteral("lastMergeTargetCommit"), QJsonObject{{QStringLiteral("commitId"), targetCommit}}},
                                     {QStringLiteral("repository"), QJsonObject{{QStringLiteral("remoteUrl"), remoteUrl}}}})
                       .toJson();
        } else {
            code = 404;
            body = R"({"message":"not found"})";
        }
        s->write("HTTP/1.1 " + QByteArray::number(code) + " X\r\nContent-Type: application/json\r\nContent-Length: "
                 + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        s->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

const QByteArray kPat = "secret-pat";
QByteArray patHeader()
{
    return "Basic " + (":" + kPat).toBase64();
}

} // namespace

class TestAdo : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true); // cache clones go to a test location
        m_secrets.reset(new QTemporaryDir);
        qputenv("YADIFF_SECRET_STORE_FILE", m_secrets->filePath(QStringLiteral("secrets.json")).toUtf8());
        for (const char *v : {"YADIFF_ADO_TOKEN", "SYSTEM_ACCESSTOKEN", "YADIFF_ADO_PAT", "AZURE_DEVOPS_EXT_PAT"})
            qunsetenv(v);
    }

    void parsesLinks()
    {
        auto r = AdoPr::parse(QStringLiteral("https://dev.azure.com/contoso/My%20Project/_git/web-app/pullrequest/42?_a=files"));
        QVERIFY(r);
        QCOMPARE(r->collectionUrl.toString(), QStringLiteral("https://dev.azure.com/contoso"));
        QCOMPARE(r->project, QStringLiteral("My Project"));
        QCOMPARE(r->repo, QStringLiteral("web-app"));
        QCOMPARE(r->id, 42);
        QVERIFY(!r->webUrl.toString().contains(u'?'));

        r = AdoPr::parse(QStringLiteral("https://contoso.visualstudio.com/DefaultCollection/Proj/_git/Repo/pullrequest/7/overview"));
        QVERIFY(r);
        QCOMPARE(r->collectionUrl.toString(), QStringLiteral("https://contoso.visualstudio.com/DefaultCollection"));
        QCOMPARE(r->project, QStringLiteral("Proj"));
        r = AdoPr::parse(QStringLiteral("https://tfs.example.com/tfs/Main/Proj/_git/Repo/pullrequest/3"));
        QVERIFY(r);
        QCOMPARE(r->collectionUrl.toString(), QStringLiteral("https://tfs.example.com/tfs/Main"));
        QVERIFY(!AdoPr::parse(QStringLiteral("https://github.com/a/b/pull/3")));
        QVERIFY(!AdoPr::parse(QStringLiteral("https://dev.azure.com/contoso/Proj/_git/Repo")));
    }

    void normalisesOrganisations()
    {
        QCOMPARE(AdoAuth::organizationUrl(QUrl(QStringLiteral("https://contoso.visualstudio.com/DefaultCollection"))).toString(),
                 QStringLiteral("https://dev.azure.com/contoso"));
        QCOMPARE(AdoAuth::organizationUrl(QUrl(QStringLiteral("https://user@dev.azure.com/contoso/Proj/_git/R"))).toString(),
                 QStringLiteral("https://dev.azure.com/contoso"));
        QCOMPARE(AdoAuth::organizationUrl(QUrl(QStringLiteral("https://tfs.example.com/tfs/Main"))).toString(),
                 QStringLiteral("https://tfs.example.com/tfs/Main"));
    }

    void matchesRemotes()
    {
        const auto r = AdoPr::parse(QStringLiteral("https://dev.azure.com/contoso/My%20Project/_git/web-app/pullrequest/1"));
        QVERIFY(AdoPr::remoteMatches(QStringLiteral("https://contoso@dev.azure.com/contoso/My%20Project/_git/web-app"), *r));
        QVERIFY(AdoPr::remoteMatches(QStringLiteral("https://contoso.visualstudio.com/My%20Project/_git/web-app"), *r));
        QVERIFY(AdoPr::remoteMatches(QStringLiteral("git@ssh.dev.azure.com:v3/contoso/My%20Project/web-app"), *r));
        QVERIFY(AdoPr::remoteMatches(QStringLiteral("contoso@vs-ssh.visualstudio.com:v3/contoso/My%20Project/web-app"), *r));
        QVERIFY(!AdoPr::remoteMatches(QStringLiteral("https://dev.azure.com/other/My%20Project/_git/web-app"), *r));
        QVERIFY(!AdoPr::remoteMatches(QStringLiteral("https://dev.azure.com/contoso/My%20Project/_git/web-app2"), *r));
        QVERIFY(!AdoPr::remoteMatches(QStringLiteral("https://github.com/contoso/web-app.git"), *r));
    }

    void credentialsFollowThePrecedenceOrder()
    {
        const QUrl org(QStringLiteral("https://dev.azure.com/contoso"));
        QString diag;
        QVERIFY(!AdoAuth::find(QUrl(QStringLiteral("https://dev.azure.com/nobody-here")), &diag));
        QVERIFY(diag.contains(QStringLiteral("Tried")));
        QString error;
        QVERIFY(AdoAuth::savePat(QUrl(QStringLiteral("https://contoso.visualstudio.com")), QStringLiteral("from-keyring"), &error));
        auto c = AdoAuth::find(org);
        QVERIFY(c); // saved under the visualstudio.com spelling, found under dev.azure.com
        QCOMPARE(c->secret, QStringLiteral("from-keyring"));
        QCOMPARE(c->authorizationHeader(), QByteArray("Basic ") + QByteArray(":from-keyring").toBase64());
        qputenv("AZURE_DEVOPS_EXT_PAT", "from-env");
        QCOMPARE(AdoAuth::find(org)->secret, QStringLiteral("from-env")); // environment wins
        qputenv("YADIFF_ADO_TOKEN", "bearer-token");
        c = AdoAuth::find(org);
        QCOMPARE(c->kind, AdoAuth::Credential::Kind::Bearer);
        QCOMPARE(c->authorizationHeader(), QByteArray("Bearer bearer-token"));
        qunsetenv("YADIFF_ADO_TOKEN");
        qunsetenv("AZURE_DEVOPS_EXT_PAT");
        QVERIFY(AdoAuth::forget(org, &error));
        QVERIFY(!SecretStore::read(QStringLiteral("ado:https://dev.azure.com/contoso")));
    }

    void neverSendsCredentialsInTheClear()
    {
        QVERIFY(Http::safeForCredentials(QUrl(QStringLiteral("https://dev.azure.com/x"))));
        QVERIFY(Http::safeForCredentials(QUrl(QStringLiteral("http://127.0.0.1:8080/x"))));
        QVERIFY(!Http::safeForCredentials(QUrl(QStringLiteral("http://tfs.example.com/tfs"))));
        QString error;
        AdoAuth::Credential c{AdoAuth::Credential::Kind::Pat, QStringLiteral("x"), {}};
        QVERIFY(!AdoAuth::validate(QUrl(QStringLiteral("http://tfs.example.com/tfs")), c, &error));
        QVERIFY(error.contains(QStringLiteral("unencrypted")));
    }

    void parsesTheRestApisView()
    {
        QCOMPARE(AdoPr::safeRelativePath(QStringLiteral("/dir/a.txt")), QStringLiteral("dir/a.txt"));
        for (const char *bad : {"/../x", "/a/../../x", "/a/./b", "/C:/x", "/a\\b", "/", "//", "/a//b"})
            QVERIFY2(AdoPr::safeRelativePath(QString::fromLatin1(bad)).isEmpty(), bad);
        const auto it = AdoPr::parseLatestIteration(R"({"value":[
            {"id":3,"sourceRefCommit":{"commitId":"h3"},"commonRefCommit":{"commitId":"b3"}},
            {"id":1,"sourceRefCommit":{"commitId":"h1"},"commonRefCommit":{"commitId":"b1"}}]})");
        QVERIFY(it && it->id == 3 && it->headCommit == QLatin1String("h3") && it->baseCommit == QLatin1String("b3"));
        int next = -1;
        const auto changes = AdoPr::parseChanges(R"({"changeEntries":[
            {"changeType":"edit, rename","originalPath":"/old.txt","item":{"path":"/new.txt","objectId":"n","originalObjectId":"o"}},
            {"changeType":"delete","item":{"path":"/gone.txt","objectId":"g"}}],"nextSkip":2000})", &next);
        QCOMPARE(next, 2000);
        QCOMPARE(changes.size(), 2);
        QCOMPARE(changes[0].oldPath, QStringLiteral("/old.txt"));
        QCOMPARE(changes[0].path, QStringLiteral("/new.txt"));
        QVERIFY(changes[1].path.isEmpty() && changes[1].oldObjectId == QLatin1String("g") && changes[1].objectId.isEmpty());
    }

    void parsesThreadsAndBuildsPayloads()
    {
        const QByteArray json = R"({"value":[{"id":5,"threadContext":{"filePath":"/src/a.cpp","rightFileStart":{"line":10},"rightFileEnd":{"line":12}},
            "comments":[{"id":1,"parentCommentId":0,"content":"root","author":{"displayName":"A"},"publishedDate":"2026-01-01"},
                        {"id":2,"parentCommentId":1,"content":"reply","author":{"displayName":"B"},"publishedDate":"2026-01-02"}]}]})";
        const auto comments = AdoPr::parseThreads(json, QUrl(QStringLiteral("https://dev.azure.com/o/p/_git/r/pullrequest/1")));
        QCOMPARE(comments.size(), 2);
        QCOMPARE(comments[0].id, qint64(5));
        QCOMPARE(comments[0].path, QStringLiteral("src/a.cpp"));
        QCOMPARE(comments[0].side, QStringLiteral("RIGHT"));
        QCOMPARE(comments[0].line, 12);
        QCOMPARE(comments[0].startLine, 10);
        QCOMPARE(comments[1].inReplyTo, qint64(5));
        QVERIFY(comments[0].url.endsWith(QStringLiteral("?discussionId=5")));
        const auto threads = GitHubReview::threads(comments);
        QCOMPARE(threads.size(), 1);
        QCOMPARE(threads[0].size(), 2);

        const QJsonObject p = QJsonDocument::fromJson(AdoPr::threadPayload(QStringLiteral("src/a.cpp"), QStringLiteral("LEFT"), 4, 2,
                                                                            QStringLiteral("hi")))
                                  .object();
        const QJsonObject ctx = p.value(QStringLiteral("threadContext")).toObject();
        QCOMPARE(ctx.value(QStringLiteral("filePath")).toString(), QStringLiteral("/src/a.cpp"));
        QCOMPARE(ctx.value(QStringLiteral("leftFileStart")).toObject().value(QStringLiteral("line")).toInt(), 2);
        QCOMPARE(ctx.value(QStringLiteral("leftFileEnd")).toObject().value(QStringLiteral("line")).toInt(), 4);
        QVERIFY(!ctx.contains(QStringLiteral("rightFileStart")));
    }

    void resolvesReviewsAndVotesAgainstAFakeServer()
    {
        QTemporaryDir tmp;
        const QString work = tmp.filePath(QStringLiteral("work"));
        const QString remote = tmp.filePath(QStringLiteral("remote.git"));
        QDir().mkpath(work);
        git(work, {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"), QStringLiteral("main")});
        write(work + QStringLiteral("/a.txt"), "one\ntwo\nthree\n");
        write(work + QStringLiteral("/gone.txt"), "going\n");
        QDir().mkpath(work + QStringLiteral("/sub"));
        write(work + QStringLiteral("/sub/r.txt"), "renamed\n");
        git(work, {QStringLiteral("add"), QStringLiteral("-A")});
        git(work, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("base")});
        QByteArray target, source;
        git(work, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}, &target);
        git(work, {QStringLiteral("switch"), QStringLiteral("-q"), QStringLiteral("-c"), QStringLiteral("feature")});
        write(work + QStringLiteral("/a.txt"), "one\nTWO\nthree\n");
        write(work + QStringLiteral("/new.txt"), "fresh\n");
        git(work, {QStringLiteral("rm"), QStringLiteral("-q"), QStringLiteral("gone.txt")});
        git(work, {QStringLiteral("mv"), QStringLiteral("sub/r.txt"), QStringLiteral("sub/r2.txt")});
        git(work, {QStringLiteral("add"), QStringLiteral("-A")});
        git(work, {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("loud")});
        git(work, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}, &source);
        git(tmp.path(), {QStringLiteral("clone"), QStringLiteral("-q"), QStringLiteral("--bare"), work, remote});

        FakeAdo ado;
        ado.acceptedAuth = patHeader();
        ado.remoteUrl = remote;
        ado.sourceCommit = QString::fromLatin1(source);
        ado.targetCommit = QString::fromLatin1(target);
        // The API's view of the PR: its changed files with their blob ids, and the blobs.
        auto blobId = [&](const QByteArray &commit, const char *path) {
            QByteArray id;
            git(work, {QStringLiteral("rev-parse"), QString::fromLatin1(commit + ':' + path)}, &id);
            QProcess cat; // not git(), which trims: a blob's last newline matters here
            cat.setWorkingDirectory(work);
            cat.start(QStringLiteral("git"), {QStringLiteral("cat-file"), QStringLiteral("blob"), QString::fromLatin1(id)});
            cat.waitForFinished();
            ado.blobs.insert(id, cat.readAllStandardOutput());
            return QString::fromLatin1(id);
        };
        auto entry = [](const char *type, const QString &path, const QString &id, const QString &oldId, const QString &original = {}) {
            QJsonObject item{{QStringLiteral("path"), path}};
            if (!id.isEmpty())
                item.insert(QStringLiteral("objectId"), id);
            if (!oldId.isEmpty())
                item.insert(QStringLiteral("originalObjectId"), oldId);
            QJsonObject e{{QStringLiteral("changeType"), QLatin1String(type)}, {QStringLiteral("item"), item}};
            if (!original.isEmpty())
                e.insert(QStringLiteral("originalPath"), original);
            return e;
        };
        ado.changesJson = QJsonDocument(QJsonObject{
            {QStringLiteral("changeEntries"),
             QJsonArray{entry("edit", QStringLiteral("/a.txt"), blobId(source, "a.txt"), blobId(target, "a.txt")),
                        entry("add", QStringLiteral("/new.txt"), blobId(source, "new.txt"), {}),
                        entry("delete", QStringLiteral("/gone.txt"), {}, blobId(target, "gone.txt")),
                        entry("rename", QStringLiteral("/sub/r2.txt"), blobId(source, "sub/r2.txt"), blobId(target, "sub/r.txt"),
                              QStringLiteral("/sub/r.txt")),
                        QJsonObject{{QStringLiteral("changeType"), QStringLiteral("edit")},
                                    {QStringLiteral("item"), QJsonObject{{QStringLiteral("path"), QStringLiteral("/sub")},
                                                                         {QStringLiteral("isFolder"), true}}}},
                        entry("add", QStringLiteral("/../escape.txt"), blobId(source, "new.txt"), {})}},
            {QStringLiteral("nextSkip"), 0}}).toJson();
        const auto ref = AdoPr::parse(ado.base() + QStringLiteral("/org/My%20Project/_git/repo/pullrequest/7"));
        QVERIFY(ref);
        const AdoAuth::Credential cred{AdoAuth::Credential::Kind::Pat, QString::fromLatin1(kPat), QStringLiteral("test")};

        // A rejected credential is reported as a sign-in problem (Azure DevOps answers 203).
        int status = 0;
        QString error;
        AdoAuth::Credential wrong{AdoAuth::Credential::Kind::Pat, QStringLiteral("nope"), {}};
        QVERIFY(!AdoPr::resolve(*ref, wrong, tmp.path(), &error, &status));
        QCOMPARE(status, 203);
        QVERIFY(!AdoAuth::authProblem(status).isEmpty());

        // Without a clone, the right one downloads the PR's changed files (both versions of
        // each) from the latest iteration, against its merge base. No git fetch at all.
        // On a worker thread, as in the window: the download waits for its own threads, and
        // this one has to keep serving the fake server meanwhile.
        auto resolveOffThread = [&] {
            auto future = QtConcurrent::run([&] { return AdoPr::resolve(*ref, cred, tmp.path(), &error, &status); });
            while (!future.isFinished())
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            return future.result();
        };
        auto pr = resolveOffThread();
        QVERIFY2(pr, qPrintable(error));
        QCOMPARE(pr->headOid, ado.sourceCommit);
        QCOMPARE(pr->baseOid, ado.targetCommit);
        QVERIFY(pr->title.contains(QStringLiteral("Make two loud")));
        QVERIFY(pr->headName.contains(QStringLiteral("feature")));
        QVERIFY(pr->repoPath.startsWith(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)));
        for (const auto &r : ado.requests)
            QVERIFY(r.path.contains("/My Project/")); // project names are URL-encoded properly
        auto read = [](const QString &path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("(missing)");
        };
        QCOMPARE(read(pr->baseDir + QStringLiteral("/a.txt")), QByteArray("one\ntwo\nthree\n"));
        QCOMPARE(read(pr->headDir + QStringLiteral("/a.txt")), QByteArray("one\nTWO\nthree\n"));
        QCOMPARE(read(pr->headDir + QStringLiteral("/new.txt")), QByteArray("fresh\n"));
        QVERIFY(!QFile::exists(pr->baseDir + QStringLiteral("/new.txt")));
        QCOMPARE(read(pr->baseDir + QStringLiteral("/gone.txt")), QByteArray("going\n"));
        QVERIFY(!QFile::exists(pr->headDir + QStringLiteral("/gone.txt")));
        QCOMPARE(read(pr->baseDir + QStringLiteral("/sub/r.txt")), QByteArray("renamed\n"));
        QCOMPARE(read(pr->headDir + QStringLiteral("/sub/r2.txt")), QByteArray("renamed\n"));
        // "/../escape.txt" was refused: nothing outside the two folders.
        QVERIFY(!QFile::exists(pr->repoPath + QStringLiteral("/escape.txt")));
        QVERIFY(!QFile::exists(QFileInfo(pr->repoPath).absolutePath() + QStringLiteral("/escape.txt")));
        // A second look at the same iteration downloads nothing.
        auto blobRequests = [&] {
            int n = 0;
            for (const auto &r : ado.requests)
                n += r.path.contains("/blobs/");
            return n;
        };
        const int before = blobRequests();
        QVERIFY(resolveOffThread());
        QCOMPARE(blobRequests(), before);

        // The two folders compare like any two folders, read-only.
        auto repo = std::optional<GitRepo>(GitRepo::forPaths(pr->repoPath));
        Comparison cmp;
        cmp.mode = Comparison::Mode::Paths;
        cmp.leftPath = pr->baseDir;
        cmp.rightPath = pr->headDir;
        cmp.readOnly = true;
        const auto changes = repo->listChanges(cmp, &error);
        QStringList paths;
        for (const FileChange &fc : changes)
            paths << fc.path;
        QVERIFY2(paths.contains(QStringLiteral("a.txt")) && paths.contains(QStringLiteral("new.txt"))
                     && paths.contains(QStringLiteral("gone.txt")) && paths.contains(QStringLiteral("sub/r2.txt")),
                 qPrintable(paths.join(u',')));
        QVERIFY(repo->diskPath(changes[0], cmp, true).isEmpty());

        // Review operations through the same entry points the window uses.
        cmp.prHost = QStringLiteral("ado");
        cmp.prCollectionUrl = ref->collectionUrl.toString();
        cmp.prProject = ref->project;
        cmp.prRepo = ref->repo;
        cmp.prNumber = ref->id;
        cmp.prWebUrl = ref->webUrl.toString();
        qputenv("YADIFF_ADO_PAT", kPat);
        const auto comments = PrReview::fetch(cmp, &error);
        QVERIFY2(comments, qPrintable(error));
        QCOMPARE(comments->size(), 3); // two in thread 5 (system message skipped), one in thread 8; thread 6 has no file
        const auto threads = GitHubReview::threads(*comments);
        QCOMPARE(threads.size(), 2);
        QCOMPARE(threads[0].first().author, QStringLiteral("Alice"));
        QCOMPARE(threads[0].size(), 2);
        QCOMPARE(threads[1].first().side, QStringLiteral("LEFT"));
        QCOMPARE(threads[1].first().startLine, 1);

        const auto posted = PrReview::post(cmp, QStringLiteral("a.txt"), QStringLiteral("RIGHT"), 2, 0, QStringLiteral("Nice"), &error);
        QVERIFY2(posted, qPrintable(error));
        QCOMPARE(posted->id, qint64(99));
        QCOMPARE(posted->line, 2);
        const QJsonObject sent = QJsonDocument::fromJson(ado.requests.last().body).object();
        QCOMPARE(sent.value(QStringLiteral("threadContext")).toObject().value(QStringLiteral("filePath")).toString(), QStringLiteral("/a.txt"));

        const auto replied = PrReview::reply(cmp, threads[0][1].id, QStringLiteral("thanks"), &error); // reply to a reply
        QVERIFY2(replied, qPrintable(error));
        QVERIFY(ado.requests.last().path.contains("/threads/5/comments")); // lands in the right thread
        QCOMPARE(replied->inReplyTo, qint64(5));

        QVERIFY2(PrReview::submit(cmp, GitHubReview::Verdict::Approve, {}, &error), qPrintable(error));
        QCOMPARE(ado.requests.last().method, QByteArray("PUT"));
        QVERIFY(ado.requests.last().path.contains("/reviewers/user-1"));
        QCOMPARE(QJsonDocument::fromJson(ado.requests.last().body).object().value(QStringLiteral("vote")).toInt(), 10);
        QVERIFY(PrReview::submit(cmp, GitHubReview::Verdict::RequestChanges, QStringLiteral("Please fix"), &error));
        QCOMPARE(ado.requests.last().method, QByteArray("POST")); // the message, after the -5 vote
        bool votedWait = false;
        for (const auto &r : ado.requests)
            votedWait |= r.method == "PUT" && QJsonDocument::fromJson(r.body).object().value(QStringLiteral("vote")).toInt() == -5;
        QVERIFY(votedWait);

        // Votes: every one Azure DevOps has, each read back, then reset; a message goes
        // with a vote as a comment.
        using Vote = GitHubReview::Vote;
        QCOMPARE(PrReview::votes(cmp).size(), 5);
        for (Vote v : {Vote::Approved, Vote::ApprovedWithSuggestions, Vote::WaitingForAuthor, Vote::Rejected, Vote::None}) {
            QVERIFY2(PrReview::setVote(cmp, v, {}, &error), qPrintable(error));
            QCOMPARE(ado.vote, PrReview::adoVoteValue(v));
            QCOMPARE(PrReview::myVote(cmp, &error), std::optional<Vote>(v));
        }
        QCOMPARE(PrReview::adoVoteValue(Vote::ApprovedWithSuggestions), 5);
        QCOMPARE(PrReview::adoVote(-10), Vote::Rejected);
        QCOMPARE(PrReview::adoVote(0), Vote::None);
        QVERIFY(PrReview::setVote(cmp, Vote::Approved, QStringLiteral("Looks good"), &error));
        QCOMPARE(ado.requests.last().method, QByteArray("POST"));
        QVERIFY(ado.requests.last().body.contains("Looks good"));
        QCOMPARE(AdoPr::parseVote(R"({"value":[{"id":"a","vote":5}]})", QStringLiteral("b")), 0); // not a reviewer
        for (const auto &r : ado.requests)
            if (r.path.contains("/pullRequests/7") && r.auth == patHeader())
                QVERIFY(true);
        qunsetenv("YADIFF_ADO_PAT");

        // And the real command line: yadiff -ado LINK --export writes the PR's report.
        const QString report = tmp.filePath(QStringLiteral("report.html"));
        QProcess yadiff;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("YADIFF_ADO_PAT"), QString::fromLatin1(kPat));
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        env.insert(QStringLiteral("XDG_CACHE_HOME"), tmp.filePath(QStringLiteral("cache")));
        env.insert(QStringLiteral("LOCALAPPDATA"), tmp.filePath(QStringLiteral("cache")));
        yadiff.setProcessEnvironment(env);
        yadiff.setWorkingDirectory(tmp.path());
        yadiff.start(QStringLiteral(YADIFF_BINARY),
                    {QStringLiteral("-ado"), ado.base() + QStringLiteral("/org/My%20Project/_git/repo/pullrequest/7"),
                     QStringLiteral("--export"), report, QStringLiteral("--log"), tmp.filePath(QStringLiteral("yadiff.log")),
                     QStringLiteral("--log-level"), QStringLiteral("debug")});
        QTRY_VERIFY_WITH_TIMEOUT(yadiff.state() == QProcess::NotRunning, 60000);
        QVERIFY2(yadiff.exitCode() == 0, yadiff.readAllStandardError().constData());
        QFile f(report);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray html = f.readAll();
        QVERIFY(html.contains("Make two loud"));
        QVERIFY(html.contains("TWO"));

        // The log traces the whole thing, and never shows the token.
        QFile logFile(tmp.filePath(QStringLiteral("yadiff.log")));
        QVERIFY(logFile.open(QIODevice::ReadOnly));
        const QByteArray log = logFile.readAll();
        if (qEnvironmentVariableIsSet("YADIFF_TEST_DUMP_LOG"))
            qInfo().noquote() << log;
        for (const char *expected : {"yadiff.auth", "personal access token from YADIFF_ADO_PAT", "GET http://127.0.0.1",
                                     "/pullRequests/7?api-version=7.1 -> HTTP 200", "resolving PR 7", "is not a git working tree",
                                     "/iterations/2/changes", "/blobs/", "PR 7 downloaded", "YADIFF_ADO_PAT (set)",
                                     "Authorization"})
            QVERIFY2(log.contains(expected), expected);
        QVERIFY(!log.contains(kPat));
        QVERIFY(!log.contains(patHeader()));
        QVERIFY(!log.contains(patHeader().mid(6))); // the base64 credential alone

        // Opening a window: the window resolves the PR itself (a render of its one file
        // proves it got there), and the start-up process doesn't resolve it first.
        QProcess window;
        env.insert(QStringLiteral("YADIFF_GRAB"), tmp.filePath(QStringLiteral("shot")));
        env.insert(QStringLiteral("YADIFF_NO_UPDATE_CHECK"), QStringLiteral("1"));
        window.setProcessEnvironment(env);
        window.setWorkingDirectory(tmp.path());
        window.start(QStringLiteral(YADIFF_BINARY),
                     {QStringLiteral("--wait"), QStringLiteral("-ado"),
                      ado.base() + QStringLiteral("/org/My%20Project/_git/repo/pullrequest/7"), QStringLiteral("--log"),
                      tmp.filePath(QStringLiteral("window.log"))});
        QTRY_VERIFY_WITH_TIMEOUT(window.state() == QProcess::NotRunning, 60000);
        QVERIFY2(window.exitCode() == 0, window.readAllStandardError().constData());
        QVERIFY(QFile::exists(tmp.filePath(QStringLiteral("shot-00-side.png"))));
        QFile windowLog(tmp.filePath(QStringLiteral("window.log")));
        QVERIFY(windowLog.open(QIODevice::ReadOnly));
        const QByteArray wlog = windowLog.readAll();
        QVERIFY(wlog.contains("a pull request, opened in the window"));
        QVERIFY(wlog.contains("in the background"));
        QVERIFY(wlog.contains("is downloaded already")); // the report's download, reused
    }

    void cleanupTestCase()
    {
        // Test mode keeps these under ~/.qttest, never the real cache; still, tidy up.
        QDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/yadiff/ado")).removeRecursively();
        QDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/yadiff/ado-pr")).removeRecursively();
    }

private:
    std::unique_ptr<QTemporaryDir> m_secrets;
};

QTEST_GUILESS_MAIN(TestAdo)
#include "test_ado.moc"
