# Documentation technique — StarFish

> Version 0.1 — 2026-10-13. Les décisions sont tracées sous forme d'ADR ; toute évolution doit mettre à jour l'ADR concerné et `STATE.md`.

## 1. Contraintes de plateforme

- Windows 10 (22H2+) et Windows 11, x64 en priorité.
- Pas de service Windows obligatoire : app desktop standard lancée au démarrage (option).
- Fonctionne sans droits administrateur.

## 2. Architecture (composants)

```
┌────────────────────────────────────────────────────┐
│ Application (Qt, thread UI)                        │
│  ├─ ProductApp     l'application (T-038)           │
│  ├─ TrayIcon       icône de notification (F-034)   │
│  ├─ SettingsDialog fenêtre de réglages (F-034)     │
│  ├─ DesktopHost    héberge les zones sur le bureau │
│  ├─ DesktopSurface surface hébergée (rendu)        │
│  ├─ FenceContainer cycle de vie des zones (T-007)  │
│  │                 + navigation (T-009)            │
│  │                 + application des règles (T-010)│
│  ├─ FenceView      rendu + interaction d'une zone  │
│  ├─ FenceGeometry  géométrie pure (hit-test, snap, │
│  │                 roll-up)                        │
│  ├─ ScreenLayout   écrans + DPR (T-014)            │
│  ├─ IconGrid       grille d'icônes d'une zone      │
│  ├─ RuleEngine     évaluation des règles (F-017)   │
│  ├─ DesktopWatcher nouveaux fichiers (F-018)       │
│  ├─ LayoutStore    lecture/écriture JSON atomique  │
│  ├─ SnapshotManager snapshots nommées (F-027/F-028)│
│  ├─ DesktopScanner lecture du dossier Bureau       │
│  ├─ ShellIconCache icônes shell (image list)       │
│  ├─ ShellBridge    menu contextuel, DnD            │
│  └─ HotkeyManager  raccourcis globaux (T-009)      │
└────────────────────────────────────────────────────┘
     │ dépend de
     ▼
  Windows API : Progman / SHELLDLL_DefView (placement,
  ADR-4), SHELL32 (SHGetKnownFolderPath, SHGetFileInfo,
  SHGetImageList, SHChangeNotify), ole32 (IShellLink,
  IContextMenu, CF_HDROP), per-monitor DPI v2,
  RegisterHotKey
```

- **DesktopHost** (`src/platform/win/desktop_host.{hpp,cpp}`) : découverte et placement bureau (parentage sous `Progman`, z-order sous `SHELLDLL_DefView`, flag layered, masquage des icônes natives, watchdog, verdict de placement). Implémenté en Phase 1 (T-016).
- **ProductApp** (`src/app/product_app.{hpp,cpp}`, T-038) : **le mode produit** — la séquence qui manquait, pas un moteur de plus. Charge le document avant la première frame, héberge les zones, câble l'auto-save, démarre le watcher (F-018) sur le vrai dossier Bureau et les dossiers des portails, enregistre les hotkeys, synchronise le tray et les réglages, puis écrit le document en sortant. Options granulaires → exécutable sans bureau. Voir §15.
- **TrayIcon / SettingsDialog** (`src/ui/tray_icon.{hpp,cpp}`, `src/ui/settings_dialog.{hpp,cpp}`, T-038) : l'icône de notification (F-034) et la fenêtre de réglages (F-034 : renommer F-002, enrouler F-020, supprimer F-007, masquage des icônes). Les deux **signalent** et ne décident de rien — même règle que `FenceView` (décision 15). Voir §15.
- **DesktopSurface** (`src/platform/win/desktop_surface.{hpp,cpp}`) : **le chemin de rendu retenu (T-020, §7 piste (a))**. Surface Win32 brute hébergée sous `Progman` qui possède son propre `WNDPROC` : elle blitte sur `WM_PAINT` une image produite par Qt hors écran (`QWidget::render()` dans un `QImage`). Qt ne touche jamais au HWND hébergé, ce qui contourne le blocage d'ADR-4 finding 3. `WS_EX_LAYERED` est posé dès `CreateWindowExW` (finding 4). Expose `attach()` / `setFrame()` / `detach()`, `syncZOrder()`, un garde `WM_WINDOWPOSCHANGING` (le `hwndInsertAfter` est réécrit en `SHELLDLL_DefView`) et un watchdog 2 s. Compteurs de peinture (`paintReport()`) pour diagnostiquer un « rien à l'écran ».
- **DesktopScanner** (`src/platform/win/desktop_scanner.{hpp,cpp}`) : lit les deux dossiers Bureau fusionnés par le Shell (`FOLDERID_Desktop` + `FOLDERID_PublicDesktop`) et remplit un `core::DesktopItem` par entrée — nom d'affichage Shell, cible des `.lnk` (via `IShellLink`) et des `.url`, attributs caché/système, taille, date. T-004.
- **ShellIconCache** (`src/platform/win/shell_icon_cache.{hpp,cpp}`) : icône telle que le Shell la dessinerait, via la liste d'images système (`SHGetFileInfo` → `SHGetImageList` → `IImageList::GetIcon` → `QImage::fromHICON`), mise en cache par (chemin, taille). T-004.
- **FenceContainer** (`src/ui/fence_container.{hpp,cpp}`, T-007) : propriétaire du **cycle de vie des zones**. Une `FenceWindow` = id + titre + moniteur + `QRect` (**le corps déplié**, même enroulé) + `FenceStyle` + `FenceView` + `DesktopSurface` optionnelle + la dernière frame rendue. Deux modes : **headless** (défaut — géométrie et widget seulement, aucun bureau requis, ce qui rend les tests et la sonde possibles) et **hébergé** (`setHostOnDesktop(true)` — chaque zone reçoit sa `DesktopSurface`, `IDropTarget` OLE câblé comme en T-006-3). `removeFence()` supprime la zone, **jamais les fichiers**. Depuis T-008 il porte aussi le mapping vers/depuis le document : `toLayout()` / `applyLayout()`. Depuis T-009 il porte l'état de navigation (roll-up, visibilité globale, Peek) — voir §11 et ADR-8. Depuis T-010 il porte aussi les **règles** (`setRules()` / `rules()`) et les applique (`classify()`, `applyRulesTo()`), sans jamais toucher au disque — voir §12. Depuis T-014 il porte aussi les **écrans** (`setScreens()` / `screens()` / `revalidateScreens()`), et chaque zone connaît le moniteur où elle se trouve — voir §16.
- **FenceGeometry** (`src/ui/fence_geometry.hpp`, T-007) : géométrie **pure** du cycle de vie et de la navigation — `FenceHandle`, `hitTest`, `resize` (avec taille minimale), `snap` (bords + centres, **par écran** depuis T-014, et autres zones), `clampIntoRect` / `clampToBounds` (origines négatives comprises), `visibleRect` (roll-up) et `applyRequest` (la demande de déplacement/redimensionnement résolue en rect stocké). Aucun widget, aucun Win32 : c'est ce qui rend les règles de T-007, de T-009 **et** de T-014 unit-testables.
- **ScreenLayout** (`src/ui/screen_layout.hpp`, T-014) : les **écrans** — rects **physiques** en coordonnées d'écran virtuel, DPR, nom de périphérique, primaire — et les deux décisions qui en découlent : à quel écran une zone appartient (`screenForRect`, par son **centre**) et **où revient un rect stocké** quand son moniteur a disparu (`restoreRect`). Pur, sans `QScreen` ni Win32 → un arrangement **DPI mixte à deux écrans se teste sans deux écrans**. Voir §16.
- **HotkeyManager** (`src/platform/win/hotkey_manager.{hpp,cpp}`, T-009) : `RegisterHotKey` sur une fenêtre message-only (`HWND_MESSAGE`), signal `triggered(id)`. Déclencheur de Peek (F-021) et de quick hide (F-022) tant que les raccourcis ne sont pas configurables (F-033). L'échec d'enregistrement est **remonté**, jamais avalé — `Win+Space` est refusé par Windows.
- **SnapshotManager** (`src/core/snapshot.{hpp,cpp}`, T-012) : **snapshots nommées** (F-027) et **export/import** (F-028). Une snapshot est une disposition schéma 1 **plus** une enveloppe `snapshot:{createdAt,note}` : restaurer, c'est appliquer une disposition, et il n'y a **aucun second format**. `saveSnapshot` / `loadSnapshot` / `removeSnapshot` / `listSnapshots` / `snapshotsDirFor` (dossier à côté du document) / `exportLayout` / `importLayout`. Qt Core seul — testable sans écran. Voir §14.
- **JsonStore / LayoutStore** (`src/core/jsonstore.{hpp,cpp}`, `src/core/layout_store.{hpp,cpp}`, T-008) : seule source de vérité pour « quelle icône dans quelle zone, où ». `JsonStore` ne connaît que le format (schéma 1, écriture atomique, sérialisation déterministe) ; `LayoutStore` possède le document (chemin, chargement, écriture différée — voir §10 et ADR-7). Les fichiers, eux, restent où ils sont sur le disque.
- **RuleEngine** (`src/core/rules.{hpp,cpp}`) : **pure** — entrée = métadonnées d'un fichier (`core::FileMeta`) + liste ordonnée de règles, sortie = id de la zone cible ou rien. Critères ET-és : extension, motif de nom, fenêtre d'âge (relative à un `now` injectable, donc testable sans horloge). Depuis T-010 le `Layout` transporte les règles et `FenceContainer::applyRulesTo()` les applique — voir §12.
- **DesktopWatcher** (`src/core/desktop_watcher.{hpp,cpp}`, T-010) : déclencheur de F-018. `QFileSystemWatcher` + instantané des entrées + diff pur (`newEntries()`) + debounce, Qt Core seul, donc testable sans bureau. Il **signale** (`itemsAdded`) ; c'est le conteneur qui range. Voir §12.
- **ShellBridge** : isole tout l'accès aux API Windows pour rendre le reste du code testable/portable (à créer en Phase 2).

## 3. Décisions techniques

### ADR-1 — Windows uniquement en v1
Fences est Windows-only et la valeur du produit est l'intégration shell. Linux (KDE) pourra être étudié après la v1. *Statut : accepté.*

### ADR-2 — Stack : C++17 + Qt 6.8 (module Widgets)
- Pour : rendu custom avancé (formes de zones, fondu, teinte), DnD mature (`QMimeData`, `QDrag`), packaging Windows simple (`windeployqt`), per-monitor DPI, licence LGPL compatible open source.
- Contre : verbosité, build lourd.
- Alternative écartée : PySide6 (itération rapide, mais perf/packaging/empreinte mémoire moins bons pour un daemon de bureau) ; WPF/WinUI (verrou Microsoft, moins portable) ; egui/Rust (écosystème DnD/windowing moins mature).

### ADR-3 — On masque les icônes natives et on dessine les nôtres
Requis par folder portals (F-024), teinte d'icônes (F-015) et pages de bureau (F-030). Conséquence : il faut réimplémenter les interactions shell (double-clic, menu contextuel via `IContextMenu`, glisser-déposer `CF_HDROP`, renommage F2). **Repli possible** : mode dégradé qui repositionne les icônes natives dans la zone (pas de portals ni de teinte) — à garder en tête si T-003/T-005 révèlent un blocage shell.

### ADR-4 — Positionnement : fenêtres enfants de Progman (**layered**), sous SHELLDLL_DefView

Technique initialement envisagée (WorkerW) **invalide sur Windows 11 récent** — findings des spikes du 2026-10-13 (`experiments/spike_workerw.py`) :
- Win11 garde `SHELLDLL_DefView` **enfant direct de Progman** ; le détournement WorkerW (message 0x052C, re-parenting de la DefView) propre à Win10 ne se produit pas.
- Les `WorkerW` présents (15 sur la machine de dev) sont des fenêtres 136×39 d'outils tiers.
- Hiérarchie réelle mesurée (`experiments/probe_desktop.py`) : `Progman` → [`SHELLDLL_DefView` (**`WS_EX_LAYERED`**, plein écran), `WorkerW` (plein écran)]. `DefView` n'a qu'un enfant : `SysListView32` « FolderView » (la liste d'icônes). **Les « fences » de Fences ne sont pas des fenêtres** : elles sont dessinées à l'intérieur du `SysListView32` natif (masquer `DefView` les fait disparaître).

**⚠️ Finding critique (session 5) — la fenêtre doit être LAYERED.** Un enfant de Progman **non-layered n'est pas composé par DWM** : `IsWindowVisible` renvoie `True`, la chaîne de z-order est correcte (juste sous `DefView`), et pourtant **rien n'est dessiné à l'écran**. Échec silencieux. Établi par : capture plein écran (0 pixel de la fenêtre), test « au-dessus de la couche d'icônes » (`HWND_TOP`) — idem, puis **confirmation visuelle par l'utilisateur** (fenêtre plein écran invisible à l'œil). Le blocage **n'est pas dû à Fences** (le correctif fonctionne avec Fences actif).
→ Le « succès » visuel de la session 4 était un **faux positif** : l'ancien papier peint était orange, de la même teinte que la fenêtre de test — une zone du fond partiellement recouverte par une fence a été prise pour la fenêtre.

**Correctif :** `WS_EX_LAYERED` + `SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)` (`DefView` est elle-même layered → condition pour être composé). Avec ce correctif la fenêtre est rendue (77,8 % de pixels orange mesurés ; icônes du bureau **et** fences dessinées par-dessus → placement conforme).

**Séquence Win32 de référence** (session 5, corrigée session 8) :
1. Créer une fenêtre `WS_POPUP` sans décoration, **`WS_EX_LAYERED` posé dès `CreateWindowExW`** (voir finding 4).
2. `SetParent(hwnd, Progman)` ; style : ajouter `WS_CHILD`, retirer `WS_POPUP`.
3. Ex-style : retirer `WS_EX_TOPMOST` ; **le flag layered doit déjà être là** — ne pas tenter de l'ajouter ici. Puis `SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)`.
4. `SetWindowPos(hwnd, SHELLDLL_DefView, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW)`.
5. Vérifier par la **chaîne de z-order** (`EnumChildWindows`) **et** par une **capture d'écran** — le z-order seul ne prouve pas le rendu. (Hit-tests non fiables : `WindowFromPoint` répond `Shell_LightDismissOverlay`.)

⚠️ Ne pas se fier au seul z-order (il peut être correct alors que la fenêtre n'est pas rendue).
⚠️ Ne pas utiliser `overrideredirect` Tk ni de framework qui restack : Tk replace ses fenêtres au-dessus après `SetWindowPos`.
**Bonus mesuré :** masquer les icônes natives = `ShowWindow(SW_HIDE)` sur la `SHELLDLL_DefView` (réversible, pas d'écriture registre sur ce build).
Z-order à recalculer sur `WM_SETTINGCHANGE`, changement de résolution et redémarrage d'Explorer.

**Portage C++/Qt (T-016)** — `src/platform/win/desktop_host.{hpp,cpp}`, classe `DesktopHost` :
- Séquence Qt : widget sans décoration (`Qt::Window | Qt::FramelessWindowHint`) → `show()` (ordre du spike : créer → montrer → re-parenter → placer) → `winId()` → `SetParent` → `WS_CHILD` sans `WS_POPUP` → `WS_EX_LAYERED` + `SetLayeredWindowAttributes(255)` → `SetWindowPos(hwnd, DefView, …)`.
- **Garde `WM_WINDOWPOSCHANGING`** : tout changement de z-order visant une de nos fenêtres voit son `hwndInsertAfter` réécrit en `SHELLDLL_DefView`. C'est ce qui empêche Qt (`show()`, redimensionnement, activation) de remonter la fenêtre au-dessus des icônes.
- **Watchdog 2 s au lieu des broadcasts** : une fenêtre `WS_CHILD` de `Progman` ne reçoit **pas** `WM_SETTINGCHANGE` / `WM_DISPLAYCHANGE` (messages diffusés aux fenêtres top-level). Le filtre natif reste en place (best effort) mais la garantie vient du timer, qui re-vérifie les handles et le z-order, détecte la mort des fenêtres (`IsWindow`) et émet `desktopReacquired()`.
- **Redémarrage d'Explorer** : une fenêtre enfant de `Progman` est détruite avec lui. `DesktopHost` retire les entrées mortes et émet `desktopReacquired()` ; le propriétaire recrée/rea-tache ses fenêtres (`attach()` gère une entrée périmée). ⚠️ Chemin **non validé en local** (pas de build) — à tester manuellement (`taskkill /f /im explorer.exe` puis relance).
- **Vérification** : `DesktopHost::verifyPlacement()` renvoie `BelowIconLayer` / `AboveIconLayer` / `Unknown` via la chaîne `EnumChildWindows` (logique pure dans `src/core/zorder.*`, testée par `tests/test_zorder.cpp`). Rappel : verdict correct ≠ rendu effectif.
- **Limite connue — transparence par pixel** : `SetLayeredWindowAttributes(alpha=255)` compose la fenêtre de façon uniforme. Un fond de zone translucide laissant voir le papier peint exige `UpdateLayeredWindow` (rendu Qt → `QImage` → push par pixels), non implémenté. `DesktopHost::setWindowAlpha()` n'offre pour l'instant qu'une opacité **uniforme** de toute la fenêtre.
- **Coordonnées** : `attach()` attend un `QRect` en coordonnées client de `Progman`, qui coïncident avec l'écran principal en mono-écran ; le décalage multi-écran est du ressort de T-014.

**⚠️ Findings session 7 (2026-10-13) — le portage Qt est piégé sur deux points.** Mesures faites avec la toolchain locale (voir §6), sur la machine de dev, Fences 6 actif :

1. **Qt retire `WS_EX_LAYERED` juste après `attach()`.** `QWindowsWindow::setWindowLayered()` calcule
   `needsLayered = (WindowTransparentForInput) || (hasAlpha && frameless) || opacity < 1.0`
   et **supprime** le flag dès que c'est faux. Une fenêtre Qt opaque (opacité 1.0) perd donc le flag quelques millisecondes après le reparentage → retour à l'échec silencieux de la session 5. Mesuré : `ex 0x80000` après `SetParent`, `ex 0x0` au premier tour de boucle d'événements.
   **Correctif retenu** : appeler `QWidget::setWindowOpacity(0.99)` avant `show()`, ce qui place Qt lui-même du côté « needs layered » ; l'alpha réel est ensuite remis à 255 par `SetLayeredWindowAttributes`. Coût : si Qt réapplique son opacité, l'alpha vaut 252/255 — invisible.

2. **Windows refuse d'ajouter `WS_EX_LAYERED` après le reparentage, silencieusement.** Sur une fenêtre `WS_CHILD` de `Progman` (fenêtre d'un autre processus), `SetWindowLongPtrW(hwnd, GWL_EXSTYLE, …)` retourne 0, laisse `GetLastError` à 0 et **ne change pas le style**. `GWL_STYLE` continue de fonctionner — d'où le diagnostic trompeur « z-order correct, rien à l'écran ». À l'inverse, le **retrait** du flag passe. `UpdateLayeredWindow` échoue aussi sur cette fenêtre (`ERROR_INVALID_PARAMETER`, 87) — la voie T-017 « transparence par pixel » est donc **bloquée en l'état** sur une fenêtre enfant cross-process.
   **→ Cause racine trouvée en session 8 (finding 4)** : ce n'est pas une bizarrerie de Windows mais le comportement de rétro-compatibilité appliqué faute de manifeste applicatif. Avec un manifeste, la même écriture est acceptée. Conséquence pratique inchangée : `attach()` ne doit jamais tenter d'*ajouter* le flag ; il le vérifie (`qCCritical` si absent).

3. **Qt ne peint pas dans la fenêtre hébergée — T-020.** Le placement est exact (`--dump` pendant la démo : `DefView[0] / SysListView32[1] / SysHeader32[2] / Qt683QWindowIcon[3] 80,80 480x320 / WorkerW[4]`, layered, alpha 255), mais **rien n'apparaît** : capture avant/après identique (`diff bbox = None`). Mesures : `PrintWindow` renvoie une image vide (`centrePixel = 0x0`) alors que `QWidget::grab()` produit bien un 480×320 peint ; `repaint()`, `hide()/show()` et `WA_Mapped` (vrai) ne changent rien. Autrement dit Qt ne fait plus le blit du backing store vers le HWND dès que celui-ci est enfant d'un parent étranger. Le spike Win32 pur, lui, rend correctement avec la même séquence (**96,6 %** de pixels dans la zone, contrôle rejoué en session 7) : la technique de placement reste valide, c'est le chemin de rendu Qt qui lâche.
   **→ Résolu en session 8 par la piste (a)** : `DesktopSurface`, surface Win32 brute hébergée qui blitte sur `WM_PAINT` une image produite par Qt hors écran. **100 % de la zone rendue à l'écran** (153600 / 153600 px, bbox exactement le rectangle de la zone), vérifié par capture. Voir §7 et finding 4.

4. **⚠️ La cause racine du refus de `WS_EX_LAYERED` : l'absence de manifeste applicatif.** Le finding 2 n'était pas une bizarrerie de Windows mais une **rétro-compatibilité** : `WS_EX_LAYERED` sur une fenêtre **enfant** n'est supporté qu'**à partir de Windows 8**, et Windows choisit le comportement d'après la section `<compatibility>` du manifeste de l'exécutable. **Un binaire MinGW n'a pas de manifeste par défaut** → comportement hérité → l'ajout du flag à une fenêtre enfant est refusé **silencieusement**. `python.exe`, lui, embarque un manifeste — ce qui explique que le spike Python (`experiments/spike_workerw.py`) n'ait jamais rencontré le problème alors que le même code en C++ échouait.
   Mesures de la session 8 (`experiments/spike_surface_min.cpp`, Win32 pur, sans Qt ; `experiments/spike_exstyle_battery.py` pour la contrepartie Python), sur un enfant de `Progman`, même rectangle, même état de fenêtre :

   | Ordre | Sans manifeste | Avec manifeste |
   |---|---|---|
   | `CreateWindowExW(WS_EX_LAYERED)` puis `SetParent` | flag gardé, alpha accepté, **152710 / 153600 px (99,4 %)** | idem |
   | `SetWindowLongW(GWL_EXSTYLE, …\|WS_EX_LAYERED)` **avant** `SetParent` | flag gardé, alpha accepté, **0 px** : le bit est posé mais aucune surface layered n'est créée | idem |
   | `SetWindowLongW(GWL_EXSTYLE, …\|WS_EX_LAYERED)` **après** `SetParent` | **refusé** (retour 0, `GetLastError` 0, style inchangé, puis `SetLayeredWindowAttributes` → 87) | **accepté**, puis **99,4 %** de pixels à l'écran |

   Le refus est **spécifique au bit LAYERED** : sur la même fenêtre, `WS_EX_TRANSPARENT`, `WS_EX_TOOLWINDOW` et `WS_EX_NOACTIVATE` passent tous. `WS_EX_TOPMOST` est refusé aussi, mais c'est attendu (une fenêtre enfant ne peut pas être topmost). Ce n'est **pas** un artefact de Qt : un processus C++ nu le reproduit, et ce n'est **pas** une question de DPI (`python.exe` et le binaire C++ sont tous deux *unaware* ; les quatre modes DPI échouent identiquement), ni de `WNDPROC`, ni de console, ni de « warm-up » layered préalable.
   **Correctif** : `src/starfish.manifest`, embarqué dans `starfish.exe` via `src/starfish.rc` (MinGW n'a pas d'étape manifeste par défaut, contrairement à MSVC). Le flag est **en plus** posé dès `CreateWindowExW`, pour que la surface fonctionne même depuis un binaire dont le manifeste aurait disparu — cas où l'échec est totalement silencieux.
   ⚠️ **Piège de diagnostic** : un manifeste XML ne peut pas contenir `--` dans un commentaire. Sans ça Windows refuse de démarrer le binaire (`configuration côte-à-côte incorrecte`), ce qui n'a rien à voir avec le problème cherché.

**Outils de diagnostic créés en session 8** (à réutiliser avant de soupçonner le placement) :
- `experiments/spike_surface_min.cpp` — reproducer Win32 pur (aucun Qt) : rejoue la séquence pas à pas, affiche chaque valeur de retour et chaque relecture, puis capture l'écran et compte les pixels de la couleur de test. Options : `--no-parent`, `--child-direct`, `--layered-at-create`, `--layered-before-parent`, `--battery`, `--default-proc`, `--warmup`, `--dpi N`, `--settle-ms N`, `--show-console`. **Une seule exécution répond aux deux questions** : le flag tient-il ? la fenêtre est-elle composée ?
- `experiments/spike_exstyle_battery.py` — la même batterie en ctypes, pour comparer Python et C++ ligne à ligne (`child-only` pour un processus neuf).
- `experiments/spike_surface_min.manifest` + `.rc` — la variante avec manifeste, pour l'A/B.
- ⚠️ Le compte de pixels n'est valable que si le bureau est visible : `spike_surface_min` **masque sa propre console** au démarrage (une fenêtre de console est une fenêtre top-level qui recouvre le rectangle de test) et **avertit** quand une fenêtre top-level recouvre ce rectangle.

### ADR-5 — Persistance JSON versionné
Exemple (§5) : lisible, diff-able, exportable (F-028). Écriture atomique (`QSaveFile`, fichier temporaire + commit). Emplacement : `%APPDATA%/StarFish/layout.json` (`QStandardPaths::AppDataLocation`), créé au premier enregistrement. **Implémenté en T-008** : `core::JsonStore` (format) + `core::LayoutStore` (propriétaire du document, écriture différée) — voir §10.

### ADR-6 — Local-first
Aucun accès réseau fonctionnel, aucune télémétrie, aucun compte (F-037). Les folder portals pointent vers des dossiers locaux ou réseau de l'utilisateur, jamais vers un service StarFish.

### ADR-7 — Le layout est la seule autorité sur le contenu d'une zone, et l'écriture est différée
Deux conséquences de T-008, à respecter par la suite :

1. **Le document, pas le disque, décide de ce qu'une zone affiche.** Une entrée restaurée dont le fichier a disparu est **conservée** (nom déduit du chemin) plutôt que retirée : une clé USB débranchée ne doit pas vider les zones de l'utilisateur. Le scan du bureau ne sert donc qu'à *résoudre* les métadonnées d'affichage (nom Shell, icône, cible de raccourci), jamais à décider de la composition des zones.
2. **Aucune écriture sans changement, aucune écriture en rafale.** Un drag émet un `fenceRectChanged` par déplacement de souris (~60/s) : `LayoutStore` compare le document candidat au document courant (sérialisation déterministe) et n'arme un timer d'écriture (500 ms, réarmé à chaque changement) que si les octets diffèrent. Une sélection qui change ne réécrit donc rien (la sélection n'est pas persistée), et un geste entier produit **une** écriture.

Corollaire : un fichier illisible, tronqué ou de schéma inconnu est **refusé** et le layout courant est **conservé** (jamais remplacé par du vide). L'erreur est remontée à l'appelant (`LayoutStore::saveFailed` / `load`), pas avalée.

### ADR-8 — La navigation change ce qui est *visible*, jamais ce qui est *stocké*

Les trois comportements « dégager le bureau » (F-020 roll-up, F-021 Peek, F-022 quick hide) partagent une règle : **l'état appartient au conteneur, et la géométrie persistée ne bouge pas**.

1. **Le rect stocké est toujours le corps déplié.** Enrouler une zone ne réécrit pas son `rect` : `FenceGeometry::visibleRect(stored, rolledUp)` dérive le rect affiché (la barre de titre) du rect stocké, et `applyRequest()` fait tout le calcul — aimantation et clamp sur le rect **visible**, hauteur dépliée préservée. Dérouler restaure donc la zone *exactement*, et le schéma JSON n'a pas eu à bouger (le champ `rolledUp` voyageait déjà depuis T-008).
2. **Le widget et la surface obéissent, ils ne décident pas.** Une `FenceView` ne se déplace pas plus qu'elle ne s'enroule : le double-clic et la molette sur la barre de titre émettent `rollUpRequested(bool)`, et c'est `FenceContainer` qui applique (même règle qu'en T-007 pour la géométrie).
3. **L'état de navigation doit survivre au watchdog.** Une zone masquée (quick hide) ou surélevée (Peek) est ré-appliquée toutes les 2 s par le watchdog de `DesktopSurface` : `place()` tient compte de `m_visible` et de `m_raisedAboveIcons`, sans quoi une zone masquée réapparaîtrait en boucle. Même chose pour la garde `WM_WINDOWPOSCHANGING`, qui n'autorise `HWND_TOP` qu'en Peek.

Corollaire assumé : Peek surélève la zone **dans le bureau** (au-dessus de la couche d'icônes), pas au-dessus des fenêtres d'application — c'est le maximum atteignable tant que les zones sont des enfants de `Progman` (ADR-4). Voir §11.

### ADR-9 — Édition manuelle : StarFish possède la couche d'icônes (session 25)

**Demande (utilisateur, session 25)** : créer des zones quand on veut, y déposer les icônes qu'on veut, organiser **à la main**, et qu'une zone déplacée **emporte ses icônes**. L'auto-organisation (F-017/F-018) n'est **pas** demandée : ne pas y investir (T-031/T-032 restent ⬜ et sans suite planifiée).

**Fait mesuré 1 — les tuiles appartiennent à la zone, pas à l'écran.** Rendre deux fois la même zone, avant et après un déplacement, produit des images **identiques octet pour octet** (la frame est le rectangle de la zone), et la fenêtre hébergée est repositionnée. « Déplacer une zone emporte ses icônes » est donc **vrai par construction**, et désormais **prouvé** (`--fence-demo`, vérification `moving a fence carries its tiles`).

**Fait mesuré 2 — sous la couche d'icônes, une zone ne reçoit pas la souris.** Au centre d'une zone hébergée, la fenêtre **visible** la plus haute de la hiérarchie du bureau est `SHELLDLL_DefView` (layered), puis `SysListView32` (plein écran, **plain**), puis notre surface — donc le clic va à la liste du bureau, pas à la zone. **Couche d'icônes masquée**, la plus haute devient `StarFishDesktopSurface`. Autrement dit : **masquer les icônes natives n'est pas cosmétique (ADR-3), c'est la condition pour qu'une zone soit utilisable.** Mesuré et vérifié par la sonde (`the fence is reachable only once the native icons are hidden`), qui restaure les icônes immédiatement après.

**Décision.** Tant que StarFish tourne, il **prend la place de la couche d'icônes** :

1. La couche native (`SHELLDLL_DefView`) est **masquée au démarrage** du mode produit, et StarFish dessine **lui-même toutes** les icônes du bureau — celles qui sont dans une zone (surfaces de zone, fait) **et celles qui n'y sont pas** (une surface de fond plein écran, **T-039**).
2. **Les icônes hors zone sont calculées, jamais stockées** : *toutes les entrées du bureau − celles qu'une zone détient*. ADR-7 reste donc intact (le document est l'autorité sur les **zones**), et deux comportements tombent d'eux-mêmes : supprimer une zone **rend** ses icônes au bureau (F-007), et sortir une icône d'une zone est un déplacement **entre deux de nos surfaces** (F-011).
3. **Un seul protocole de glisser-déposer, le nôtre.** Un geste à l'intérieur du bureau ne quitte jamais nos fenêtres ; le chemin `CF_HDROP` de la coquille reste nécessaire **uniquement** pour ce qui vient de l'Explorateur (déjà câblé, T-006-3). Déposer un fichier de l'Explorateur sur une zone continue de fonctionner.
4. **Corollaire qui économise une tâche** : parce que nous possédons le fond, les gestes « sur le bureau vide » (F-001 créer une zone en glissant, F-022 quick hide au double-clic) arrivent **directement** à notre surface de fond. **Le hook souris global de T-028 n'est pas nécessaire** pour eux ; T-028 ne se justifie plus que pour un geste démarré **hors** de nos fenêtres.

**Alternative écartée (Option B)** : garder la couche native visible et placer les zones **au-dessus** d'elle. Moins de travail, et le clic atteint la zone — mais **elle ne résout pas la demande** : un fichier rangé dans une zone existerait **deux fois** à l'écran (l'icône native à sa place, la tuile dans la zone), ce qui est précisément ce que l'utilisateur veut éviter en organisant à la main. B est donc écartée **par la duplication**, pas par le goût.

**Ce que ça coûte, et ce qui reste à faire.** Réimplémenter les interactions de bureau sur les icônes hors zone (renommage, couper/coller, menu contextuel — le menu existe déjà, T-006-1) : c'est le prix assumé de l'ADR-3. `DefView` masquée doit être **rendue** à la sortie, y compris en cas de plantage (**T-042**). Le schéma du document **ne bouge pas** : aucune des trois tâches n'ajoute de champ.

**✅ TRANCHÉ (session 28, par l'utilisateur)** : **option A adoptée**, avec deux précisions qui viennent de lui :

1. **StarFish dessine toutes les icônes** (celles des zones **et** celles du bureau), et **la couche native est masquée pendant qu'il tourne**. Réponse à sa question — « que se passe-t-il quand je ferme StarFish ? » : **rien n'est perdu**, parce que StarFish ne déplace **aucun** fichier (règle d'or) et que la sortie propre **rend** déjà la couche native (`ProductApp::shutdown()`, ordre : écrire le document → détruire les zones → rendre les icônes). Ce qui disparaît à la fermeture : les zones (nos fenêtres) et le placement que StarFish donnait aux icônes hors zone — elles reprennent la grille d'Explorer. Le **seul** cas gênant est la **fin brutale** (plantage, `kill`) : la couche reste masquée, donc bureau sans icônes. D'où le **filet, à faire AVANT la surface de fond** (**T-046**) : un marqueur écrit quand on masque, une **restauration inconditionnelle au démarrage** si le marqueur est là, et une **sortie de secours** (« rendre mes icônes ») dans le tray. Risque résiduel assumé : planter **et** ne plus jamais relancer StarFish → il faut relancer Explorer.
2. **L'affichage NORMAL est : les zones au-dessus des icônes** — « l'option peek est l'affichage normal » (session 28). Autrement dit, l'aspect *affichage* de l'option B est **adopté**, mais **à l'intérieur** de l'option A : comme c'est nous qui dessinons les icônes, il n'y a **aucune duplication** — la surface de fond (icônes hors zone) est **sous** les surfaces de zone, et la couche native masquée n'est plus un concurrent. Conséquence à traiter en T-039 : **le raccourci de Peek (F-021) perd son sens** (son effet devient l'état permanent) ; il peut être retiré, ou requalifié pour « au-dessus des fenêtres d'application » (T-029) si on le fait un jour.

Le reste d'ADR-9 (icônes hors zone **calculées**, jamais stockées ; F-007/F-011 qui en découlent ; T-028 économisé) est **inchangé** et confirmé.

### ADR-11 — Une fenêtre layered par pixel n'existe pour la souris que là où son alpha n'est pas nul (T-060, session 34)

**Fait (MSDN, « Layered Windows »)** : « Hit testing of a layered window is based on the shape and transparency of the window. This means that the areas of the window that are color-keyed or whose alpha value is zero will let the mouse messages through. » **Mesuré** sur la machine de dev (`experiments/spike_layer_alpha_shape.cpp`) : une fenêtre layered par pixel dont le cadre est à alpha 0 est **sautée** par le test de toucher — le curseur trouve la fenêtre **parente** — et à alpha 1 il trouve la fenêtre elle-même.

**Conséquence, et c'est la décision.** Le fond de bureau (T-039) est un window **plein écran dont l'intérêt est d'être transparent** : il laissait donc passer la souris **partout**, et les gestes du bureau vide (F-001/T-055, F-022) n'existaient que dans les sondes, qui postent leurs événements **directement à l'objet** au lieu de laisser Windows les livrer. Le cadre est donc rempli à **`kInputAlphaFloor = 1`** (`DesktopBackdrop`), et ce niveau est **une contrainte, pas un choix esthétique** : un alpha sur 255 est invisible sur n'importe quel papier peint, et il transforme le cadre d'une **forme** (les seules icônes) en **rectangle**.

**Ce qui ne change pas** : *répondre* à un clic reste une décision séparée et distincte (`setClickThrough` → `HTTRANSPARENT` dans `WM_NCHITTEST`). Une fenêtre que le curseur **peut** trouver et qui choisit de s'écarter n'est pas la même chose qu'une fenêtre qu'il ne peut pas trouver du tout — et c'est cette distinction qui garde le fond inerte quand les gestes sont éteints.

**Corollaire pour toute fenêtre future** : une surface layered par pixel (zone translucide de T-057, par exemple) qui laisserait des pixels à alpha **exactement** 0 perdrait la souris sur ces pixels. Le plancher est la réponse, et la sonde le mesure (`the frame has nowhere the cursor falls through`, et la règle elle-même en `--host`).

### ADR-12 — Le papier peint appartient à `SHELLDLL_DefView`, les icônes à son enfant (T-061, session 34)

Retour utilisateur : « ça marche uniquement quand on est dans ce **fond d'écran gris très bizarre** que je n'ai jamais envie d'utiliser… le mode normal, c'est d'avoir **le fond d'écran affiché** ». Mesuré le 2026-10-16, en comparant le bureau à lui-même dans trois états :

| état | pixels du bureau changés | ce qu'on voit |
|---|---|---|
| `SysListView32` masqué | **6 %** (les icônes) | le papier peint **intact** — c'est ce que fait « Afficher les icônes du bureau » de Windows |
| `SHELLDLL_DefView` masqué | **87 %** | un fond **gris uni `(20,20,20)`** : le papier peint a disparu, c'est le fond de Progman |
| StarFish (avant ce changement) | 87 % | donc le gris — StarFish masquait le DefView |

**Les deux fenêtres ne portent pas la même chose** : le **papier peint** est la surface de `SHELLDLL_DefView`, les **icônes** sont son enfant `SysListView32`. StarFish masquait le premier ; c'est de là que venait le gris. Décision :

1. **On masque la liste des icônes, pas le DefView** (`SysListView32`, avec repli sur le DefView quand il n'y en a pas — même mécanisme qu'avant, marqueur T-046 compris, autre fenêtre).
2. **Toutes nos surfaces vivent au-dessus du DefView** : le fond prend la place **juste au-dessus du papier peint** (sous les zones), les zones vont en haut de l'ordre des enfants de `Progman`. Le papier peint transparaît par les pixels transparents du fond, et le clic sur le bureau vide continue d'arriver **à nous** — **mesuré : 1 398 des 1 523 points du bureau vide répondent avec le fond**, icônes natives encore visibles, sans aucun artefact de sonde.
   ⚠️ Piège Win32 : `SetWindowPos(hwnd, X, …)` place la fenêtre **sous** X. « Au-dessus du papier peint » s'exprime donc comme « **sous la fenêtre qui est déjà directement au-dessus du papier peint** » (la zone la plus basse, ou le haut de la pile), avec un **saut sur soi-même** pour que l'opération reste idempotente — sinon le watchdog pousserait le fond au-dessus des zones qu'il doit laisser devant.
3. **Ce que masquer les icônes est devenu** : ce n'est plus la condition pour qu'une zone soit cliquable (elle l'est désormais **toujours**, icônes natives visibles ou non), c'est la condition pour qu'une icône **ne soit pas dessinée deux fois** (native + tuile). C'est la raison de l'option A qui subsiste, et elle est plus facile à expliquer que la précédente.
4. **Vérification permanente** (`--backdrop-demo --host`) : `hiding the icons leaves the wallpaper alone` — le bureau est capturé **avant et après** le masquage, et les pixels que notre propre disposition ne réclame pas doivent être **inchangés**. Mesuré : **5 081 des 5 409**. Masquer le DefView rend ce test **rouge** (tout le fond change) : c'est le retour utilisateur transformé en filet.

**Coût accepté, et il reste** : les icônes que nous dessinons sont **disposées par nous** (flux en colonnes depuis le haut-gauche), pas aux positions où l'utilisateur les avait. Les lire demanderait `LVM_GETITEMPOSITION` sur la liste native **avant** de la masquer (tâche ouverte).

### ADR-13 — Le bureau reste celui de l'utilisateur : StarFish n'en masque plus rien (T-061/T-062, session 34)

Deuxième demande de la même session, et elle est sans ambiguïté : *« le mode Hide Desktop Icons n'a pas lieu d'être. Jamais. Encore moins que ce soit le défaut de lancement »*, et *« je voudrais que le clic droit marche sur le bureau normal, et que ça rajoute l'entrée au menu contextuel normal de Windows »*.

**Ce que la mesure de la session a établi**
- Masquer `SHELLDLL_DefView` efface le **papier peint** (87 % du bureau remplacés par un gris uni `(20,20,20)`) : c'était le « fond d'écran gris très bizarre ». Masquer `SysListView32` — ce que fait Windows — ne change que les icônes (**6 %**). Le papier peint est la surface du DefView ; les icônes sont son enfant. (**ADR-12**.)
- Le clic : notre fond vit au-dessus du papier peint, donc le clic sur un point **nu** nous arrive **même avec les icônes natives visibles** (mesuré : **1 398 / 1 523** points). Et `WM_NCHITTEST` peut répondre `HTTRANSPARENT` **là où une icône se trouve** : mesuré sur la vraie liste de cette machine, **88 icônes** contournées, un point d'icône → `HTTRANSPARENT` (le clic va à l'icône de l'utilisateur, à sa position, avec son menu et son glisser), un point nu → `HTCLIENT` (il est à nous).

**Décision**
1. **Rien n'est masqué au lancement.** `Options::claimDesktop` passe à **faux** ; `--claim-desktop` devient le chemin de développement, et le menu de la zone de notification ne propose plus « Hide desktop icons » ni « Restore my icons » (`TrayIcon::setIconHidingOffered`, faux par défaut).
2. **Les icônes du bureau sont celles du bureau.** Notre couche ne dessine plus les icônes libres (`setDrawFreeIcons(false)` par défaut ; mesuré : **0 pixel peint**, 86 icônes laissées au bureau). Ce qu'elle fait, c'est **répondre au bouton droit du bureau nu** : tracer une zone (F-001) puis le menu du bureau, **construit depuis le vrai `IContextMenu` de la coquille** avec notre entrée en tête (T-055).
3. **Le clic passe à travers nos surfaces partout où il ne nous appartient pas** : `HTTRANSPARENT` sur un rectangle d'icône native, et sur tout point quand le **bouton gauche** est enfoncé (la sélection rectangulaire du bureau est celle de Windows, StarFish n'a rien à y ajouter). `HTCLIENT` ailleurs.
4. **Une icône n'est déplacée que pour une zone qui la détient, et remise exactement où elle était** (`LVM_SETITEMPOSITION`, avec la position gardée depuis la lecture) — F-007/F-011. « Parquer » n'est pas « masquer » : c'est déplacer, et c'est réversible.
5. **Deux replis, tous les deux mesurés plutôt que supposés** :
   - `LVM_GETITEMPOSITION` et `LVM_GETITEMTEXTW` sont des **lectures inter-processus** (tampon alloué dans `explorer.exe` par `VirtualAllocEx`) ; un échec rend un vecteur **vide** et un avertissement — jamais une valeur inventée.
   - **Déplacer une icône n'est pas une option : c'est mesuré et abandonné.** `LVM_SETITEMPOSITION` vers `(-32000,-32000)` se relit **`(8,2)`** sur ce bureau — l'icône atterrit dans le coin haut-gauche, ce qui est **pire** que le doublon qu'on voulait éviter, et son ancienne position est perdue dès que la trace en mémoire disparaît. L'alignement sur la grille n'y est pour rien (mesuré : `LVS_AUTOARRANGE` **absent** du style `0x56003a40`). Donc **aucune icône n'est jamais déplacée** : quand une zone détient une icône, StarFish reprend la **liste** à sa charge — masquée (**jamais le DefView** : le papier peint reste), ses icônes redessinées **aux positions lues** — 
   - **On ne lit la liste que quand la coquille la dessine** (`nativeDesktopIconsVisible()`). Une vue qui n'est pas dessinée n'a rien à décrire, et si une autre instance tient le bureau, le bon geste est de ne rien toucher du tout. ⚠️ **Mesure honnête** : la première version de ce garde-fou invoquait une lecture qui rendait « 88 items, noms vides, positions `1,1` / `1,2` » — c'était **mon harnais de mesure**, pas la coquille : un pointeur 64 bits passé à `SendMessageW` sans `argtypes` est tronqué à 32 bits, et la vue écrit alors à une adresse bidon. L'instrument compilé, lui, lit correctement dans tous les états observés. Le garde-fou reste ; la justification est celle-ci.

**Invariants inchangés** : le plancher d'alpha (ADR-11), « jamais de cadre layered transparent », la discipline du marqueur (T-046), et la règle d'or des fichiers — rien n'est déplacé sur le disque.

## 4. Points d'intégration Windows (à valider en Phase 1)

| Besoin | API / technique |
|---|---|
| Masquer les icônes natives | Basculer `HideIcons` (`HKCU\...\Explorer\Advanced`) ou styler la fenêtre `SHELLDLL_DefView` ; rendre le basculement instantané et réversible — **implémenté** : `DesktopHost::setIconsHidden()` (`ShowWindow` sur DefView, aucune écriture registre) |
| Z-order bureau | Enfant de `Progman`, **`WS_EX_LAYERED` obligatoire**, inséré sous `SHELLDLL_DefView` (ADR-4) — **implémenté et rendu** : `DesktopSurface::attach()` (T-020). Le flag doit être posé dès `CreateWindowExW` et le binaire doit porter un manifeste (finding 4) |
| Dossiers du Bureau | `SHGetKnownFolderPath(FOLDERID_Desktop / FOLDERID_PublicDesktop)` + `QDir` — **implémenté** : `DesktopScanner::scan()` (T-004) |
| Nom d'affichage Shell | `SHGetFileInfo(SHGFI_DISPLAYNAME)` (retire l'extension pour les types connus) — **implémenté** (T-004) |
| Cible d'un raccourci | `IShellLinkW` + `IPersistFile` pour `.lnk` ; lecture INI (`QSettings`) pour `.url` — **implémenté** (T-004) |
| Attributs caché/système | `GetFileAttributesW` (`FILE_ATTRIBUTE_HIDDEN`/`SYSTEM`) — **implémenté** (T-004) |
| Icônes système | `SHGetFileInfo(SHGFI_SYSICONINDEX)` + `SHGetImageList(SHIL_*)` + `IImageList::GetIcon` → `QImage::fromHICON` ; repli `SHGFI_ICON` ; cache par (chemin, taille) — **implémenté** : `ShellIconCache` (T-004). Mesuré : 84/84 icônes résolues à 48 px **et** 256 px |
| Notifications de changement du bureau | `SHChangeNotify` / `ReadDirectoryChangesW` sur le dossier Bureau |
| Écrans | `QScreen` (logique) × `devicePixelRatio` = **rect physique** en coordonnées d'écran virtuel, qui sont celles de `Progman` et de Win32 ; `screenAdded` / `screenRemoved` / `primaryScreenChanged` déclenchent la revalidation — **implémenté** (T-014a, §16) |
| Menu contextuel | `IContextMenu` (shell) — phase 2 |
| Glisser-déposer sortant | `CF_HDROP` via OLE (phase 2) |
| DPI | Per-monitor DPI v2, **imposé par Qt 6** : le manifeste ne doit **pas** porter d'entrée `dpiAware` (mesuré : ce n'était pas la cause des échecs de rendu — ADR-4 finding 4). StarFish raisonne en **pixels physiques** et convertit aux deux seules frontières concernées : le rendu du frame et le routage souris — **implémenté** (T-014a, §16) |
| Hotkeys globales | `RegisterHotKey` (Win+Espace pour Peek = F-021) — **implémenté** : `HotkeyManager` (T-009). `Win+Space` est refusé par Windows (sélecteur de langue) ; raccourcis configurables = F-033 / T-029 |

## 5. Modèle de données (ébauche)

Le document ci-dessous est celui que `core::JsonStore` écrit réellement depuis T-008 (schéma 1) ; `docs/technique.md` §10 décrit qui l'écrit et quand.

```json
{
  "schema": 1,
  "fences": [
    {
      "id": "fence-001",
      "title": "Travail",
      "monitor": "\\\\.\\DISPLAY1",
      "rect": { "x": 0, "y": 0, "w": 480, "h": 320 },
      "style": {
        "background": "#CC1F2937", "radius": 10, "padding": 12,
        "iconSize": 48, "gap": 8, "titleBar": true
      },
      "sort": "name",
      "sortAscending": true,
      "rolledUp": false,
      "items": [
        { "kind": "file",  "path": "C:\\Users\\x\\Desktop\\a.pdf" },
        { "kind": "portal","path": "C:\\Users\\x\\Documents\\Factures" }
      ]
    }
  ],
  "rules": [
    { "id": "uuid", "name": "Images → Médias",
      "match": { "type": ["jpg", "png"] },
      "targetFence": "uuid", "moveFiles": false }
  ]
}
```

Notes de format :

- `style.background` est en **`#AARRGGBB`**, c'est-à-dire exactement ce que produit `QColor::name(QColor::HexArgb)` : la chaîne se relit sans perte. `iconTint` est omis quand il est vide.
- `items` est la **composition de la zone** (voir ADR-7) ; `kind` vaut `"file"` ou `"portal"` (§13, T-011). Une entrée `"portal"` ne désigne pas une tuile mais **un dossier à refléter** : ses tuiles sont listées depuis le dossier à chaque chargement et ne sont donc **jamais** écrites comme items `"file"` — le document ne contient que le chemin du dossier.
- `rules` est **écrit depuis T-010** (§12). Les critères vivent dans un objet `match` (`type`, `name`, `newerThanDays`, `olderThanDays`) pour qu'un futur critère soit un ajout plutôt qu'une rupture de schéma ; un `match` vide est la règle « attrape-tout ». Un document **sans** `rules` (ce que T-008 écrivait) se charge normalement — le tableau vaut alors vide. Le schéma reste **1** : `rules` était réservé depuis l'origine, ce n'est pas une rupture.
- Un **fichier de snapshot** (§14) est ce même document **plus** une enveloppe `snapshot: {"createdAt", "note"}` : `JsonStore` n'a pas à la connaître, la snapshot est relue en retirant l'enveloppe puis en parsant le reste comme un document schéma 1. L'id d'une snapshot **n'est pas un champ** : c'est le **nom du fichier** (`<id>.json`).
- Règle d'or : `moveFiles: false` par défaut — les règles ne **déplacent jamais un fichier sur le disque**. Le déplacement réel n'est d'ailleurs pas implémenté (T-031).

## 6. Build, packaging, qualité

- **Build** : CMake ≥ 3.24, Qt 6.8, compilateur MSVC 2022 (cible de référence, celle de la CI). **Toolchain locale de développement** (depuis la session 7, la machine de dev n'a ni MSVC ni Qt) : Qt 6.8.3 `win64_mingw` + MinGW 13.1.0, installés sans droits admin via `aqtinstall` dans `C:/Users/<user>/Qt` — voir `devlog/2026-10-13.md` session 7 pour les commandes exactes. Le code compile proprement sur les deux ; MSVC reste la cible publiée.
  - Portabilité : MinGW-w64 déclare `IID_IImageList` mais ne fournit aucun symbole dans ses bibliothèques d'import → l'IID est défini localement dans `shell_icon_cache.cpp` (valeur reprise du SDK). MSVC utilise le symbole du SDK.
  - **Manifeste applicatif obligatoire** (`src/starfish.manifest` + `src/starfish.rc`, `enable_language(RC)`) : MinGW n'a **pas** d'étape manifeste par défaut, contrairement à MSVC. Sans lui, Windows applique le comportement pré-Windows 8, `WS_EX_LAYERED` est refusé sur les fenêtres enfants et **les zones hébergées ne s'affichent jamais** — silencieusement (ADR-4, finding 4). Ne pas retirer ce fichier du `CMakeLists.txt` : la panne ne produit aucun message. **Corollaire côté MSVC (mesuré le 2026-10-15, premier build MSVC du projet)** : MSVC génère **et embarque un manifeste de son propre chef** (`/MANIFEST:EMBED,ID=1`) ; comme `starfish.rc` embarque le nôtre sous l'id 1, CVTRES refuse les deux ressources `RT_MANIFEST` (`CVT1100: duplicate resource`) et le link meurt en `LNK1123`. D'où `if(MSVC) target_link_options(starfish PRIVATE "/MANIFEST:NO")` — et un pas de CI (`mt.exe` sur le binaire construit) qui **prouve** que le manifeste final est bien le nôtre.
- **CI** : GitHub Actions `windows-latest` → configure/build + `ctest` (**10 suites**) à chaque push sur `main` et à chaque PR, plus un pas qui relit le manifeste embarqué dans l'exécutable. **Première exécution validée le 2026-10-15** — après un échec instructif : voir la note sur le manifeste ci-dessus. Les sondes `--xxx-demo` sont des exécutables GUI et ne tournent **pas** en CI (`ctest` seulement).
- **Packaging** : zip portable (`windeployqt`) pour les nightly ; installeur (WiX ou Inno Setup) pour les releases ; publication `winget` plus tard.
- **Tests** : unitaires pour la logique pure (règles, sérialisation, verdict de z-order, ordre/fusion des entrées du bureau, listage d'un portail, snapshots, géométrie des zones, **écrans**) ; manuels documentés pour l'intégration shell (checklist dans `devlog/`).
- **Style** : clang-format (LLVM, 2 espaces), commits en anglais, Conventional Commits.
- **Diagnostics CLI** (binaire en sous-système GUI) : `--dump`, `--list-desktop [--icons] [--icon-size N] [--icon-dump DIR]`, `--host-demo [--seconds N] [--hide-icons]`, `--render-probe [--surface] [--no-parent] [--show-desktop]`, `--fence-demo [--host] [--seconds N]`, `--persist-demo [--host] [--seconds N] [--layout FILE]`, `--nav-demo [--host] [--seconds N]`, `--rules-demo [--host] [--seconds N] [--layout FILE] [--watch DIR]`, `--portal-demo [--host] [--seconds N] [--layout FILE] [--portal-folder DIR]`, `--snapshot-demo [--layout FILE] [--snapshot-id ID] [--snapshot-note NOTE]`, `--product-probe`, `--context-menu-verbs PATH`. **Sans option, `starfish.exe` démarre le mode produit** ; `--seconds N` le referme au bout de N secondes, et `--no-host` / `--no-tray` / `--no-settings` / `--no-watch` / `--no-hotkeys` en retirent une pièce à la fois. Si l'appelant a redirigé stdout/stderr (fichier ou pipe), `main.cpp` **ne détourne pas** la sortie vers `CONOUT$` et force `QT_ASSUME_STDERR_HAS_CONSOLE=1` : sans quoi Qt route `qCInfo`/`qCWarning` vers `OutputDebugString` et tous les diagnostics CLI sont perdus.
  - `--render-probe --surface` est la commande de vérification de T-020 : elle mesure ce que l'écran montre réellement (avant/après, bbox, % du rectangle de la zone) **et** ce que la fenêtre contient (`PrintWindow`). Les deux sont nécessaires — un z-order correct ne prouve pas le rendu, et une capture faite pendant qu'une autre fenêtre couvre le bureau ne prouve rien.

## 7. Chemin de rendu des zones (tranché, session 8)

**Décision : piste (a).** Une **surface Win32 brute hébergée** possède le HWND et le `WNDPROC` ; Qt ne sert qu'au rendu hors écran.

- `src/platform/win/desktop_surface.{hpp,cpp}` — `DesktopSurface` : crée la fenêtre avec `WS_EX_LAYERED` dès `CreateWindowExW`, la re-parente sous `Progman`, la place sous `SHELLDLL_DefView`. `setFrame(QImage)` publie une image ; `WM_PAINT` la blitte en GDI (`StretchDIBits`, `SRCCOPY`). Qt ne voit jamais le HWND hébergé.
- L'appelant produit l'image avec `QWidget::render()` dans un `QImage` — c'est le seul usage de Qt côté rendu, et il reste du Qt Widgets classique (ADR-2 intact).
- **Pourquoi pas (b)** — faire peindre Qt dans le HWND : c'est exactement ce qui échoue (ADR-4 finding 3). Qt cesse de blitter son backing store dès que la fenêtre est enfant d'un parent étranger, et `PrintWindow` renvoie alors une image vide.
- **Pourquoi pas (c)** — surface hors de Qt : équivaut à (a) mais en abandonnant Qt pour le rendu, ce qui coûterait le rendu custom des zones (formes, fondu, teinte) qui justifie ADR-2.

**Résultat mesuré** (`starfish --render-probe --surface`, bureau visible) : `153600 / 153600` pixels de la zone changent à l'écran (bbox = exactement `80,80 480x320`), relecture `PrintWindow` = `0xff1f2937` (le panneau Qt). Vérifié aussi à l'œil (`experiments/out/t020_surface_on_desktop.png`). Le blocage de la Phase 1 est levé.

**Conséquences / suites :**
- `DesktopHost` (T-016) reste utile pour ce qu'il fait bien — découverte des handles, masquage des icônes natives, watchdog, verdict de placement — mais **il n'est plus le chemin de rendu**.
- **T-017 (transparence par pixel)** : désormais à re-tenter via `UpdateLayeredWindow` **sur `DesktopSurface`**, qui possède sa propre fenêtre — le blocage du finding 2 portait sur une fenêtre créée par Qt.
- **T-005** (icônes dans une zone) peut démarrer : il ne reste qu'à dessiner `DesktopScanner` + `ShellIconCache` dans le `QImage` hors écran.
- **Coût** : chaque image est un aller-retour CPU (rendu Qt → `QImage` → blit GDI). À surveiller quand les zones afficheront des icônes ; c'est acceptable pour la Phase 1.

**Reste ouvert :**
- Rafraîchissement : `setFrame()` invalide la fenêtre entière. Un rendu partiel (régions sales) sera nécessaire quand les zones bougeront (T-007).
- Fréquence de re-rendu et coût de `QWidget::render()` sur une zone contenant 50 icônes (à mesurer en T-005).
- **Transparence par pixel** (T-017, implémentée session 9, §8) : `UpdateLayeredWindow` fonctionne **même sur un enfant cross-process de Progman**, à condition de n'avoir **jamais appelé `SetLayeredWindowAttributes`** sur la fenêtre — sinon err 87. Reste la validation visuelle à l'écran.
- Stratégie de renommage inline (F2) vs menu shell uniquement.
- Comportement Peek quand aucune zone n'est définie.
- Format exact des hotkeys configurables (JSON dans les réglages).

### Les cellules d'icônes sont locales au contenu (session 27)

`FenceView::contentRect()` réserve la barre de titre **et** une marge (`kTitleBarHeight + kPadding` = 42 px). `IconLayout::compute()` produit ses cellules dans cet espace **local au contenu** — c'est aussi celui que `hitTest` consomme —, mais `IconGrid::paint()` dessinait ces cellules **dans les coordonnées du widget**, sans translater le `painter`. La grille entière était donc peinte **42 px trop haut**, et le `setClipRect(contentRect)` qui suit **tranchait le haut de la première rangée**. Signalé de vive voix, capture à l'appui : « l'icône est à moitié cachée par la barre de titre ». La coupure, mesurée dans l'image, est exactement à `panelTop + 42`.

Corrigé par `painter.translate(contentRect.topLeft())` puis un clip dans le repère local. `paintDropIndicator` translatait déjà ses cellules (`cells.at(index).icon.translated(content.topLeft())`) : c'est précisément cette **incohérence interne** entre deux consommateurs du même calcul qui a laissé le bug passer.

**Leçon de sonde, plus importante que le bug.** La première vérification écrite pour l'attraper **passait avec le bug en place** : elle comptait les pixels différents dans la bande de la barre de titre (`y < kTitleBarHeight`), or cette bande est **au-dessus du clip** — elle est vide quoi que fasse le painter. Une vérification **bornée par le clip ne teste rien**. La bonne instrumentation porte sur la plage où les pixels faux **atterrissent** : le premier pixel peint doit être **strictement sous** le haut du contenu (`firstDifferingRow > contentRect.top()`) et près de son origine. Avec le bug, la première ligne peinte vaut **exactement** le haut du contenu (42) : l'icône est coupée net à cet endroit. La vérification est **colorimétrique-ment agnostique** — la même zone est rendue **avec** et **sans** ses tuiles, et seule la différence des deux est inspectée — donc aucune couleur d'icône n'est supposée.

## 8. Transparence par pixel (T-017, session 9)

**Finding (spikes `experiments/spike_ulw.py` / `spike_ulw2.py`, ctypes)** : sur un enfant cross-process de `Progman`, `UpdateLayeredWindow` (ULW) :

| Séquence | Résultat |
|---|---|
| enfant de Progman, ULW direct | ✅ accepté (err 0) |
| enfant same-process, ULW | ✅ accepté |
| top-level layered, ULW | ✅ accepté |
| **`SetLayeredWindowAttributes(LWA_ALPHA)` puis ULW** | ❌ **err 87 (`ERROR_INVALID_PARAMETER`)** |
| ULW avec `hdcDst = GetDC(NULL)` explicite | ✅ accepté |

**Cause racine du blocage T-017** (session 7, attribuée à tort au cross-process et à Qt) : l'appel `SetLayeredWindowAttributes` est **une porte à sens unique** — une fois appliqué à une fenêtre enfant, tout push ULW échoue avec err 87. Le cross-process n'y est pour rien.

**Implémentation** : `DesktopSurface::setPerPixelAlpha(bool)`. En mode per-pixel, `attach()` **saute** l'appel `SetLayeredWindowAttributes` et les frames partent via ULW (DIB 32 bpp premultiplié, `AC_SRC_ALPHA`, `Format_ARGB32_Premultiplied`). Changer de mode sur une surface vivante **recrée la fenêtre** (le mode layered est posé à la création). En cas d'échec ULW, repli automatique sur le chemin opaque `WM_PAINT` (invisible est pire qu'opaque). `WM_PAINT` ne fait que valider la région en mode per-pixel ; `place()` re-pousse la frame (ULW porte la position).

Sonde : `starfish --render-probe --surface --pixel-alpha` (fence rendue translucide, alpha 96 + opacité 0,85). **Validation visuelle à l'écran en attente** (bureau non visible pendant la session 9 — captures polluées par une fenêtre au premier plan).

## 9. Cycle de vie des zones (T-007, session 13)

**Décision : séparer la géométrie (pure) du propriétaire des zones.** `FenceGeometry` ne connaît ni widget ni Win32 ; `FenceContainer` est le seul objet qui sait combien de zones existent et où elles sont. C'est cette séparation qui rend l'aimantation testable et qui **débloque le drag-drop inter-zones** : une zone ne peut pas décider seule où elle atterrit.

### Règles de géométrie

| Règle | Comportement retenu | Raison |
|---|---|---|
| Zone de saisie | `hitTest` : 6 px de bord, 30 px de barre de titre ; **les coins gagnent sur les bords** | sur une petite zone, les bandes haut/bas se recouvrent ; sans priorité aux coins, le redimensionnement diagonal serait inatteignable |
| Redimensionnement ouest/nord | l'origine bouge, **le bord opposé est épinglé** ; sous la taille minimale, on s'arrête au minimum | la zone rétrécit, elle ne se retourne jamais sur elle-même |
| Aimantation | gauche/centre/droite et haut/milieu/bas, contre les bords **et le centre** de l'écran **et de chaque autre zone**, à 8 px | le centre est ce qui permet d'aligner deux zones verticalement ou horizontalement sans calcul |
| Après aimantation | `clampToBounds` : la zone est **toujours entièrement à l'écran** | une zone traînée hors écran serait irrécupérable à la souris |

Le candidat retenu sur chaque axe est le plus proche ; les deux axes sont indépendants. `snap()` renvoie aussi quels axes ont bougé, pour dessiner un repère.

### Répartition des responsabilités

- `FenceView` **ne se déplace jamais lui-même** : au clic gauche, `FenceGeometry::hitTest` est évalué **avant** la grille d'icônes ; un titre ou un bord démarre un geste de géométrie et émet `geometryDragStarted` / `geometryDragDelta(handle, delta)` / `geometryDragFinished`. Les deux drags (icône vs zone) sont donc mutuellement exclusifs **par construction**, pas par convention.
- `FenceContainer` capture le rect de référence au début du geste et applique chaque delta à **ce même** rect : l'arrondi ne s'accumule pas.
- Le delta est appliqué **en direct** (la zone suit le curseur) ; le relâchement ne fait que clore le geste — c'est là que T-008 persistera le layout.

### ⚠️ Le geste se mesure à l'écran, jamais dans le widget (session 26)

Le delta de geste était calculé depuis `event->pos()`, une position **relative au widget** — or ce widget (et la surface hébergée qui le porte) **se déplace sous le curseur** pendant le geste. Chaque événement décrit alors le curseur par rapport à une fenêtre qui l'a déjà suivi : le déplacement appliqué vaut `réel − appliqué`, ce qui **converge vers la moitié** de la distance parcourue, en saccades. C'est ce que l'utilisateur a décrit de vive voix (« ça le fait mais pas très très bien, c'est un peu bizarre ») avant que le code soit lu : **la zone se déplaçait de la moitié** de ce que le curseur parcourait.

Corrigé en prenant la référence **à l'écran** (`event->globalPosition()`), le seul repère qui ne bouge pas. La surface poste déjà des positions écran **physiques** (ce que `TrackPopupMenuEx` attend), et le conteneur raisonne en physique : les deux espaces coïncident donc, et à 100 % le correctif ne change rien d'autre.

**Pourquoi aucune sonde ne l'avait vu** : elles envoient **un seul** `QMouseEvent` de déplacement, donc il n'y a pas de boucle de rétroaction. La vérification ajoutée (`--fence-demo`, `a multi-step drag lands where the cursor went`) livre un geste **réaliste** — 12 mouvements de (5,4), chaque événement rapportant le curseur **relatif au rect courant de la zone**, comme le fait Windows sous capture. La prémisse est prouvée : avec l'ancienne référence locale, la zone finit à **1230,924** au lieu de **1260,948**, soit exactement la moitié du déplacement — la vérification resterait donc rouge sur l'ancien code.

### Vérification

- `tests/test_fence_geometry.cpp` — **14 cas** : classement des régions, priorité des coins, croissance est/sud, épinglage du bord opposé, clamp ouest et nord, no-op sur un handle non-redimensionnant, aimantation bords/centre/autre zone, absence d'aimantation au-delà du seuil, seuil 0, `clampToBounds` (dedans, débordement, négatif, surdimensionné).
- `starfish --fence-demo [--host] [--seconds N]` — scénario **non interactif** qui exerce la vraie chaîne `FenceView → signaux → FenceContainer` en envoyant de vrais `QMouseEvent` au widget ; rapport PASS/FAIL dans `build/fence_demo.txt`, code de sortie 0/1. Mesuré : **8/8 PASS** (session 13), dont aimantation 394 → **400** (bord droit d'une autre zone), (3,5) → **(0,0)**, clamp 50×40 → **160×120**, drag de barre de titre (-77,+37) → **(0,120,320,240)**, poignée bas-droite +60,+60 → **380×300**, suppression → count 1. **Session 26 : 10/10 headless, 11/11 `--host`**, avec `a multi-step drag lands where the cursor went` en plus des deux vérifications de la session 25.
- `--fence-demo --host` héberge **3 surfaces simultanément** (`layered ex 0x80000`, `alpha 1 (err 0)`, `placed 1`) : la coexistence de plusieurs `DesktopSurface` sur le même Progman est donc vérifiée, la zone de démo étant remplie des 83 entrées réelles du bureau.

### Limites assumées

- Le geste à la souris **sur le bureau hébergé** (barre de titre / poignées) n'est pas vérifiable depuis l'agent : la sonde prouve le câblage et la **géométrie** du geste, pas le ressenti. Validation humaine, comme T-021 — et c'est l'utilisateur qui a trouvé ce bug.
- Une sonde qui envoie **un** mouvement ne peut pas voir une boucle de rétroaction : tout geste dont l'objet se déplace doit être testé par une **séquence** de mouvements dont les coordonnées locales sont recalculées à chaque étape.
- Coordonnées en client de `Progman`, donc moniteur principal uniquement — offsets multi-écrans en T-014.
- Pas encore d'aimantation magnétique « collante » (la zone ne reste pas accrochée) ni de guides visuels dessinés ; ni de redimensionnement au clavier.

## 10. Persistance du layout (T-008, session 14)

**Décision : deux pièces, comme pour la géométrie.** `core::JsonStore` ne connaît que le *format* (et rien d'autre) ; `core::LayoutStore` possède le *document* — son chemin, sa lecture, et le moment de l'écriture. Ni l'un ni l'autre ne connaît Qt Widgets ou Win32, donc les deux sont testables sans écran.

| Pièce | Rôle | Fichier |
|---|---|---|
| `JsonStore` | Format schéma 1 : `toJson` / `fromJson` / `save` (atomique, crée le dossier parent) / `load`. Sérialisation **déterministe** (`QJsonObject` trie ses clés) — c'est ce qui permet de comparer deux documents. | `src/core/jsonstore.{hpp,cpp}` |
| `LayoutStore` | Chemin du document (défaut `%APPDATA%/StarFish/layout.json`), `load`, `setLayout` (retourne `true` **seulement** si le document a changé), `scheduleSave` (timer 500 ms réarmé), `saveNow`, signaux `layoutLoaded` / `layoutChanged` / `saved` / `saveFailed`. | `src/core/layout_store.{hpp,cpp}` |
| `FenceContainer` | `toLayout()` (les zones vivantes → document) et `applyLayout()` (document → zones, ids conservés). Les chemins d'items repassent par un `ItemResolver` injectable. | `src/ui/fence_container.{hpp,cpp}` |

**Le câblage produit** est celui que la sonde exerce : `fenceCreated` / `fenceRemoved` / `fenceRectChanged` / `fenceContentChanged` → `store.setLayout(container.toLayout())` + `store.scheduleSave()`. Le relâchement d'un geste n'a donc rien de spécial à faire : la dernière position arrive par le même chemin que les autres.

**Resolver d'items.** `FenceContainer::setItemResolver()` permet de résoudre un chemin restauré en `DesktopItem`. Défaut : `core::itemFromPathOnly()` (nom déduit du chemin, drapeaux dossier/caché depuis le système de fichiers local — **aucun appel Shell**), ce qui rend la restauration testable et utilisable en mode headless. Le produit y branche `DesktopScanner::itemForPath()` pour que la tuile restaurée soit exactement ce que le Shell dessinerait. Le resolver est aussi utilisé pour les drops de l'Explorer, donc un seul chemin de résolution.

**Restaurer, c'est remplacer.** `applyLayout()` détruit les zones courantes (jamais les fichiers) puis les recrée avec leurs **ids d'origine**, et `addFence` avance le compteur d'ids au-delà du plus grand numéro restauré : une zone créée après un redémarrage ne peut pas réutiliser l'id d'une zone restaurée. Un schéma inconnu fait échouer `applyLayout` **sans rien toucher**.

**Éléments persistés mais pas encore agis** : l'alpha de `style.background` (T-017) et les champs d'apparence `iconSize` / `padding` / `gap` / `titleBar` (T-014b — les thèmes de zones). `monitor` **est agi depuis T-014** (il désigne l'écran de la zone et sert d'indice de restauration) et `rolledUp` depuis T-009. Ils voyagent déjà, donc le schéma ne bougera pas quand ces tâches arriveront. Le rayon et la couleur RGB du fond, eux, sont **appliqués** (`FenceView::setPanelColor` / `setCornerRadius`), et le remplissage opaque de `renderFence` est dérivé de la couleur du panneau : les deux ne peuvent plus diverger.

### Vérification

- `tests/test_layout_store.cpp` — **9 cas** : aller-retour de tous les champs (style, tri, items, `rolledUp`, `monitor`), refus d'un autre schéma, refus d'un JSON invalide, création du dossier parent, fichier absent = premier lancement, fichier corrompu refusé **avec le layout conservé**, `setLayout` qui ignore un document identique, rafale de 10 changements → **une seule** écriture, échec d'écriture signalé et document laissé sale.
- `tests/test_desktop_item.cpp` — **+1 cas** : `itemFromPathOnly` conserve une entrée dont le fichier n'existe plus.
- `starfish --persist-demo [--host] [--seconds N] [--layout FILE]` — **27/27 PASS**, `rc=0`, rapport dans `build/persist_demo.txt`. Points mesurés : rafale de 6 signaux → **1 écriture** ; une sélection qui change → **0 écriture** ; rect aimanté **440,400 280x200** retrouvé à l'identique après « redémarrage » ; document **identique octet pour octet** après aller-retour ; zone supprimée absente du fichier ; fichier corrompu refusé (`unterminated object`) avec le layout conservé.
- `--persist-demo --host` : les zones restaurées sont réellement hébergées — **8 `DesktopSurface`** au total pendant le scénario, toutes `layered ex 0x80000`, `alpha call 1 (err 0)`, `placed 1`, dont les deux zones restaurées à `120,90 320x240` et **`440,400 280x200`** (la position aimantée d'avant le redémarrage).
- Non-régression : `ctest` **6/6 verts** ; `--fence-demo` **8/8 PASS** ; `--fence-demo --host` **3 surfaces** + 83 entrées réelles du bureau.

### Limites assumées

- Le démarrage de l'application (chargement du layout avant la première frame) n'est pas encore branché sur un mode produit : il n'existe pas encore de « mode normal » (Phase 6 / T-015). La sonde exerce exactement la séquence prévue, mais c'est elle qui l'orchestre.
- Pas de sauvegarde sur `aboutToQuit` dans un mode produit (elle viendra avec le mode normal) ; le debounce de 500 ms suffit en pratique, mais un arrêt brutal dans les 500 ms qui suivent un déplacement perd ce déplacement.
- Pas de versionnage au-delà du refus : un schéma 2 futur devra prévoir une migration (F-028 / T-012).
- `kind: "portal"` est produit depuis T-011 (§13) ; ce qui reste ouvert, c'est la moitié **écriture** du miroir (T-036).

## 11. Navigation : roll-up, quick hide, Peek (T-009, session 15)

Les trois comportements de `docs/produit.md` section D. Tous suivent l'ADR-8 : le conteneur possède l'état, la géométrie persistée ne change pas, et le widget comme la surface ne font qu'obéir.

| Fonctionnalité | Ce qui change | Où vit l'état | Geste / déclencheur | Limite assumée |
|---|---|---|---|---|
| **F-020 roll-up** | le rect **visible** d'une zone (sa barre de titre) | `FenceWindow::rolledUp` (persisté) | double-clic **ou** molette sur la barre de titre | aucun : c'est complet |
| **F-022 quick hide** | toutes les zones disparaissent / reviennent | `FenceContainer::m_fencesVisible` | hotkey (état et câblage testés) | le double-clic sur le bureau **vide** exige un hook souris global (T-028) |
| **F-021 Peek** | le z-order : les zones passent au-dessus de la couche d'icônes | `FenceContainer::m_peekActive` | hotkey `Win+Space` (F-021) | ne flotte pas au-dessus des fenêtres d'application (T-029) |

### Roll-up (F-020)

- **Pur** : `FenceGeometry::visibleRect(stored, rolledUp)` et `FenceGeometry::applyRequest(stored, rolledUp, requested, others, bounds, minSize, threshold, snap)`. Les deux règles qui diffèrent du cas normal : une zone enroulée n'a pas de corps visible (seuls x, y et largeur changent ; la hauteur dépliée est préservée), et l'aimantation comme le clamp portent sur le rect **visible** — une zone enroulée peut donc se coller au bas de l'écran avec son corps caché qui dépasse.
- **Gestes** : double-clic sur la barre de titre (`rollUpRequested(!rolledUp)`) et molette sur la barre de titre (haut = enrouler, bas = dérouler). Ailleurs, le geste reste celui de la grille : un double-clic dans le contenu lance l'icône, il n'enroule pas.
- **Persistance** : `rolledUp` est dans le document depuis T-008 ; `setFenceRolledUp` émet `fenceRolledUpChanged`, que le produit câble au même auto-save que les autres signaux de cycle de vie. Une zone restaurée enroulée revient **enroulée**.
- **Le widget est redimensionné au rect visible** par le conteneur (`applyVisibleGeometry`) : une `FenceView` enroulée ne fait que 30 px de haut et `contentRect()` est vide, donc rien n'est peint sous la barre de titre.

### Quick hide (F-022)

- `FenceContainer::setFencesVisible(bool)` : les **surfaces** hébergées passent par `DesktopSurface::setVisible()` (`ShowWindow(SW_HIDE)` / `SW_SHOWNA`), les widgets ne sont touchés qu'en mode fenêtré (et jamais un widget jamais affiché, sinon un conteneur headless ferait apparaître des fenêtres).
- **Rien n'est détruit** : `removeFence` n'est pas appelé, les fichiers ne sont pas touchés, le layout est inchangé.
- `place()` n'ajoute `SWP_SHOWWINDOW` que si la surface est visible : sans ça le watchdog la ressusciterait toutes les 2 s.

### Peek (F-021)

- `DesktopSurface::setRaisedAboveIcons(true)` → `place()` ancre sur `HWND_TOP` au lieu de `SHELLDLL_DefView`, et la garde `WM_WINDOWPOSCHANGING` réécrit `hwndInsertAfter` en `HWND_TOP` au lieu de la DefView. `HWND_TOP` est le point le plus haut atteignable pour un enfant de `Progman` : la zone passe **au-dessus de la couche d'icônes**.
- **Mesuré** (`--nav-demo --host`, z-order relu dans la liste des enfants de Progman) : normalement `surface [1]` vs `DefView [0]` (verdict `BelowIconLayer`), en Peek `surface [0]` vs `DefView [4]` (`AboveIconLayer`), puis retour à `[1]`/`[0]`. Le Peek n'est donc pas qu'un booléen.
- **Ce n'est pas le Peek de Fences** : mettre les zones au-dessus des *fenêtres d'application* suppose un modèle top-level (reparentage ou fenêtre d'overlay), pas un enfant de `Progman`. À trancher en T-029.

### Hotkeys (`src/platform/win/hotkey_manager.{hpp,cpp}`)

- `RegisterHotKey` sur une fenêtre **message-only** (`HWND_MESSAGE`) : elle n'est jamais composée, n'apparaît pas dans le z-order et ne peut donc pas interférer avec le placement de l'ADR-4. Elle existe uniquement pour recevoir `WM_HOTKEY`.
- **L'échec est signalé, jamais avalé** : `Win+Space` — le raccourci donné par F-021 — est **refusé** par Windows (`ERROR_HOTKEY_ALREADY_REGISTERED`), qui le réserve au sélecteur de langue. C'est mesuré et imprimé par la sonde. Quick hide s'enregistre sur `Win+Shift+H`.
- Les raccourcis ne sont pas encore configurables (T-030 / F-033) : c'est là que la valeur par défaut de Peek devra changer.

### Vérification

- `tests/test_fence_geometry.cpp` — **19 cas** (14 de T-007 + 5 de T-009) : `visibleRect` enroulé/déplié, hauteur stockée préservée, aimantation de la barre de titre, barre de titre ramenée à l'écran, non-régression du clamp de taille normale.
- `starfish --nav-demo [--host] [--seconds N]` — rapport dans `build/nav_demo.txt`, code de sortie 0/1. **22/22 PASS** en headless, **29/29 PASS** en `--host`. Points mesurés : rect visible `320×30` et rect stocké inchangé ; redimensionnement enroulé sans effet sur la hauteur ; barre de titre aimantée au bas de l'écran ; dérouler restaure `320×240` ; double-clic et molette passent par les vrais événements Qt et un double-clic dans la grille n'enroule pas ; `rolledUp` écrit dans le document et restauré ; quick hide sans destruction ; z-order mesuré sous puis au-dessus de la DefView.
- Non-régression : `ctest` **7/7** ; `--fence-demo` **8/8** ; `--fence-demo --host` **3 surfaces** ; `--persist-demo` **26/26** ; `--persist-demo --host` **8 surfaces**.

### Limites assumées

- **Un `WM_HOTKEY` réel** (l'appui effectif sur la touche) n'est pas testable depuis l'agent : la sonde prouve l'enregistrement et le câblage, pas la frappe. Validation humaine, comme T-021.
- Le geste du double-clic sur le bureau vide (F-022) et le Peek au-dessus des fenêtres (F-021) sont les deux écarts restants par rapport à Fences 6 (T-028 / T-029).

## 12. Auto-organisation : règles et surveillance du dossier (T-010, session 17)

F-017 (les règles) et F-018 (leur déclencheur). Le critère de sortie de la Phase 4 est ici : « un fichier déposé dans `~/Desktop` est automatiquement rangé selon une règle ».

Trois pièces, découpées comme les précédentes — aucune ne connaît les widgets, et deux sont testables sans écran :

| Pièce | Rôle | Dépendances |
|---|---|---|
| `core/rules.{hpp,cpp}` (`RuleEngine`) | **décider** : quelle règle s'applique à ce fichier | Qt Core seul, pur |
| `core/desktop_watcher.{hpp,cpp}` (`DesktopWatcher`) | **déclencher** : dire qu'un fichier vient d'apparaître | Qt Core seul (`QFileSystemWatcher`) |
| `FenceContainer::applyRulesTo()` | **appliquer** : mettre l'item dans la zone visée | le conteneur, déjà propriétaire des zones |

### Critères de correspondance (F-017)

`Rule` porte des critères **ET**-és ; un critère vide vaut « n'importe lequel », et la **première règle qui correspond gagne** (l'ordre de la liste est l'ordre de priorité).

| Critère | Champ | Remarque |
|---|---|---|
| type (extension) | `extensions` (`match.type`) | minuscules, sans point ; comparé au suffixe du **nom de fichier sur disque** |
| nom (motif) | `namePattern` (`match.name`) | joker `*`/`?` via `QRegularExpression::fromWildcard`, insensible à la casse |
| date | `newerThanDays` / `olderThanDays` | **relatif à maintenant**, bornes exactes (`now.addDays(-N)`) |
| emplacement cible | `targetFenceId` (`targetFence`) | l'id d'une zone du même document |

Deux choix de conception à ne pas défaire :

1. **Les dates sont relatives, pas absolues.** Une règle « modifié il y a moins de 7 jours » garde son sens dans le temps ; une règle écrite avec des horodatages absolus deviendrait fausse au bout d'une semaine, et un layout restauré sur une autre machine ne correspondrait plus à rien.
2. **Le nom comparé est celui du disque, extension comprise**, pas le nom d'affichage du Shell. Le Shell masque l'extension des types connus (« photo.png » s'affiche « photo ») : un critère de type qui ne verrait pas l'extension ne correspondrait **jamais**. C'est le rôle de `core::fileMetaFromItem()`.

Un fichier dont la date de modification est illisible (`modified` invalide) **ne satisfait pas** un critère de date : il ne doit pas tomber dans « plus vieux que 30 jours » par accident.

### Déclencheur : `DesktopWatcher` (F-018)

`QFileSystemWatcher` dit seulement « ce dossier a changé », jamais comment. Le watcher garde donc un **instantané** des entrées et le diffé contre l'état courant (`newEntries()`, fonction **pure**, testée sur des listes synthétiques).

- **Le diff est calculé au moment du debounce**, pas à la réception de l'événement : un fichier créé puis supprimé dans la fenêtre ne sera jamais signalé, ce qui est le comportement voulu.
- **Debounce (400 ms par défaut)** : Windows émet plusieurs notifications par fichier créé, et le fichier peut encore être en cours d'écriture à la première. Une rafale donne **un** `itemsAdded()`.
- **La ligne de base est prise au démarrage** : les entrées déjà présentes ne sont **pas** signalées. Une règle ne doit pas re-ranger tout le bureau à chaque lancement.
- **Ré-armement** : `QFileSystemWatcher` oublie un chemin qui disparaît (dossier supprimé puis recréé, lecteur démonté). Le watcher le ré-ajoute au lieu de devenir sourd en silence.

### Application, et la règle d'or

`FenceContainer::applyRulesTo(items)` met chaque item dont la règle désigne une zone **existante** dans la grille de cette zone, et retourne combien ont été réellement placés.

- **Aucun fichier n'est déplacé sur le disque, jamais.** Une règle décide quelle zone **montre** un item. `moveFiles` est persisté et relu, mais le déplacement réel sur disque n'est **pas implémenté** (T-031) : une règle mal écrite qui relocalise silencieusement des fichiers utilisateur est irréversible, et la règle d'or d'`AGENTS.md` est de ne jamais déplacer un fichier sans opt-in explicite — ce que « ne jamais déplacer » satisfait par construction. La sonde le **prouve** : après application, le fichier est toujours à son chemin d'origine.
- **Idempotence** : un item déjà présent dans la zone visée (comparaison de chemins insensible à la casse, Windows oblige) n'est pas ajouté une seconde fois — sinon une notification répétée re-rendrait la zone et réécrirait le document pour rien.
- **Une règle orpheline** (elle vise une zone supprimée) est signalée et ignorée : l'item reste où il est, aucune exception.
- **Les règles appartiennent au document** (`Layout::rules`, tableau `rules` du §5) : elles sont donc sauvegardées, restaurées et exportables avec les zones. `applyLayout()` les réinstalle **sans émettre `rulesChanged`** — appliquer le document n'est pas le modifier, et l'annoncer ferait passer un démarrage pour une édition.

### Vérification

- `tests/test_rules.cpp` — **13 cas** (7 de T-002 + 6 de T-010 : fenêtre d'âge, bornes, date illisible, ET avec les autres critères, adaptation depuis un `DesktopItem`).
- `tests/test_desktop_watcher.cpp` — **5 cas** : diff pur, la ligne de base ne se signale pas elle-même, un fichier qui apparaît est signalé, une rafale de trois donne **un** signal, une suppression n'est pas un ajout (et le watch survit).
- `tests/test_layout_store.cpp` — **+2 cas** : aller-retour du tableau `rules` (tous les critères, octets identiques) et un document **sans** `rules` (ce que T-008 écrivait) qui continue de se charger.
- `starfish --rules-demo [--host] [--seconds N] [--layout FILE] [--watch DIR]` — rapport `build/rules_demo.txt`, code de sortie 0/1. **28/28 PASS**, mesuré : la première règle qui correspond gagne, la fenêtre d'âge rejette un fichier du jour et accepte un fichier de 4000 jours, les règles reviennent intactes après un aller-retour par le disque, un fichier **réellement créé** dans un dossier surveillé est signalé en **300 ms** et se retrouve dans la bonne zone, le `.txt` trop récent n'est **pas** rangé, et les deux fichiers sont toujours à leur place d'origine.
- Non-régression : `ctest` **7/7** ; `--fence-demo` **8/8** ; `--persist-demo` **26/26** ; `--nav-demo` **22/22**.

### Limites assumées

- **Pas de déplacement sur disque** (`moveFiles: true`), volontairement : T-031.
- **Pas d'interface de création de règles** : elles se déclarent en JSON (le format est stable) ou par `setRules()`. Un éditeur de règles est une tâche d'interface, à raccrocher à la page de réglages (F-034 / T-015).
- **Le watcher ne surveille que le premier niveau** d'un dossier. Les règles ne s'appliquent donc pas à ce qui apparaît dans un sous-dossier — ce qui est cohérent avec F-018 (« nouveaux éléments du bureau »). Depuis T-011 un portail s'appuie sur le même watcher, et un dossier surveillé est re-listé à chaque changement (§13) ; surveiller l'**intérieur** d'un dossier reste T-033.
- **`DesktopScanner` n'est pas branché sur le watcher dans un mode produit** : il n'existe pas encore de « mode normal » (T-015). La sonde câble la séquence exacte (`itemsAdded` → `itemForPath` → `applyRulesTo`) ; en production le resolver sera `DesktopScanner::itemForPath`, comme pour les drops de l'Explorer.

## 13. Folder portals (T-011, session 18)

F-024 : « portail vers un dossier quelconque (miroir en lecture/écriture) ». Un portail est une **zone qui montre le contenu d'un dossier** au lieu d'une liste d'items figée. C'est la dernière tâche de la Phase 4, et elle ferme le critère de sortie : « un portail affiche un dossier arbitraire ».

### Ce que le document contient, et ce qu'il ne contient pas

L'entrée de portail existait dans le schéma depuis l'origine (`FenceItem.kind`) et était **relue** comme le dossier qu'elle désigne (T-008) ; T-011 la **produit** :

```json
{ "kind": "portal", "path": "C:\\Users\\x\\Documents\\Factures" }
```

- **Le document ne stocke que le chemin du dossier**, jamais son contenu. Les tuiles qu'un portail apporte sont listées **depuis le disque** à chaque restauration. C'est ce qui fait d'un portail un miroir plutôt qu'un instantané : un fichier ajouté au dossier après la sauvegarde apparaît, un fichier supprimé disparaît.
- Conséquence directe et testée : **une tuile apportée par un portail n'est jamais persistée comme item `"file"`**. `toLayout()` retire de la liste tout chemin qu'un portail fournit, et écrit à la place l'entrée `"portal"`. Sans ça la liste serait écrite deux fois : la zone afficherait des doublons au rechargement, et le miroir cesserait de suivre le dossier (les entrées persistées gèleraient l'ancien contenu).
- **Le dossier lui-même n'est pas dessiné** : un portail montre ce que le dossier contient, pas le dossier.
- Le chemin est conservé **tel que déclaré**, sans normalisation : `QDir::cleanPath()` convertit `C:\x` en `C:/x`, et le document cesserait de faire l'aller-retour octet pour octet (§10).

### Les pièces

| Pièce | Rôle | Dépendances |
|---|---|---|
| `core/portal.{hpp,cpp}` (`Portal`, `listPortal`, `isPortalKind`, `sameFolder`) | **lister** : ce que le dossier contient | Qt Core + système de fichiers local, aucun Shell |
| `FenceContainer::addPortal` / `removePortal` / `refreshPortalsFor` | **appliquer** : porter les tuiles dans la zone | le conteneur, déjà propriétaire des zones |
| `DesktopWatcher::folderChanged()` | **déclencher** : dire qu'un dossier a changé | Qt Core seul |

`listPortal()` ne fait aucun appel Shell, exactement comme `itemFromPathOnly()` : un portail se teste donc sans bureau et sans Windows. Il ne liste que le **premier niveau** (même périmètre que F-018) et écarte les entrées en `.` (`isDotHiddenName`) — les entrées cachées par attribut sont conservées, comme le scanner de bureau les conserve.

Un dossier **injoignable** (clé débranchée, partage hors ligne) n'est pas une erreur fatale : `Portal::error` est renseigné, la liste est vide, et **l'entrée reste dans le document**. La supprimer reviendrait à effacer le portail de l'utilisateur à cause d'un disque débranché.

### Le miroir suit le dossier (F-018)

`DesktopWatcher` gagne un second signal, plus grossier que `itemsAdded()` :

| Signal | Sens | Consommateur |
|---|---|---|
| `itemsAdded(paths)` | ces entrées sont **nouvelles** (ajout seul) | les règles (T-010) |
| `folderChanged(folder)` | ce dossier a changé, **peu importe comment** | les portails (T-011) |

Un miroir ne raisonne pas en différentiel : il **re-liste** le dossier. Il a donc besoin de savoir qu'un fichier est *parti* autant que qu'un fichier est *arrivé* — ce que `itemsAdded()` ne dira jamais. `folderChanged()` est émis pour chaque dossier qui a notifié, après le même debounce ; il est volontairement indifférent au type de changement.

Effet de bord nécessaire : la ligne de base (`m_known`) est **élaguée** des entrées disparues. Sans ça, un fichier supprimé puis recréé sous le même nom ne serait plus jamais signalé comme nouveau (l'ancien chemin resterait dans la ligne de base).

### La règle d'or, et pourquoi la moitié « écriture » n'est pas faite

F-024 dit « miroir en **lecture/écriture** ». Seule la moitié **lecture** est implémentée ici, volontairement :

- Écrire signifierait **déplacer un fichier sur le disque** (déposer un fichier dans une zone-portail le rangerait dans le dossier cible). C'est une mutation irréversible du système de fichiers de l'utilisateur, exactement ce que la règle d'or d'`AGENTS.md` interdit sans opt-in explicite — et le même raisonnement qui a fait reporter `moveFiles` en T-031.
- Déposer un fichier sur une zone-portail continue donc de **l'ajouter comme tuile à la zone**, sans toucher au dossier. Aucune sonde, aucun test, aucun code ne déplace quoi que ce soit.

Le reste de F-024 (F-025, F-026) reste en P2 : taille d'icônes propre aux portails, et navigation dans les sous-dossiers.

### Vérification

- `tests/test_portal.cpp` — **8 cas** : `isPortalKind` (dont un `kind` absent, qui doit rester un item fichier), chemin conservé verbatim (y compris pour un dossier absent), listage naturel (dossier + fichiers, `shot9` avant `shot10`, taille/date d'un fichier, taille `-1` d'un dossier), entrées en `.` écartées, dossier vide sans erreur, dossier absent et fichier (au lieu d'un dossier) signalés sans exception, `sameFolder` (séparateurs, segments relatifs, casse, chemins vides).
- `starfish --portal-demo [--host] [--seconds N] [--layout FILE] [--portal-folder DIR]` — rapport `build/portal_demo.txt`, code de sortie 0/1. **29/29 PASS** headless, **31/31 PASS** en `--host` (deux vérifications d'hébergement en plus). Mesuré : la zone affiche les 4 entrées du dossier et **pas** le dossier ; le document contient **1 entrée `portal` et 0 entrée `file`** ; le listing n'est **pas** figé dans le document ; l'aller-retour par le disque rend le portail **et le relit depuis le dossier** ; le document est **identique octet pour octet** après aller-retour ; un portail vers un dossier absent est **conservé** et n'affiche rien ; un fichier **réellement créé** dans le dossier apparaît dans la zone, un fichier **supprimé** disparaît de la zone ; **le document n'est pas réécrit** quand le dossier change ; le dossier contient toujours exactement ce que la sonde y a laissé, et **toutes les tuiles pointent sur les fichiers eux-mêmes** (aucune copie).
- Non-régression (session 18) : `ctest` **8/8** ; `--fence-demo` **8/8** ; `--persist-demo` **26/26** ; `--nav-demo` **29/29** en `--host` (22/22 headless) ; `--rules-demo` **28/28**.

### Piège trouvé par la sonde : un signal émis au milieu d'une mutation

`FenceView::appendItems()` émet `contentChanged()`, que le conteneur relaie en `fenceContentChanged()`, que le produit écoute pour **lire `toLayout()` et sauvegarder**. La première version de `addPortalTo()` ajoutait les tuiles **puis** enregistrait le portail dans la liste. Résultat : au moment du signal, la zone avait 4 tuiles et **aucun portail** — le document lu (et donc sauvegardé) contenait les 4 fichiers comme items `"file"`, sans entrée `"portal"`. Le portail n'existait plus après un redémarrage, et le premier rafraîchissement du dossier réécrivait le document (les deux symptômes mesurés par la sonde).

**Règle qui en sort : l'état doit être cohérent *avant* le signal, jamais après.** Le portail est donc enregistré avant l'ajout de ses tuiles. C'est le pendant exact du piège d'ordre d'évaluation des arguments des sondes (§12) : ici, ce n'est pas le rapport qui ment, c'est le document qui est lu trop tôt. La sonde en garde une vérification dédiée — « le document est cohérent quand le signal part » — qui rejoue la lecture **pendant** l'émission.

### Limites assumées

- **Pas d'écriture** : déposer un fichier sur un portail ne le range pas dans le dossier cible (T-036).
- **Pas de récursion** : seul le premier niveau du dossier est listé ; naviguer dans un sous-dossier est F-026 (P2), et surveiller récursivement est T-033.
- **Pas d'interface** : un portail se déclare en JSON (`{"kind":"portal","path":...}`) ou par `addPortal()`. Créer un portail en déposant un dossier n'existe pas encore.
- **Pas de taille d'icônes propre au portail** (F-025, P2) : un portail utilise le style de sa zone.

## 14. Snapshots et export/import (T-012, session 20)

F-027 (« sauvegarder / restaurer une disposition ») et F-028 (« export / import d'une disposition »). T-012 ouvre la Phase 5.

### Un seul format, pas deux

Une **snapshot** est une **disposition nommée** :

```json
{
  "schema": 1,
  "fences": [ … ],
  "rules": [ … ],
  "snapshot": { "createdAt": "2026-10-15T15:52:03", "note": "avant la modif" }
}
```

- C'est **exactement** le document schéma 1, **plus une enveloppe**. Conséquence directe : **restaurer une snapshot, c'est appliquer une disposition** (`FenceContainer::applyLayout`), et rien d'autre — il n'existe donc **aucun second format** à tenir synchronisé avec le premier. `JsonStore` n'a pas eu à apprendre les snapshots : `fromJson` retire l'enveloppe et parse le reste avec ses validations habituelles.
- **L'id d'une snapshot n'est pas un champ du document** : c'est le **nom du fichier** (`<id>.json`). Il est donc **validé avant de devenir un chemin** — séparateurs, `:`, jokers, `.`/`..`, espace initial et point final refusés (Windows rogne les deux derniers, ce qui ferait entrer deux ids en collision sur disque).
- **Le dossier `snapshots/` vit à côté du document** (`SnapshotManager::snapshotsDirFor`) : `--layout FILE` déplace donc zones **et** snapshots ensemble.

### Sauver, restaurer, lister, supprimer

| Opération | API | Remarque |
|---|---|---|
| sauver | `saveSnapshot(dir, layout, id, note)` | `QSaveFile` (atomique) ; le dossier est créé au besoin |
| restaurer | `loadSnapshot(dir, id, out)` | refuse un **fichier de disposition** (sans enveloppe) : c'est une disposition, pas une snapshot |
| lister | `listSnapshots(dir)` | ids triés, insensibles à la casse |
| supprimer | `removeSnapshot(dir, id)` | `false` si absent — et `false` aussi sur un id invalide |

### Export / import (F-028)

`exportLayout` écrit le document **schéma 1** tel quel ; `importLayout` le relit par `JsonStore`. Une snapshot **n'est pas** une disposition, d'où deux gardes :

- l'**import** refuse un fichier de snapshot : l'avaliser perdrait l'enveloppe en silence ;
- l'**export** refuse d'écraser un fichier de snapshot, pour la même raison.

### ⚠️ Piège trouvé par le test : un préfixe d'octets ne décide pas d'un JSON

La première version détectait l'enveloppe en lisant les **4096 premiers octets** du fichier. Or `QJsonObject` **trie ses clés** — `fences`, `rules`, `schema`, `snapshot` — donc l'enveloppe est écrite **en dernier**. Au-delà de ~4 Ko la lecture ne trouvait plus rien : `exportLayout` **écrasait** la snapshot et `importLayout` l'**avalait**. Le test existant passait, parce que sa disposition témoin faisait moins de 4 Ko.

Correction : **parser** le fichier (`hasSnapshotEnvelope`). Le test `largeSnapshotIsNotALayoutFile` construit une snapshot de plus de 4 Ko, **vérifie que le marqueur est bien au-delà de la fenêtre** (sinon il n'exercerait plus rien), puis exige que l'import **et** l'export la refusent. Généralisation : **ne jamais conclure du contenu d'un JSON en lisant un préfixe d'octets quand la sérialisation trie les clés.**

### Vérification

- `tests/test_snapshot.cpp` — **13 cas** : ids invalides refusés, aller-retour save/list, métadonnées relues, fichier de disposition refusé comme snapshot, id inconnu, suppression (et double suppression), deux snapshots indépendantes, export/import, export refusé sur un fichier de snapshot, dossier à côté du document, **grande snapshot refusée** (la régression ci-dessus).
- `starfish --snapshot-demo [--layout FILE] [--snapshot-id ID] [--snapshot-note NOTE]` — **11/11 PASS**, `rc=0`, rapport `build/snapshot_demo.txt`. Mesuré : snapshot écrite sur disque et listée ; rechargée **avec sa note** ; **restaurée** — `FenceContainer::applyLayout` reconstruit la zone avec le **même id** et le **même rect** ; disposition exportée puis ré-importée ; snapshot supprimée et **fichier exporté nettoyé** (rien n'est laissé dans `%APPDATA%`).
- Non-régression (session 20) : `ctest` **9/9** ; `--fence-demo` **8/8** ; `--persist-demo` **26/26** ; `--nav-demo` **22/22** headless / **29/29** `--host` ; `--rules-demo` **28/28** ; `--portal-demo` **29/29** / **31/31** `--host`.

### Limites assumées

- **Pas d'interface** : les snapshots se pilotent par `SnapshotManager` ou la sonde. L'écran (liste, restauration, export guidé) viendra avec la page de réglages du mode produit (**T-015**).
- La restauration n'est **pas** branchée sur un mode produit — il n'existe pas encore (T-015). La sonde exerce exactement la séquence prévue, mais c'est elle qui l'orchestre.
- Les deux critères de sortie de la Phase 5 — **passe de tests manuels multi-écrans** et **capture avant/après restauration de snapshot** — restent des validations humaines (bureau libre + yeux, décision 13 de `STATE.md`).

## 15. Mode produit (T-038, session 21)

Jusqu'ici, `starfish.exe` sans argument affichait « skeleton starting » et sortait : le projet livrait un **moteur joignable par des sondes**, pas une application (écart n°1 de l'audit de la session 19). Le mode produit est la **séquence** qui manquait, pas un moteur de plus.

### Ce que le produit fait, et dans quel ordre

| Étape | Ce qui se passe | Pourquoi cet ordre |
|---|---|---|
| 1. Document | `LayoutStore::load()` puis `FenceContainer::applyLayout()` | **avant la première frame** : ce que l'utilisateur voit est ce que le document dit, jamais un bureau vide qui se remplit après coup |
| 2. Premier lancement | une zone **vide** est matérialisée | sinon lancer l'application ne montre rien du tout. Elle reste **vide exprès** (voir Limites) |
| 3. Câblage | auto-save sur les signaux de cycle de vie, watcher, hotkeys, tray, réglages | **après** le chargement, pour qu'un chargement ne ressemble jamais à une modification — et n'arme donc pas d'écriture |
| 4. Sortie | arrêter le watcher → désenregistrer les hotkeys → **écrire le document** → détruire les zones → **restaurer les icônes** | écrire **avant** de détruire les zones (ce sont elles qu'on décrit) ; restaurer les icônes **en dernier**, sinon elles réapparaissent sous les zones pendant un instant |

`--seconds N` referme l'application au bout de N secondes : c'est la seule façon d'exercer une sortie **propre** sans main sur le tray, et l'essai le plus simple pour un humain.

### Les trois pièces

| Pièce | Rôle | Ne fait jamais |
|---|---|---|
| `app/product_app.{hpp,cpp}` (`ProductApp`) | la séquence ci-dessus | décider à la place des pièces |
| `ui/tray_icon.{hpp,cpp}` (`TrayIcon`) | l'icône de notification et son menu (F-034) | appliquer quoi que ce soit : il **émet** |
| `ui/settings_dialog.{hpp,cpp}` (`SettingsDialog`) | la fenêtre de réglages (F-034) | posséder les zones : elle **lit** le conteneur et **demande** |

Les deux interfaces suivent la règle de la décision 15 — **le widget signale, le propriétaire décide** — exactement comme `FenceView` et `FenceContainer`.

`ProductApp::Options` porte `layoutPath`, `hostOnDesktop`, `tray`, `settingsWindow`, `showSettingsOnStart`, `hotkeys`, `watchDesktop`, `starterFence`. Chacune peut être éteinte, ce qui rend **la même séquence** exécutable sans bureau — c'est ce que fait `--product-probe`.

### ⚠️ Le blocage réel : la capture souris

`DesktopSurface::forwardMouse` routait correctement la souris, et pourtant **une zone hébergée était impossible à déplacer** : rien ne prenait la **capture**. Windows cesse d'envoyer `WM_MOUSEMOVE` à une fenêtre dès que le curseur sort de son rectangle, donc un déplacement de barre de titre mourait au premier pixel franchi.

Aucune sonde ne pouvait le voir : elles envoient leurs `QMouseEvent` **directement au widget**, ce qui contourne exactement le mécanisme en cause. C'est l'écart n°6 de l'audit, et il ne s'est révélé qu'en écrivant le mode produit — c'est-à-dire en se demandant ce qu'un utilisateur ferait vraiment.

Correction, entièrement dans `DesktopSurface` :

1. `SetCapture` sur `WM_LBUTTONDOWN` / `WM_RBUTTONDOWN` ;
2. `ReleaseCapture` sur le button-up correspondant, **après** avoir transmis l'événement (sinon le `WM_CAPTURECHANGED` déclenché par le relâchement ressemble à un geste perdu) ;
3. `WM_CAPTURECHANGED` → `releaseStuckGesture()`, qui poste les relâchements manquants au dernier endroit connu. Sans lui, une zone à demi tirée continue de suivre le curseur sans bouton enfoncé.

### Masquage des icônes natives (ADR-3)

`setDesktopIconsHidden()` / `desktopIconsHidden()` sont désormais **statiques sur `DesktopSurface`** — la couche d'icônes appartient au bureau, pas à une surface — au lieu de vivre sur `DesktopHost`, qui n'est plus le chemin de rendu (écart n°5 de l'audit).

Deux règles :

1. **Non masquées par défaut.** La bascule est dans le tray et dans les réglages : c'est un choix de l'utilisateur, pas un effet de bord du lancement.
2. **On ne restaure que ce qu'on a masqué.** La première version appelait `ShowWindow(SW_SHOW)` au shutdown sans condition : un utilisateur qui avait caché ses icônes lui-même les aurait vues réapparaître. `ProductApp` tient `m_iconsHidden` et ne restaure que dans ce cas ; si l'appel échoue, la case reste **décochée** plutôt que de mentir sur l'état du bureau.
3. **Depuis T-046, masquer laisse une trace** : `setDesktopIconsHidden()` écrit un **marqueur** (`core::IconHidingMarker`) **avant** de masquer et ne le retire qu'après avoir rendu la couche ; un marqueur qui ne peut pas être écrit **refuse** le masquage. C'est ce qui permet la restauration inconditionnelle au démarrage suivant après une fin brutale — §18.

### Langue

Les libellés passent par `tr()` avec des chaînes sources **en anglais** : l'i18n (F-036 / T-014) devient un fichier de traduction au lieu d'une chasse. L'interface est donc **en anglais** pour l'instant — assumé et noté.

### Une seule instance à la fois (session 26)

Deux copies de l'application se disputent exactement les deux choses qui font paraître le produit cassé : le **document** (dernier écrivain gagne, donc des zones disparaissent) et l'**état de la couche d'icônes natives** (chaque copie restaure des icônes que l'autre croyait masquées). Mesuré sur le vrai bureau : l'utilisateur en avait lancé **deux** à une minute d'intervalle, et la confusion se lit comme un bug des zones.

`main()` prend donc un `CreateMutexW(L"Local\\StarFish.Desktop")` au démarrage du **mode produit** : si le mutex existe déjà, la seconde copie journalise et sort `0` sans rien toucher. Deux choix explicites :

- **Le garde est dans le mode produit seulement.** Une application ouverte ne doit pas empêcher les **sondes** de tourner — c'est précisément ce qui les rend utiles pour diagnostiquer une application vivante.
- **Pas de boîte de dialogue.** Un utilitaire de zone de notification qui ne fait rien au second lancement est le comportement attendu ; une boîte modale à chaque double-clic serait pire que le mal, et elle rendrait le garde non testable sans intervention humaine. Le message va dans le journal, qui est capturé quand la sortie est redirigée.

Le descripteur n'est **pas** fermé : c'est lui qui tient le verrou, et le système le libère à la fin du processus.

### Vérification

- `starfish --product-probe` — **16/16 PASS**, `rc=0`, rapport `build/product_probe.txt`, **sans bureau, sans tray, sans hotkey**, contre un document jetable dans `build/product_probe/` **supprimé en fin de sonde**. Mesuré : démarrage, matérialisation de la première zone, existence de la fenêtre de réglages, renommage, enroulement, bascule des icônes (mémorisée **et** réversible), placement d'une nouvelle zone **sans recouvrement** (x = 423, à droite de la précédente), suppression, écriture du document, contenu relu, **restauration au second démarrage avec le titre conservé**, nettoyage.
- **Mode produit réel** (`--product --layout build/product_live/layout.json --seconds 6`, mesuré) : surface hébergée — `layered: ex 0x80000 (expected 0x80000); alpha call 1 (err 0)` puis `surface 0xd0d7a hosted at 80,80 320x240 (layered, alpha 255, placed 1)` ; watcher sur les **2 dossiers Bureau** (Desktop + Public Desktop, 86 entrées connues) ; hotkeys `Alt+Win+Space` et `Shift+Win+H` **enregistrés** ; `saved 1 fence(s)` à la sortie ; document relu conforme ; `native desktop icons restored` **absent** (les icônes n'avaient pas été masquées).
- Non-régression (session 21) : `ctest` **9/9** ; `--fence-demo` **8/8** ; `--persist-demo` **26/26** ; `--nav-demo` **22/22** headless / **29/29** `--host` ; `--rules-demo` **28/28** ; `--portal-demo` **29/29** / **31/31** `--host` ; `--snapshot-demo` **11/11**.

### Limites assumées

- **Pas de création de zone par geste** (F-001) : dessiner une zone sur le bureau vide demande un **hook souris global** — la création passe par le tray et les réglages (**T-028**, comme le geste de quick hide).
- **Le premier lancement ne remplit pas la zone** : y verser le bureau revendiquerait des fichiers que l'utilisateur n'a pas mis là, et il n'existe **aucune représentation d'une icône hors zone** (F-011/F-007, arbitrage non tranché — audit point 2). Les icônes entrent par glisser-déposer depuis l'Explorer.
- Hotkeys **en dur** (T-030) ; **pas de libellés français** (T-014 / F-036) ; pas de lancement au démarrage (F-035) ; la fenêtre de réglages ne configure rien qui survive à la session ; les placements sont en **pixels physiques** de l'écran virtuel (T-014a : multi-écran fait, mais **le contrôle visuel à deux écrans reste à faire**), et l'interface reste **en anglais** — l'i18n est T-014b / F-036.
- **Un essai à la main reste nécessaire** : la souris sur une zone hébergée, l'icône de notification et la fenêtre de réglages ne se valident pas depuis l'agent (bureau libre + yeux, décision 13).

## 16. Multi-écran et DPI (T-014a / F-032)

**Le problème.** Tout le modèle était mono-écran par construction : des bornes réduites à un `QSize`, une aimantation contre les bords de ce seul rectangle, et des rects documentés comme « coordonnées client de `Progman` == moniteur principal ». Sur deux écrans, une zone se faisait ramener sur le primaire, l'aimantation ignorait le bord **interne** des moniteurs, et une zone enregistrée sur un écran débranché restait hors d'atteinte. T-014a corrige ces trois choses.

**Deux espaces de coordonnées, une seule frontière.** Les rects (`FenceWindow::rect`, le document, `SetWindowPos`, `UpdateLayeredWindow`, les messages souris) sont en **pixels physiques**, en coordonnées d'écran virtuel Windows — c'est exactement l'espace client de `Progman`. Le primaire est à (0,0) ; un moniteur placé à gauche ou au-dessus a des coordonnées **négatives**. Qt, lui, raisonne en **logique** ; `devicePixelRatio` est le pont, franchi à deux endroits seulement :

| Frontière | Qui convertit | Comment |
|---|---|---|
| Lire les écrans | `ProductApp::screenLayout()` | `QScreen::geometry()` (logique) × DPR = rect physique ; l'origine logique de Qt est le coin haut-gauche du primaire, donc le primaire reste à (0,0) |
| Rendre une zone | `FenceContainer::renderFence()` | image à la taille **physique** du rect visible, `setDevicePixelRatio(dpr)` ; le widget est dimensionné en logique et Qt peint dedans |
| Recevoir la souris | `DesktopSurface::devicePixelRatio()` | coordonnées client physiques ÷ DPR = coordonnées du widget |

À 100 % les deux espaces coïncident : c'est pourquoi ces conversions n'ont pas changé un pixel du comportement v1 — **mesuré** (les sept sondes existantes rendent leurs compteurs d'origine, à l'unité près).

**Les règles.**

1. **Une zone appartient à l'écran de son centre, et y est entièrement clampée** (`ScreenLayout::clampRectFor`). Jamais dans la **zone morte** — les aires du *bounding box* qu'aucun moniteur ne dessine, dès que deux écrans ont des tailles différentes. Une zone là serait invisible et impossible à rattraper à la souris : c'est le raisonnement de T-007 (« une zone traînée hors écran est irrécupérable »), appliqué au multi-écran.
2. **L'aimantation cible chaque écran** (`ScreenLayout::frames()`), donc le bord interne du moniteur voisin. L'ancienne règle — un seul rectangle englobant — ne pouvait pas voir ce bord : deux cas de `tests/test_screen_layout.cpp` le **prouvent** (ils resteraient rouges si l'on y revenait).
3. **Restauration** (`ScreenLayout::restoreRect`) : rect encore sur un écran → inchangé ; sinon l'écran **enregistré** par la zone s'il existe (le moniteur a bougé, pas la zone) ; sinon le plus proche ; sinon le primaire. `FenceWindow::monitor` est **dérivé du rect**, jamais l'inverse : le document ne peut pas nommer un moniteur où la zone ne se trouve pas.
4. **Un changement d'écrans revalide** : `screenAdded` / `screenRemoved` / `primaryScreenChanged` → `ProductApp::refreshScreens()` → `FenceContainer::setScreens()` → `revalidateScreens()`. Seules les zones réellement déplacées émettent `fenceRectChanged()`, donc le document n'est réécrit que si quelque chose a bougé — même discipline que le debounce de T-008 et le rafraîchissement des portails.
5. **Le menu contextuel Shell reçoit une position écran physique**, ce qu'attend `TrackPopupMenuEx`, alors que Qt utilise du logique pour `globalPosition()`. C'est délibéré (et documenté dans `DesktopSurface::forwardMouse`) : **à vérifier visuellement sur un écran à 150 %**.

**Mesuré, et non mesuré.**

- Mesuré : build MinGW/Qt 6.8.3 vert, 0 warning ; `ctest` **10/10** (`ui_screen_layout`, 20 cas) ; les sondes aux compteurs d'origine ; l'arrangement réel de la machine de dev, **lu par la sonde** : **1 moniteur** 2560×1440, `dpr 1`, **DPI 96 (100 %)**.
- **Mesuré (session 24) — sonde `--screen-demo`, 19/19 PASS, `rc=0`** : arrangement réel ; **contexte d'awareness DPI réellement obtenu = `DPI_AWARENESS_PER_MONITOR_AWARE (2)`** (lu, pas supposé : le manifeste ne déclare rien, c'est donc ce que Windows a accordé après la demande de Qt — **pas** `PER_MONITOR_AWARE_V2`, contrairement à ce qu'on prêterait à Qt 6 ; ne pas s'en servir comme d'un acquis, le relire sur toute machine où le DPI compte) ; arrangements **synthétiques** exercés à travers un **vrai** `FenceContainer` : moniteur à gauche (coordonnées négatives), **trou** entre deux moniteurs de tailles différentes (prouvé : ce point n'appartient à aucun écran, sans quoi le test de clamp serait vacun), clamp hors de ce trou, aimantation sur le bord interne, **écran débranché → zone ramenée** (1 seul signal, donc pas de réécriture), **idempotence** (rejouer le même arrangement n'émet rien), et restauration d'un document nommant un moniteur absent (revient sur un écran qui existe) **puis** présent (revient inchangé sur le bon moniteur, au pixel).
- **Mesuré (session 24) — la mise à l'échelle de la peinture, elle était supposée** : dans la même sonde, une bordure de **1,0 pixel logique** est comptée en couverture sur le canal bleu (remplissage `#1F2937` → 55, bordure `#3B82F6` → 246) et couvre **1,00 / 1,50 / 2,00 pixels physiques** aux ratios `dpr` 1 / 1,5 / 2. **Prémisse prouvée** : en neutralisant la conversion dans `renderFence` (test temporaire, revert), la mesure tombe à **1,00** (dpr 1,5) et **0,00** (dpr 2 — la colonne échantillonnée tombe alors hors du widget non mis à l'échelle) : deux `FAIL`. La sonde distingue donc réellement une peinture mise à l'échelle d'une peinture qui l'ignore, ce que rien d'autre dans la chaîne ne faisait.
- **Non mesuré — à ne pas présenter comme acquis** : le multi-écran **réel** et le DPI **mixte** exigent deux écrans (la machine de dev n'en a qu'un), et le **placement du menu contextuel Shell sur un écran à 150 %** reste à vérifier à l'œil : ce sont des validations **humaines**, bloquées par le matériel.
- Le manifeste reste **sans** entrée `dpiAware` : la sonde mesure `PER_MONITOR_AWARE (2)`, la mesure de la session 9 avait écarté le DPI comme cause des refus de `WS_EX_LAYERED` (ADR-4, finding 4), et Qt 6 ne s'en trouve pas empêché. Ne pas l'ajouter sans re-mesurer — c'est précisément ce que la sonde permet désormais.



## 17. Édition manuelle : ce qui marche, et le mur du z-order (session 25)

**Le contexte.** L'utilisateur a précisé ce qu'il veut : créer des zones quand il veut, y mettre les icônes qu'il veut, organiser **à la main**, et qu'une zone déplacée **emporte ses icônes** — l'auto-organisation (F-017/F-018) ne l'intéresse pas. La demande a été confrontée au code avant toute promesse, et deux vérifications ont été ajoutées à `--fence-demo` (elles tournent headless **et** en `--host`).

### Ce qui est vrai, et maintenant prouvé

**« Déplacer une zone emporte ses icônes » : vrai par construction, et mesuré.** `renderFence` produit une image **de la taille du rectangle de la zone** et peint le widget dedans : le contenu n'a aucune coordonnée écran. Rendre la même zone avant et après un déplacement (`1200,900` après avoir été à `80,600`) donne des images **identiques octet pour octet** — et la vérification exige **aussi** que le rectangle ait changé, sinon elle passerait sur une zone qui n'a pas bougé.

**Les tuiles sont bien celles de la zone** : la sonde hôte la zone de démo avec les **85 entrées réelles** du bureau, et chaque tuile pointe sur le fichier lui-même.

### Le mur : sous la couche d'icônes, une zone ne reçoit pas la souris

La position **normale** d'une surface (ADR-4) est **juste sous `SHELLDLL_DefView`**. La sonde énumère donc les fenêtres du bureau qui couvrent le centre de la zone, **les plus hautes d'abord**, en ignorant les fenêtres masquées (une fenêtre masquée ne prend aucun clic — sans ce filtre, la mesure ne distingue pas « icônes masquées » de « rien n'a changé ») :

| État | Fenêtre visible la plus haute au centre de la zone |
|---|---|
| icônes natives **visibles** | `SHELLDLL_DefView` (2560×1440, layered) |
| | `SysListView32` (2560×1440, **plain**) |
| | `StarFishDesktopSurface` (360×260, layered) ← **nous, en troisième** |
| icônes natives **masquées** | `StarFishDesktopSurface` ← **nous, en premier** |

Conséquence mesurée : avec les icônes natives visibles, **le clic va à la liste du bureau**, pas à la zone. Masquer la couche d'icônes n'est donc **pas cosmétique** : c'est la condition pour qu'une zone soit utilisable. La vérification est **falsifiable** — elle exige les deux moitiés (visible → pas nous ; masquée → nous), donc elle resterait rouge si la couche d'icônes cessait d'être au-dessus, ou si nous n'étions pas dessous. La sonde **restaure les icônes immédiatement** après la mesure (« restore only what we hid », décision 22).

Cette mesure est un **proxy** : `WindowFromPoint` et la pile de z-order disent quelle fenêtre serait choisie, pas ce qu'un curseur réel fait (le bureau de la machine de dev est recouvert — la mesure a d'ailleurs renvoyé `Chrome_WidgetWin_1` au premier essai, ce qui ne dit rien du bureau). La validation au vrai curseur reste humaine (T-021).

### Les deux moitiés de la demande, et la décision

- **Mettre des icônes dans une zone : oui**, et ça marche déjà — déposer un fichier depuis l'Explorateur arrive par le `IDropTarget` OLE (`CF_HDROP`, T-006-3), et une règle (F-017) n'est pas nécessaire pour ranger à la main.
- **Ranger les icônes *déjà* sur le bureau dans une zone : non**, et c'est l'écart n°2 de l'audit : il n'existe **aucune représentation d'une icône hors zone**. D'où la décision **ADR-9** : StarFish prend la place de la couche d'icônes et dessine lui-même toutes les icônes du bureau, celles des zones **et** celles qui n'y sont pas (T-039), les secondes étant **calculées** (toutes les entrées − celles qu'une zone détient) et donc **jamais stockées**. Trois comportements tombent alors d'eux-mêmes : F-007 (supprimer une zone **rend** ses icônes au bureau), F-011 (sortir une icône d'une zone), et — corollaire qui **économise T-028** — les gestes sur le bureau vide (F-001, F-022) arrivent directement à notre surface de fond, sans hook souris global.

### Vérification (mesurée, session 25)

- `--fence-demo` : **9/9** headless ; `--fence-demo --host` : **10/10**, dont `moving a fence carries its tiles` (prémisse : le rectangle a changé) et `the fence is reachable only once the native icons are hidden` (les deux moitiés).
- Non-régression : `ctest` **10/10** ; `--persist-demo` 26/26, `--nav-demo` 22/22, `--rules-demo` 28/28, `--portal-demo` 29/29, `--snapshot-demo` 11/11, `--product-probe` 16/16, `--screen-demo` 19/19.
- La sonde hôte la zone de démo avec les entrées réelles du bureau et **laisse les icônes de l'utilisateur dans l'état où elle les a trouvées** (rapport : `native icons restored: yes`).

### Limites assumées

- **Aucune validation au vrai curseur** : la pile de z-order est un modèle de l'atteignabilité, pas un clic. T-021 reste ouverte, et c'est le critère qui manque pour dire « on peut bouger une zone à la souris ».
- **L'option B** (zones au-dessus de la couche native, sans surface de fond) est documentée et **écartée** par la duplication d'affichage qu'elle crée — voir ADR-9.
- **ADR-9 est ✅ tranchée** (option A, confirmée par l'utilisateur en session 28 — voir sa section dédiée, §3). Le **filet** qui rend l'option A sûre (T-046) est décrit en **§18**.

## 18. Le filet de sécurité : un marqueur, et une restauration inconditionnelle (T-046, session 29)

**Le problème.** ADR-9 (option A) fait masquer la couche d'icônes natives (`SHELLDLL_DefView`) pendant que StarFish tourne. Ce masquage est un changement **de toute la session** : le seul processus qui l'aurait défait est celui qui l'a fait. Une **fin brutale** (plantage, `kill`, coupure) laissait donc le bureau **sans aucune icône**, sans rien à l'écran pour l'expliquer. La règle actée en session 28 : *on ne construit pas une dépendance sans son filet* — et le filet se fait **avant** la surface de fond de T-039.

### Un marqueur sur disque, et deux ordres qui comptent

`core::IconHidingMarker` (`src/core/icon_hiding_marker.{hpp,cpp}`, Qt Core seul, testé) possède une note écrite dans `%APPDATA%/StarFish/icons-hidden.marker` — le même dossier que le document, parce que c'est le même état produit. Elle ne dit pas *quoi* réparer (il n'y a rien à décrire : rendre la couche est un acte unique), elle dit **qu'il y a quelque chose à réparer**.

Deux propriétés portent tout le reste :

1. **C'est la *présence* du fichier qui décide, jamais son contenu.** Un marqueur illisible (tronqué par un disque plein, modifié à la main) compte quand même : « la couche a-t-elle été masquée ? » ne doit pas dépendre de la relecture de la note. Testé (`presenceIsDecidedByTheFileNotByItsContent`).
2. **Les ordres sont choisis pour que le pire cas reste réparable.**
   - **À l'aller** : le marqueur est écrit **avant** `ShowWindow(SW_HIDE)`. Un plantage entre les deux laisse un marqueur **sans** masquage — la « réparation » qui suivra sera un simple `SW_SHOW` sur une couche déjà visible, sans effet et sans dégât. L'inverse (masquer puis écrire) laisserait une fenêtre où le bureau est sans icônes **et** sans note : le seul état que rien ne peut réparer. **Si le marqueur ne peut pas être écrit, le masquage est refusé** (`setDesktopIconsHidden(true)` retourne `false`, journalisé en `critical`) — et la case du tray reste décochée plutôt que de mentir.
   - **Au retour** : `ShowWindow(SW_SHOW)` **puis** suppression du marqueur. Un plantage entre les deux laisse un marqueur sans masquage : la réparation suivante est encore sans effet. L'inverse (supprimer puis montrer) recréerait l'état irréparable.

Le marqueur n'est **pas** dans le document : la couche d'icônes appartient à la session, pas à une disposition — deux documents différents partagent le même bureau. `DesktopHost::setIconsHidden()` (chemin hérité, encore utilisé par les sondes `--host-demo` / `--render-probe`) **délègue** désormais à `DesktopSurface::setDesktopIconsHidden()` : le masquage et sa trace ne peuvent pas diverger.

### La restauration au démarrage : inconditionnelle, et la première chose faite

`ProductApp::start()` appelle `repairIconHidingFromPreviousRun()` **avant tout le reste** — avant le chargement du document, avant la première zone, et **quel que soit le mode** (une sonde headless comprise) : les icônes absentes sont un fait de la machine, pas de ce document ni de ces options. Si le marqueur est là, la couche est rendue, le marqueur disparaît ; si la restauration **échoue** (pas d'Explorer à cet instant), le marqueur est **conservé** — le lancement suivant, ou la sortie de secours, doit encore pouvoir réparer.

C'est la restauration *inconditionnelle* de la session 28 : elle ne dépend **d'aucun** état interne du processus mort, seulement du fichier. Et elle est sûre parce que le marqueur ne peut exister que si un StarFish a **réellement** masqué la couche (règle 2 ci-dessus).

### La sortie de secours : « Restore my icons »

Le tray gagne une entrée **« Restore my icons »** (`TrayIcon::restoreIconsRequested` → `ProductApp::restoreDesktopIcons()`) — une **commande**, pas une bascule. La différence est le point : la bascule ordinaire (`setDesktopIconsHidden(false)`) n'agit **que** si l'état demandé diffère de l'état **mémorisé** (`m_iconsHidden`), donc elle ne peut rien faire dans le cas précis où l'on a besoin d'elle — la couche masquée par un autre run pendant que celui-ci ne se souvient de rien (mesuré : la vérification `the ordinary toggle cannot reach a state it does not know` de la sonde vit dans cet état). La sortie de secours demande au bureau de montrer les icônes et **croit sa réponse**.

Déviation documentée : c'est le **seul** endroit qui touche la couche réelle même avec `--no-host` (mode fenêtré, où le produit ne gère pas la couche). C'est voulu — « rendez-moi mes icônes » est une demande de l'utilisateur au bureau, pas une pièce de notre comptabilité.

### Vérification : la fin brutale est simulée, pour de vrai

`starfish --crash-demo` : **20/20 PASS**, `rc=0`, rapport `build/crash_demo.txt`. La sonde **masque les vraies icônes du bureau** un instant, puis **tue le processus qui les a masquées** :

1. **Première moitié** — un processus enfant (`starfish --crash-victim`, la moitié cachée de la sonde) masque les icônes **par le chemin du produit** (`ProductApp::setDesktopIconsHidden`, le même code que la bascule du tray) et attend d'être tué. Le parent **lit de l'extérieur** les deux faits : le marqueur sur disque et `SHELLDLL_DefView` masquée dans la fenêtre d'Explorer. Rien n'est un auto-rapport.
2. **La fin brutale** — `QProcess::kill()` (`TerminateProcess`, aucun teardown ne s'exécute). Deux vérifications **prouvent que la fin était vraiment brutale** : le marqueur est encore là et les icônes encore masquées — si un atelier quelconque avait tourné, les deux seraient revenus et la suite ne mesurerait rien.
3. **Le lancement suivant** — un `ProductApp::start()` neuf, **sans mémoire du processus mort**, rend les icônes (état relu dans la fenêtre d'Explorer, pas dans notre comptabilité) et supprime le marqueur.
4. **La sortie de secours** — la sonde fabrique l'état pour lequel elle existe (couche masquée **derrière le dos** du produit, `m_iconsHidden == false`), **constate que la bascule ordinaire n'y peut rien** (prémisse — l'assertion passe quand les icônes restent masquées), puis `restoreDesktopIcons()` les rend quand même.
5. **Nettoyage** — la sonde ne rend que ce qui est à elle (une couche masquée **avec** marqueur), retire le marqueur, supprime son dossier, et vérifie qu'elle a laissé le bureau **comme elle l'a trouvé**.

**La prémisse est prouvée, pas affirmée** : en neutralisant la réparation (`repairIconHidingFromPreviousRun()` en `return;` — test temporaire, revert), `--crash-demo` tombe à `rc=1` avec **exactement deux `FAIL`** (`the next launch gives the icons back` et `...and removes the marker`), le reste restant vert. La sonde distingue donc réellement « un lancement qui répare » d'un lancement qui ne fait rien — et le rapport de cette exécution ratée montre au passage la limite que la sortie de secours comble : la ligne d'état du produit disait `desktop icons visible` alors que la couche était masquée (la comptabilité suit *ses* actions, pas celles du run mort).

Messages de la sonde, pour mémoire : `--crash-victim` est aussi utilisable seul (« lance ça, tue-le, puis lance l'application et regarde »), et le journal du victime montre l'ordre réel (`icon-hiding marker written` **puis** `native desktop icons hidden`).

### Limites assumées

- **Planter *et* ne plus jamais relancer StarFish** laisse le bureau sans icônes : il faut alors relancer Explorer. C'est le risque résiduel assumé de la session 28, réduit ici au seul cas « on ne relance jamais ». La sortie de secours du tray n'existe que si StarFish tourne.
- **Un marqueur ne dit pas qui a masqué.** Si l'utilisateur avait masqué ses icônes lui-même **avant** que StarFish les masque, la réparation les rendra visibles : c'est la même imprécision assumée que la sortie propre depuis T-038 (« on ne restaure que ce qu'on a masqué » vaut pour *notre* action, pas pour l'état antérieur). Une note sur disque ne peut pas mieux savoir ; le contraire — rester sans icônes pour épargner une préférence — serait pire.
- L'ordre **write-ahead** (marqueur avant masquage) est **par construction**, pas mesurable de l'extérieur à la résolution d'une horloge (la fenêtre fait quelques microsecondes) : la sonde mesure ses **conséquences** (marqueur présent + couche masquée après la mort), pas la séquence. La séquence est fixée par une seule fonction et lue dans le journal du victime.


## 19. Le fond de bureau : StarFish dessine les icônes hors zone (T-039, session 30)

Le mur décrit en §17 se franchit en deux temps, et le premier (T-046, §18) n'existait que pour rendre le second réparable : **la couche d'icônes natives est masquée** pendant que StarFish tourne, et **StarFish dessine alors les icônes lui-même**.

### Ce que « hors zone » veut dire

```
   toutes les entrées du bureau
 − les entrées qu'une zone détient
 = ce que le fond dessine
```

Cet ensemble est **recalculé à chaque changement** (`BackdropLayout::freeItems`) et **stocké nulle part**. C'est la décision centrale, et elle porte trois conséquences qui n'ont pas eu à être écrites :

- **ADR-7 reste intact.** Le document ne décrit que des zones ; le schéma ne bouge pas d'un octet, et la sonde le **mesure** (deux rendus complets, document identique octet pour octet ; 3 chemins écrits quand le bureau en a 86).
- **F-007 et F-011 tombent tout seuls.** Sortir une icône d'une zone, ou supprimer la zone, c'est *ne plus la détenir* : elle réapparaît sur le fond. **Aucun code de restauration n'existe**, et il n'en faut pas (`--backdrop-demo` : 86 → 83 → 86).
- **Le fond ne peut pas mentir sur ce qu'il ne sait pas.** Il n'a aucune autorité, donc aucun état à désynchroniser.

### Où il se place, et pourquoi il n'a pas fallu remonter les zones

`DesktopSurface::Layer` remplace le booléen « au-dessus des icônes » par trois **rôles**, honorés par `place()` **et** par la garde `WM_WINDOWPOSCHANGING` (invariant du watchdog 2 s) :

| rôle | ancre | qui l'utilise |
|---|---|---|
| `BelowIconLayer` | juste sous `SHELLDLL_DefView` | les zones (ADR-4) |
| `AboveIconLayer` | `HWND_TOP` | Peek (F-021) |
| `Backdrop` | `HWND_BOTTOM` | le fond (T-039) |

`setRaisedAboveIcons()` **délègue** au rôle, donc **aucun appelant existant ne change** — et le z-order mesuré par `--nav-demo` (`surface [1]` vs `DefView [0]`) est **inchangé**. C'est le point qu'on pouvait craindre de devoir payer : « les zones au-dessus des icônes » (session 28) n'exige **pas** de remonter les zones, parce que la couche native est **masquée** et que le fond se met **sous** elles. Les zones restent là où ADR-4 les a mises.

### Le fond ne prend aucun geste (T-039 — état d'origine, voir §21)

`setClickThrough(true)` → `WM_NCHITTEST` répond `HTTRANSPARENT`, et la surface **n'enregistre aucun `IDropTarget`**. Le fond peint des icônes mais n'en possède encore aucun comportement (clic, sélection, lancement, menu contextuel, renommage, couper/coller = T-042) : **avaler un clic ou un drop qu'on ne peut pas honorer serait une régression silencieuse**, alors que laisser passer garde le bureau atteignable tel qu'il était. Deux prix sont assumés et tracés :

- **T-052** — déposer un fichier sur le **bureau vide** ne fait plus rien : c'est `SHELLDLL_DefView`, désormais masquée, qui recevait le drop. Le corriger veut dire réimplémenter le dépôt — et « déposer sur le bureau » **déplace** le fichier, donc c'est la famille T-031/T-036 (mutation du disque, opt-in explicite).
- **T-053** — le **Peek** (F-021) n'a plus d'effet visible : les zones sont déjà au-dessus de tout ce qui est dessiné sur le bureau. À retirer, ou à requalifier en « au-dessus des fenêtres d'application » (T-029).

### La règle qui garde le bureau utilisable

`DesktopBackdrop::attach()` suit la même discipline que le marqueur de T-046 — **si le geste sûr est impossible, on refuse le geste risqué** :

1. il **rend la frame d'abord** (une fenêtre layered sans contenu poussé est invisible) ;
2. il héberge sur le rôle `Backdrop`, en **per-pixel alpha** obligatoire — le fond couvre **tout l'écran virtuel**, et sans `UpdateLayeredWindow` il cacherait le papier peint ;
3. il **relit `perPixelAlpha()`** : si le push a échoué, `DesktopSurface` est déjà retombé sur le chemin opaque → **le fond est refusé** ;
4. `ProductApp` **ne masque alors pas** la couche native : le bureau garde ses vraies icônes.

Au retour, l'ordre est **inverse** : la couche native revient **d'abord**, le fond part **ensuite** — jamais de bureau sans icônes, pas même une frame.

### Le rythme

Le rendu est **coalescé à 120 ms** (`kRenderCoalesceMs`) et `setOccupiedPaths()` **sort tôt** si l'ensemble n'a pas changé. Un drag de zone émet ~60 `fenceRectChanged` par seconde, et chaque rendu téléverse l'écran virtuel entier (14 Mo à 2560×1440) : sans l'un ou l'autre, déplacer une zone serait un test de bande passante. Le fond suit le dossier Bureau comme un portail suit son dossier — scan au démarrage, rescan sur `itemsAdded` **et** sur `folderChanged` (un fichier qui *part* doit partir du fond aussi, et `itemsAdded` ne le dira jamais).

### La disposition est en colonnes, pas en lignes

Le bureau Windows remplit une **colonne** de haut en bas, puis la suivante. `IconLayout` — la grille d'une **zone** — remplit des **lignes**. Ce sont deux règles différentes, donc deux implémentations (`BackdropLayout::compute`), et un test qui échoue si on revient à l'autre : `cellsFillColumnsNotRows`. **Prémisse prouvée** : disposition forcée en lignes (patch temporaire, revert) → `rc=2`, **exactement deux `FAIL`**, les 12 autres cas verts.

La cellule du fond est aussi **plus large que l'icône** (72 px pour une icône de 48), ce qui est exactement le manque de la grille d'une zone (T-049 : `cellW = iconSize`, libellé élidé au milieu).

### Ce que la sonde mesure (`--backdrop-demo`)

Headless : 86 entrées / 86 visibles ; 86 libres sans zone, 83 avec une zone de 3 ; la comparaison **casse + séparateur** retire une icône de plus ; **deux rendus complets ne changent pas un octet du document** (65 octets identiques) ; le document porte 3 chemins quand le bureau en a 86 ; supprimer la zone rend les 86 ; 16 lignes puis colonne suivante à x=98 ; la frame est peinte sur une petite fraction de l'écran (**231 738 px** — 6 % à 2560×1440, 11 % à 1920×1080) et le compte **tombe** à 223 887 quand une zone réclame 3 icônes. En `--host` : `layered ex 0x80000`, **push per-pixel confirmé** (aucun repli opaque), couverture de l'écran virtuel, `HTTRANSPARENT`, **z-order `backdrop [1]` vs `DefView [0]`**, et « turning it off leaves no window behind ». Elle **ne masque jamais** les icônes natives. Depuis T-041 elle porte en plus les **16 vérifications des gestes du bureau vide** (§21) : **31/31** headless, **40/40** en `--host`.

> **La vérification de transparence ne dépend plus de la taille de l'écran** (session 31) : elle comparait à `< 10 %` de l'**écran**, ce qui la rendait rouge sur un bureau 1920×1080 alors que le nombre de pixels peints n'avait pas bougé d'un pixel (**le dénominateur avait changé, pas la frame**). Elle se mesure maintenant contre le **budget de pixels des cellules** (507 744 pour 86 cellules) : elle tient à n'importe quelle résolution, et elle ne peut plus être satisfaite par un grand écran. Même famille : `--fence-demo` dérive désormais son rect cible de `desktopBounds()` au lieu de l'écrire en dur.

### T-017 est levé au passage

Le chemin per-pixel était « codé, jamais validé visuellement » depuis la session 9 (§8). Le fond l'exerce sur une fenêtre **plein écran** enfant de `Progman` — plus grand cas de figure que ce que la session 9 avait mesuré — et la sonde **mesure** que `UpdateLayeredWindow` a réussi. La conclusion de la session 9 est donc confirmée à l'exécution, et la validation à l'œil n'est plus qu'un confort.

### Limites assumées

- **Aucune interaction** sur les icônes du fond (T-042) : c'est le prix de l'ADR-3. Le **click-through** est ce qui l'annonçait honnêtement ; depuis T-041 le fond **prend** la souris, mais uniquement pour les gestes du **bureau vide** qu'il sait honorer (drag = créer une zone, double-clic = quick hide, §21) — un clic sur une de ses icônes ne fait toujours rien, et c'est T-042.
- **Les icônes libres sont ré-agencées en flux** : réclamer une icône décale les suivantes, au lieu de laisser un trou comme le fait le bureau Windows. Lire les vraies positions demanderait `LVM_GETITEMPOSITION` sur la liste native **avant** de la masquer, et ne couvrirait pas les fichiers ajoutés ensuite.
- **Une seule échelle pour tout l'écran virtuel** (le `dpr` du moniteur primaire) : sur un poste à **DPI mixte**, les icônes du fond sont à la taille du primaire. Même famille que T-014a : validation humaine à deux écrans.
- **La validation à l'œil reste humaine** : la sonde compte des pixels, elle ne juge pas une impression (position, lisibilité, scintillement).

## 20. Sortir une icône d'une zone (F-011 / T-040, session 31)

Le §19 avait laissé la conclusion la plus utile à exploiter : *ce qu'une zone ne détient pas est sur le bureau*. **F-007** (supprimer une zone **rend** ses icônes) n'était donc pas à écrire — la sonde du fond le mesurait déjà (86 → 83 → 86). Ce qui manquait était le **geste** de **F-011**, et une chose que personne n'avait vue : **une zone ne se redessinait jamais quand son contenu changeait**.

### Le geste

Tirer une tuile **hors** de la zone et lâcher là : l'icône retourne au bureau. La frontière entre les deux gestes est une seule expression, `rect().contains(release)`, et elle est vérifiée **des deux côtés** — un lâcher dedans reste un réordonnancement (`a tile released inside the fence still reorders`), un lâcher dehors rend la tuile (`a tile released outside the fence goes back to the desktop`).

C'est le geste de Fences, il ne coûte **aucun hook global** (il démarre dans notre fenêtre), et il ne touche à rien :

```
   la zone cesse de lister la tuile
 → l'icône sort de l'ensemble « occupé »
 → le fond (T-039) la dessine
```

Aucun code de restauration, aucune écriture de plus dans le document (une entrée de moins), **aucun fichier déplacé, supprimé ni masqué** — la règle d'or d'`AGENTS.md` tient par construction.

### Qui décide quoi

La règle de la session 13 (« le widget signale, le conteneur décide ») reste tenue :

- **`FenceView::itemReleaseRequested(paths)`** — l'intention. Le widget ne sait pas si la tuile est à lui.
- **`FenceContainer::releaseItems(id, paths)`** — la décision. Une tuile qu'un **portail** fournit est **refusée** et journalisée : elle appartient au **dossier**, et le prochain rafraîchissement la remettrait aussitôt (T-011). Le refus est vérifié dans `--portal-demo` (`a folder's own icon is not the fence's to give back`).

L'indice visuel est délibérément **autre chose** que le repère d'insertion : bordure pointillée accentuée + « release to put it back on the desktop » (`paintReleaseHint`). L'un dit « la grille prend », l'autre « le bureau prend » — et l'état est recalculé à **chaque** mouvement, donc revenir dans la zone redésarme le retour au bureau.

### ⚠️ Ce que la session a trouvé en chemin : une zone ne se redessinait pas

`DesktopSurface` blitte une **image** produite hors écran. Le `paintEvent` du widget ne va **jamais** au bureau, et **rien** ne connectait `FenceView::contentChanged` à `renderFence` : un fichier déposé, un rafraîchissement de portail ou une icône relâchée n'existaient donc que **dans le modèle**, jusqu'au prochain changement de géométrie (qui, lui, rendait). Corrigé par une connexion dans `addFence()`, et les deux `renderFence()` devenus redondants (`setFenceStyle`, `setFenceSort`) ont disparu : **un seul chemin mène le contenu à l'écran**.

La vérification `the fence was redrawn without the released tile` compare la frame **avant/après** la sortie de la tuile — sans nommer aucune tuile, donc agnostique des couleurs. **Prémisse prouvée** : connexion neutralisée → `rc=1`, **exactement 1 `FAIL`**.

### Ce que la sonde mesure (`--fence-demo`, `--portal-demo`)

`--fence-demo` : **17/17** headless / **18/18** `--host` (11/12 avant). Les vérifications neuves, dans l'ordre où elles se tiennent :

1. `the drag starts on the first tile` — **la prémisse**. Les centres de cellules sont calculés avec les métriques de la grille **puis hit-testés** : un changement de métriques fait donc échouer cette vérification au lieu de laisser les suivantes tester le vide.
2. `a tile released outside the fence goes back to the desktop` — le point de lâcher **est** dehors (`outside=yes`), la zone passe de 3 à 2 tuiles, et la tuile relâchée n'y est plus.
3. `the released icon is drawn on the desktop again` — les icônes libres passent de **2 à 3 sur 5**, et la tuile est **dans** l'ensemble libre. C'est le cœur de F-011 : rien ne l'a restaurée, elle n'est simplement plus détenue.
4. `the document no longer holds the released icon` — deux entrées `file`, et la tuile n'y est pas.
5. `the fence was redrawn without the released tile` — la frame a changé.
6. `a tile released inside the fence still reorders` — la frontière du geste.

**Prémisse prouvée** : branche F-011 neutralisée (`if (false && …)`, rebuild, revert) → `rc=1`, **exactement 3 `FAIL`** (2, 3 et 4), les 15 autres verts — dont, et c'est le point, la vérification de frontière.

`--portal-demo` : **30/30** / **32/32** `--host`, dont le refus d'une tuile de portail.

### Une règle de méthode, apprise à cette occasion

**Une vérification ne doit pas dépendre de la taille de l'écran du développeur.** Deux vérifications de la session 30 étaient rouges sur le bureau d'aujourd'hui (**1920×1080**, lu par Qt *et* par WinForms — pas un artefact de harnais), alors que le code n'avait pas bougé : l'une écrivait un rect en dur dont le bas (1140) ne tient pas dans 1080, l'autre comparait la frame peinte à `< 10 %` de l'**écran** — or 231 738 px font 6 % à 2560×1440 et 11 % à 1920×1080. **Le dénominateur avait changé, pas la frame.** Vérifié sur la révision non modifiée (`git stash` + rebuild) : **exactement les mêmes deux `FAIL`**, donc pas une régression. Corrigé par un rect **dérivé** de `desktopBounds()` et un seuil **dérivé** du budget de pixels des cellules (507 744) — deux mesures qui tiennent à n'importe quelle résolution, et que la taille de l'écran ne peut plus satisfaire à la place du code.

### Reste humain

L'essai au **vrai curseur** (avec *Hide desktop icons* actif, tirer une tuile hors d'une zone) : la sonde prouve le câblage et le modèle, pas le ressenti du geste — même famille que T-043/T-045, où c'est l'utilisateur qui avait trouvé les deux bugs.

## 21. Les gestes du bureau vide : créer une zone, quick hide (F-001 / F-022, T-041, session 32)

`docs/produit.md` était explicite depuis le début, et ces deux lignes avaient l'air inatteignables :

| | |
|---|---|
| **F-001** | Créer une zone : **glisser sur le bureau vide**, ou menu contextuel |
| **F-022** | Quick hide : **double-clic sur le bureau vide** pour masquer/afficher |

Le bureau vide appartient à `SysListView32`, une fenêtre d'**un autre processus**. C'est ce qui avait fait naître **T-028** (« il faut un hook souris global `WH_MOUSE_LL` ») — et un hook global mal validé affecte tout le poste, ce que l'agent ne peut pas tester.

**T-039 a supprimé le problème au lieu de le résoudre.** Depuis que StarFish possède la couche d'icônes, le fond **est** le bureau : une fenêtre plein écran de nous, sous les zones. Le geste démarre donc **dans une de nos fenêtres**, il n'y a plus rien à intercepter au niveau du système, et **T-028 ne se justifie plus que pour un geste parti hors de nos fenêtres**.

### La machine à états est pure

`src/ui/backdrop_gesture.hpp` (header-only, aucun Win32, aucun widget, aucun type d'événement) : l'appelant traduit ses événements souris en `press` / `move` / `release` / `doubleClick` avec un point en **pixels d'écran virtuel** — l'espace des cellules du fond *et* des rects stockés (T-014) — dit si ce point est **sur une des icônes que le fond dessine**, et reçoit une action : `None`, `Drawing`, `FenceDrawn(rect)`, `Cancelled`, `ToggleVisibility`. Les règles se testent donc sans bureau, et `DesktopBackdrop` ne fait plus que de la plomberie.

### Ce qui ne doit RIEN créer

C'est là que se joue la fonctionnalité. Sur un bureau, une création non demandée est le bug coûteux, donc les refus sont la moitié du travail — et chacun est vérifié :

| situation | pourquoi elle ne crée rien |
|---|---|
| **clic** (press + release sans mouvement) | l'utilisateur a tapé le bureau, pas dessiné |
| **mouvement sous le seuil** (6 px) | un clic un peu tremblant n'est pas un geste |
| **press sur une de nos icônes** | ces icônes appartiennent au bureau ; leur comportement est **T-042**, et une zone dessinée *par-dessus* une icône serait une zone non demandée |
| **rectangle < la taille minimale** (160×120) | un geste aussi court est une main qui glisse ; le **grossir** au minimum poserait une zone là où l'utilisateur n'en a pas dessiné |
| **double-clic sur une icône** | ce n'est pas notre geste (F-022 dit « bureau **vide** ») |

Le minimum n'est pas recopié : `ProductApp::wireBackdrop()` lit `container->minimumFenceSize()` et le passe au geste, donc **le geste et le conteneur ne peuvent pas diverger** sur ce qu'est une zone.

### Le clic-through devient une conséquence, pas un état

Le fond était click-through parce qu'il n'avait rien à répondre. Dès qu'il a deux gestes à honorer, la bonne formulation n'est plus « décoratif donc transparent » mais **« il prend la souris là où il a une réponse »** : `setGestureEnabled(true)` bascule `setClickThrough(false)` **et** `setInputTarget(this)`, `setGestureEnabled(false)` remet les deux. Les deux vont ensemble parce que ce sont les deux moitiés de la même décision.

### Le drop target se découple du click-through

⚠️ **C'est la correction la moins visible de la session, et elle protège T-052.** Jusqu'ici `attach()` enregistrait le `IDropTarget` OLE **si la surface n'était pas click-through** — vrai tant que les seules surfaces non-click-through étaient des zones, et que toute zone a un handler de drop. Le fond casse cette équivalence : il **prend** la souris et n'a **aucun** comportement de dépôt. En gardant l'ancienne condition, il se serait enregistré comme cible de drop et aurait **avalé un dépôt qu'il ne peut pas honorer** — exactement la régression silencieuse que T-052 documente.

La condition devient donc **« un handler de drop existe »**, ce qui est la vraie raison d'enregistrer une cible de drop : une surface qui n'a rien pour répondre à un dépôt n'enregistre rien, et le drag atteint ce qui est dessous. Vérifié en `--host` (`the backdrop still swallows no drop`).

### L'aperçu et le rythme

Le rectangle en cours de tracé est peint **dans la frame** (bordure pointillée + remplissage translucide, délibérément pas la couleur d'un panneau : c'est un aperçu de **geste**, pas encore une zone), dans les coordonnées d'écran virtuel comme les cellules. Le rendu reste **coalescé à 120 ms** : un drag émet ~60 mouvements par seconde et chaque rendu téléverse l'écran virtuel entier. Une frame de plus pendant un tracé est donc visible à ~8 Hz, ce qui suffit pour un aperçu.

### Le rectangle va du geste au document sans conversion

Le geste est mesuré **à l'écran** (`event->globalPosition()`, règle de T-043 — le fond ne bouge pas, mais la règle vaut pour tout geste) et le fond poste des positions écran **physiques** : exactement l'espace d'un rect stocké (T-014). `ProductApp::onFenceDrawn(rect)` crée donc la zone **là où elle a été dessinée** — aucun placement par défaut, aucune aimantation, aucune conversion à se tromper. Le conteneur borne ensuite comme pour tout autre rect (`restoreRect` + clamp dans l'écran du centre).

### Ce que la sonde mesure (`--backdrop-demo`, section « the empty desktop's gestures »)

**16 vérifications**, headless, avec de **vrais `QMouseEvent`** envoyés au fond — les mêmes événements que `DesktopSurface::forwardMouse` poste depuis le WndProc de la fenêtre hébergée. Mesuré : le fond prend les gestes quand on le lui demande ; le point « vide » et le point « sur une icône » sont **des prémisses** (si le bureau est trop étroit pour avoir du vide à côté des icônes, la sonde **s'arrête en le disant** au lieu de mesurer le vide) ; la bande élastique suit le drag (`365,40 300x200`) et **est peinte** (231 738 → **273 665** pixels opaques, +41 927) ; le rect demandé est **exactement** celui qui a été dessiné ; la zone existe **au pixel près** là où elle a été dessinée ; **dessiner une zone ne réclame aucun fichier** (0 chemin dans le document, 1 zone) ; puis les quatre refus, et les deux double-clics (le vrai bascule `fencesVisible`, celui sur une icône ne fait rien).

En `--host`, trois vérifications de plus : **sans les gestes le fond reste hors du chemin** (`HTTRANSPARENT`, aucune cible d'entrée), **les gestes prennent la souris** (le WndProc route vers le fond), et **il n'avale toujours aucun dépôt** (aucun handler, donc aucune cible OLE enregistrée).

**Prémisses prouvées** (patch temporaire, rebuild, revert) :

| neutralisation | résultat |
|---|---|
| `iconAt()` → `-1` (le test de collision des icônes) | `rc=1`, **1 `FAIL`** — la **prémisse** elle-même : la sonde refuse de mesurer au lieu de passer à vide. Unitaire : **2 `FAIL`** sur 20 cas. |
| le refus de taille neutralisé | `rc=1`, **2 `FAIL`** (le refus, puis une cascade). Unitaire : **1 `FAIL`**. |
| l'émission de `fenceDrawRequested` neutralisée | `rc=1`, **7 `FAIL`** — toute la chaîne F-001, plus trois cascades. |

### Unité

`tests/test_backdrop_gesture.cpp` — **20 cas**, 14ᵉ exécutable `ctest` (`ui_backdrop_gesture`). Les règles testées sont celles qui décident *si* un geste a eu lieu (seuil **mesuré à un pixel près** : 5 px ne dessine pas, 6 px dessine ; bornes de taille **inclusives** ; normalisation des quatre directions), le fait qu'un mouvement **sans** press ne fait rien, que le double-clic **désarme** un tracé en cours (la vraie séquence est press → release → double-clic → release), et que les cellules portent bien des coordonnées d'écran virtuel — y compris **négatives** (moniteur à gauche du primaire).

### Prix assumé, nouveau et tracé

Le **menu contextuel du bureau vide** est perdu pour la même raison que le dépôt (T-052) : « Nouveau > Document texte », « Actualiser », « Paramètres d'affichage » venaient de `SysListView32`/`DefView`, désormais masquée, et le fond ne les remplace pas → **T-054**. C'est aussi la moitié « ou menu contextuel » de F-001.

### Reste humain

Le geste au **vrai curseur** : dessiner une zone sur le bureau (avec *Hide desktop icons* actif) et double-cliquer pour masquer/afficher. La sonde prouve le câblage, le modèle et les refus ; elle ne juge pas si 6 px est le bon seuil au doigt.


## 22. Le bureau par défaut, et les gestes qui y mènent (T-055 / T-056, session 33)

La session 32 avait livré le geste de F-001 et l'avait mesuré. Le premier retour utilisateur a été : **« je n'arrive pas à créer une fence de la manière que tu décris »**. La session 33 a cherché pourquoi avant d'écrire quoi que ce soit — et la réponse n'était pas dans le geste.

### ⚠️ La cause : le produit ne prenait jamais le bureau

`setDesktopIconsHidden(true)` n'était appelé que par le **tray** et les sondes. Jamais au lancement. Conséquences en chaîne, toutes silencieuses :

| | |
|---|---|
| le **fond** n'était jamais hébergé | donc `DesktopBackdrop` n'existait pas, donc **aucun geste du bureau vide** ne pouvait se produire : la sonde mesurait une machine à états que le produit n'activait jamais |
| les zones restaient **sous** `SHELLDLL_DefView` | elles étaient **incliquables** — le mur du z-order de la session 25, que T-039 avait précisément pour but de lever |
| les icônes natives restaient visibles | et la **fenêtre de réglages s'ouvrait au lancement**, listant les zones et leurs dimensions — ce que l'utilisateur a décrit comme « un menu séparé… ce n'est que pour la démo » |

**ADR-3 dit depuis l'origine que StarFish dessine les icônes ; personne ne le branchait.** T-056 est ce branchement : `Options::claimDesktop` (défaut vrai), `setDesktopIconsHidden(true)` en fin de `start()` — donc après le câblage, et **par la séquence sûre qui existait déjà** (le fond est hébergé et son push per-pixel prouvé *avant* que la couche native ne soit retirée). `--no-claim-desktop` existe pour une démo qui ne doit pas toucher la couche.

**Mesuré de bout en bout** (`--seconds 4`, pendant puis après) : `backdrop hosted at 0,0 1920x1080, 77 free icon(s)` → `marker written` → `native desktop icons hidden` → `4 fence(s) | desktop icons hidden | desktop backdrop: 77 icon(s) on the desktop, 9 held by fences` ; à la sortie **marqueur disparu, document non réécrit**.

C'est la leçon de méthode de la session : **une fonctionnalité mesurée dans une sonde n'existe pas tant que le produit ne se met pas dans l'état où elle s'applique.** Les sondes savaient tout du geste et rien de ce qui le rendait possible.

### Le geste se répartit sur les deux boutons, et c'est une décision

| bouton | geste | pourquoi |
|---|---|---|
| **droit** | glisser → rectangle → **menu du bureau** (F-001) ; clic simple → le même menu (T-054) | c'est le geste de Fences, c'est ce que l'utilisateur a demandé, et il laisse le gauche libre |
| **gauche** | double-clic → quick hide / show (F-022) ; le reste ne fait **rien** | la sélection des icônes du bureau est T-042, et un bouton gauche qui dessinerait laisserait une zone derrière chaque tentative de sélection |

Le partage est **testé** (`theLeftButtonDrawsNothing`), pas seulement commenté — et `release(point, button)` ignore un relâchement du **mauvais** bouton, sans quoi un clic gauche au milieu d'un glisser droit ouvrirait le menu ou annulerait le tracé.

### Le menu du bureau est le vrai menu du Shell

Ce n'est pas une imitation : `SHGetDesktopFolder()` → `IShellFolder::CreateViewObject(IID_IContextMenu)` — c'est le menu **de fond**, il n'y a donc pas d'item et pas de `GetUIObjectOf`. `QueryContextMenu` reçoit une plage d'ids **volontairement étroite** (`0x100`–`0x6FFF`) pour laisser `0x7000` à notre entrée, insérée **en tête**, suivie d'un séparateur. `TrackPopupMenuEx` avec `TPM_RETURNCMD` rend l'id choisi : notre entrée crée la zone, n'importe quelle autre est **invoquée par nous** (`InvokeCommand`, index zéro-base), comme dans le menu d'item de T-006-1.

Mesuré sur la machine : **6 verbes du Shell** (`git_gui`, `git_shell`, `Display`, `Personalize`, une extension `{7A1E471F…}`, `workbuddy-ai`) **plus** `"Create a fence here"`. T-054 tombe donc avec T-055 : le menu que T-039 avait pris au bureau est **rendu**, avec une entrée de plus.

### ⚠️ Le bug qui plantait, et qui était là depuis T-006-1

`GetMenuItemID()` renvoie **`(UINT)-1` pour un sous-menu** et **`0` pour un séparateur**. Le code ne testait que `-1`, donc l'id `0` d'un séparateur passait pour un verbe : `id - idCmdFirst` **débordait** en `4294967040` et le shell **plantait** dans `GetCommandString` — reproduit en `rc=139`, isolé au point près par instrumentation, puis corrigé.

Les deux boucles testent maintenant `GetMenuState(...) & MF_SEPARATOR` **et** `id < idCmdFirst` (un id hors de la plage qu'on a donnée au shell ne peut pas être un verbe valide). **La version item portait le même défaut depuis la session 10** et n'avait jamais planté : le shell place ses propres séparateurs avec des ids qui se trouvaient être dans la plage. Un bug qui n'a pas encore eu lieu reste un bug.

### Les matériaux de zone : ce qui est possible, et ce qui ne l'est pas

Décision utilisateur : **la translucidité d'abord**.

- **Faisable aujourd'hui** : l'alpha par pixel. `style.background` est déjà en `#AARRGGBB` et persisté, et le chemin per-pixel (`setPerPixelAlpha`, T-017) est **prouvé à l'exécution** depuis que le fond l'emploie sur une fenêtre plein écran (session 30). Il reste à l'activer **par zone** : aujourd'hui une zone prend `SetLayeredWindowAttributes`, dont le blit GDI **ignore l'alpha** — un panneau est donc opaque en pratique, et l'alpha persisté est inerte (T-051 le disait depuis la session 29 ; c'est maintenant spécifié).
- **Pas faisable directement** : les matériaux DWM documentés (**Mica**, `DWMWA_SYSTEMBACKDROP_TYPE`) visent une **fenêtre de premier niveau avec un cadre DWM**. Une zone est une **enfant de `Progman`** : pas de flou système. Deux issues, aucune décidée — `SetWindowCompositionAttribute` (non documenté, donc **à mesurer**) ou un **flou calculé par nous** (le fond publie une image du papier peint que la zone réutilise comme panneau). **Un spike d'abord**, comme T-003 l'a été pour le placement : c'est la règle de la maison pour toute technique de composition non prouvée.

Ce sera **ADR-10** quand ce sera tranché ; d'ici là c'est **T-057** (F-038), et le suivi du rapport d'origine est **T-051**. *(Livré en session 35 : la translucidité, §26. L'acrylique reste ouvert, et toujours derrière un spike.)*

### Ce que la sonde mesure (`--backdrop-demo`)

**34/34** headless (31 avant) / **43/43** `--host` (40 avant). Les vérifications neuves, dans l'ordre où elles se tiennent :

1. `the rectangle offered is the one that was drawn` — le geste rend **le rectangle** et **le point** du relâchement, et le rectangle est celui qu'on a tracé (aucune conversion entre le geste et le rect stocké).
2. `the offered fence is created where it was drawn` — et la zone existe **au pixel près** à cet endroit.
3. `a plain right-click opens the menu with no rectangle` — le menu de T-054, celui que T-039 avait pris.
4. `a wobble offers no rectangle either` et `a right-draw too small to be a fence offers no rectangle` — les deux refus, qui n'empêchent pas le menu de s'ouvrir.
5. `the left button draws nothing` — la moitié gauche du partage.
6. `our entry sits on top of the desktop's menu` et `the shell's own entries are still there` — le menu est **construit** (sans être affiché : un popup bloquerait sur un utilisateur absent), notre entrée est **relue dans le menu**, et les verbes du Shell sont listés.

**Prémisses prouvées** (patch temporaire, revert) : règle du bouton droit neutralisée → `rc=1`, **2 `FAIL`** (dont une cascade) et **1 `FAIL`** unitaire sur 22 cas ; insertion de notre entrée neutralisée → `rc=1`, **exactement 1 `FAIL`** — et le détail mesuré est alors `""`, ce qui est la mesure qui parle.

## 23. Le geste ne pouvait pas arriver : une fenêtre par pixel où l'alpha est nul n'est pas cliquable (T-060, session 34)

Second retour utilisateur, après T-055/T-056 : **« tracer un rectangle au bouton droit ne fonctionne pas »**. Cette fois la cause était bien dans le code, et elle était invisible pour toutes les sondes.

### ⚠️ La cause

`buildFrame()` commençait par `frame.fill(Qt::transparent)`. Or **une fenêtre layered par pixel est testée au toucher contre son propre alpha** : Windows laisse passer la souris **là où l'alpha est nul** (MSDN, « Layered Windows »). Le fond est un window **plein écran qui doit être transparent presque partout** — c'est son intérêt — donc il laissait passer **tous** les clics du bureau vide : le geste ne pouvait pas commencer, et les vérifications de T-041/T-055 passaient quand même parce qu'elles **postent leurs `QMouseEvent` directement à l'objet** au lieu de laisser Windows les livrer.

C'est la même famille que la leçon de T-056 : la sonde mesurait la **machine à états**, pas le **chemin réel**. La nuance ici est qu'aucune sonde ne pouvait le voir **telle qu'elle était écrite** — il fallait mesurer la **livraison** du clic.

### Le correctif, et ce qui le rend mesurable

Le cadre est rempli à **`DesktopBackdrop::kInputAlphaFloor = 1`** : un niveau sur 255, invisible sur n'importe quel papier peint, qui transforme le cadre d'une **forme** (les seules icônes) en **rectangle** — ce que le test de toucher exige. C'est **ADR-11**.

**Mesurer** a demandé un détour, et le détour est la partie instructive :

1. **On ne peut pas interroger le fond lui-même.** `WindowFromPoint` sur un point du bureau vide rend **l'application qui recouvre le point** : *tout* enfant de `Progman` est sous *toute* fenêtre de premier niveau. La première tentative de sonde l'a mesuré — le curseur trouvait le navigateur de l'utilisateur **même avec le cadre à alpha 1**. C'est un relevé de la **session**, pas du **code** : aucune vérification qui dépendrait de ce que l'utilisateur a d'ouvert ne peut être écrite là-dessus.
2. **Le spike construit donc la même relation avec nos fenêtres** (`experiments/spike_layer_alpha_shape.cpp`, hors Qt) : un parent **en haut de tout** (non layered, comme `Progman`) et un enfant **layered par pixel** dedans. Mesuré : **alpha 0 → le curseur trouve le parent** (l'enfant est sauté), **alpha 1 → il trouve l'enfant**. La règle est mesurée **dans les deux sens**, et le premier sens est le bug de l'utilisateur reproduit.
3. **Trois autres pistes ont été écartées par la mesure** : `GetWindowRgn` **ne rend aucune région** sur une fenêtre layered (donc pas de forme dérivée de l'alpha à inspecter) ; `ChildWindowFromPointEx` est un test **de rectangle**, aveugle à l'alpha ; et un enfant de `Progman` n'est pas mesurable du tout quand une application couvre le point.

### ⚠️ Le piège qui a menti deux fois

`UpdateLayeredWindow` **déplace** la fenêtre à `pptDst`. La première version du spike passait `(0, 0)` en se disant « pour un enfant, la position est ignorée » : la fenêtre topmost a sauté dans le coin, et **deux exécutions ont conclu que « alpha 1 ne suffit pas »** — une réponse fausse produite par la sonde, pas par Windows. Corrigé en passant la position réelle (ou `NULL` pour ne pas bouger), et en **relisant le rect après le push** pour échantillonner le centre courant.

### Ce que la sonde mesure maintenant

- **headless (toujours)** : `the frame has nowhere the cursor falls through` — **0 pixel à alpha 0** sur 3 686 400. C'est l'invariant : il tomberait au rouge si le plancher disparaissait.
- **`--host` (toujours)** : `a transparent layered child lets the mouse through` et `one alpha level is enough for the cursor to find it` — la règle, mesurée par `WindowFromPoint` sur la paire de fenêtres du harnais.
- **la lecture directe, sur le vrai bureau de l'utilisateur** (ligne d'information, pas une vérification) : le fond est monté au-dessus de la couche d'icônes — artefact de sonde assumé, puisque cette sonde laisse les icônes **visibles** — et le curseur est interrogé sur une **grille de 1 523 points du bureau vide**. **Avec le correctif : 48 points répondent avec le fond lui-même** (premier à `165,30`). **Sans lui (plancher forcé à 0, trois exécutions) : 0 point, systématiquement** — c'est le bug de l'utilisateur, reproduit sur son bureau. Les 1 475 autres points sont recouverts par une application (le détail nomme le premier : `TrayNotifyWnd 398x1440`), ce qui est un relevé de la **session** ; la ligne le dit au lieu de se taire.
- `paintedPixels` compte désormais **au-dessus** du plancher : les compteurs d'icônes restent identiques (**231 738** peints, **223 887** après qu'une zone réclame 3 icônes), et l'ancien comptage `alpha != 0` aurait dit que l'écran entier est peint.

**Mesuré** : `--backdrop-demo` **35/35** headless / **46/46** `--host`, `rc=0` ; `ctest` **13/13** ; tous les autres compteurs **identiques** (`--product-probe` 17/17, `--fence-demo` 17/17 / 18/18, `--persist-demo` 26/26, `--nav-demo` 22/22 / 29/29, `--rules-demo` 28/28, `--portal-demo` 30/30 / 32/32, `--snapshot-demo` 11/11, `--screen-demo` 19/19).

**Prémisse prouvée** (patch temporaire `kInputAlphaFloor = 0`, revert + rebuild) : headless → `rc=1`, **1 `FAIL`** (`3454662 of 3686400 pixels at alpha 0`) ; `--host` → **2 `FAIL`**, et le second dit exactement le bug : `at 500,500 the cursor finds 0x… StarFishHitProbeParent with the frame at alpha 0`. La sonde **reproduit donc le symptôme de l'utilisateur** quand on retire le correctif.

## 24. Le bureau gris, et la fenêtre qui portait le papier peint (T-061, session 34)

Le second retour de la session 34 : *« ça marche uniquement quand on est dans ce fond d'écran gris très bizarre… j'aimerais pouvoir créer les fences directement avec le bureau normal d'affiché »*. Le geste, lui, fonctionnait (mesuré au vrai curseur, `SendInput`) — mais **seulement dans l'état que StarFish impose au bureau**, celui où il masque les icônes natives.

### La mesure, et pourquoi elle a demandé un détour

Le chemin direct (regarder l'écran) ne suffisait pas : trois captures successives ont été nécessaires, parce que **comparer deux états sans contrôler ce qui est au premier plan ne mesure rien** (une première comparaison avait « vu » un gris qui était en fait la fenêtre d'une application). La mesure retenue prend les quatre états **dans un seul processus**, capture à chaque fois, et compare les couleurs par plages :

| état | `SysListView32` | plage « papier peint » (milieu) | verdict |
|---|---|---|---|
| normal | visible | `(83,118,6)` — vert du papier peint | référence |
| liste masquée | **masquée** | `(85,120,2)` | papier peint **intact** |
| DefView masqué | masquée | `(20,20,20)` | **gris uni** |
| StarFish (ancien code) | masquée | `(20,20,20)` | **gris uni** |

Autrement dit : le papier peint **est** la surface du `SHELLDLL_DefView`, et StarFish masquait précisément celui-là. Windows masque la **liste** (son enfant) pour « Afficher les icônes du bureau » — et garde le papier peint.

### Le changement

Voir **ADR-12** : masquer la liste, et poser toutes nos surfaces **au-dessus du DefView** (le fond juste au-dessus du papier peint, les zones au-dessus du fond). Conséquence agréable : le clic sur le bureau vide nous arrive **sans que les icônes natives soient masquées** (1 398 / 1 523 points du bureau vide répondent avec le fond), donc masquer les icônes n'est plus ce qui rend une zone cliquable — c'est ce qui évite qu'une icône soit dessinée deux fois.

### Les vérifications qui ont bougé

Trois vérifications encodaient l'ancien ordre et ont été **requalifiées**, pas supprimées :
- `--fence-demo --host` : « la zone n'est atteignable qu'une fois les icônes masquées » → « la zone est **au-dessus** des couches du bureau, icônes visibles ou non » (les deux états sont mesurés, et le détail affiche la pile).
- `--nav-demo --host` : « normalement la surface est sous la couche d'icônes » → « **au-dessus** des couches du bureau » ; et Peek, qui ne change plus rien, est désormais **asserté comme tel** (`Peek no longer changes anything (T-053)`).
- `--backdrop-demo --host` : l'ancien « le fond est sous les zones **et** sous la couche d'icônes » → « le fond est **au-dessus du papier peint et sous les zones** » (`backdrop [3] vs DefView [4]`), et l'artefact de sonde qui remontait le fond pour interroger le curseur a **disparu** — il n'est plus nécessaire.

### Ce qui reste ouvert

- **Les positions des icônes** : nous les disposons en colonnes depuis le haut-gauche, pas là où l'utilisateur les avait (`LVM_GETITEMPOSITION` avant masquage = tâche ouverte).
- **Le geste sur un bureau *complètement* intact** (icônes natives en place, clic droit qui crée une zone sans rien changer à l'écran) demanderait d'intercepter un clic destiné à la fenêtre d'un **autre processus** : c'est le hook souris global de **T-028**, que Fences 6 installe, et la décision revient à l'utilisateur.

## 25. « Normal comme Fences 6 » : ce que ça a coûté, et ce qui reste (session 34)

L'utilisateur a demandé deux fois la même chose dans une session, ce qui a requalifié T-061 : non pas « le geste du clic droit ne marche pas », mais « **il ne marche que dans un état du bureau que je ne veux pas** ». La réponse tient en deux mesures et une décision (ADR-13).

### La chaîne, mesurée de bout en bout
| question | mesure |
|---|---|
| Le lancement prend-il encore le bureau ? | **non** : aucun marqueur écrit, le statut dit `desktop icons visible` |
| Le clic atteint-il les icônes de l'utilisateur ? | **oui** : `WM_NCHITTEST` sur « starfish - Raccourci » → `HTTRANSPARENT`, **88 icônes** contournées |
| Le bureau nu est-il à nous ? | **oui** : point nu → `HTCLIENT` |
| Le papier peint survit-il au repli ? | **oui** : 5 409 des 5 409 pixels hors de notre disposition **inchangés** |
| Le geste marche-t-il pour de vrai ? | **oui** : un glisser-droit réel (`SendInput`) ouvre le menu du bureau — `#32768`, 257×209 — avec **« Create a fence here (76 x 255) »** en tête, puis « Configure Fences », « Open Git GUI here », « Nouveau », « Paramètres d'affichage », « Personnaliser » |
| Peut-on déplacer une icône isolée ? | **non** — `LVM_SETITEMPOSITION` vers `(-32000,-32000)` se relit **`(8,2)`** : l'icône atterrit dans le coin, et l'alignement sur la grille n'y est pour rien (`LVS_AUTOARRANGE` **absent**, mesuré). D'où la reprise de la **liste** à notre charge : *« drawing the desktop's icons here instead, at the positions read from the shell »* |
| La sortie est-elle propre ? | **oui** : `native desktop icons restored (SysListView32)`, marqueur retiré, aucun processus restant |

Capture de contrôle (locale, non commitée, comme les rapports de sonde) : `build/t061_bureau_papier_peint.png` (le bureau pendant que le produit tourne : papier peint visible, zones, menu ouvert).

### Ce que la session a appris au-delà du code
- **`SetWindowPos(hwnd, X)` place la fenêtre *sous* X** : « au-dessus du papier peint » s'écrit donc « sous la fenêtre déjà au-dessus du papier peint » (ADR-12), sinon le watchdog remonte le fond devant les zones.
- **Un échec de position mesuré vaut mieux qu'une cause supposée** : j'ai d'abord attribué le `(8,2)` à l'alignement sur la grille ; la mesure du style (`0x56003a40`, `LVS_AUTOARRANGE` **absent**) a montré que la cause était autre, et la conclusion utile n'a pas changé — **on ne déplace pas les icônes de quelqu'un**, parce qu'une écriture qui rate les déplace quand même, et que le dégât est visible.
- **Une lecture n'est valide que dans l'état où elle a été mesurée** : une vue liste masquée rend 88 items, des noms vides et des positions `1,1` / `1,2` / `1,3`. La précondition (`nativeDesktopIconsVisible()`) est désormais dans le code, et la minuterie saute le tour.
- **Le clic droit sur le bureau nu n'a pas besoin du hook souris global** (T-028) : notre surface *est* le point nu. Le hook ne resterait nécessaire que pour ajouter notre entrée au menu d'une **icône** native, ou pour tracer un rectangle qui **commence sur une icône** — deux choses à décider, pas à supposer.
- **Lire la coquille est bon marché** : 88 icônes, deux messages chacune, toutes les deux secondes, sans effet mesurable.

## 26. Les matériaux de zone : la translucidité d'abord (T-057 / F-038, session 35)

Décision utilisateur de la session 33 : **la translucidité d'abord**. Elle est livrée, et elle n'a demandé **aucun champ de schéma** — c'est le résultat le plus intéressant de la session.

### Un matériau n'est pas un second style, c'est l'alpha qui dormait

`FenceStyle::background` est écrit en **`#AARRGGBB`** depuis T-008, et son canal alpha était **persisté, relu, défauté** — et **inerte** : `FenceView::setPanelColor()` le *jetait* (`QColor(color.red(), green, blue)`), et même conservé il n'aurait pas atteint l'écran, puisque le chemin opaque blitte en GDI, qui **ignore l'alpha**. Conséquence : un document écrit avant T-057 (`"#CC1F2937"`, le défaut du schéma **de ce temps-là** — T-074 l'a depuis déplacé vers l'opaque, §32) est **déjà** le preset « translucide ». Les presets vivent donc dans un header **pur** (`src/ui/fence_material.hpp`) qui ne connaît que des noms et de l'arithmétique sur la chaîne :

| preset | alpha | `#AARRGGBB` du fond par défaut |
|---|---|---|
| `opaque` (**défaut du schéma** depuis T-074, §32) | 255 | `#FF1F2937` |
| `translucent` (le défaut **jusqu'à T-074**) | 204 | `#CC1F2937` |
| `very-translucent` | 128 | `#801F2937` |

`withAlpha()` **réécrit les deux chiffres d'alpha et recopie les six chiffres RGB verbatim** (casse comprise) : la chaîne que l'utilisateur ou un document ancien a écrite survit, et l'aller-retour octet pour octet du document n'est pas cassé. Une couleur illisible (un nom, un `#abc`) est **laissée telle quelle** et compte comme opaque — pas de matériau appliqué à l'aveugle, pas de réécriture. **16 cas** dans `tests/test_fence_material.cpp` (14ᵉ exécutable `ctest`), dont un qui vérifie que notre parseur **est d'accord avec `QColor`** : c'est le kit qui définit la forme du schéma, pas notre croyance.

### La chaîne complète, et ce qui la rend vraie

```
document (#AARRGGBB)  ->  FenceView::setPanelColor (garde l'alpha)
                      ->  FenceContainer::renderFence (l'alpha de la frame)
                      ->  DesktopSurface::setPerPixelAlpha (mode de la fenêtre)
                      ->  DWM compose sur le papier peint
```

Le **mode de la fenêtre** est décidé par le matériau : `setPerPixelAlpha(true)` si l'alpha < 255. Il est posé **avant `attach()`** quand une zone est créée ou restaurée, parce que le mode layered est fixé à la **création** (`attach()` saute l'appel `SetLayeredWindowAttributes` en per-pixel — la porte à sens unique de la session 9) ; changer de matériau sur une zone vivante passe donc par `setPerPixelAlpha()`, qui **recrée la fenêtre**. Les deux chemins sont mesurés séparément (`a translucent fence is hosted through the per-pixel push`, `a fence restored from a translucent document is born per-pixel`).

### ⚠️ Le corollaire d'ADR-11 s'applique aux zones, et ce n'est pas théorique

Une fenêtre layered **par pixel** est testée au toucher **contre son propre alpha** (T-060). Une zone translucide a des pixels **hors du panneau** — les quatre coins arrondis — qui resteraient à alpha 0 : la souris **les traverserait**, et seulement eux. Le symptôme aurait été « les coins de mes zones ne répondent pas », c'est-à-dire un bug attribué aux coins arrondis plutôt qu'au matériau. La frame est donc remplie à **`DesktopSurface::kInputAlphaFloor = 1`** avant que le panneau soit peint (le remplissage opaque pour un matériau opaque ; c'est la **même** constante que le fond de bureau, et la définition a déménagé chez `DesktopSurface` — la fenêtre layered — avec un alias `DesktopBackdrop::kInputAlphaFloor` pour ne pas casser les sondes ni la doc).

**Prémisse prouvée** : plancher remis à 0 (patch temporaire, revert) → `--fence-demo` `rc=1`, **exactement 2 `FAIL`**, et le détail nomme le dégât : `corner alpha 0 (floor 1), 76 of 32000 pixel(s) at alpha 0` — les 76 pixels sont les quatre coins.

### ⚠️ Et un piège de remplissage, mesuré plutôt que supposé

Le remplissage passe par un **`QPainter`** (`CompositionMode_Source`) et **pas** par `QImage::fill(QColor)`, parce que la sortie de `QPainter` sur une image prémultipliée est prémultipliée **par construction**. Un pixel « prémultiplié » dont le RGB dépasse son alpha ne s'affiche pas comme un pixel presque invisible : la composition le **rajoute** au fond, et le coin devient une bande claire. La sonde vérifie donc l'invariant sur **chaque pixel** de la frame (`the frame is valid premultiplied alpha` : aucun canal au-dessus de son propre alpha), et pas seulement l'alpha attendu au centre.

### Exposer le choix, sans faire de la fenêtre de réglages le produit

- **Zone de notification → « Fence material »** : les trois presets, appliqués à **toutes** les zones (elle ne sait pas en désigner une). Une coche n'est mise que si toutes les zones s'accordent ; un bureau mixte n'affiche **rien** plutôt que de choisir pour l'utilisateur (`a mixed desktop is reported as mixed`).
- **Fenêtre de réglages → « Material: »** : le choix **par zone**, à côté de la liste — c'est ce que T-059 garde de cette fenêtre, avec la ligne d'état (qui dit désormais `material: Translucent` / `materials: mixed`).
- **La ligne de statut** est le seul endroit où le produit décrit son état en mots, donc le matériau y est écrit — et une sonde peut le lire sans capture d'écran.

### Le bug que la sonde a attrapé

Le câblage « fenêtre de réglages → produit » **manquait** dans `wireSettings()`, et la sonde l'a dit : `a material chosen in the settings window takes` est passée au **rouge** avec un statut qui disait encore `Very translucent`. C'est le genre de trou que le modèle ne peut pas révéler (le conteneur, lui, obéissait) — il fallait **piloter les vrais widgets** (`QListWidget` / `QComboBox` retrouvés par `objectName`, donc la même chose qu'un clic). Deux vérifications l'encadrent : le combo **suit** le document, et **mettre l'état à l'écran n'est pas un choix** (`0 document change(s) for a refresh and a selection`) — sans quoi un rafraîchissement aurait réécrit le document à chaque changement.

### Prémisses prouvées (patch temporaire, revert + rebuild)

| ce qui est neutralisé | résultat |
|---|---|
| le plancher d'alpha (`0` au lieu de `kInputAlphaFloor`) | headless `rc=1`, **2 `FAIL`** (`corner alpha 0`, `76 of 32000` pixels à alpha 0) |
| l'alpha gardé par `FenceView::setPanelColor` | headless **3 `FAIL`**, `--host` **5 `FAIL`** — dont les deux vérifications de **mode**, donc toute la chaîne tient à ce seul `QColor(...)` |
| le changement de mode (`applyMaterialToSurface` neutralisé) | `--host` **1 `FAIL`** (`an opaque fence goes back to the blit path`) : la paire de vérifications est porteuse **dans les deux sens** |

### Ce qui n'est **pas** mesuré, et le dit

**Le résultat visuel** — « le papier peint traverse la zone » — reste une validation **humaine** : la machine a un écran, et les fenêtres de l'utilisateur recouvrent le bureau (décision 13). Ce qui est mesuré ici, c'est la **décision** (le mode de la fenêtre) et le **contenu poussé** (l'alpha de chaque pixel de la frame), pas la composition finale à l'œil.

### Comptes (session 35)

build MinGW **0 warning** ; `ctest` **14/14** (`ui_fence_material`) ; `--fence-demo` **25/25** headless / **29/29** `--host` (17/18 avant) ; `--product-probe` **23/23** (17 avant) ; `--backdrop-demo` **36/36** / **50/50** ; `--persist-demo` 26/26 ; `--nav-demo` 22/22 / 29/29 ; `--rules-demo` 28/28 ; `--portal-demo` 30/30 / 32/32 ; `--snapshot-demo` 11/11 ; `--screen-demo` 19/19 — **inchangés**.

### Reste ouvert

- **L'acrylique** (le flou) : hors de portée d'une enfant de `Progman` par les matériaux DWM documentés → `SetWindowCompositionAttribute` (non documenté, à mesurer) ou un flou calculé par nous. **Un spike d'abord** (ADR-10 quand ce sera tranché).
- **Le choix au clic droit sur une zone** (« Material ▸ » sur la barre de titre, comme Fences 6) : c'est la place naturelle, et elle n'existe pas encore — **T-064**.
- Les autres champs de style persistés **restent inertes** (`iconSize`, `padding`, `gap`, `titleBar`) : c'est **T-014b**.

## 27. Le bureau n'est plus jamais pris, et les zones ne s'empilent plus (T-066 / T-067, session 36)

Le retour utilisateur qui ouvre cette session, verbatim :

> Je veux que : starfish ne masque pas les icones de bureau normaux. afficher les fences sur le bureau normal. ne pas masquer les icones qui ne sont pas dans les fences. et que ce soit comme ça dès le démarrage. Là en l'état c'est inutilisable.

Deux défauts distincts, tous les deux **reproduits et chiffrés avant** d'être corrigés, et tous les deux **dans le chemin que le produit emprunte tout seul** — pas dans une option, pas dans une sonde.

### 27.1 Le symptôme principal : la première icône rangée prenait le bureau (T-066)

`ProductApp::syncNativeIcons()` (le rafraîchissement de deux secondes, **et le démarrage**) contenait :

```cpp
if (!held.isEmpty() && !m_ownsIcons)
    takeOverDesktopIcons();   // masque SysListView32, le bureau devient le nôtre
```

`held` = les icônes du bureau qu'une zone **détient** (intersection « items du bureau » ∩ « chemins occupés par les zones »). Donc : **ranger une icône dans une zone suffisait à prendre le bureau**. Ce qui suit n'est pas un détail — masquer la liste veut dire que le bureau est désormais dessiné **par nous**, et que les icônes que nous dessinons ne répondent **à rien** : pas de clic, pas de sélection, pas de menu contextuel, pas de double-clic, pas de glisser. C'est **T-042**, non écrit. Le bureau devenait donc muet quelques minutes après un lancement normal, **sans retour possible** sinon quitter l'application.

Pourquoi aucune sonde ne l'avait vu : `--claim-desktop` **choisit** cet état (les sondes de T-056/T-061 le mesurent donc volontairement), et les sondes de produit travaillent sur des documents jetables **sans icône du bureau** (`held` reste vide dans un dossier temporaire). Le produit, lui, atteignait cet état **tout seul** dès la première icône rangée. C'est l'exact inverse du piège de T-061 (une fonctionnalité n'existe pas tant que le produit ne se met pas dans l'état où elle s'applique) : **un état que le produit atteint seul n'avait jamais été regardé**.

**Correctif** — la liste n'est plus jamais prise par le produit :

```cpp
if (!m_options.claimDesktop) {
    if (m_ownsIcons)
        giveBackDesktopIcons();   // reliquat du chemin claimDesktop, ou d'un ancien run
    return;
}
```

`takeOverDesktopIcons()` / `giveBackDesktopIcons()` survivent derrière **`--claim-desktop`** (chemin de développement et sondes qui mesurent l'ancien comportement), où les icônes sont réellement les nôtres à dessiner.

**Le prix, assumé et écrit** : une icône qu'une zone détient est désormais dessinée **deux fois** — par le bureau à sa position, et comme tuile dans la zone. C'est la conséquence directe de « ne touche pas à mon bureau » tant que T-042 n'existe pas : on ne peut pas à la fois laisser à l'utilisateur ses icônes **et** les faire disparaître de là où elles sont sans les déplacer (déplacer est refusé : ADR-13, `LVM_SETITEMPOSITION` mesuré non fiable). Le jour où les icônes que nous dessinons seront utilisables, reprendre la couche redeviendra un **choix à offrir**, et non un effet de bord du rangement.

### 27.2 « Afficher les fences » : trois zones au même endroit (T-067)

Mesuré **dans le document de l'utilisateur** (`%APPDATA%/StarFish/layout.json`) : **5 zones**, dont **trois au même rect** `(80, 1200)` — empilées en bas de l'écran, n'en laissant voir qu'**une**. Ce n'était pas trois zones perdues : c'étaient trois zones **créées depuis la zone de notification**, à des moments différents, toutes posées au même point.

La politique précédente était « à droite de tout, puis on passe à la ligne suivante ». Le calcul de la ligne suivante donnait `max(bas des zones) + 24`, et le **clamp à l'écran** ramenait ce y à l'intérieur des limites — c'est-à-dire, une fois la dernière ligne atteinte, **toujours au même endroit**. Chaque nouvelle zone repartait donc du même point, indéfiniment.

**Correctif** : `FenceGeometry::freeSpot(others, size, bounds, gap)` (`src/ui/fence_geometry.hpp`, **pur**, sans Qt Widgets) — la **première place libre** en ordre de lecture (gauche→droite puis vers le bas) sur une grille espacée du `gap`, en excluant tout rect qui **touche** un rect existant. Le produit l'appelle sur l'écran **primaire**, avec une marge de `kPlacementMargin = 80` px — ce qui garde aussi la première rangée d'icônes dégagée (une zone qui se pose pile sur la première icône du coin serait « correcte » et désagréable). Si **rien** n'est libre (écran saturé), le candidat **le moins recouvert** gagne : un chevauchement se voit et se corrige à la souris, une zone cachée sous une autre ne se voit **pas** — c'est le même raisonnement que celui qui a produit `freeSpot`.

**7 cas** ajoutés à `tests/test_fence_geometry.cpp` (**26 cas** au total). Le plus intéressant **prouve sa prémisse** : une zone **haute** occupe la deuxième ligne **de sa colonne seulement**, donc la place libre doit être **l'autre** colonne à la même hauteur — « la ligne suivante » à l'aveugle échoue là où `freeSpot` réussit.

Vérifié par `--product-probe` (**24/24**, +1) : **six** zones créées depuis le tray, mesurées **0 doublon, 0 recouvrement, 0 hors écran** (`every fence created lands somewhere of its own`). ⚠️ Les zones **déjà** empilées dans le document de l'utilisateur ne sont **pas** déplacées : un document de l'utilisateur ne se corrige pas tout seul (à proposer, pas à imposer — décision 39).

### 27.3 La sonde qui manquait : `--icons-demo`

La question « le bureau est-il encore celui de l'utilisateur ? » est une question sur **le bureau**, pas sur notre modèle — donc la sonde mesure **de l'extérieur** : la liste du shell, le fichier marqueur, et le test de survol que Windows lui-même utilise.

Elle construit un document jetable sous `build/` dont **une zone détient une vraie icône du bureau** (le premier item de `DesktopScanner` avec un chemin), la zone étant placée par `freeSpot` pour n'en recouvrir aucune, puis lance le produit **hébergé** dessus et vérifie dix choses :

| vérification | ce qu'elle mesure |
|---|---|
| `the desktop's icons are the shell's to begin with` | **précondition** : liste visible, > 1 icône lisible, aucun marqueur. Sans elle, la sonde mesurerait « les icônes sont toujours là » sur un bureau dont les icônes étaient **déjà** masquées |
| `the product starts with a fence that holds an icon` | le cas qui déclenchait le rachat est bien celui qu'on exécute (`1 fence(s)`), et le statut dit `desktop icons visible … (85 on the desktop, 1 held by fences)` |
| `the fence is hosted and drawn on the normal desktop` | la zone existe **et est hébergée** (`isAttached`), avec une frame non vide — « afficher les fences » |
| `the fence itself answers the mouse` | `WM_NCHITTEST` **sur la zone** → `HTCLIENT` : une zone posée sur un bureau normal est utilisable |
| `the mouse still reaches the user's own icon` | `WM_NCHITTEST` **sur le fond** au centre de la première icône → `HTTRANSPARENT` (**88 rectangles** contournés) : le clic va à l'icône de l'utilisateur |
| `the launch leaves the desktop's icons alone, even when a fence holds one` | **la demande exacte** : `SysListView32` est encore visible alors qu'une zone détient une icône |
| `no icon-hiding marker was written` | rien sur le disque ne dit le contraire (le marqueur est ce que la réparation d'un crash relit) |
| `the desktop is exactly the one it started with` | après la sortie : mêmes icônes (noms triés), même nombre devant/derrière, toujours visibles, pas de marqueur |
| `a throwaway document holds one real desktop icon` / `the probe cleaned up after itself` | la sonde est honnête sur son montage et ne laisse rien derrière elle |

**Prémisse prouvée** : l'ancien rachat remis en place (patch temporaire de `syncNativeIcons`, revert immédiat) → `rc=1` et **exactement 2 `FAIL`** : `the launch leaves the desktop's icons alone…` (statut `desktop icons hidden`) et `no icon-hiding marker was written` (`pid 24728, hidden at 2026-10-16T22:53:59`). Après la sonde, `SysListView32 visible = True` et marqueur **absent** : la sonde **rend ce qu'elle a pris**, y compris dans l'état cassé (c'est le chemin de réparation de T-046 qui le garantit).

**Mesure de bout en bout sur le document réel de l'utilisateur** (copie dans `%TEMP%`, mode produit hébergé, sondé depuis un autre processus pendant que le produit tourne) : **6 surfaces** hébergées sous `Progman` — le fond (`0,0 2560x1440`) **et les 5 zones** de son document, chacune à son rect enregistré —, `SysListView32 visible = True`, `SHELLDLL_DefView visible = True`, marqueur **absent**, et après la sortie : **0** surface, bureau inchangé, document (copie) inchangé.

### 27.4 Comptes (session 36)

build MinGW **0 warning** ; `ctest` **14/14** (`ui_fence_geometry` portée à **26 cas**) ; `--icons-demo` **10/10** (nouvelle) ; `--product-probe` **24/24** (23 avant) ; **tous les autres compteurs inchangés** : `--fence-demo` 25/25 / 29/29 `--host`, `--backdrop-demo` 36/36 / 50/50, `--persist-demo` 26/26, `--nav-demo` 22/22 / 29/29, `--rules-demo` 28/28, `--portal-demo` 30/30 / 32/32, `--snapshot-demo` 11/11, `--screen-demo` 19/19.

### 27.5 Ce qui reste ouvert

- **T-042** (interactions sur les icônes qu'on dessine) — c'est **le** correctif de fond : tant qu'il n'existe pas, l'utilisateur a soit **ses** icônes (dessinées deux fois si elles sont dans une zone), soit les nôtres (utilisables pour nous, muettes pour lui). Le choix retenu aujourd'hui est explicite : **le bureau gagne**.
- **Le rachat redevient un choix** : `--claim-desktop` est aujourd'hui le chemin de développement. Si un jour la couche reprise est utilisable, l'option pourra redevenir une case à cocher — mais jamais un effet de bord du rangement.
- **Les trois zones empilées du document de l'utilisateur** : elles sont **toujours** au même rect (T-067 ne touche pas aux zones existantes). À proposer à l'utilisateur : un décalage en cascade le rangerait, mais c'est **son** bureau.
- **L'acrylique** et **T-064** (matériau au clic droit) restent ouverts (§26).

## 30. ADR-14 — le bureau reste au bureau : trois reproches, une fenêtre de trop (T-071, session 40)

Retour utilisateur, trois symptômes rapportés ensemble :

> - on ne peut plus bouger les icones du bureau (à part les rajouter dans fences. Est attendu ?)
> - le menu clic droit sans bouger le curseur n'est plus le menu original de windows.
> - le clic glissé ne dessine pas le rectangle habituel sous windows.
> JE PENSE : si le fait de pouvoir faire une fence avec un glissé cliqué sur clic droit entraîne trop de contrainte, alors on annule la feature, on se contentera de faire une fence avec une entrée dans le menu clic droit sur le bureau de windows normal.

### La cause unique, et la mesure qui manquait

Le fond plein écran que le produit hébergeait sur le bureau (T-039, puis ADR-12/13) refusait **les dépôts** : un glisser d'icône se termine par un *dépôt*, Windows cherche une **cible de dépôt** sous le curseur, il trouve **notre** fenêtre — qui n'en enregistre **aucune**, volontairement, pour ne pas avaler un dépôt qu'elle ne saurait pas honorer (**T-052**). L'icône se laissait donc **attraper et jamais reposer**. C'est la famille de T-069 (*une réponse n'est pas une livraison*) poussée d'un cran : **une absence de cible est un refus**.

Et la même fenêtre tenait les **deux boutons du bureau nu** : le **gauche** (le rectangle de sélection de la coquille, et le clic dans le vide qui désélectionne) et le **droit** (d'où un menu **reconstruit par nous** — T-055 demande le menu du fond au Shell et y insère notre entrée — que l'utilisateur a immédiatement reconnu comme « plus celui de Windows »). Le fond aura donc coûté **quatre** choses en quatre sessions : les clics des zones (T-068), les clics des icônes (T-069), les **dépôts** des icônes (T-071) et **les deux boutons du bureau nu** (T-071). Chacune a été « réparée » séparément avant que la vraie réponse n'apparaisse : **il n'y a rien à réparer, il y a une fenêtre à ne pas héberger.**

### La décision (ADR-14)

**Le produit ne pose plus aucune fenêtre sur le bureau**, et **F-001 change de porteur** :

1. **Rien n'est hébergé.** `ProductApp::start()` n'active plus le fond (il ne vit que pour `--claim-desktop`, où il dessine les icônes, et pour les sondes qui le mesurent). Conséquence : papier peint, icônes (**clic, glisser, menu, renommage**), menu **original** de Windows, rectangle de sélection et désélection — tout est la coquille, **par construction**, sans trou d'alpha ni clause d'effacement. Les correctifs de T-068 (une zone prend son propre clic) et de T-069 (les trous d'alpha) restent : le premier est vrai pour les zones, le second pour le fond **là où il est encore hébergé**.
2. **Créer une zone = une entrée du menu de Windows.** Un **verbe statique**, par utilisateur :

   ```
   HKCU\Software\Classes\DesktopBackground\Shell\StarFish
     (défaut)  = StarFish: create a fence
     Icon      = "<chemin>\starfish.exe",0
     Position  = Top
     command\(défaut) = "<chemin>\starfish.exe" --new-fence
   ```

   C'est **Explorer** qui construit son menu et nous appelle : on ne prend plus le bouton droit, on ne reconstruit plus aucun menu, et l'entrée est là **aussi quand StarFish ne tourne pas**. Aucune élévation (per-utilisateur), et `--uninstall-desktop-menu` la retire. Installée par `--install-desktop-menu` ; vérifiée par `reg query`.
3. **`--new-fence`** : si une instance tourne, le CLI lui **demande** la zone par un **événement nommé** (`Local\StarFish.NewFence`, sondé toutes les 250 ms par `platform::win::NewFenceWatcher`) — un second processus serait refusé par le garde mono-instance (T-044) et l'entrée paraîtrait cassée ; sinon il **démarre** le produit, qui honore la demande au lancement. La zone se pose où `FenceGeometry::freeSpot` la met (première place libre).

### Deux pièges mesurés, payés une fois

- **Un événement nommé ne porte pas une demande vers un processus qui n'existe pas encore.** Le CLI crée l'événement, le signale, et **ferme son handle** : l'objet (et son état) meurt avant que le produit ne l'ouvre. Mesuré : le premier essai de `--new-fence` sans instance n'a créé **aucune** zone. Corrigé en passant la demande par l'**option** quand rien ne tourne.
- **`LVM_HITTEST` attend un *pointeur* vers `LVHITTESTINFO`**, pas des coordonnées : passées dans `lParam`, elles deviennent un pointeur bidon que la **liste du bureau** lit… et **explorer plante et redémarre** (mesuré : le bureau est revenu intact, avec ses 88 icônes, mais une vérification de la sonde est passée au rouge le temps de la reconstruction — « 88 icônes avant, 0 après »). **Règle** : ne jamais envoyer à une fenêtre d'un autre processus un message dont l'argument est un pointeur qu'on n'a pas alloué **dans son** espace d'adressage.

### Vérifications

- `--icons-demo` passe de 10 à **12/12**, avec deux vérifications neuves : `the product puts no window of ours on the desktop` (`backdrop hosted=0, our surface(s) under Progman=1 for 1 fence(s), full-screen=0`) et `a click on one of the desktop's icons still reaches the shell` (l'instrument est le **compte de sélection de la coquille** : clic sur une icône → `0 -> 1`, nettoyage au `Ctrl+clic` → `0`, 0 `SendInput` refusé). Le nettoyage est **best-effort et journalisé** : deux façons de le faire ont été essayées et sont fausses (deviner un point « nu » depuis les rectangles d'icônes — la zone sensible du Shell inclut le libellé ; et `LVM_HITTEST`, pour la raison ci-dessus).
- `--fence-demo` **31/31** `--host` (une vraie pression du bouton gauche) ; `--backdrop-demo` **36/36** / **50/50** (le fond est mesuré là où il vit encore : `--claim-desktop` et sa propre sonde) ; `--product-probe` **24/24** ; `ctest` **14/14** ; build **0 warning**.
- **Bout en bout** : entrée **relue** par `reg query` (`command = "…\build\starfish.exe" --new-fence`) ; `--new-fence` **sans** instance → document `3 → 4` zones ; **avec** instance → `asked the running StarFish for a new fence` et **4 → 5** fenêtres hébergées, bureau inchangé (icônes visibles, aucun marqueur).

### Ce que ça coûte, nommé

- **T-072** : le **double-clic sur le bureau vide** (F-022, quick hide) était porté par le fond. Restent le raccourci `Win+Shift+H` et la notification. À trancher : s'en tenir là, ou une seconde entrée statique du menu du bureau.
- **T-070 est annulé** : le hook souris n'a plus d'objet — il n'y a plus rien à intercepter.


## 29. Un trou d'alpha, pas une réponse : le bureau redevient atteignable (T-069, session 38)

Suite directe de T-068, et c'est **la mesure qui a séparé les deux problèmes** :

> le problème du bureau et des icônes du bureau non clickable n'est pas résolu
> je veux dire pendant que starfish est lancé, le bureau n'est plus atteignable

T-068 avait réparé les **zones** (elles répondaient `HTTRANSPARENT` au moment de la pression). Restait le **bureau** : nos surfaces le prenaient en entier.

### Mesure de *livraison*, pas de réponse

La question « le clic arrive-t-il à l'icône ? » ne se pose pas à notre fenêtre, mais à **la coquille**. Instrument : `Ctrl+clic` sur une icône **bascule** sa sélection, donc *deux* `Ctrl+clic` rendent la sélection à son état de départ tout en prouvant que les deux clics sont arrivés ; on lit le compte d'items sélectionnés de `SysListView32` (`LVM_GETSELECTEDCOUNT`) avant, entre et après.

| | points d'icônes où la coquille a **reçu** le clic |
|---|---|
| sans StarFish (contrôle) | **1 / 4** (un point tombe vraiment sur une icône) |
| **avec StarFish, binaire de T-068** | **0 / 4** |
| **avec StarFish, binaire de T-069** | **1 / 4** — le même point que le contrôle |

Corroboré par ce que le système répond du point : `WindowFromPoint` sur ce point d'icône rendait **notre fond** avant le correctif, et rend **`SysListView32`** après.

### La cause : `HTTRANSPARENT` ne traverse pas les processus

`DesktopBackdrop` est une fenêtre plein écran, enfant de `Progman`, posée au-dessus du `DefView`. Son `WM_NCHITTEST` répond `HTTRANSPARENT` sur les rectangles d'icônes — la bonne réponse. Mais la documentation de `WM_NCHITTEST` est explicite : `HTTRANSPARENT` fait continuer la recherche **dans le même thread**. Or `SysListView32` appartient à `explorer.exe` : **un autre thread, un autre processus**. La recherche s'arrête donc chez nous, et le clic n'arrive jamais.

⚠️ C'est la **troisième fois** que la même leçon revient (T-060 : un geste ne se mesure pas en *postant* l'événement ; T-068 : une question posée *au repos* mesure le mauvais instant) — et ici elle est encore plus simple : **une réponse n'est pas une livraison**. ADR-13 disait « nos surfaces répondent `HTTRANSPARENT`, donc le clic reste à l'utilisateur » : c'était une **déduction tirée d'un `SendMessageW` sur notre propre fenêtre**, jamais une mesure. La mesure la contredit.

### Le correctif : découper les icônes dans notre propre couche

Une fenêtre layered **par pixel** n'existe pas pour la souris là où ses pixels sont à **alpha 0** — propriété mesurée **dans les deux sens sur cette classe de fenêtre** par T-060, et contrairement à `HTTRANSPARENT`, elle traverse les processus. Les rectangles d'icônes sont donc **découpés** dans la frame (`CompositionMode_Source` + brosse transparente, donc des pixels prémultipliés `(0,0,0,0)`), **avant** toute peinture pour que la bande élastique du geste puisse se dessiner par-dessus.

- `DesktopBackdrop::buildFrame()` : le remplissage au plancher d'alpha est suivi du découpage.
- `DesktopBackdrop::setNativeIconRects()` demande désormais un **rendu** (`refresh()`) : sans cela, un icône ajoutée sur le bureau resterait intouchable jusqu'au prochain rendu venu d'ailleurs.
- La réponse `HTTRANSPARENT` du `WndProc` est **conservée** : elle ne coûte rien et c'est la bonne réponse — mais ce n'est pas elle qui livre le clic, et le commentaire le dit.

### Vérifications

- `--backdrop-demo` : **36/36 → 37/37**, avec une vérification neuve — *« an icon's rectangle is cut out of the layer, so the shell gets its click »* : **818 006** pixels à alpha 0 **sur les 88 rectangles d'icônes**, et **0 ailleurs**. La seconde moitié est l'invariant de T-060 (`the frame has nowhere the cursor falls through`), qui reste vrai **partout où le bureau n'a pas d'icône** : les trous sont exactement ce qu'ils doivent être, et rien de plus.
- **Prémisse prouvée** : trous retirés (patch temporaire, revert) → `rc=1`, la vérification tombe à `0 alpha-0 pixel(s) over 88 icon rect(s)`.
- **Livraison** : voir le tableau ci-dessus (0/4 → 1/4, plus `WindowFromPoint` qui change de réponse).
- `ctest` **14/14** ; toutes les autres sondes à leurs compteurs (`--fence-demo` 25/25 / 31/31 `--host`, `--product-probe` 24/24, `--icons-demo` 10/10, `--persist-demo` 26/26, `--nav-demo` 22/22 / 29/29, `--rules-demo` 28/28, `--portal-demo` 30/30 / 32/32, `--snapshot-demo` 11/11, `--screen-demo` 19/19).

### Ce qui reste, et qui est nommé

- **Le bouton gauche sur le bureau *nu* n'est toujours pas à l'utilisateur** : il est à notre fond (c'est ce qui fait exister le geste du bouton droit, F-001 — le fond *est* le point nu, et c'est ce qui a supprimé le besoin d'un hook global). Conséquence : pas de rectangle de sélection au glisser gauche, et **pas de désélection en cliquant sur le vide**. La clause `HTTRANSPARENT` du bouton gauche ne peut pas le rendre non plus, pour exactement la même raison (même thread). Trois issues, à trancher : garder tel quel (le prix de F-001), **rejouer** le clic vers la coquille (famille refusée par T-042 : un clic rejoué est un autre clic), ou déplacer le geste vers un **hook souris** (T-028 ; c'est ce que fait Fences 6, et c'est une décision utilisateur).
- **Les icônes sous une zone** : la zone prend le clic (T-068) et l'icône affichée dessous est le doublon de T-042 — elle reste intouchable tant que le doublon existe. C'est le prix déjà écrit de T-066.

### Leçon

**Une réponse n'est pas une livraison.** Avant de conclure « le clic reste à l'utilisateur », il faut le mesurer **là où il atterrit** (la liste du shell, un compteur de sélection), pas là où on le pose (notre fenêtre). Trois sessions, trois formes de la même erreur : *poster* un événement (T-060), *demander au repos* (T-068), *demander à soi-même* (T-069).


## 28. Une règle écrite pour le fond s'appliquait aussi aux zones (T-068, session 37)

Question de l'utilisateur, verbatim — et c'est la deuxième fois que « ça ne marche plus » vient d'une règle ajoutée pour *un autre* client :

> pourquoi j'arrive plus à bouger les fences ou à toucher une icône du bureau quand starfish est lancé ?

### Le fait, mesuré avant toute théorie

Les fenêtres étaient toutes en place et le bureau normal. Mesure faite **sur les fenêtres du produit en fonctionnement**, en leur posant la question que Windows leur pose (`WM_NCHITTEST`) :

| point | au repos | **bouton gauche enfoncé** |
|---|---|---|
| barre de titre d'une zone | `HTCLIENT` (1) | **`HTTRANSPARENT` (-1)** |
| corps d'une zone | `HTCLIENT` (1) | **`HTTRANSPARENT` (-1)** |
| fond, bureau nu | `HTCLIENT` (1) | **`HTTRANSPARENT` (-1)** |

Un clic, un glisser et un dépôt (OLE) sont des événements **bouton gauche enfoncé** — et Windows demande `WM_NCHITTEST` **au moment où le bouton s'enfonce**. Donc, à l'instant précis qui compte, une zone répondait « je ne suis pas là » : elle ne pouvait être **ni attrapée, ni déplacée, ni redimensionnée, ni cliquée, ni recevoir un dépôt d'icône**.

### La cause : une clause écrite pour le fond, dans un WndProc partagé

`DesktopSurface::WM_NCHITTEST` portait deux clauses destinées au **fond de bureau** (ADR-13) :

```cpp
if (m_clickThrough) { *result = HTTRANSPARENT; return true; }        // fond : click-through
for (icone : m_nativeIconRects) if (...) { HTTRANSPARENT; return; }  // fond : une icône n'est pas à nous
if (::GetAsyncKeyState(VK_LBUTTON) < 0) { HTTRANSPARENT; return; }   // fond : le bouton gauche est au bureau
*result = HTCLIENT;
```

Les trois sont justes **pour le fond** — c'est la surface qui se tient au-dessus des couches du bureau sans rien y dessiner. Mais `DesktopSurface` est **la** classe des zones **et** du fond : les clauses s'appliquaient donc **aussi aux zones**, silencieusement. Une zone répondait `HTTRANSPARENT` dès qu'on la pressait ; un fond répond `HTCLIENT` dès qu'on le presse, ce qui masquait le problème (le fond, lui, *doit* prendre le bouton droit).

### Le correctif

La règle est **gardée par la couche**, pas par la classe :

```cpp
if (m_layer != Layer::Backdrop) { *result = HTCLIENT; return true; }  // une zone ne s'efface devant rien
```

`Layer` existe depuis T-039 (`BelowIconLayer` / `AboveIconLayer` / `Backdrop`) : c'était déjà *la* façon de dire « fond » dans ce fichier, et c'est désormais aussi la façon de dire « et seulement le fond s'efface ». Une zone se tient **au-dessus** du contenu du bureau et **dessine le sien** : ce que l'utilisateur voit là est à nous, donc le clic y est à nous. Le fond, lui, garde ses trois clauses (mesuré : `--backdrop-demo` **36/36** / **50/50**, inchangés).

### Les deux vérifications ajoutées (et un aveu)

Dans `--fence-demo --host` (**29/29** → **31/31**) :

1. **« a fence steps aside for nothing -- not even the backdrop's icon list »** — la liste d'icônes du fond est **plantée** sur la surface de la zone (c'est la fuite qu'on garde), la zone est **déplacée sur une des icônes de l'utilisateur**, et on demande le point de cette icône : `HTCLIENT` attendu.
   ⚠️ **Ma première version de cette vérification passait avec le bug en place** : je l'interrogeais au **centre de la zone**, or le centre tombe rarement dans un rectangle d'icône, donc la clause ne se déclenchait jamais. C'est le même piège que la vérification bornée par le clip de T-045 : **une vérification qui ne peut pas échouer ne mesure rien**. Corrigé en la posant **sur** l'icône.
2. **« a fence answers the mouse with the left button down »** — la vraie moitié, celle que l'utilisateur sent : une **vraie** pression du bouton gauche (`SendInput`), puis la question. C'est la seule façon de mesurer que `GetAsyncKeyState` est bien lu à l'instant du clic. Elle n'est posée **que si la fenêtre sous le curseur est bien cette zone** (sinon la pression irait à la fenêtre de quelqu'un d'autre) : quand c'est impossible, la sonde écrit `NOT asked` avec la classe qui couvre le point, plutôt que d'échouer pour une raison étrangère au code (décision 13).

**Prémisse prouvée** : clauses dégardées (patch temporaire, revert) → `rc=1` et **exactement 2 `FAIL`**, les deux nouvelles, chacune à `-1` (`HTTRANSPARENT`).

### Mesure de bout en bout, au vrai curseur

Sur une **copie** du document de l'utilisateur, produit hébergé, souris déplacée par `SendInput` (position du curseur sauvegardée puis rendue) :

- `WM_NCHITTEST` au repos → `HTCLIENT` ; **bouton gauche enfoncé → `HTCLIENT`** (avant : `-1`) ;
- glisser de (+60, +40) en dix pas → la zone **suit au pixel près** (deux décrochages d'aimantation de 3 px en cours de route), position finale **exacte**, et le document **enregistre** le nouveau rect.

Les icônes du bureau, elles, n'ont jamais cessé d'être celles de la coquille : le fond continue de s'effacer sur leurs rectangles (`HTTRANSPARENT`), c'est ce que `--backdrop-demo --host` mesure. Ce qui était mort, c'est **tout ce qu'on touche sur nos surfaces** — et une icône affichée **dans** une zone répondait à la place de l'icône du bureau située dessous.

### Leçons

- **Une règle ajoutée pour un client doit être gardée par ce client** dès l'instant où le code est partagé. `DesktopSurface` est la classe de **deux** rôles : toute clause qu'on y écrit doit dire *pour qui* elle est vraie (`m_layer`). La même famille que T-060 (le plancher d'alpha, valable pour le fond, appliqué aux coins d'une zone) : **le fond et les zones partagent leurs surfaces, pas leurs besoins**.
- **Une sonde qui interroge *au repos* mesure le mauvais instant.** `WindowFromPoint` et `WM_NCHITTEST` sans bouton prouvent le **z-order**, pas l'utilisabilité : la souris pose ses questions **au moment de la pression**. Un geste, un clic et un dépôt se mesurent donc **avec le bouton enfoncé** — d'où `SendInput`, et d'où le fait que ce bug ait vécu une session entière sans être vu (six sondes mesuraient des fenêtres au repos).
- **Une vérification doit planter la condition qu'elle garde** : sans les rectangles d'icônes plantés, la première vérification ci-dessus passait avec le bug. Même piège que le préfixe de 4096 octets (décision 21) et le clip de T-045.

### Comptes (session 37)

build MinGW **0 warning** ; `ctest` **14/14** ; `--fence-demo` **25/25** headless / **31/31** `--host` (29 avant) ; **tous les autres compteurs inchangés** (`--backdrop-demo` 36/36 / 50/50, `--product-probe` 24/24, `--icons-demo` 10/10, `--persist-demo` 26/26, `--nav-demo` 22/22 / 29/29, `--rules-demo` 28/28, `--portal-demo` 30/30 / 32/32, `--snapshot-demo` 11/11, `--screen-demo` 19/19).


## 31. La cellule d'une zone est plus large que son icône, et la légende se replie (T-049, session 41)

Reproche utilisateur, capture à l'appui (session 29) :

> pas assez d'espace pour les icônes : les noms ne s'affichent pas entièrement

**Mesuré dans le code, et c'était exactement ça** : `IconLayout::compute` faisait `cellW = iconSize` — la bande de légende était donc **la largeur de l'icône** — et `IconGrid::paint` dessinait **une seule ligne, élidée au milieu** (`Qt::ElideMiddle`). « Epic Games Launcher » sortait en « Epi...her ». L'élision au milieu est le pire des deux mondes : elle coupe **le début et la fin**, donc elle rend un nom méconnaissable tout en gardant ses deux extrémités.

### Deux moitiés, deux mesures

1. **La cellule se découple de l'icône.** `IconLayout::cellWidthFor(iconSize)` rend **1,5×** la taille d'icône (72 px pour 48), soit l'ordre du bureau Windows (~75 px pour 48 px) — et c'est déjà ce que fait le fond de bureau depuis T-039 (sa `Metrics::cellWidth` vaut 72 pour la même icône). `compute` et `columnsForWidth` prennent désormais une **largeur de cellule** ; `IconGrid::cellWidth()` la **dérive** de `m_iconSize`, donc la cellule et l'icône ne peuvent pas diverger (l'icône est **centrée** dans la cellule, elle n'est pas décalée). Une largeur de cellule plus petite que l'icône est **clampée** : l'icône ne peut pas être rognée par un réglage incohérent.
2. **La légende se replie, et s'élide à droite.** `wrapToLines(text, maxLines, width, widthOf)` remplit chaque ligne **mot entier par mot entier**, ouvre une **deuxième ligne** si nécessaire, et élide la **dernière** ligne **à droite** (`elidedRight`) quand le texte ne tient toujours pas. Un mot plus large que la ligne est coupé **avec la même ellipse** — il n'existe pas de meilleure coupure *à l'intérieur* d'un mot, et le découper en deux serait pire. **Deux lignes au plus** : une troisième ne tiendrait pas dans la bande (`labelHeight` = 34) et passerait sous l'icône de la ligne suivante.

La fonction est **pure** et prend la mesure du texte **en paramètre** (`widthOf`) : `QFontMetrics::horizontalAdvance` à la peinture, une métrique de test (`7 px par caractère`, arithmétique exacte) dans les cas unitaires. Le header **reste sans QtGui**, et le repli se prouve **sans écran** — c'est la même discipline que `IconLayout::hitTest` (T-005) et `FenceGeometry` (T-007).

`hitTest` et le repère d'insertion (`FenceView::paintDropIndicator`) consomment **la même géométrie** : la zone sensible est la cellule entière (icône **et** bande de légende), pas la colonne de l'icône. La leçon de T-045 est tenue — deux consommateurs du même calcul ne peuvent pas diverger.

### Le prix, nommé

À largeur de zone égale, **il y a moins de colonnes** : une zone de 320 px passe de **5** à **3** colonnes. C'est de la place prise ailleurs, et c'est le sens de la demande (la place manquait **pour les noms**). Une zone plus large reprend des colonnes.

### ⚠️ Le piège de sonde : une zone vide ne rend pas la même chose qu'une zone pleine

La première version de la vérification mesurait la légende **par différence avec la même zone rendue sans sa tuile** — la technique exacte de T-045, qui teste « les tuiles sont peintes depuis l'origine du contenu ». Elle a rendu **2 `FAIL`**, dont un « 124 pixels au-delà de la cellule » difficile à croire.

**Cause** : une zone **vide** dessine dans sa zone de contenu son propre message — « *empty fence — no icons yet* » — et c'est **ce message** que la différence mesurait (il s'étale bien au-delà d'une cellule, et sur presque toute la hauteur de la bande). Autrement dit : **la référence n'était pas neutre.** La bonne référence est **la couleur du panneau, lue dans la frame elle-même** (un point du contenu qu'aucune cellule ne peut couvrir, sur une zone qui ne détient qu'une tuile) : un glyphe est alors « tout pixel nettement hors du panneau ». Aucune couleur n'est écrite dans la sonde, aucune seconde zone n'est rendue, et il n'y a plus de contenu tiers à confondre avec la légende. Le seuil (24 en distance RGB) n'a qu'à dépasser le bruit du panneau, qui est **nul** (un seul remplissage).

### Prémisses prouvées (patch temporaire, revert + rebuild)

Les deux moitiés sont prouvées **séparément**, parce qu'elles ne disent pas la même chose :

| patch | résultat | ce que ça prouve |
|---|---|---|
| `cellWidthFor` → `iconSize` | `rc=1`, **2 `FAIL`** (`a caption is drawn wider than its icon` : `0 glyph pixel(s)` ; `a caption stays inside its own cell`) | la vérification mesure bien **la cellule élargie** — elle ne peut pas passer sur l'ancienne géométrie |
| peinture d'une **seule ligne élidée au milieu**, cellule large conservée | `rc=1`, **1 `FAIL`** (`a long caption takes a second line` : lignes `4..13` contre `4..11`) | elle mesure bien **le repli sur deux lignes**, et pas seulement la largeur de la bande |

La vérification « seconde ligne » compare **deux légendes rendues de la même façon** (une longue, une courte) au lieu d'un seuil en pixels : le seuil serait faux pour une autre police, la **différence** ne l'est pas.

### Vérifications

- `ctest` **14/14**, `ui_icon_grid` **20 cas** (15 avant, **+5**) ; build MinGW **0 warning**.
- `--fence-demo` **28/28** headless (25 avant) / **33/33** `--host` (30 avant). Les trois neuves : `a caption is drawn wider than its icon` (**50** pixels de glyphe au-delà de l'icône), `a caption stays inside its own cell` (**0** au-delà de la cellule : la légende ne déborde pas chez sa voisine), `a long caption takes a second line` (lignes **4..26** contre **4..11**).
- **Toutes les autres sondes rejouées** : compteurs **inchangés** (`--backdrop-demo` 37/37 / 51/51, `--persist-demo` 26, `--nav-demo` 22, `--rules-demo` 28, `--portal-demo` 30, `--snapshot-demo` 11, `--screen-demo` 19, `--product-probe` 24). Les compteurs annoncés par la documentation pour `--fence-demo --host` (31) et `--backdrop-demo --host` (50) sont **antérieurs** à cette session : mesurés sur la révision d'origine, ils valent **30** et **51**.
- Aucune écriture dans `%APPDATA%/StarFish/` ; le document de l'utilisateur n'a pas été touché.

### Reste humain

**Regarder** une zone qui contient une icône au nom long. La sonde mesure le repli, la largeur de la bande et l'absence de débordement ; elle ne dit pas si **deux lignes** suffisent pour les noms de l'utilisateur, ni si la coupure d'un mot très long est lisible à l'œil.

## 32. Le panneau opaque a des coins ronds, et le défaut du schéma devient l'opaque (T-074 / T-075, session 43)

Les deux retours de la session 42 traitaient en fait **d'une seule chose** : « fences **opaques par défaut**, pas transparentes, pour l'instant » (T-074) et « nettoyer les coins : les coins sont ronds mais on voit un **débordement type angle droit** » (T-075). Rendre l'opaque par défaut **sans** corriger les coins aurait livré un rectangle. Les deux sont faits ensemble.

### T-074 — un littéral, et rien qui migre

`FenceStyle::background` valait `#CC1F2937` (alpha **204**) depuis T-008 ; il vaut `#FF1F2937` (255). Un matériau *est* cet alpha et la trame du document n'a **aucun** champ `material` (T-057) : une **zone neuve** est donc opaque, et **ce qui est déjà écrit garde son alpha**. Aucune migration, aucun changement de schéma, un seul littéral déplacé.

Ce que ça donne sur les sondes, sans qu'aucune ne l'ait demandé : le document d'une zone neuve porte `#FF1F2937` (`--persist-demo`), le **premier lancement** du produit annonce `material: Opaque` (`--product-probe`), et `--fence-demo` vérifie que le défaut **est** le preset opaque en le **lisant dans `core::FenceStyle`** — jamais en recopiant la chaîne dans la sonde, sinon le test suivant ne suivrait pas.

⚠️ **Ce qui n'est pas touché, et qu'il faut dire** : son document à lui contenait déjà **quatre** zones en `#FF1F2937` (il les avait passées en opaque par le tray/réglages) — la question « faut-il convertir les zones existantes ? » de la ligne de tâche **ne se pose donc pas** : ses zones étaient le cas que T-075 décrit.

### T-075 — ce n'était pas les coins, c'était le chemin GDI

La cause tenait à **une seule expression**, dans `FenceContainer::applyMaterialToSurface` : le mode de la fenêtre était décidé par le matériau (`setPerPixelAlpha(isTranslucent(alpha))`). Un panneau **opaque** partait donc sur le chemin **opaque**, qui blitte en GDI — et GDI **n'a aucun alpha à dépenser** sur les pixels *hors* du panneau arrondi. La frame était remplie en opaque sous le panneau (le commentaire de T-057 le disait : « sinon les coins sortiraient noirs »), et ce remplissage **carré** se voyait derrière une bordure, elle, **arrondie**. Exactement le symptôme rapporté.

Le correctif est un **déplacement de la règle** : ce n'est plus le matériau qui décide du chemin, c'est la **forme** de la fence. Toute fence est un panneau arrondi, donc **toute fence est poussée par pixel** (`addFence` la fait naître ainsi, `applyMaterialToSurface` ne fait plus que le vérifier), et le matériau ne dit plus que **l'opacité du panneau**. `FenceMaterial::isTranslucent()` reste — c'est une classification, les sondes s'en servent — mais elle ne commande plus rien.

Corollaire : le **remplissage** sous le panneau suit **la fenêtre à laquelle la frame est destinée**, jamais le matériau —

| la fenêtre | le remplissage | pourquoi |
|---|---|---|
| per-pixel (le cas normal) | plancher d'alpha **1** | les pixels hors du panneau *sont* la forme : il faut qu'ils laissent passer le papier peint, et le plancher les garde joignables à la souris (ADR-11) |
| repli GDI (le push per-pixel a été refusé) | opaque | GDI ignore l'alpha : un pixel transparent sortirait **noir** |

### La mesure — et une première version qui mesurait sa propre erreur

La mesure que la ligne de tâche demandait (« compter les pixels opaques **hors de la forme arrondie**, dans la frame puis dans la fenêtre hébergée ») est écrite dans `--fence-demo` : sur la frame, **et** sur le bitmap que la fenêtre hébergée a réellement reçu (`DesktopSurface::frame()` — un `PrintWindow` d'une fenêtre layered ne rend pas l'alpha, c'est donc la lecture honnête côté fenêtre).

⚠️ **La première instrumentation était fausse, et de peu.** Elle reconstruisait la forme à la main — un disque de rayon `r` centré sur `(r + 0.5, r + 0.5)`, tolérance 1,5 px pour la bordure antialiasée. Un demi-pixel d'écart sur le centre a suffi : elle annonçait **216 pixels de débordement** sur une frame dont les coins sont propres (mesuré : la carte d'alpha du coin montre bien le remplissage au plancher dès le premier pixel hors de l'arc). Le bon instrument est le **masque que Qt dessine lui-même** : le même `drawRoundedRect`, même rect, même rayon, dans une image vide ; on ne compte que les pixels que le masque **ne couvre pas du tout** (l'arc antialiasé n'est donc jamais compté, il est légitimement en train de se fondre dans le papier peint) et qui sont pourtant peints. Même leçon que le parseur confronté à `QColor` (§26) : **la référence d'une forme appartient à l'outil qui la dessine**, pas à notre reconstruction.

### Prémisses prouvées (patch temporaire, revert + rebuild)

| ce qui est neutralisé | résultat | ce que ça prouve |
|---|---|---|
| le remplissage repassé en opaque partout | `rc=1`, **4 `FAIL`** — `76 pixel(s) painted outside the rounded shape of a 200x160 frame`, `corner alpha 255`, et le coin du matériau très translucide | la mesure attrape bien **le débordement**, et pas autre chose |
| le mode de la fenêtre redécidé par le matériau (l'ancienne règle) | `--host` `rc=1`, **4 `FAIL`** — dont `a fence restored from an opaque document is born per-pixel too : per-pixel=0` | le **chemin de la fenêtre** est porteur, pas seulement le remplissage |

Le **même nombre** apparaît des deux côtés et il est une **coïncidence utile** : **76** pixels, c'est aussi le chiffre qu'ADR-11 avait mesuré en session 35 pour les quatre coins à alpha 0 (`76 of 32000`). Deux instruments indépendants, même région, même cardinal.

### Vérifications

- `ctest` **14/14** ; `ui_fence_material` gagne la lecture du **défaut du schéma** et perd le nom qui mentait (`translucencyIsWhatNeedsThePerPixelPath` → `translucencyOnlyClassifiesTheMaterial`) ; `core_layout_store` vérifie `FenceStyle().background == "#FF1F2937"`.
- `--fence-demo` **30/30** headless (28 avant) / **38/38** `--host` (33 avant). Les neuves : `nothing is painted outside the rounded panel of an opaque fence` (**0** pixel), `an opaque fence's corners are round, not square` (coin **1**, plancher), `an opaque fence keeps the per-pixel push (its corners are round)` (`per-pixel=1`), `the bitmap the hosted window shows has nothing outside its rounded panel either` (**0**), `a fence restored from an opaque document is born per-pixel too`.
- La vérification « *an opaque fence has no transparency at all* » de T-057 est **remplacée** : elle exigeait `corner alpha 255`, c'est-à-dire qu'elle **encodait** le coin carré que l'utilisateur voyait.
- **Toutes les autres sondes rejouées**, compteurs **inchangés** : `--persist-demo` 26, `--rules-demo` 28, `--portal-demo` 30, `--snapshot-demo` 11, `--screen-demo` 19, `--product-probe` 24, `--backdrop-demo` 37. ⚠️ `--nav-demo` **21/22 + 1 `FAIL`** — et ce n'est **pas** une régression : l'instance StarFish de l'utilisateur tournait pendant la session et **tient `Win+Shift+H`**, donc `the quick-hide hotkey registers` échoue côté système (`error 1409`), ce que T-044 a précisément prévu (`a second manager cannot steal the same hotkey` **passe**).
- ⚠️ `build/starfish.exe` est **verrouillé** par l'instance de l'utilisateur : toute la session a été construite et mesurée dans **`build2/`** (le `.gitignore` prévoit le second dossier depuis T-044).
- Build MinGW **0 warning** ; `%APPDATA%/StarFish/layout.json` **non touché** (4 zones, en `#FF1F2937`, inchangées).

### Reste ouvert, et ce que cette session déplace

- **T-050 (les clignotements) change de terrain** : l'opaque passe désormais **aussi** par le chemin per-pixel, donc le watchdog 2 s qui repousse `SetWindowPos` + `pushLayeredFrame()` s'applique à **toutes** les zones, alors qu'il ne touchait que les translucides. La note de `TODO.md` qui annonçait « T-074 les déplacerait sur le chemin GDI » est **fausse depuis cette session** et a été corrigée : c'est un chemin, une seule cadence, et c'est celle-là qu'il faut mesurer.
- **T-058** (« coins : rayon, liseré, ombre ») reste une revue **esthétique**, donc à l'œil — mais son sujet « coins » n'est plus partagé avec T-075, qui est mesuré.
- **Reste humain** : *regarder* le résultat. La sonde mesure que rien n'est peint hors de la forme et que le panneau est opaque là où il doit l'être ; elle ne dit pas si le coin **plaît**.

## 33. Une cadence n'est pas une raison : le watchdog qui répare au lieu de refaire (T-050 / T-077, session 44)

Les deux retours restants de la session 42 sont la **même question posée des deux côtés** : « il y a toujours des clignotements, peut-être à **intervalles réguliers**, de **toutes** les fences » (T-050) et « instabilité dimensionnelle lors des redimensionnements… pendant le glissement du curseur, la fence elle-même paraît instable, bien qu'au final tout aille bien » (T-077). Dans les deux cas la **surface fait un travail visible pour un changement qui n'a pas eu lieu** : elle replace et repousse une zone identique toutes les 2 s, et elle pousse, à chaque pas d'un redimensionnement, une frame qui n'est **pas** à la taille demandée.

Ce qu'il a fallu ajouter d'abord, c'est de quoi **compter** : `paintReport()` ne disait que le chemin GDI (`WM_PAINT`, `WM_ERASEBKGND`), et depuis T-075 il n'y a **plus qu'un** chemin, le push par pixel. La classe expose donc désormais quatre nombres, et deux d'entre eux comptent le travail **évité** — sans quoi « n'avait rien à faire » et « n'a rien fait » sont indiscernables de l'extérieur :

| compteur | ce qu'il compte |
|---|---|
| `layeredPushes()` | les `UpdateLayeredWindow` réussis |
| `mismatchedFramePushes()` | ceux dont la frame **n'avait pas la taille de la fenêtre** — un push fixe aussi l'**étendue** de la fenêtre, donc c'est le défaut lui-même. Doit rester **0** |
| `staleFrameSkips()` | les push **non faits** parce que la frame allait être remplacée à la bonne taille juste après |
| `currentPlacements()` | les `place()` qui ont trouvé la fenêtre **déjà** où elle devait être |

### T-050 — le watchdog demande d'abord, répare ensuite

`watchdogTick()` appelle `place()` **toutes les 2 s** pour **chaque** zone, et `place()` faisait jusqu'ici tout le travail **sans condition** : un `SetWindowPos` et un push complet d'une frame inchangée, pour une fenêtre qui n'avait pas bougé. Mesuré (sonde, 6,5 s au repos, une seule zone, **avant** correctif) : **3 à 4 push par période de 2 s, 0 placement trouvé courant**.

Le correctif tient dans une question : **« est-ce que quelque chose a été dérangé ? »** au lieu de **« est-ce l'heure ? »**. `place()` lit `GetWindowRect`, relit la visibilité, et vérifie l'invariant de placement ; si les trois concordent, il **retourne sans rien faire** (compteur `currentPlacements`).

⚠️ **Deux pièges de mesure dans cette seule vérification, et les deux se sont vus.**

1. **`GetWindowRect` répond en pixels *écran*, `m_rect` est en pixels *client* du Progman** (T-014). Sur un moniteur à gauche du primaire, les origines diffèrent : une comparaison qui suppose `(0,0)` ne matcherait **jamais** — le watchdog garderait son ancien comportement **en silence**, ce qui est exactement le genre de no-op que ce projet passe son temps à mesurer. L'origine cliente est donc **soustraite** (`ClientToScreen`), pas supposée.
2. **Le premier invariant écrit était faux, et la sonde l'a dit tout de suite.** J'avais formulé le placement d'une zone comme « rien au-dessus de moi » (le sens de `HWND_TOP`). Mesure : **3 push en 6,5 s et 0 placement courant** — parce que, dans une sonde qui héberge **plusieurs** zones, seule la dernière créée est au sommet de l'ordre des enfants : toutes les autres ont légitimement un frère au-dessus d'elles. L'invariant du projet n'est pas `HWND_TOP` (c'est **comment** une zone y arrive), c'est **« le papier peint est en dessous »** : en descendant la chaîne `GW_HWNDNEXT` depuis notre fenêtre, on doit **atteindre le `SHELLDLL_DefView`**. Un invariant uniforme pour toutes les couches, celui qu'ADR-12 énonce vraiment.

**Ce que le watchdog ne fait pas moins bien** : la clause de z-order est **exercée**. Le mur, d'abord, mesuré : on **ne peut pas** déranger le z-order d'une zone depuis l'extérieur — `SetWindowPos(fence, HWND_BOTTOM)` est réécrit en `HWND_TOP` par la garde `WM_WINDOWPOSCHANGING` de la surface elle-même (ADR-4, la même garde qui empêche de poser quoi que ce soit au-dessus de la couche d'icônes). La **seule** porte est le papier peint : la sonde met le `DefView` au-dessus de la zone (`SetWindowPos(defView, HWND_TOP)`), vérifie que la zone est bien **sous** lui, et attend une période — la zone est **rendue** (au-dessus du papier peint). La clause de rectangle est exercée de même, par un déplacement depuis l'extérieur (`+40,+40`) : le watchdog la ramène.

### T-077 — la frame périmée, et le rendu qui ne servait à rien

⚠️ **La cause n'était pas dans le widget, elle était dans l'ordre des appels.** `applyVisibleGeometry` faisait : redimensionner le widget → `surface->setGeometry(visible)` → `renderFence()`. Or `setGeometry` → `place()`, et `place()` **poussait la frame courante** — celle de la **taille précédente** — avec `dst = m_rect` (**la nouvelle position**). Et comme `UpdateLayeredWindow` fixe **aussi l'étendue** de la fenêtre d'après la frame, chaque pas d'un redimensionnement faisait : `SetWindowPos` (nouvelle taille) → push (nouvelle position, **ancienne taille**) → `renderFence` → push (la bonne). **Deux push par pas, dont un faux**, dans le même message.

Mesuré (12 pas, geste piloté **par le widget**, `--host`) : `24 push, 12 size-mismatched` avec l'ancien code ; `12 push, 0 size-mismatched, 12 stale skips` avec le correctif. C'est-à-dire : la fenêtre **alternait** entre la nouvelle et l'ancienne taille à chaque mouvement de souris, et rien d'autre ne le disait.

Le correctif est **une condition** : `place()` ne pousse que la frame **à la taille du rect** qu'il vient de demander. Celle qui convient est le travail de l'appelant, qui rend **immédiatement après** — et le refus est **compté**, jamais silencieux.

Deuxième moitié, du même raisonnement : `applyVisibleGeometry()` **re-rendait** la zone à chaque changement de géométrie, y compris un **déplacement** — où la taille ne change pas, donc où l'image ne peut pas changer d'un octet (une frame est le rectangle **de la zone**, T-039). Un drag de barre de titre relançait donc la disposition de la grille et la peinture de toutes les tuiles ~60 fois par seconde sous le curseur, pour rien. Le rendu n'a lieu que si la **taille peinte** (ou le `devicePixelRatio`) a changé — et l'image **est** le souvenir de ce pour quoi elle a été rendue (sa taille, son ratio), il n'y a donc pas de seconde source de vérité à tenir. Le contenu, lui, n'a jamais été l'affaire de cette fonction : il arrive par `contentChanged` (T-040).

⚠️ **Corollaire de méthode** : la vérification de la session 25 (« *moving a fence carries its tiles* », images **identiques octet pour octet** avant/après déplacement) devient **vraie par construction** — un déplacement ne re-rend plus du tout. Elle reste, c'est la définition de ce que « les tuiles voyagent avec la zone » veut dire, mais elle ne peut plus échouer pour la raison qui l'avait fait écrire ; la revendication plus fine (« **un** push, et **le même tampon**, pas une reconstruction ») est mesurée séparément, sur le chemin hébergé, et c'est elle qui tombe au rouge si le rendu revient.

### Prémisses prouvées (patch temporaire, revert + rebuild, `--fence-demo --host`)

| ce qui est neutralisé | résultat | ce que ça prouve |
|---|---|---|
| le court-circuit « déjà en place » (`if (false && …)`) | `rc=1`, **exactement 1 `FAIL`** — `4 layered push(es) in 6.5 s at rest, 0 placement(s) found already current` | la cadence du watchdog est bien ce qui repoussait les zones au repos |
| le push inconditionnel dans `place()` (le comportement d'avant) | `rc=1`, **exactement 1 `FAIL`** — `12 step(s) → 24 push(es), 12 size-mismatched, 0 stale frame(s)`, dont le compteur d'invariant | la frame périmée **est** le défaut d'un redimensionnement, et elle est comptée |
| le rendu inconditionnel dans `applyVisibleGeometry` | `rc=1`, **exactement 1 `FAIL`** — `moving a fence… : 2 push(es), frame rebuilt` | « ne pas redessiner un déplacement » est mesuré (tampon et push), pas supposé |

### Vérifications (session 44)

- `ctest` **14/14** ; build MinGW **0 warning** (reconstruction complète).
- `--fence-demo` **30/30** headless (inchangé) / **42/42** `--host` (**38** avant : +4). Les neuves : `a fence at rest is asked about and not pushed again (T-050)` (**0** push en 6,5 s, **3** placements courants), `the watchdog still repairs a fence moved behind its back`, `the watchdog still lifts a fence the wallpaper was stacked over`, `a resize pushes once per changed size, never a frame of the wrong size (T-077)` (**12 pas → 12 push, 0 mismatched, 12 stale**), `moving a fence re-pushes its image instead of redrawing it` (**1** push, tampon **réutilisé**).
  - ⚠️ La vérification « au repos » exige **les deux moitiés** dans son assertion : zéro push **et** `currentPlacements ≥ 3`. Sans la seconde, elle serait verte sur un watchdog **arrêté** — le piège que ce projet a déjà payé trois fois (T-060, T-068, T-069).
- Compteurs **inchangés** partout ailleurs : `--persist-demo` 26, `--nav-demo` **22/22** headless / **29/29** `--host` (les hotkeys s'enregistrent : **aucune instance de l'utilisateur ne tournait**, `build/` n'était pas verrouillé — contrairement à la session 43), `--rules-demo` 28, `--portal-demo` 30 / **32** `--host`, `--snapshot-demo` 11, `--screen-demo` 19, `--product-probe` 24, `--backdrop-demo` **37** / **51** `--host`, `--icons-demo` 10 (+1 *NOT asked*, décision 13) ; `--crash-demo` **20/20** — le filet de T-046 rejoué sur la nouvelle garde de placement (il masque les vraies icônes, **tue** le processus, et rend le bureau au lancement suivant).
- `%APPDATA%/StarFish/` : **aucun marqueur**, `layout.json` **non réécrit** (mtime d'avant la session).

### Reste humain, et ce qui n'est pas affirmé

- La sonde mesure **une cadence supprimée** et **deux push par pas rendus à un**. Elle ne dit **pas** que le clignotement a disparu : c'est un ressenti, il se juge au vrai curseur, et le watchdog 2 s était le **seul** rythme régulier du code — s'il subsiste un clignotement régulier après ce correctif, le suspect suivant est la composition DWM d'un enfant layered de `Progman`, et il faudra le mesurer autrement.
- Le gain du déplacement (ne plus redessiner) est mesuré **sur le tampon et sur les push**, pas à l'œil : c'est la même famille que T-043, où c'est le vrai curseur qui avait trouvé le bug.

## 34. Le push par pixel ne compose pas sur un enfant de `Progman` : les zones repassent au blit, la forme devient une région (T-079, session 46)

**ADR-15.** Le rapport « *je ne vois plus les fences créées, ou nouvellement créée, **même en matériel opaque*** » a une cause unique — et le « **même en opaque** » était l'indice : **depuis T-075, toute zone passe par `UpdateLayeredWindow`**, sur un **enfant de `Progman`**, et **ce push n'atteint pas l'écran**.

### La mesure qui tranche (six fenêtres de test, sur le bureau de l'utilisateur)

Instrument **hors produit** (`t079spike2.cpp`, compilé **avec le manifeste du projet** lié par `windres` — sans lui, un enfant layered retombe sur le comportement pré-Windows-8 et le test ne prouve rien, ADR-4), six fenêtres `320x160` **côte à côte**, même rect, même processus, toutes enfants de `Progman` :

| variante | chemin | vue à l'œil |
|---|---|---|
| rouge | `WS_EX_LAYERED` + `LWA_ALPHA, 255` + blit GDI | **oui** |
| vert | idem + `SetWindowRgn` arrondie | **oui**, coins ronds |
| bleu | `LWA_ALPHA, 170` | **oui**, translucide |
| jaune | `LWA_ALPHA, 170` + région | **oui** |
| magenta | `UpdateLayeredWindow` (per-pixel) | **non** |
| cyan | enfant **nu** (sans `WS_EX_LAYERED`) | **non** |

Trois instruments concordent : l'œil de l'utilisateur, **sa capture d'écran**, et un échantillonnage `CopyFromScreen`/`GetPixel` toutes les 1,5 s (le magenta et le cyan n'apparaissent à **aucun** instant en 2 min 30).

### Pourquoi c'était silencieux

Le projet n'avait **jamais** mesuré qu'un push `UpdateLayeredWindow` **apparaisse** — seulement qu'il **réussisse**. La sonde de la session 9 dit « *the same push succeeds — even on a cross-process child of Progman* » : c'était une **réponse**, pas une **livraison** (même famille que T-060, T-068, T-069). Aggravant : le mode per-pixel **désactive le blit dans `WM_PAINT`**, donc une zone dont le push ne compose pas n'a **aucun autre chemin** — elle n'est pas dégradée, elle est **muette**. Et T-075, en envoyant *toutes* les zones sur ce chemin (l'opaque comprise, pour ses coins ronds), a rendu **toutes** les zones invisibles.

### La décision (ADR-15)

- Le **matériau est l'alpha uniforme de la fenêtre** (`SetLayeredWindowAttributes(hwnd, 0, a, LWA_ALPHA)`) : c'est tout ce qu'un panneau d'une seule couleur peut dire.
- Le **panneau arrondi est une *région*** (`SetWindowRgn` + `CreateRoundRectRgn`) : c'est **elle** qui retire les pixels hors du panneau.
- La **frame est opaque** et **blittée** par `WM_PAINT` ; elle ne porte plus la forme.
- Le per-pixel reste, **marqué chemin de développement**, pour le seul fond de bureau plein écran — que le produit **n'héberge plus** (T-071). Rien de ce que l'utilisateur voit n'en dépend.

`place()` régénère la région quand la **taille** change (une région est en coordonnées **fenêtre**) et **l'inclut dans sa garde « déjà en place »** (T-050) : sans cette clause, la garde était satisfaite par une fenêtre au bon rect et la région n'était **jamais** construite — mesuré (`region type ERROR` sur une zone bien placée), et c'est la **sonde** qui l'a dit, pas une relecture.

### Les prix, écrits plutôt que découverts plus tard

1. **Une région est 1 bit** : le bord du coin n'est **pas anticrénelé**. La revue esthétique est **T-058**.
2. **Les pixels du coin ne sont plus ceux de la fenêtre** : un clic pile sur un coin arrondi tombe au bureau. **ADR-11** (le plancher d'alpha du coin) visait exactement ce cas et **devient sans objet** — il n'existait que parce que le chemin per-pixel arrondissait les coins.
3. **La translucidité est uniforme par fenêtre** et non par pixel : pour un panneau d'une seule couleur, c'est la même chose — et c'est ce que le matériau voulait dire depuis le début.

Gain collatéral : **plus aucune fenêtre n'est recréée** quand le matériau change (T-057 recréait la surface pour changer de mode) — c'est un appel.

### Vérification

- `ctest` **14/14** ; build MinGW **0 warning**.
- `--fence-demo` **28/28** headless / **41/41** `--host`, toutes vertes.
- ⚠️ **Les vérifications qui encodaient T-075 ont été *remplacées*, pas ajustées** : elles exigeaient `per-pixel = 1`, un `corner alpha == plancher` et « 0 pixel peint hors du panneau ». La forme est désormais vérifiée **sur la région que Windows détient** (`GetWindowRgn` + `PtInRegion` au coin et au centre), et la frame doit être **opaque** au coin — l'alpha est passé du côté de la fenêtre.
- **Hors produit, à l'écran** : les pixels au centre des zones passent du **papier peint** (`#010300`, `#182b0f`, `#499003`) au **panneau** (`#1f2937`). L'instrument qui avait servi à décrire le défaut sert à constater le correctif.
- ⚠️ **Le compteur de placements a d'abord dit 143 au lieu de 3** : l'utilisateur bougeait sa souris **sur les zones** pendant la sonde. Rejoué sans lui : **3**. Une mesure de repos se prend quand rien ne touche le sujet — même famille que « ne pas juger sur une capture sans vérifier que le bureau est visible ».

## 35. ADR-16 — le menu d'une zone : le panneau est à nous, les tuiles restent au Shell (T-076 / T-064, session 47)

La demande est de la session 42 : « un menu **rien qu'à nous**, de type fermer, afficher/masquer barre des titres et je ne sais quoi ». Jusqu'ici une zone ne pouvait montrer **qu'un** menu : celui du **Shell**, pour la **tuile** sous le curseur (`ShellContextMenu::showForPath`, T-006). La barre de titre n'avait aucun menu, et le panneau vide non plus — et le choix du matériau **par zone** n'existait que dans la fenêtre de réglages que **T-059** veut déclasser (**T-064**).

### La règle (ADR-16)

- **Sur une tuile** : le menu du Shell. Inchangé, et il n'y a rien à décider — un menu Shell a besoin d'un **chemin**, et une tuile en a un.
- **Partout ailleurs dans la zone** (barre de titre, panneau vide, et la barre entière d'une zone **enroulée**) : le menu est **le nôtre**.
- **Le contenu est une donnée pure** (`ui::FenceMenu::entries()` / `materialEntries()` : `Kind`, `Action`, libellé, coché, actif) — testable sans écran, comme `fence_geometry` et `fence_material`. Le **mécanisme** (`QMenu`) et l'**effet** vivent dans le produit, et chaque entrée appelle **la méthode que le tray appelle déjà** (`applyMaterialToFence`, `onRollUpToggled`, `onRenameFence`, `onRemoveFence`) : le menu n'ouvre **pas de troisième chemin** vers le document.
- **Une entrée grisée est une promesse** : rien n'est offert que le produit ne sait pas faire. C'est pourquoi le **second demi de T-076** — masquer la barre de titre — est **absent** et non grisé : une zone se déplace **par sa barre** (T-007), donc masquer la barre troquerait une entrée contre un geste. Les couleurs/rayon par zone (**T-080**) arriveront ici, quand ils existeront.
- Le contenu livré : `Material ▸` (trois presets, **radio** sur celui en vigueur, et **aucune** marque si l'alpha est custom), `Roll up` / `Roll down`, `Rename...`, une séparation, `Remove this fence` (dernière et isolée : une glissade de souris ne doit pas pouvoir l'atteindre).

### La mesure qui compte : le menu **atteint l'écran**

C'est la leçon de la session 46 (T-079) appliquée d'avance : un widget qui **existe** n'est pas un menu **composé**. La sonde `--fence-menu-probe` (nouvelle) héberge **une vraie zone** sur le vrai bureau, **poste un vrai clic droit** à la surface (`WM_RBUTTONDOWN`/`UP` aux coordonnées clientes de la barre de titre), puis interroge **trois instruments** pendant que le menu est ouvert :

| instrument | lecture |
|---|---|
| les **pixels** (`grabWindow` avant / pendant / après) | **17 678** des 18 012 pixels du rect du menu ont changé (**98 %**) |
| le **hit test du système** (`WindowFromPoint` au centre) | répond **notre** popup — le menu possède la souris, comme un menu doit le faire |
| l'**état Qt** (`activePopupWidget`) | un `QMenu` visible, `hwnd` réel, rect `360,215 158x114` |

Et après fermeture : **0** pixel du rect diffère de la ligne de base. Trois lectures, une seule conclusion : le menu s'affiche, prend la souris, et rend les pixels en partant. ⚠️ Un instrument de plus a servi : la **capture relue à l'œil** (`menu_up.png`) montre bien `Material ▸`, `Roll up`, `Rename...`, la séparation et `Remove this fence` — les **libellés** ne sont pas mesurés par les pixels.

### Ce que ça déplace

- **T-064 est livré** : le matériau **par zone** a enfin un endroit naturel. La condition que **T-059** posait (« T-064 doit exister avant de retirer la liste des zones ») est remplie.
- **T-080** (couleurs + rayon par zone) vit désormais **dans ce menu** : il ne lui manque plus d'endroit, seulement le schéma (couleur de barre de titre) et l'interface.
- **T-076 reste ouvert** d'une entrée : masquer/afficher la barre de titre, qui a besoin d'un geste de déplacement qui survive à sa disparition.

### Vérification (session 47)

- `ctest` **15/15** — `ui_fence_menu` est nouveau (**9 cas**) : une entrée par action, l'ordre (destructif dernier, derrière sa séparation), le libellé d'enroulement qui **nomme le geste**, le radio qui **suit le document** (trois presets), **aucune** marque sur un alpha custom, et **aucune** entrée grisée.
- `--fence-demo` **35/35** headless (28 avant) / **48/48** `--host` (41 avant) : les sept vérifications ajoutées portent sur le **chemin** (un clic droit atteint le produit avec le bon id **et** le bon point d'écran, un clic gauche n'atteint rien, une zone enroulée reste joignable) et sur la **fidélité du contenu** à l'état de la zone.
- `--fence-menu-probe` **7/7** ; build MinGW **0 warning**.
- ⚠️ **Piège de test payé** : `find()` rendait un pointeur **dans la liste temporaire** de `FenceMenu::entries()` — les deux libellés comparés vivaient dans le **même tampon libéré**, et le cas a échoué en accusant le modèle. La liste est gardée dans une locale.
- **Reste humain** : le geste au **vrai curseur** (clic droit sur la barre, choix dans le sous-menu `Material ▸`, renommage, « Remove this fence ») — la sonde mesure l'**ouverture** du menu, pas le **sous-menu** ni les actions qu'on y choisit.





## 36. La cellule d'une zone s'élargit pour le plus long des noms (T-078, session 48)

### Ce qui restait après T-049

T-049 (§31) a sorti la cellule de la largeur de l'icône et l'a portée à **1,5×** l'icône (72 px pour 48), avec une légende sur **deux lignes au plus**, élidée **à droite**. C'est exactement ce qu'il faut au nom moyen, et le retour utilisateur de la session 42 ajoutait : « lisible pour la plupart des noms, **manque encore un peu pour les plus longs** ». Un nom qui ne tient pas sur deux lignes à 72 px est élidé, à droite, **définitivement** — la largeur était une constante, elle ne pouvait pas dépendre du texte.

### La règle

Des quatre pistes écrites dans la tâche (cellule **2×**, légende sur **3 lignes**, **police plus petite**, largeur **dérivée du plus long libellé**), c'est la **dernière** qui a été prise — la seule qui ne prenne rien à personne — et elle a été **bornée** :

| | valeur | pourquoi |
|---|---|---|
| plancher | `cellWidthFor(icon)` = **1,5×** | la règle de T-049, inchangée : un nom qui tenait tient toujours, dans la **même** cellule |
| cible | la plus petite largeur à laquelle **chaque** libellé de la zone se replie sur ≤ 2 lignes **sans élision** | c'est le texte qui décide, pas une constante choisie pour lui |
| plafond | `maxCellWidthFor(icon)` = **2×** | un nom monstrueux ne doit pas appauvrir toute la grille (le plafond est le point où la largeur cesse de servir : au-delà, la légende est élidée quoi qu'on fasse) |

`IconLayout::cellWidthForCaptions()` est **pure** et, surtout, elle interroge **`wrapToLines`** — la fonction avec laquelle le peintre replie réellement. La géométrie ne peut donc pas contredire ce qui est dessiné, et c'est la même propriété dont §31 s'était servi pour l'élision. Elle balaie les largeurs **qu'un repli peut distinguer** (la largeur de chaque groupe de mots consécutifs) au lieu de chercher à l'aveugle : entre deux de ces largeurs, le repli glouton prend les mêmes décisions.

### Le coût annoncé n'existait pas

La tâche écrivait que dériver la largeur du texte demanderait « un aller-retour modèle → géométrie ». En réalité la **grille possède déjà ses textes** (`IconGrid` porte les items) : il ne lui manquait que la **police**, et c'est la seule chose qui a été ajoutée — `IconGrid::setBaseFont()`, que `FenceView` appelle dans son constructeur. Une seule source, donc :

- `cellWidth()` mesure avec cette police ;
- `paint()` **repose** la même sur le peintre avant d'écrire les légendes ;
- `hitTest()`, appelé sur un événement souris (**aucun peintre en vue**), mesure le même texte — donc les cellules touchées sont celles qui ont été peintes, ce qui est la contrainte que §31 avait déjà posée pour la géométrie de la grille.

Une grille **sans** police (test unitaire, grille sans vue) ne mesure rien et garde la cellule fixe d'avant : le repli est une décision, pas un accident.

Il n'y a **aucun champ de document** : la largeur est dérivée des items, jamais stockée — et un test vérifie qu'elle **redescend** quand le nom long quitte la zone (ce n'est pas un cliquet).

### Vérification (session 48)

| lecture | avant | après |
|---|---|---|
| `ctest` | 15/15 | **15/15** — `ui_icon_grid` **25 cas** (+5 : plancher, largeur exacte, plafond, grille sans police, grille avec) |
| `--fence-demo` headless | 35/35 | **36/36** |
| `--fence-demo --host` | 48/48 | **50/50** (dont la vérification de z-order du watchdog, **conditionnelle**, demandée cette fois) |
| la cellule d'une zone à nom long | 72 px, légende élidée | **96 px** (le plafond), l'encre de la légende à **106 px** à droite de l'icône, **0** au-delà de sa cellule |
| la cellule d'une zone à noms courts | 72 px | **72 px** — inchangée |
| toutes les autres sondes | — | **inchangées** (26/28/30/11/19/24/37/22) |

**Prémisse prouvée** : la dérivation neutralisée (`return floorWidth;` en tête de `IconGrid::cellWidth()`), `--fence-demo` tombe à `rc=1` avec **exactement 1 `FAIL`** — « long caption cell 72 » là où la vérification attend 96. Les trois vérifications de légende de T-049, elles, restaient vertes : ce sont bien les nouvelles qui portent la revendication.

⚠️ **Hors périmètre, dit** : le **fond de bureau** (`BackdropLayout`, chemin de développement depuis ADR-13) garde sa cellule fixe de 72 px. T-078 ne concerne que les zones.
