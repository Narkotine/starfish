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
#include <QTimer>

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
class DesktopBackdrop;

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
    // ⚠️ T-082: the fence's **icon overlay** -- a second hosted window, stacked
    // directly above `surface`, opaque (alpha 255) and whose region is the union
    // of the tile rectangles (DesktopSurface::setRegionRects). It is what lets a
    // translucent *panel* carry crisp icons: a window carries one alpha, so the
    // tiles cannot stay opaque in the very window that is fading.
    //
    // It is **click-through** and owns nothing: the panel below keeps the mouse,
    // the input target and the OLE drop target, and the overlay is exactly on top
    // of it, so a click on a tile is the same click it was (T-042's rule -- a
    // replayed click is a different click).
    //
    // Null whenever there is nothing to keep crisp: an opaque material, a fence
    // whose `fadeIcons` is on, an empty fence, a rolled-up one, or a headless
    // container.
    starfish::platform::win::DesktopSurface* iconOverlay = nullptr;
    // T-082: the size the overlay's shape was last built for, and whether that
    // build found any tile at all. The shape is a function of the size alone
    // (ADR-15), so a *move* must not re-measure every caption of the grid -- and
    // measuring one costs a font metric. renderFence() invalidates the record,
    // which is what makes a content change re-shape the window.
    QSize overlayShapeSize;
    bool overlayHasTiles = false;

    // Last frame produced by renderFence(). Kept even without a surface, so the
    // lifecycle can be exercised headless (tests, CI, --fence-demo).
    QImage frame;
    // T-098 (session 63): on the drawn desktop, the fence's own pixels -- the
    // panel at its transparency, the title and the icons opaque -- over a
    // transparent background. `frame` is then this over the desktop under the
    // fence (and the fences below it): an opaque image, so the window needs
    // no alpha and the icons no second window. Rebuilt when the content
    // changes; a move or a change underneath only re-composes.
    QImage content;
    QRect composedAt; // the visible rect `frame` was last composed for

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
    // The desktop's own icons may be drawn differently from the fences' tiles
    // (the shortcut arrow is a setting for tiles, never for the desktop).
    // Unset, the backdrop uses the fences' provider.
    void setBackdropIconProvider(IconGrid::IconProvider provider);
    // Every fence draws its tiles again (an icon setting changed).
    void refreshFenceIcons();
    // Session 60: the desktop's icons again (the desktop's arrow setting).
    void refreshBackdropIcons();
    // What the desktop must keep clear of (session 60, overlap off): the
    // visible rects of the fences on screen, physical px -- none while hidden.
    QList<QRect> fenceObstacleRects() const;
    // BackdropLayout::pathKey of every item a fence holds.
    QSet<QString> filedPathKeys() const;

    // Session 60: whether an addition to a fence (a drop, a filing) sorts the
    // fence by name. Off by default -- the user's order is kept, and a fence
    // created while it is off starts in "manual" order.
    void setSortOnAdd(bool sort);
    bool sortOnAdd() const { return m_sortOnAdd; }

    // Session 60: where a fence's title text is on screen, in *logical* pixels
    // (a Qt window's frame) -- where the inline rename's edit is laid.
    QRect fenceTitleScreenRect(const QString& id) const;

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

    // --- materials (T-057 / F-038) ------------------------------------------
    //
    // The material a fence's panel is shown in: one of FenceMaterial's presets.
    // It is deliberately **not** a field of its own in the document -- a material
    // *is* the alpha channel of FenceStyle::background, which the schema has
    // carried (and ignored) since T-008. So applying one goes through
    // setFenceStyle like every other appearance change: no schema bump, no second
    // source of truth, and a document written before T-057 is already the
    // "translucent" preset.
    //
    // What is new is the *effect*: a panel that is not fully opaque needs the
    // per-pixel push (the opaque GDI blit ignores alpha), so the hosted surface
    // is switched to it -- and the frame keeps an invisible input alpha floor in
    // the corners (ADR-11), so a translucent fence is still reachable by the
    // mouse over its whole rectangle.
    //
    // An unknown material name is refused and logged; a name that changes nothing
    // is a no-op, so a repeated choice does not rewrite the document.
    void setFenceMaterial(const QString& id, const QString& material);

    // The preset a fence's colour is shown in, or an empty string when its alpha
    // is a value of its own (see FenceMaterial::materialOf).
    QString fenceMaterial(const QString& id) const;

    // T-098 (session 63): transparency. A fence either follows the general
    // default (`defaultOpacity`, the default) or has its own alpha (that of its
    // `background`). Alphas are 0-255; the interface speaks percent.
    void setDefaultFenceAlpha(int alpha);
    int defaultFenceAlpha() const { return m_defaultAlpha; }
    // `alpha` < 0: follow the default again.
    void setFenceOpacity(const QString& id, int alpha);
    bool fenceFollowsDefaultOpacity(const QString& id) const;
    int fenceEffectiveAlpha(const QString& id) const;
    // Every fence back to the default.
    void resetFencesToDefaultOpacity();
    // True when fences are composed over the drawn desktop (one opaque window,
    // no icon overlay); false on the native desktop (ADR-15/17's windows).
    bool composedFences() const { return m_composed; }
    // Re-composes the fences over what changed underneath (virtual-screen
    // pixels). Public for the probes.
    void recompose(const QRect& dirty);
    int composeCount() const { return m_composeCount; }

    // Session 64: stacking. The fence list is the order, bottom to top (what
    // fenceAt, the composition and the document follow); the windows are kept
    // in that order. A fence the user presses comes to the top.
    void raiseFence(const QString& id);
    // Session 65: one selection for the whole desktop, as in Explorer -- every
    // fence but `keepId` (empty: all of them) drops its selection.
    void clearFenceSelections(const QString& keepId = QString());
    // Session 65: tiles of `sourceId` dragged to `screenPoint` (physical); on
    // `dropped`, moved into the fence under it, or put back on the desktop.
    // Returns how many tiles another fence took.
    int dragTilesOutside(const QString& sourceId, const QStringList& paths, const QPoint& screenPoint,
                         bool dropped);
    QPoint physicalFromFenceLocal(const QString& id, const QPoint& local) const;
    // Session 64: "Autoriser la superposition des zones". Off, a fence moved or
    // resized by the user stops against the others (FenceGeometry::avoidOverlap).
    void setAllowFenceOverlap(bool allowed) { m_allowOverlap = allowed; }
    bool allowFenceOverlap() const { return m_allowOverlap; }
    // The ids as the *windows* are stacked, bottom to top (Win32's own order),
    // and whether that is the list's order. For the probes.
    QStringList windowStackOrder() const;
    bool stackingMatchesWindows() const;
    qint64 composeMicros() const { return m_composeUs; }

    // ⚠️ T-082 -- where the material applies. F-038 says the *panel* is
    // translucent, and until now a material faded everything the fence drew,
    // because it was the window's uniform alpha (ADR-15, one alpha per window).
    // The user, after trying it: « je ne pensais pas que changer material
    // s'appliquerait aussi aux icônes. Est-il possible de ne l'appliquer qu'à la
    // fence ? et du coup de garder l'option de l'appliquer aux icônes en option,
    // par fence ? »
    //
    // So the default is now the panel (and its title bar): the fence's icons stay
    // opaque, which is what the second hosted window below is for. `fadeIcons`
    // restores the old behaviour *for that fence* -- an option, never the effect.
    // A default never rewrites what is already there: a document written before
    // T-082 reads `false` for the key it does not carry, so an *existing* fence
    // changes appearance once, and every fence written afterwards keeps what the
    // document says.
    bool setFenceIconsFollowMaterial(const QString& id, bool fadeIcons);
    bool fenceIconsFollowMaterial(const QString& id) const;

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

    // T-042: the fence whose *visible* rect contains `point` (virtual-screen
    // pixels -- the space the backdrop's cells and this container's rects share),
    // or an empty id. The visible rect, not the stored one: a rolled-up fence
    // offers its title bar, which is what the user sees and aims at. Fences are
    // searched in reverse creation order, so the one drawn last -- the one on top
    // -- is the one that answers where two overlap.
    QString fenceAt(const QPoint& point) const;

    // T-042: files items into a named fence, the way a rule does, without a rule --
    // the other half of dragging one of the icons the backdrop draws onto a fence
    // (F-040). Returns how many were really added: an item the fence already holds
    // is not appended twice (that would rewrite the document for nothing), and a
    // fence that is gone files nothing. **No file is moved** (the golden rule):
    // what changes is which fence *shows* the entry.
    int fileInto(const QString& fenceId, const QList<core::DesktopItem>& items);
    // Session 60: the same, inserted where `screenPoint` (virtual-screen, physical
    // pixels, like fenceAt) falls in the grid -- before the tile under it, or at
    // the end on empty space.
    int fileIntoAt(const QString& fenceId, const QList<core::DesktopItem>& items, const QPoint& screenPoint);

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
    // T-082: the second hosted window of a fence whose icons must stay opaque.
    // Null in the cases listed on FenceWindow::iconOverlay -- so a probe can ask
    // the container rather than guess from the material.
    starfish::platform::win::DesktopSurface* iconOverlay(const QString& id) const;
    QImage frame(const QString& id) const;

    // How many times a fence's frame has been *rendered* (the work a resize, a
    // content change or a size change implies). It exists because T-077's claim
    // -- "one image per changed size, and none at all for a move" -- used to be
    // measured on the layered push count, and fences no longer push anything
    // (ADR-15: they blit). The claim is about the render, so it is the render
    // that is counted; a probe that counted pushes would now pass by accident.
    int frameRenderCount() const { return m_frameRenders; }

    // Renders the fence's widget offscreen and pushes it to its surface.
    void renderFence(const QString& id);

    // T-090: a fence's window died with the desktop (Explorer restarted). Every
    // lost fence is hosted again once the desktop is back, on a 1 s timer for a
    // minute at most. Public so a probe can count what came back.
    int rehostedFenceCount() const { return m_rehosted; }

    // All rects except `excludeId`, for snapping.
    QList<QRect> otherRects(const QString& excludeId) const;

    // --- the desktop backdrop (T-039, ADR-9 option A) -----------------------
    //
    // When StarFish owns the icon layer -- the native one is hidden, which is
    // what makes a hosted fence clickable at all -- the icons no fence holds
    // still have to be drawn somewhere. That is the backdrop: a click-through
    // full-screen surface under the fences, whose content is *computed* from the
    // fences on every change and never stored (ADR-7 stays intact, and
    // F-007/F-011 follow from it for free).
    //
    // It is attached to the container because the container is the only object
    // that knows which icons a fence holds; the surface and the painting live in
    // DesktopBackdrop.
    DesktopBackdrop* backdrop() const { return m_backdrop; }

    // Turns the backdrop on. Returns false when it could not be hosted -- an
    // opaque full-screen window would hide the wallpaper, so the caller must
    // read this before hiding the native icons: no backdrop means the desktop
    // keeps its own.
    bool setBackdropEnabled(bool enabled);
    bool backdropEnabled() const;

    // The whole desktop, as the Shell presents it. The backdrop subtracts the
    // fences' own items from it.
    void setDesktopItems(QList<core::DesktopItem> items);

    // Recomputes which icons the fences hold and refreshes the backdrop. Called
    // by the container itself on every lifecycle signal; public so the product
    // can force a pass after a desktop rescan.
    void refreshBackdrop();

    // --- F-011 / T-040: giving an icon back to the desktop ------------------
    //
    // Taking a tile out of a fence is nothing but no longer holding it. The
    // container drops it from the fence's items; the desktop backdrop (T-039)
    // then draws it again because it has left the occupied set. There is no
    // restore step, nothing is written to the document beyond the shorter item
    // list, and the file itself is never touched (AGENTS.md golden rule) --
    // F-007 (deleting a fence) is the same movement, whole-scale.
    //
    // A tile a *portal* contributes is refused: that icon is the folder's, not
    // the fence's, and the next refresh would put it straight back (T-011).
    // Returns how many tiles were actually released.
    int releaseItems(const QString& fenceId, const QStringList& paths);

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
    // Session 64: the stacking order changed (the document keeps it).
    void fenceStackChanged(const QString& id);
    void fenceCreated(const QString& id);
    void fenceRemoved(const QString& id);
    void fenceRectChanged(const QString& id, const QRect& rect);
    void fenceContentChanged(const QString& id);
    // T-009. Roll-up is persisted (it is part of the document), so the product
    // wires this to the same auto-save as the other lifecycle signals.
    void fenceRolledUpChanged(const QString& id, bool rolledUp);
    void fencesVisibilityChanged(bool visible);
    void peekChanged(bool active);
    // T-076: the user asked for the fence's **own** menu -- a right-click that
    // landed on the title bar or on an empty panel, never on a tile (that one
    // stays the Shell's menu). The container only says *which* fence and *where*:
    // the content is ui::FenceMenu's, the actions are the product's.
    void fenceMenuRequested(const QString& id, const QPoint& globalPos);
    // Session 65: a right-click on tiles of a fence (`point` physical).
    void fenceItemMenuRequested(const QString& id, const QStringList& paths, const QPoint& point);
    // Session 60: a double-click on a fence's title text (inline rename).
    void fenceTitleEditRequested(const QString& id);
    // T-010. The rule list is persisted, so the product wires this to the same
    // auto-save as the other document-level signals. It is deliberately NOT
    // emitted by applyLayout: restoring the document is not changing it.
    void rulesChanged();
    // An item was filed by a rule. Diagnostics and, later, an undo trail.
    void ruleApplied(const QString& path, const QString& fenceId);

private:
    QString m_tileDropTarget; // session 65: the fence showing an insertion mark
    void onFenceSurfaceLost();
    void rehostLostFences();
    QTimer m_rehostTimer;
    int m_rehostTries = 0;
    int m_rehosted = 0;

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

    // Puts a fence's surface in the shape and the transparency its panel needs
    // (T-057, and since T-079/ADR-15 the only mode that reaches the screen): the
    // material is the window's **uniform alpha** and the rounded panel is the
    // window's **region**. No window is recreated, so a material change is one
    // call. Returns false only when the window refused -- the caller keeps going
    // either way, because a fence whose alpha is wrong is still a fence, while a
    // fence whose window failed to come back is not.
    bool applyMaterialToSurface(FenceWindow& fence);
    // T-098: the alpha the fence shows, and the view's colours from it.
    int effectiveAlpha(const core::FenceStyle& style) const;
    void applyStyleToView(FenceWindow& fence);
    // Whether the fences should be composed now; switches every fence over
    // when the answer changed.
    void syncCompositionMode();
    void composeFence(FenceWindow& fence);
    int indexOf(const QString& id) const;

    // --- the icon overlay (T-082) -------------------------------------------
    // Whether this fence wants its icons kept out of the material: hosted, a
    // material that is not fully opaque, and `fadeIcons` off. The last condition
    // is the whole option -- a fence that asks for the material on its icons has
    // no second window at all, and falls back to exactly what it did before
    // T-082.
    bool wantsIconOverlay(const FenceWindow& fence) const;
    // The tiles of a fence, in the overlay's own pixels (the surface is physical,
    // the widget logical -- T-014).
    QVector<QRect> iconOverlayTiles(const FenceWindow& fence, qreal dpr) const;
    // Creates the overlay if it is wanted and missing. Returns true when there is
    // one afterwards.
    bool attachIconOverlay(FenceWindow& fence);
    // Creates it, moves it with its panel, re-shapes it for the tiles it has now,
    // or destroys it when it is no longer wanted. Idempotent, and cheap when
    // nothing changed: every call it makes is a no-op in that case.
    void syncIconOverlay(FenceWindow& fence);
    void destroyIconOverlay(FenceWindow& fence);

    QString nextId();

    QList<FenceWindow> m_fences;
    IconGrid::IconProvider m_iconProvider;
    IconGrid::IconProvider m_backdropIconProvider;
    bool m_sortOnAdd = false;
    ItemResolver m_itemResolver;
    // T-039: the layer the desktop's own icons are drawn on while the native one
    // is hidden. Owned here, and always present -- it is inert until enabled.
    DesktopBackdrop* m_backdrop = nullptr;
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
    // T-077 / ADR-15: how many frames have been rendered. Counted because the
    // claim being measured ("one image per changed size, none for a move") is
    // about the render, and the surface no longer pushes anything to count.
    int m_frameRenders = 0;
    int m_defaultAlpha = 255;
    bool m_allowOverlap = true;
    bool m_composed = false;
    int m_composeCount = 0;
    qint64 m_composeUs = 0;
};

} // namespace starfish::ui
