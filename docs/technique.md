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
- **FenceContainer** (`src/ui/fence_container.{hpp,cpp}`, T-007) : propriétaire du **cycle de vie des zones**. Une `FenceWindow` = id + titre + moniteur + `QRect` (**le corps déplié**, même enroulé) + `FenceStyle` + `FenceView` + `DesktopSurface` optionnelle + la dernière frame rendue. Deux modes : **headless** (défaut — géométrie et widget seulement, aucun bureau requis, ce qui rend les tests et la sonde possibles) et **hébergé** (`setHostOnDesktop(true)` — chaque zone reçoit sa `DesktopSurface`, `IDropTarget` OLE câblé comme en T-006-3). `removeFence()` supprime la zone, **jamais les fichiers**. Depuis T-008 il porte aussi le mapping vers/depuis le document : `toLayout()` / `applyLayout()`. Depuis T-009 il porte l'état de navigation (roll-up, visibilité globale, Peek) — voir §11 et ADR-8. Depuis T-010 il porte aussi les **règles** (`setRules()` / `rules()`) et les applique (`classify()`, `applyRulesTo()`), sans jamais toucher au disque — voir §12.
- **FenceGeometry** (`src/ui/fence_geometry.hpp`, T-007) : géométrie **pure** du cycle de vie et de la navigation — `FenceHandle`, `hitTest`, `resize` (avec taille minimale), `snap` (bords + centres, écran et autres zones), `clampToBounds`, `visibleRect` (roll-up) et `applyRequest` (la demande de déplacement/redimensionnement résolue en rect stocké). Aucun widget, aucun Win32 : c'est ce qui rend les règles de T-007 **et** de T-009 unit-testables.
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
| Menu contextuel | `IContextMenu` (shell) — phase 2 |
| Glisser-déposer sortant | `CF_HDROP` via OLE (phase 2) |
| DPI | Per-monitor DPI v2 (`SetProcessDpiAwarenessContext`) |
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
  - **Manifeste applicatif obligatoire** (`src/starfish.manifest` + `src/starfish.rc`, `enable_language(RC)`) : MinGW n'a **pas** d'étape manifeste par défaut, contrairement à MSVC. Sans lui, Windows applique le comportement pré-Windows 8, `WS_EX_LAYERED` est refusé sur les fenêtres enfants et **les zones hébergées ne s'affichent jamais** — silencieusement (ADR-4, finding 4). Ne pas retirer ce fichier du `CMakeLists.txt` : la panne ne produit aucun message.
- **CI** : GitHub Actions `windows-latest` → configure/build + tests unitaires (`RuleEngine`, `LayoutStore`) à chaque PR.
- **Packaging** : zip portable (`windeployqt`) pour les nightly ; installeur (WiX ou Inno Setup) pour les releases ; publication `winget` plus tard.
- **Tests** : unitaires pour la logique pure (règles, sérialisation, verdict de z-order, ordre/fusion des entrées du bureau, listage d'un portail, snapshots) ; manuels documentés pour l'intégration shell (checklist dans `devlog/`).
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

### Vérification

- `tests/test_fence_geometry.cpp` — **14 cas** : classement des régions, priorité des coins, croissance est/sud, épinglage du bord opposé, clamp ouest et nord, no-op sur un handle non-redimensionnant, aimantation bords/centre/autre zone, absence d'aimantation au-delà du seuil, seuil 0, `clampToBounds` (dedans, débordement, négatif, surdimensionné).
- `starfish --fence-demo [--host] [--seconds N]` — scénario **non interactif** qui exerce la vraie chaîne `FenceView → signaux → FenceContainer` en envoyant de vrais `QMouseEvent` au widget ; rapport PASS/FAIL dans `build/fence_demo.txt`, code de sortie 0/1. Mesuré : **8/8 PASS**, dont aimantation 394 → **400** (bord droit d'une autre zone), (3,5) → **(0,0)**, clamp 50×40 → **160×120**, drag de barre de titre (-77,+37) → **(0,120,320,240)**, poignée bas-droite +60,+60 → **380×300**, suppression → count 1.
- `--fence-demo --host` héberge **3 surfaces simultanément** (`layered ex 0x80000`, `alpha 1 (err 0)`, `placed 1`) : la coexistence de plusieurs `DesktopSurface` sur le même Progman est donc vérifiée, la zone de démo étant remplie des 83 entrées réelles du bureau.

### Limites assumées

- Le geste à la souris **sur le bureau hébergé** (barre de titre / poignées) n'est pas vérifiable depuis l'agent : la sonde prouve le câblage, pas le ressenti. Validation humaine, comme T-021.
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

**Éléments persistés mais pas encore agis** : `monitor` (T-014), `rolledUp` (F-020 / T-009), l'alpha de `style.background` (T-017). Ils voyagent déjà, donc le schéma ne bougera pas quand ces tâches arriveront. Le rayon et la couleur RGB du fond, eux, sont **appliqués** (`FenceView::setPanelColor` / `setCornerRadius`), et le remplissage opaque de `renderFence` est dérivé de la couleur du panneau : les deux ne peuvent plus diverger.

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

### Langue

Les libellés passent par `tr()` avec des chaînes sources **en anglais** : l'i18n (F-036 / T-014) devient un fichier de traduction au lieu d'une chasse. L'interface est donc en anglais pour l'instant — assumé et noté.

### Vérification

- `starfish --product-probe` — **16/16 PASS**, `rc=0`, rapport `build/product_probe.txt`, **sans bureau, sans tray, sans hotkey**, contre un document jetable dans `build/product_probe/` **supprimé en fin de sonde**. Mesuré : démarrage, matérialisation de la première zone, existence de la fenêtre de réglages, renommage, enroulement, bascule des icônes (mémorisée **et** réversible), placement d'une nouvelle zone **sans recouvrement** (x = 423, à droite de la précédente), suppression, écriture du document, contenu relu, **restauration au second démarrage avec le titre conservé**, nettoyage.
- **Mode produit réel** (`--product --layout build/product_live/layout.json --seconds 6`, mesuré) : surface hébergée — `layered: ex 0x80000 (expected 0x80000); alpha call 1 (err 0)` puis `surface 0xd0d7a hosted at 80,80 320x240 (layered, alpha 255, placed 1)` ; watcher sur les **2 dossiers Bureau** (Desktop + Public Desktop, 86 entrées connues) ; hotkeys `Alt+Win+Space` et `Shift+Win+H` **enregistrés** ; `saved 1 fence(s)` à la sortie ; document relu conforme ; `native desktop icons restored` **absent** (les icônes n'avaient pas été masquées).
- Non-régression (session 21) : `ctest` **9/9** ; `--fence-demo` **8/8** ; `--persist-demo` **26/26** ; `--nav-demo` **22/22** headless / **29/29** `--host` ; `--rules-demo` **28/28** ; `--portal-demo` **29/29** / **31/31** `--host` ; `--snapshot-demo` **11/11**.

### Limites assumées

- **Pas de création de zone par geste** (F-001) : dessiner une zone sur le bureau vide demande un **hook souris global** — la création passe par le tray et les réglages (**T-028**, comme le geste de quick hide).
- **Le premier lancement ne remplit pas la zone** : y verser le bureau revendiquerait des fichiers que l'utilisateur n'a pas mis là, et il n'existe **aucune représentation d'une icône hors zone** (F-011/F-007, arbitrage non tranché — audit point 2). Les icônes entrent par glisser-déposer depuis l'Explorer.
- Hotkeys **en dur** (T-030) ; **pas de libellés français** (T-014 / F-036) ; pas de lancement au démarrage (F-035) ; la fenêtre de réglages ne configure rien qui survive à la session ; les placements restent en coordonnées client de `Progman` (**moniteur principal**, T-014).
- **Un essai à la main reste nécessaire** : la souris sur une zone hébergée, l'icône de notification et la fenêtre de réglages ne se valident pas depuis l'agent (bureau libre + yeux, décision 13).


