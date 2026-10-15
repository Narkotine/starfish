#pragma once

#include <functional>
#include <optional>
#include <QImage>
#include <QList>
#include <QObject>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>

#include "core/desktop_item.hpp"
#include "core/jsonstore.hpp"
#include "core/portal.hpp"
#include "core/rules.hpp"
#include "ui/fence_geometry.hpp"
#include "ui/icon_grid.hpp"
#include "ui/screen_layout.hpp"

namespace starfish::platform::win {
class DesktopSurface;
}

namespace starfish::ui {

class FenceView;

// One live fence (T-007): the model geometry, the widget that renders it and,
// when hosted, the raw Win32 surface that shows it on the desktop.
struct FenceWindow {
    QString id;
    QString title;
    // Device name of the screen the fence sits on ("\\.\DISPLAY1"), persisted
    // since T-008 and **acted upon** since T-014: it is what brings a fence back
    // to the right monitor when a stored rect no longer lands on any screen
    // (ScreenLayout::restoreRect). It always describes where the fence *is* --
    // it is derived from the rect, never the other way round.
    QString monitor;
    // The *stored* rect: the expanded body, even while rolled up. Roll-up only
    // changes what is visible -- FenceGeometry::visibleRect derives that.
    // Physical pixels in Windows' virtual-screen coordinates, which is exactly
    // Progman's client space; a monitor left of or above the primary one has
    // negative coordinates (T-014).
    QRect rect;
    core::FenceStyle style; // panel colour + radius are applied to the view
    bool rolledUp = false;  // F-020: persisted (T-008) and acted upon (T-009)
    // T-011: the folder portals this fence mirrors, in declaration order. Each
    // one carries its last listing, which is how the tiles a portal
    // contributed are told apart from the fence's own items -- the document
    // persists the folder path, never the listing.
    QList<core::Portal> portals;
    FenceView* view = nullptr;
    starfish::platform::win::DesktopSurface* surface = nullptr;

    // Last frame produced by renderFence(). Kept even without a surface, so the
    // lifecycle can be exercised headless (tests, CI, --fence-demo).
    QImage frame;

    // Geometry drag bookkeeping: `dragStartRect` is captured when a gesture
    // begins, so every delta is applied to the same reference and rounding
    // cannot accumulate.
    QRect dragStartRect;
    bool dragging = false;
};

// Owns the set of fences and their lifecycle: create, move, resize, delete and
// snapping (T-007). It is the piece that makes an inter-fence gesture possible
// -- a FenceView only reports "the user is dragging my title bar / my edge",
// the container decides where that lands once the other fences are known.
//
// Two modes:
//   * headless (default) -- only FenceView + geometry, no desktop window. Used
//     by the unit tests and by --fence-demo when --host is not passed.
//   * hosted -- every fence additionally gets a DesktopSurface attached under
//     the icon layer, and the rendered frame is pushed to it. This is the real
//     product path.
class FenceContainer : public QObject
{
    Q_OBJECT

public:
    explicit FenceContainer(QObject* parent = nullptr);
    ~FenceContainer() override;

    FenceContainer(const FenceContainer&) = delete;
    FenceContainer& operator=(const FenceContainer&) = delete;

    // Icon source bound to every FenceView created afterwards.
    void setIconProvider(IconGrid::IconProvider provider);

    // Resolves a path coming back from the persisted layout into a DesktopItem
    // (T-008). The default resolver uses core::itemFromPathOnly (no Shell, no
    // Win32); the application binds DesktopScanner::itemForPath so a restored
    // tile is exactly what the Shell would draw.
    using ItemResolver = std::function<core::DesktopItem(const QString& path)>;
    void setItemResolver(ItemResolver resolver);

    // The screens the fences live on: which monitor each fence belongs to,
    // which edges it snaps to and how large its frame must be rendered (T-014 /
    // F-032). Setting a layout also **revalidates** every fence, so a monitor
    // that has just been unplugged brings its fences back on screen instead of
    // leaving them unreachable.
    void setScreens(const ScreenLayout& layout);
    const ScreenLayout& screens() const { return m_screens; }

    // The single-screen case: one screen anchored at (0,0), no scaling. Kept
    // because most callers (the probes, the older tests) only know a size.
    void setDesktopBounds(const QSize& bounds);
    QSize desktopBounds() const { return m_screens.bounds().size(); }

    // Puts every fence back on a screen: the rect a stored document describes
    // may no longer land on any monitor (unplugged, re-arranged, resolution
    // changed). Returns how many fences actually moved, and emits
    // fenceRectChanged() for those only -- a fence that did not move must not
    // make the product rewrite the document.
    int revalidateScreens();

    void setHostOnDesktop(bool enabled) { m_host = enabled; }
    bool hostsOnDesktop() const { return m_host; }

    void setSnapThreshold(int px) { m_snapThreshold = qMax(0, px); }
    int snapThreshold() const { return m_snapThreshold; }

    void setMinimumFenceSize(const QSize& size) { m_minSize = size; }
    QSize minimumFenceSize() const { return m_minSize; }

    // --- lifecycle ----------------------------------------------------------

    // Creates a fence with a fresh id and returns it. The rect is clamped to
    // the desktop bounds. When hosting is on, the surface is attached and the
    // first frame is pushed.
    QString createFence(const QString& title, const QRect& rect,
                        const QList<core::DesktopItem>& items = {});

    // Deletes a fence. The files themselves are never touched (AGENTS.md golden
    // rule): only the zone disappears. Returns false when the id is unknown.
    bool removeFence(const QString& id);

    bool contains(const QString& id) const { return find(id) != nullptr; }
    int fenceCount() const { return m_fences.size(); }
    QStringList fenceIds() const;

    QString fenceTitle(const QString& id) const;
    QRect fenceRect(const QString& id) const;
    QList<core::DesktopItem> fenceItems(const QString& id) const;
    core::FenceStyle fenceStyle(const QString& id) const;
    bool fenceRolledUp(const QString& id) const;

    void setFenceTitle(const QString& id, const QString& title);
    void setFenceStyle(const QString& id, const core::FenceStyle& style);
    void setFenceSort(const QString& id, const QString& by, bool ascending);

    // --- navigation (T-009) -------------------------------------------------
    //
    // The three "get the desktop out of the way" behaviours of docs/produit.md
    // section D. The container owns every piece of state; the widget and the
    // surface only obey. Roll-up changes what a fence shows, quick hide takes
    // every fence off screen, Peek raises them above the icon layer.

    // Roll-up (F-020). The stored rect keeps the expanded size, so rolling back
    // down restores the fence exactly. Idempotent.
    bool setFenceRolledUp(const QString& id, bool rolledUp);
    bool toggleFenceRolledUp(const QString& id);

    // The rect a fence actually shows (its title bar when rolled up).
    QRect fenceVisibleRect(const QString& id) const;

    // Quick hide (F-022): hide every fence at once, reversibly. The zones are
    // only taken off screen -- nothing is destroyed and no file is touched.
    bool setFencesVisible(bool visible);
    bool fencesVisible() const { return m_fencesVisible; }
    bool toggleFencesVisible() { return setFencesVisible(!m_fencesVisible); }

    // Peek (F-021): raise every fence above the icon layer while active.
    bool setPeekActive(bool active);
    bool peekActive() const { return m_peekActive; }
    bool togglePeek() { return setPeekActive(!m_peekActive); }

    // Applies a new rect. The rect is clamped to the desktop bounds and, unless
    // `snap` is false, snapped to the other fences and the screen edges. Returns
    // the rect actually applied (an unknown id returns a null rect).
    QRect setFenceRect(const QString& id, const QRect& rect, bool snap = true);

    // --- rules (T-010, F-017 / F-018) ---------------------------------------
    //
    // Auto-organisation. The rules are part of the document (docs/technique.md
    // section 5) and are evaluated by core::RuleEngine, which knows nothing
    // about fences, widgets or Win32. Applying a rule decides which fence
    // *shows* an item -- the filesystem is never touched, whatever `moveFiles`
    // says. See the golden rule in AGENTS.md.
    void setRules(QList<core::Rule> rules);
    QList<core::Rule> rules() const { return m_rules; }

    // Resolves a path through the same resolver the layout restore uses, so a
    // file dropped by the Explorer and a file restored from the document are
    // described identically.
    core::DesktopItem itemForPath(const QString& path) const;

    // The fence an item belongs to according to the rules, or nullopt.
    std::optional<QString> classify(const core::DesktopItem& item) const;

    // Files a batch of items: every item with a matching rule whose target
    // fence still exists is appended to that fence. Returns how many were
    // actually placed (an item already in the target fence is not placed
    // twice, so a repeated notification does not rewrite the document).
    int applyRulesTo(const QList<core::DesktopItem>& items);

    // --- folder portals (T-011, F-024) --------------------------------------
    //
    // A portal mirrors a folder into a fence: the tiles it contributes are the
    // folder's *contents*, and the folder itself is never drawn. What the
    // document holds is the folder path (`{"kind":"portal","path":...}`), never
    // the listing -- so the mirror follows the folder, and a folder changing
    // never rewrites the document.
    //
    // Read mirror only: adding a file to the folder *shows* it, nothing is ever
    // moved into the folder (that is the write half, T-035, and it would be an
    // irreversible disk mutation -- see the golden rule in AGENTS.md).

    // Mirrors `folderPath` into a fence. Returns false when the fence is
    // unknown, the path is empty, the folder is already mirrored by this fence,
    // or it could not be listed -- a portal to a folder that is not there is
    // still accepted (and reported), because an offline network share must not
    // silently disappear from the document.
    bool addPortal(const QString& fenceId, const QString& folderPath);

    // Stops mirroring a folder. The tiles it contributed go; the files and the
    // folder itself are untouched. Returns false when the fence or the portal
    // is unknown.
    bool removePortal(const QString& fenceId, const QString& folderPath);

    QList<core::Portal> portals(const QString& fenceId) const;

    // Re-lists the portals pointing at `folder` -- every portal when it is
    // empty -- and updates the fences that hold them. Returns how many portals
    // were refreshed. The document is not touched: only the folder path is
    // persisted, so a refresh can never trigger a write.
    int refreshPortalsFor(const QString& folder = QString());

    // Every folder a portal points at: what the product must watch (F-018) for
    // the mirrors to follow their folders.
    QStringList portalFolders() const;

    // Access for tests / diagnostics. May be null in headless mode (surface).
    FenceView* view(const QString& id) const;
    starfish::platform::win::DesktopSurface* surface(const QString& id) const;
    QImage frame(const QString& id) const;

    // Renders the fence's widget offscreen and pushes it to its surface.
    void renderFence(const QString& id);

    // All rects except `excludeId`, for snapping.
    QList<QRect> otherRects(const QString& excludeId) const;

    // --- persistence (T-008) -------------------------------------------------
    // The live fences as a versioned document, ready for core::JsonStore. The
    // order of `fences` is the creation order.
    core::Layout toLayout() const;

    // Replaces every fence with the ones in `layout`: this is what runs at
    // startup. Fence ids, titles, rects, styles, sort and item paths are
    // restored, and the item paths go through the item resolver. Returns false
    // with an error message when the schema is unsupported -- in which case
    // nothing is touched. Files are never affected, only zones.
    bool applyLayout(const core::Layout& layout, QString* error = nullptr);

signals:
    void fenceCreated(const QString& id);
    void fenceRemoved(const QString& id);
    void fenceRectChanged(const QString& id, const QRect& rect);
    void fenceContentChanged(const QString& id);
    // T-009. Roll-up is persisted (it is part of the document), so the product
    // wires this to the same auto-save as the other lifecycle signals.
    void fenceRolledUpChanged(const QString& id, bool rolledUp);
    void fencesVisibilityChanged(bool visible);
    void peekChanged(bool active);
    // T-010. The rule list is persisted, so the product wires this to the same
    // auto-save as the other document-level signals. It is deliberately NOT
    // emitted by applyLayout: restoring the document is not changing it.
    void rulesChanged();
    // An item was filed by a rule. Diagnostics and, later, an undo trail.
    void ruleApplied(const QString& path, const QString& fenceId);

private:
    FenceWindow* find(const QString& id);
    const FenceWindow* find(const QString& id) const;

    // Shared body of createFence and applyLayout. An empty `requestedId` means
    // "allocate a fresh one"; a restored id is kept verbatim so that the
    // document is stable across restarts. `portalPaths` are the folders the
    // restored fence mirrors (T-011); they are listed before the fence is
    // announced, so fenceCreated() never describes half a fence.
    // `monitorHint` is the screen the document recorded for this fence (T-014),
    // used only when the stored rect no longer lands on any screen.
    QString addFence(const QString& requestedId, const QString& title, const QRect& rect,
                     const core::FenceStyle& style, const QString& sortBy, bool sortAscending,
                     bool rolledUp, const QList<core::DesktopItem>& items,
                     const QStringList& portalPaths = {}, const QString& monitorHint = {});

    core::DesktopItem resolveItem(const QString& path) const;

    // Shared body of addPortal and the restore path. `announce` is false while
    // a fence is being built, where the tiles are appended before any signal is
    // connected and fenceCreated() is the single announcement.
    bool addPortalTo(FenceWindow& fence, const QString& folderPath, bool announce);

    // Re-lists one portal and swaps the tiles it contributed in the fence's
    // view. Returns the number of tiles the folder holds now.
    int applyPortalListing(FenceWindow& fence, core::Portal& portal);

    // Resizes the view and the surface to the fence's *visible* rect and pushes
    // a fresh frame. The stored rect is untouched -- that is the whole point of
    // keeping roll-up out of the document's geometry.
    void applyVisibleGeometry(FenceWindow& fence);

    // FenceView geometry gestures -> move/resize with snapping.
    void beginGeometryDrag(const QString& id);
    void applyGeometryDrag(const QString& id, FenceHandle handle, const QPoint& delta);
    void endGeometryDrag(const QString& id);

    QString nextId();

    QList<FenceWindow> m_fences;
    IconGrid::IconProvider m_iconProvider;
    ItemResolver m_itemResolver;
    // T-010: the automation rules, in priority order (first match wins).
    QList<core::Rule> m_rules;
    // T-014: every screen the fences may live on. Defaults to the v1 single
    // screen, so a caller that only knows a size behaves exactly as before.
    ScreenLayout m_screens = ScreenLayout::single(QSize(1920, 1080));
    QSize m_minSize{160, 120};
    int m_snapThreshold = 8;
    bool m_host = false;
    int m_nextId = 1;
    // T-009 navigation state. Both are applied to every fence created
    // afterwards, so a fence made while hidden or peeking behaves like the rest.
    bool m_fencesVisible = true;
    bool m_peekActive = false;
};

} // namespace starfish::ui
