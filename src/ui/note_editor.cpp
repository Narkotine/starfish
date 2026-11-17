#include "ui/note_editor.hpp"

#include "ui/fence_widgets.hpp"

#include <QFocusEvent>
#include <QKeyEvent>
#include <QTextOption>

namespace starfish::ui {

NoteEditor::NoteEditor(const QString& text, std::function<void(const QString&)> save,
                       std::function<void()> finish, QWidget* parent)
    : QPlainTextEdit(parent), m_save(std::move(save)), m_finish(std::move(finish)), m_saved(text)
{
    setPlainText(text);
    setFrameShape(QFrame::NoFrame);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Session 120: Tab inserts a tab (a column separator), it does not leave
    // the note. Escape, a click elsewhere, or Ctrl+Enter still close it.
    setTabChangesFocus(false);
    setFont(widgets::noteFont());
    document()->setDocumentMargin(0);
    QTextOption opt = document()->defaultTextOption();
    opt.setTabStopDistance(widgets::noteTabStopDistance());
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    document()->setDefaultTextOption(opt);
    const QMargins pad = widgets::noteContentMargins();
    setStyleSheet(QStringLiteral("QPlainTextEdit { padding: %1px %2px %3px %4px; }")
                      .arg(pad.top())
                      .arg(pad.right())
                      .arg(pad.bottom())
                      .arg(pad.left()));
    m_timer.setSingleShot(true);
    m_timer.setInterval(kSaveDelayMs);
    connect(&m_timer, &QTimer::timeout, this, [this] { flush(); });
    // Session 83: typing waits for a pause (one write per pause, not per
    // keystroke); a *paste* is written now. The user pasted a wall of text into a
    // note, the app went down before the debounce got its turn, and the text was
    // gone at the next launch -- the document must have it the moment it lands.
    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        if (qAbs(int(document()->characterCount()) - int(m_saved.size())) >= kPasteChars)
            flush();
        else
            m_timer.start();
    });
}

bool NoteEditor::flush()
{
    m_timer.stop();
    const QString text = toPlainText();
    if (text == m_saved)
        return false;
    m_saved = text;
    if (m_save)
        m_save(text);
    emit textSaved(text);
    return true;
}

void NoteEditor::finishOnce()
{
    if (m_finished)
        return;
    m_finished = true;
    m_timer.stop();
    if (m_finish)
        m_finish();
}

void NoteEditor::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        // Nothing is thrown away that was already saved: the note keeps what it
        // had when the editor was left, as every in-place edit here does.
        event->accept();
        finishOnce();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && event->modifiers().testFlag(Qt::ControlModifier)) {
        // Ctrl+Enter: write it down now and close.
        event->accept();
        flush();
        finishOnce();
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

void NoteEditor::focusOutEvent(QFocusEvent* event)
{
    QPlainTextEdit::focusOutEvent(event);
    // Clicking anywhere else saves and leaves, like Explorer's rename box.
    flush();
    finishOnce();
}

} // namespace starfish::ui
