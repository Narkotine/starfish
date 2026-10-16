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

### Le fond ne prend aucun geste

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

Headless : 86 entrées / 86 visibles ; 86 libres sans zone, 83 avec une zone de 3 ; la comparaison **casse + séparateur** retire une icône de plus ; **deux rendus complets ne changent pas un octet du document** (65 octets identiques) ; le document porte 3 chemins quand le bureau en a 86 ; supprimer la zone rend les 86 ; 16 lignes puis colonne suivante à x=98 ; la frame 2560×1440 est peinte à **231 738 / 3 686 400 px (6 %)** et le compte **tombe** à 223 887 quand une zone réclame 3 icônes. En `--host` : `layered ex 0x80000`, **push per-pixel confirmé** (aucun repli opaque), couverture de l'écran virtuel, `HTTRANSPARENT`, **z-order `backdrop [1]` vs `DefView [0]`**, et « turning it off leaves no window behind ». **15/15** headless, **21/21** en `--host`. Elle **ne masque jamais** les icônes natives.

### T-017 est levé au passage

Le chemin per-pixel était « codé, jamais validé visuellement » depuis la session 9 (§8). Le fond l'exerce sur une fenêtre **plein écran** enfant de `Progman` — plus grand cas de figure que ce que la session 9 avait mesuré — et la sonde **mesure** que `UpdateLayeredWindow` a réussi. La conclusion de la session 9 est donc confirmée à l'exécution, et la validation à l'œil n'est plus qu'un confort.

### Limites assumées

- **Aucune interaction** sur les icônes du fond (T-042) : c'est le prix de l'ADR-3, et le click-through est ce qui l'annonce honnêtement.
- **Les icônes libres sont ré-agencées en flux** : réclamer une icône décale les suivantes, au lieu de laisser un trou comme le fait le bureau Windows. Lire les vraies positions demanderait `LVM_GETITEMPOSITION` sur la liste native **avant** de la masquer, et ne couvrirait pas les fichiers ajoutés ensuite.
- **Une seule échelle pour tout l'écran virtuel** (le `dpr` du moniteur primaire) : sur un poste à **DPI mixte**, les icônes du fond sont à la taille du primaire. Même famille que T-014a : validation humaine à deux écrans.
- **La validation à l'œil reste humaine** : la sonde compte des pixels, elle ne juge pas une impression (position, lisibilité, scintillement).
