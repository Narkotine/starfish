#pragma once

#include <QString>

namespace starfish::core {

// The choices that belong to the **user**, not to a document (T-088).
//
// Deliberately not in `layout.json`: a layout describes fences, it is exported
// and imported (F-028), and a preference that arrived inside somebody else's file
// would have flipped a switch about **this** desktop. Deliberately not in the
// Windows registry either: the registry is where the Shell's verbs live (T-073)
// and nothing else, and a choice the user can read back is a plain file sitting
// next to the document it applies to.
//
// Same discipline as every other write in this codebase: atomic (QSaveFile), and
// a file that exists but cannot be read or parsed is reported and then falls back
// to the **defaults** -- never to a guess. That direction matters here: the one
// preference below is the one that takes the desktop's icons away, so a corrupt
// file has to end up with the safe answer.
class Preferences
{
public:
    // %APPDATA%/StarFish/preferences.json -- the same directory as the layout
    // document (LayoutStore::defaultFilePath), because it is the same product
    // state, with the same home-directory fallback.
    static QString defaultFilePath();

    explicit Preferences(const QString& path = defaultFilePath());

    QString filePath() const { return m_path; }

    // Reads the file. A missing file is not an error -- it means "nothing has
    // been chosen yet", and the defaults already in place are the right answer.
    // Returns false with a message when the file is there but unreadable or
    // unparseable; the values then stay at their defaults (see the class comment).
    bool load(QString* error = nullptr);

    // Writes the file atomically, creating the parent directory if needed.
    bool save(QString* error = nullptr) const;

    // T-088: whether StarFish draws the desktop's own icons.
    //
    // True means: while it runs, the native icon list is hidden and StarFish
    // draws the icons instead -- those a fence holds **inside that fence only**,
    // and the others at the very slots the Shell had given them (read before the
    // list goes away). False is the product, and it is the default: the icons
    // StarFish draws do not answer the mouse yet (T-042), so a desktop taken over
    // by accident is a desktop that answers nothing, which is the failure the user
    // named in session 36 (« là en l'état c'est inutilisable », T-066).
    bool desktopIconsOwned() const { return m_desktopIconsOwned; }
    void setDesktopIconsOwned(bool owned) { m_desktopIconsOwned = owned; }

private:
    QString m_path;
    bool m_desktopIconsOwned = false;
};

} // namespace starfish::core
