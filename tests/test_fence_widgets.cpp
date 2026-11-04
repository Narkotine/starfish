#include <QtTest>

#include <QFontMetrics>
#include <QImage>
#include <QPainter>

#include "core/desktop_item.hpp"
#include "ui/fence_widgets.hpp"
#include "ui/note_editor.hpp"

using namespace starfish::core;
using namespace starfish::ui;

namespace {

QFont testFont()
{
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(14);
    return font;
}

QImage painted(const DesktopItem& note, int w = 200, int h = 200)
{
    QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widgets::PaintContext context;
    context.text = QColor(0xE5, 0xE7, 0xEB);
    context.shadow = false;
    context.font = testFont();
    context.now = QDateTime(QDate(2026, 11, 3), QTime(9, 30));
    widgets::paint(painter, QRect(0, 0, w, h), note, context);
    painter.end();
    return image;
}

} // namespace

class TestFenceWidgets : public QObject
{
    Q_OBJECT

private slots:
    // Session 81: the notes kind is a widget like the clock, and the only one
    // whose content is written by the user.
    void kindsSayWhatIsWritable()
    {
        const QList<widgets::Kind> list = widgets::kinds();
        QVERIFY(list.size() >= 2);
        bool clock = false;
        bool notes = false;
        for (const widgets::Kind& kind : list) {
            if (kind.type == QLatin1String("clock")) {
                clock = true;
                QVERIFY(!kind.editable);
            }
            if (kind.type == QLatin1String("notes")) {
                notes = true;
                QVERIFY(kind.editable);
                QCOMPARE(kind.defaultSpan, QSize(2, 2));
                QVERIFY(kind.defaults.contains(QStringLiteral("text")));
                QVERIFY(kind.defaults.value(QStringLiteral("text")).toString().isEmpty());
            }
        }
        QVERIFY(clock && notes);
        QVERIFY(widgets::isKnown(QStringLiteral("notes")));
        QVERIFY(widgets::isEditable(QStringLiteral("notes")));
        QVERIFY(!widgets::isEditable(QStringLiteral("clock")));
        QVERIFY(!widgets::isEditable(QStringLiteral("weather"))); // a widget we do not know
    }

    void aNewNoteIsAWidgetWithEmptyText()
    {
        const DesktopItem note = widgets::create(QStringLiteral("notes"));
        QVERIFY(note.isWidget());
        QCOMPARE(note.widgetType, QStringLiteral("notes"));
        QVERIFY(note.path.startsWith(QLatin1String("widget:")));
        QVERIFY(widgets::noteText(note).isEmpty());
        QCOMPARE(note.span, QSize(2, 2));

        const DesktopItem written = widgets::withNoteText(note, QStringLiteral("lundi : appeler Marc"));
        QCOMPARE(widgets::noteText(written), QStringLiteral("lundi : appeler Marc"));
        QVERIFY(widgets::noteText(note).isEmpty()); // the original is untouched
    }

    // The wrapping the paint uses, measured on its own: the text never sticks
    // out of the card, and a note too long says so instead of ending nowhere.
    void noteLinesWrapAndElide()
    {
        const QFont font = testFont();
        const QFontMetrics metrics(font);
        const int width = 150;

        const QStringList one = widgets::noteLines(QStringLiteral("courses"), font, width, 5);
        QCOMPARE(one.size(), 1);
        QCOMPARE(one.first(), QStringLiteral("courses"));

        bool elided = false;
        const QString longText =
            QStringLiteral("penser a sortir les poubelles avant la reunion de demain matin a neuf heures");
        const QStringList wrapped = widgets::noteLines(longText, font, width, 3, &elided);
        QVERIFY(elided);
        QCOMPARE(wrapped.size(), 3);
        for (const QString& line : wrapped)
            QVERIFY2(metrics.horizontalAdvance(line) <= width, qPrintable(line));

        // The user's own line breaks are kept.
        const QStringList typed = widgets::noteLines(QStringLiteral("ligne 1\nligne 2"), font, width, 5);
        QCOMPARE(typed, (QStringList{QStringLiteral("ligne 1"), QStringLiteral("ligne 2")}));

        // A word wider than the card is cut, not left to overflow.
        bool cutElided = false;
        const QStringList cut = widgets::noteLines(QString(60, QLatin1Char('m')), font, width, 5, &cutElided);
        QVERIFY(cut.size() > 1);
        for (const QString& line : cut)
            QVERIFY2(metrics.horizontalAdvance(line) <= width, qPrintable(line));
        QVERIFY(!cutElided);

        // Nothing to draw: no lines at all (the paint shows the hint instead).
        QVERIFY(widgets::noteLines(QString(), font, width, 5).isEmpty());
        QVERIFY(widgets::noteLines(QStringLiteral("x"), font, 0, 5).isEmpty());
    }

    // What the fence actually puts on screen: a card, and the note's text in it.
    void theNoteIsPaintedInItsCell()
    {
        const DesktopItem empty = widgets::create(QStringLiteral("notes"));
        const QImage hint = painted(empty);
        // The card is a wash over the panel: something is drawn even when empty.
        QVERIFY(hint.pixelColor(100, 100).alpha() > 0);

        const DesktopItem note = widgets::withNoteText(empty, QStringLiteral("sortir les poubelles"));
        const QImage written = painted(note);
        QVERIFY(written.pixelColor(100, 100).alpha() > 0);
        QVERIFY(hint != written); // the hint is not the note

        // A note never changes by itself: no timer, no repaint.
        QCOMPARE(widgets::msUntilNextChange(note, QDateTime::currentDateTime()), -1);
    }

    // Session 83: a note whose text was pasted in whole used to freeze the
    // product. The card only ever shows `maxLines` lines, so that is all that may
    // be wrapped -- and an over-wide word must be cut by bisection, not by
    // measuring every prefix (that scan is quadratic: 2 000 characters in one
    // run measured 22 s, 20 000 never finished; the fence repaints while the
    // user types, so StarFish stopped answering and the note was never saved).
    void aWallOfPastedTextCostsTheCardNotTheDocument()
    {
        const QFont font = testFont();
        const int width = 150;
        const int maxLines = 8;

        QString blob;
        blob.fill(QLatin1Char('a'), 200000); // one run, no space to wrap on
        QElapsedTimer timer;
        timer.start();
        bool elided = false;
        const QStringList lines = widgets::noteLines(blob, font, width, maxLines, &elided);
        const qint64 elapsed = timer.elapsed();
        qInfo() << "200 000 characters in one run ->" << elapsed << "ms";
        QCOMPARE(lines.size(), maxLines);
        QVERIFY(elided);
        const QFontMetrics metrics(font);
        for (const QString& line : lines)
            QVERIFY2(metrics.horizontalAdvance(line) <= width, qPrintable(line));
        QVERIFY2(elapsed < 1000, qPrintable(QStringLiteral("wrapping took %1 ms").arg(elapsed)));

        // Prose, many paragraphs: same cost, and the same answer.
        QString prose;
        for (int i = 0; i < 20000; ++i)
            prose += QStringLiteral("mot numero %1\n").arg(i);
        timer.restart();
        const QStringList proseLines = widgets::noteLines(prose, font, width, maxLines, &elided);
        QCOMPARE(proseLines.size(), maxLines);
        QVERIFY(elided);
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QStringLiteral("wrapping took %1 ms").arg(timer.elapsed())));

        // What is drawn is bounded; what is *kept* is not: the document still
        // holds the whole text, and the ellipsis is the only sign of it.
        const DesktopItem note = widgets::withNoteText(widgets::create(QStringLiteral("notes")), blob);
        QCOMPARE(widgets::noteText(note).size(), blob.size());

        // The fence's own paint of that note: the card, and `maxLines` lines.
        timer.restart();
        const QImage card = painted(note);
        QVERIFY2(timer.elapsed() < 1000,
                 qPrintable(QStringLiteral("painting took %1 ms").arg(timer.elapsed())));
        QVERIFY(card.pixelColor(100, 100).alpha() > 0);

        // A note whose first lines are blank keeps at least the sign that there
        // is more (a card with no line at all reads as an empty note).
        bool blankElided = false;
        const QStringList blank = widgets::noteLines(QStringLiteral("\n\n\nabc"), font, width, 2, &blankElided);
        QVERIFY(!blank.isEmpty());
        QVERIFY(blankElided);
    }

    // Session 83: a pasted wall of text is in the document before anything else
    // happens; typing still waits for a pause.
    void aPasteIsSavedAtOnceAndTypingWaits()
    {
        QString saved;
        int saves = 0;
        NoteEditor editor(QString(), [&](const QString& text) {
            saved = text;
            ++saves;
        }, [] {});
        QVERIFY(saved.isEmpty());

        // Typing: one character does not write yet.
        QTest::keyClicks(&editor, QStringLiteral("a"));
        QCOMPARE(saves, 0);
        // A paste (here: the same thing a paste does, a whole block at once)
        // goes to the document immediately.
        const QString pasted = QString(5000, QLatin1Char('p'));
        editor.setPlainText(pasted);
        QCOMPARE(saves, 1);
        QCOMPARE(saved, pasted);
        QVERIFY(editor.savedTextIsCurrent());

        // Clearing a note that big is just as large a change.
        editor.setPlainText(QString());
        QCOMPARE(saves, 2);
        QVERIFY(saved.isEmpty());
        QVERIFY(editor.savedTextIsCurrent());
    }

    // The editor saves while it is typed, and only when something changed.
    void theEditorSavesOnDemand()
    {
        QString saved;
        int saves = 0;
        NoteEditor editor(QStringLiteral("premier jet"), [&](const QString& text) {
            saved = text;
            ++saves;
        }, [] {});
        QCOMPARE(editor.saveDelay(), 600);
        QCOMPARE(editor.toPlainText(), QStringLiteral("premier jet"));
        QVERIFY(editor.savedTextIsCurrent());

        editor.setPlainText(QStringLiteral("deuxieme jet"));
        QVERIFY(!editor.savedTextIsCurrent());
        QVERIFY(editor.flush());
        QCOMPARE(saved, QStringLiteral("deuxieme jet"));
        QCOMPARE(saves, 1);
        QVERIFY(!editor.flush()); // nothing new: no second write
        QCOMPARE(saves, 1);
    }

    void escapeLeavesAndCtrlEnterWrites()
    {
        bool finished = false;
        NoteEditor leaving(QStringLiteral("x"), [](const QString&) {}, [&] { finished = true; });
        QTest::keyClick(&leaving, Qt::Key_Escape);
        QVERIFY(finished);

        QString saved;
        bool closed = false;
        NoteEditor writing(QStringLiteral("a"), [&](const QString& text) { saved = text; }, [&] { closed = true; });
        writing.setPlainText(QStringLiteral("b"));
        QTest::keyClick(&writing, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(saved, QStringLiteral("b"));
        QVERIFY(closed);
    }
};

QTEST_MAIN(TestFenceWidgets)
#include "test_fence_widgets.moc"
