#pragma once

#include <QPlainTextEdit>
#include <QTimer>

#include <functional>

namespace starfish::ui {

// Session 81: the Notes widget's editor.
//
// A fence is an image on the desktop, so a widget's text is edited in the
// product's own window laid over the widget's cell -- the same shape as the
// title edit (session 60) and the caption rename (session 74). What differs is
// that the text *is* the content: it is saved while it is typed (a debounce, no
// OK button), Return writes a new line, Escape leaves without the keystrokes
// typed since the last save.
class NoteEditor : public QPlainTextEdit
{
    Q_OBJECT

public:
    // `save` receives the text once typing has stopped for kSaveDelayMs, and
    // again when the editor is left; `finish` closes it (the product owns the
    // window, so it decides what closing means).
    explicit NoteEditor(const QString& text, std::function<void(const QString&)> save,
                        std::function<void()> finish, QWidget* parent = nullptr);

    static constexpr int kSaveDelayMs = 600;
    // Session 83: a change this large is a paste, not typing, and is written
    // straight away (see the constructor). A note pasted and then followed by a
    // crash used to lose everything typed after the last pause.
    static constexpr int kPasteChars = 200;
    int saveDelay() const { return kSaveDelayMs; }

    // Saves the text if it changed since the last save. Returns whether it did.
    bool flush();
    bool savedTextIsCurrent() const { return toPlainText() == m_saved; }

signals:
    void textSaved(const QString& text);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    void finishOnce();

    std::function<void(const QString&)> m_save;
    std::function<void()> m_finish;
    QTimer m_timer;
    QString m_saved;
    bool m_finished = false;
};

} // namespace starfish::ui
