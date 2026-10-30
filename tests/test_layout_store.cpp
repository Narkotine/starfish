// T-008 -- persistence of the layout: the document format (core::JsonStore) and
// the writer that owns it (core::LayoutStore). Pure Qt Core, no Win32, no
// widgets: the desktop-facing half is exercised by `starfish --persist-demo`.
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "core/jsonstore.hpp"
#include "core/layout_store.hpp"

using namespace starfish::core;

namespace {

Fence makeFence(const QString& id, const QString& title, const QRect& rect)
{
    Fence fence;
    fence.id = id;
    fence.title = title;
    fence.monitor = QStringLiteral("\\\\.\\DISPLAY1");
    fence.rect = rect;
    fence.style.background = QStringLiteral("#CC1F2937");
    fence.style.radius = 14;
    fence.style.iconSize = 64;
    fence.style.titleBar = false;
    fence.style.iconTint = QStringLiteral("#80FFAA00");
    // T-082: the option that hands the material to the icons as well. Written for
    // every fence, so the round-trip below covers it like any other field.
    fence.style.fadeIcons = true;
    // T-080: the fence's own title bar colour -- written only when it has one, so
    // the fence that carries it is the one the round-trip below proves.
    fence.style.titleBarColor = QStringLiteral("#CC112233");
    fence.sortBy = QStringLiteral("size");
    fence.sortAscending = false;
    fence.rolledUp = true;
    fence.items = {{QStringLiteral("file"), QStringLiteral("C:/Desktop/a.pdf")},
                   {QStringLiteral("portal"), QStringLiteral("C:/Docs/Factures")}};
    return fence;
}

// T-010: a rule carrying every criterion, so the round-trip test below covers
// the whole `match` object and not just the easy half.
Rule makeRule()
{
    Rule rule;
    rule.id = QStringLiteral("rule-001");
    rule.name = QStringLiteral("Images → Médias");
    rule.extensions = {QStringLiteral("jpg"), QStringLiteral("png")};
    rule.namePattern = QStringLiteral("shot*");
    rule.newerThanDays = 30;
    rule.olderThanDays = 1;
    rule.targetFenceId = QStringLiteral("fence-001");
    rule.moveFiles = true;
    return rule;
}

Layout makeLayout()
{
    Layout layout;
    layout.fences = {makeFence(QStringLiteral("fence-001"), QStringLiteral("Travail"),
                               QRect(10, 20, 480, 320)),
                     makeFence(QStringLiteral("fence-002"), QStringLiteral("Perso"),
                               QRect(500, 20, 200, 160))};
    layout.rules = {makeRule()};
    return layout;
}

} // namespace

class TestLayoutStore : public QObject
{
    Q_OBJECT

private slots:
    void jsonRoundTripsEveryField();
    // T-080: a fence with no title bar colour of its own must not gain a key.
    void anEmptyTitleBarColourWritesNoKey();
    void jsonRejectsAnotherSchema();
    void jsonRejectsGarbage();
    void saveCreatesMissingParentDirectory();
    void loadMissingFileIsFirstRun();
    void loadKeepsTheLayoutWhenTheFileIsCorrupt();
    void setLayoutIgnoresAnIdenticalDocument();
    void scheduleSaveDebouncesABurst();
    void saveNowReportsFailureOnAnUnwritablePath();
    // T-010 -- the `rules` array.
    void jsonRoundTripsRules();
    void jsonAcceptsADocumentWithoutRules();
    // T-098 -- a fence's own transparency, or the default.
    void defaultOpacityRoundTripsAndMigrates();
};

void TestLayoutStore::jsonRoundTripsEveryField()
{
    const Layout original = makeLayout();
    const QByteArray payload = JsonStore::toJson(original);

    QString error;
    const auto parsed = JsonStore::fromJson(payload, &error);
    QVERIFY2(parsed.has_value(), qPrintable(error));
    QCOMPARE(parsed->schema, 1);
    QCOMPARE(parsed->fences.size(), 2);

    // T-074: a fence the code creates without saying anything about its style is
    // the *opaque* preset now. The document has no `material` field (a material
    // is this alpha), so this default is the whole of the change -- and nothing
    // migrates: what an older document holds is what it keeps.
    QCOMPARE(FenceStyle().background, QStringLiteral("#FF1F2937"));

    const Fence& fence = parsed->fences.at(0);
    QCOMPARE(fence.id, QStringLiteral("fence-001"));
    QCOMPARE(fence.title, QStringLiteral("Travail"));
    QCOMPARE(fence.monitor, QStringLiteral("\\\\.\\DISPLAY1"));
    QCOMPARE(fence.rect, QRect(10, 20, 480, 320));
    QCOMPARE(fence.style.background, QStringLiteral("#CC1F2937"));
    QCOMPARE(fence.style.radius, 14);
    QCOMPARE(fence.style.iconSize, 64);
    QCOMPARE(fence.style.titleBar, false);
    QCOMPARE(fence.style.iconTint, QStringLiteral("#80FFAA00"));
    QCOMPARE(fence.style.fadeIcons, true);
    QCOMPARE(fence.style.titleBarColor, QStringLiteral("#CC112233"));
    QCOMPARE(fence.sortBy, QStringLiteral("size"));
    QCOMPARE(fence.sortAscending, false);
    QCOMPARE(fence.rolledUp, true);
    QCOMPARE(fence.items.size(), 2);
    QCOMPARE(fence.items.at(0).kind, QStringLiteral("file"));
    QCOMPARE(fence.items.at(0).path, QStringLiteral("C:/Desktop/a.pdf"));
    QCOMPARE(fence.items.at(1).kind, QStringLiteral("portal"));
    QCOMPARE(fence.items.at(1).path, QStringLiteral("C:/Docs/Factures"));

    // Deterministic serialisation is what lets LayoutStore detect "no change".
    QCOMPARE(JsonStore::toJson(*parsed), payload);

    // T-082: and a document that predates the key -- or a hand-written one -- simply
    // does not carry it. It has to read as `false` (the material on the panel, not
    // on the icons), because that is what makes this key need no migration and no
    // schema bump: the *default* is the new behaviour, and what a document says it
    // keeps.
    QCOMPARE(FenceStyle().fadeIcons, false);
    QJsonObject root = QJsonDocument::fromJson(payload).object();
    QJsonArray fences = root[QStringLiteral("fences")].toArray();
    for (int i = 0; i < fences.size(); ++i) {
        QJsonObject one = fences.at(i).toObject();
        QJsonObject style = one[QStringLiteral("style")].toObject();
        style.remove(QStringLiteral("fadeIcons"));
        one[QStringLiteral("style")] = style;
        fences.replace(i, one);
    }
    root[QStringLiteral("fences")] = fences;
    const auto withoutTheKey =
        JsonStore::fromJson(QJsonDocument(root).toJson(QJsonDocument::Indented), &error);
    QVERIFY2(withoutTheKey.has_value(), qPrintable(error));
    QVERIFY(!withoutTheKey->fences.isEmpty());
    for (const Fence& fenceWithout : withoutTheKey->fences)
        QCOMPARE(fenceWithout.style.fadeIcons, false);

    // T-080: same promise for the title bar's colour -- and here the *absent* key
    // has to read as "no band of its own", which is the look of every fence that
    // existed before the key did (an empty string, not a colour).
    QCOMPARE(FenceStyle().titleBarColor, QString());
    QJsonObject root2 = QJsonDocument::fromJson(payload).object();
    QJsonArray fences2 = root2[QStringLiteral("fences")].toArray();
    for (int i = 0; i < fences2.size(); ++i) {
        QJsonObject one = fences2.at(i).toObject();
        QJsonObject style = one[QStringLiteral("style")].toObject();
        style.remove(QStringLiteral("titleBarColor"));
        one[QStringLiteral("style")] = style;
        fences2.replace(i, one);
    }
    root2[QStringLiteral("fences")] = fences2;
    const auto withoutTheBand =
        JsonStore::fromJson(QJsonDocument(root2).toJson(QJsonDocument::Indented), &error);
    QVERIFY2(withoutTheBand.has_value(), qPrintable(error));
    QVERIFY(!withoutTheBand->fences.isEmpty());
    for (const Fence& fenceWithout : withoutTheBand->fences)
        QCOMPARE(fenceWithout.style.titleBarColor, QString());
}

void TestLayoutStore::anEmptyTitleBarColourWritesNoKey()
{
    // T-080: the key exists only for a fence that has a band. This is the promise
    // that makes the change invisible to every document written before it: not one
    // existing file gains a line, and LayoutStore's "identical bytes, no write"
    // test keeps saying nothing changed when nothing did.
    Layout layout;
    Fence plain = makeFence(QStringLiteral("fence-001"), QStringLiteral("Travail"),
                            QRect(10, 20, 480, 320));
    plain.style.titleBarColor.clear();
    layout.fences = {plain};

    const QByteArray payload = JsonStore::toJson(layout);
    QVERIFY(!payload.contains("titleBarColor"));

    const auto parsed = JsonStore::fromJson(payload);
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->fences.at(0).style.titleBarColor, QString());

    // ... and a fence that *has* one keeps it, of course.
    plain.style.titleBarColor = QStringLiteral("#FF224466");
    layout.fences = {plain};
    const QByteArray withBand = JsonStore::toJson(layout);
    QVERIFY(withBand.contains("titleBarColor"));
    const auto reParsed = JsonStore::fromJson(withBand);
    QVERIFY(reParsed.has_value());
    QCOMPARE(reParsed->fences.at(0).style.titleBarColor, QStringLiteral("#FF224466"));
}

void TestLayoutStore::jsonRejectsAnotherSchema()
{
    QByteArray payload = JsonStore::toJson(makeLayout());
    payload.replace(QByteArrayLiteral("\"schema\": 1"), QByteArrayLiteral("\"schema\": 7"));

    QString error;
    QVERIFY(!JsonStore::fromJson(payload, &error).has_value());
    QVERIFY(error.contains(QStringLiteral("unsupported schema")));
}

void TestLayoutStore::jsonRejectsGarbage()
{
    QString error;
    QVERIFY(!JsonStore::fromJson(QByteArrayLiteral("{ this is not json"), &error).has_value());
    QVERIFY(!error.isEmpty());
}

void TestLayoutStore::saveCreatesMissingParentDirectory()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("StarFish/nested/layout.json"));
    QVERIFY(!QFileInfo::exists(path));

    QString error;
    QVERIFY2(JsonStore::save(makeLayout(), path, &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(path));

    const auto reloaded = JsonStore::load(path, &error);
    QVERIFY2(reloaded.has_value(), qPrintable(error));
    QCOMPARE(reloaded->fences.size(), 2);
    QCOMPARE(reloaded->fences.at(1).rect, QRect(500, 20, 200, 160));
}

void TestLayoutStore::loadMissingFileIsFirstRun()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LayoutStore store;
    store.setFilePath(dir.filePath(QStringLiteral("absent.json")));

    QString error;
    QVERIFY2(store.load(&error), qPrintable(error));
    QVERIFY(error.isEmpty());
    QCOMPARE(store.layout().fences.size(), 0);
    QCOMPARE(store.isDirty(), false);
}

void TestLayoutStore::loadKeepsTheLayoutWhenTheFileIsCorrupt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("layout.json"));

    LayoutStore store;
    store.setFilePath(path);
    QVERIFY(store.setLayout(makeLayout()));
    QVERIFY(store.saveNow());
    QCOMPARE(store.isDirty(), false);

    // Someone (or something) truncates the file: a bad document must be
    // reported, never silently turned into "the user has no fences".
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("{ truncated");
    file.close();

    QString error;
    QVERIFY(!store.load(&error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(store.layout().fences.size(), 2);
}

void TestLayoutStore::setLayoutIgnoresAnIdenticalDocument()
{
    LayoutStore store;
    int changes = 0;
    connect(&store, &LayoutStore::layoutChanged, [&changes] { ++changes; });

    QVERIFY(store.setLayout(makeLayout()));
    QVERIFY(!store.setLayout(makeLayout()));
    QCOMPARE(changes, 1);

    // A real difference is noticed, in either direction.
    Layout changed = makeLayout();
    changed.fences[0].rect.moveLeft(11);
    QVERIFY(store.setLayout(changed));
    QCOMPARE(changes, 2);
}

void TestLayoutStore::scheduleSaveDebouncesABurst()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LayoutStore store;
    store.setFilePath(dir.filePath(QStringLiteral("layout.json")));
    store.setDebounceInterval(40);

    int writes = 0;
    connect(&store, &LayoutStore::saved, [&writes] { ++writes; });

    // Ten rect updates in a row: the first four re-arm the timer before it can
    // fire, so the burst must collapse into a single write.
    for (int i = 0; i < 10; ++i) {
        Layout layout = makeLayout();
        layout.fences[0].rect.moveLeft(100 + i);
        QVERIFY(store.setLayout(layout));
        store.scheduleSave();
        QTest::qWait(4);
    }
    QVERIFY(store.hasPendingSave());
    QVERIFY(QTest::qWaitFor([&store] { return !store.hasPendingSave(); }, 2000));
    QCOMPARE(writes, 1);

    // Nothing changed since: an explicit save is a no-op, not a rewrite.
    QVERIFY(store.saveNow());
    QCOMPARE(writes, 1);

    QString error;
    const auto reloaded = JsonStore::load(store.filePath(), &error);
    QVERIFY2(reloaded.has_value(), qPrintable(error));
    QCOMPARE(reloaded->fences.at(0).rect.left(), 109);
}

void TestLayoutStore::saveNowReportsFailureOnAnUnwritablePath()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    LayoutStore store;
    // A directory where the file should be: QSaveFile cannot commit onto it.
    const QString path = dir.filePath(QStringLiteral("layout.json"));
    QVERIFY(QDir().mkpath(path));
    store.setFilePath(path);

    int failures = 0;
    QString signalled;
    connect(&store, &LayoutStore::saveFailed, [&failures, &signalled](const QString&, const QString& error) {
        ++failures;
        signalled = error;
    });

    QVERIFY(store.setLayout(makeLayout()));
    QString error;
    QVERIFY(!store.saveNow(&error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(failures, 1);
    QCOMPARE(signalled, error);
    // Still dirty: the next change retries rather than losing the layout.
    QCOMPARE(store.isDirty(), true);
}

void TestLayoutStore::jsonRoundTripsRules()
{
    const Layout original = makeLayout();
    const QByteArray payload = JsonStore::toJson(original);

    // The shape is the documented one (docs/technique.md section 5): criteria
    // nested under `match`, the destination named `targetFence`.
    QVERIFY(payload.contains(QByteArrayLiteral("\"rules\"")));
    QVERIFY(payload.contains(QByteArrayLiteral("\"match\"")));
    QVERIFY(payload.contains(QByteArrayLiteral("\"targetFence\": \"fence-001\"")));

    QString error;
    const auto parsed = JsonStore::fromJson(payload, &error);
    QVERIFY2(parsed.has_value(), qPrintable(error));
    QCOMPARE(parsed->rules.size(), 1);

    const Rule& rule = parsed->rules.at(0);
    QCOMPARE(rule.id, QStringLiteral("rule-001"));
    QCOMPARE(rule.name, QStringLiteral("Images → Médias"));
    QCOMPARE(rule.extensions, QStringList({QStringLiteral("jpg"), QStringLiteral("png")}));
    QCOMPARE(rule.namePattern, QStringLiteral("shot*"));
    QCOMPARE(rule.newerThanDays.value_or(-1), 30);
    QCOMPARE(rule.olderThanDays.value_or(-1), 1);
    QCOMPARE(rule.targetFenceId, QStringLiteral("fence-001"));
    QCOMPARE(rule.moveFiles, true);

    // Still byte-identical, so an unchanged rule list does not rewrite the file.
    QCOMPARE(JsonStore::toJson(*parsed), payload);

    // An absent criterion stays absent instead of becoming a zero that would
    // silently mean "modified within the last 0 days".
    Rule bare;
    bare.targetFenceId = QStringLiteral("fence-002");
    Layout sparse;
    sparse.rules = {bare};
    const auto reread = JsonStore::fromJson(JsonStore::toJson(sparse), &error);
    QVERIFY2(reread.has_value(), qPrintable(error));
    QCOMPARE(reread->rules.at(0).extensions.size(), 0);
    QVERIFY(!reread->rules.at(0).newerThanDays.has_value());
    QVERIFY(!reread->rules.at(0).olderThanDays.has_value());
    QCOMPARE(reread->rules.at(0).moveFiles, false);
}

void TestLayoutStore::jsonAcceptsADocumentWithoutRules()
{
    // Exactly what T-008 wrote: no `rules` key at all. Adding the key must not
    // make an existing layout unreadable.
    const QByteArray legacy = QByteArrayLiteral(
        "{\n  \"schema\": 1,\n  \"fences\": [\n    {\n      \"id\": \"fence-001\",\n"
        "      \"title\": \"Travail\",\n      \"rect\": { \"x\": 0, \"y\": 0, \"w\": 100, \"h\": 80 }\n"
        "    }\n  ]\n}");

    QString error;
    const auto parsed = JsonStore::fromJson(legacy, &error);
    QVERIFY2(parsed.has_value(), qPrintable(error));
    QCOMPARE(parsed->fences.size(), 1);
    QCOMPARE(parsed->fences.at(0).title, QStringLiteral("Travail"));
    QCOMPARE(parsed->rules.size(), 0);
}

void TestLayoutStore::defaultOpacityRoundTripsAndMigrates()
{
    Layout layout = makeLayout();
    layout.fences[0].style.defaultOpacity = false;
    layout.fences[1].style.defaultOpacity = true;
    auto parsed = JsonStore::fromJson(JsonStore::toJson(layout));
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->fences.at(0).style.defaultOpacity, false);
    QCOMPARE(parsed->fences.at(1).style.defaultOpacity, true);

    // A document from before T-098: a fence made translucent keeps its own
    // value; an opaque one follows the default (0 % unless changed).
    QJsonObject root = QJsonDocument::fromJson(JsonStore::toJson(layout)).object();
    QJsonArray fences = root[QStringLiteral("fences")].toArray();
    const QStringList backgrounds = {QStringLiteral("#CC1F2937"), QStringLiteral("#FF1F2937")};
    for (int i = 0; i < fences.size(); ++i) {
        QJsonObject one = fences.at(i).toObject();
        QJsonObject style = one[QStringLiteral("style")].toObject();
        style.remove(QStringLiteral("defaultOpacity"));
        style[QStringLiteral("background")] = backgrounds.at(i);
        one[QStringLiteral("style")] = style;
        fences[i] = one;
    }
    root[QStringLiteral("fences")] = fences;
    parsed = JsonStore::fromJson(QJsonDocument(root).toJson());
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->fences.at(0).style.defaultOpacity, false);
    QCOMPARE(parsed->fences.at(1).style.defaultOpacity, true);
    QCOMPARE(FenceStyle().defaultOpacity, true);
}

QTEST_MAIN(TestLayoutStore)
#include "test_layout_store.moc"