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
│  ├─ DesktopHost    héberge les zones sur le bureau │
│  ├─ DesktopSurface surface hébergée (rendu)        │
│  ├─ FenceContainer cycle de vie des zones (T-007)  │
│  ├─ FenceView      rendu + interaction d'une zone  │
│  ├─ FenceGeometry  géométrie pure (hit-test, snap) │
│  ├─ IconGrid       grille d'icônes d'une zone      │
│  ├─ RuleEngine     évaluation des règles (F-017)   │
│  ├─ LayoutStore    lecture/écriture JSON atomique  │
│  ├─ DesktopScanner lecture du dossier Bureau       │
│  ├─ ShellIconCache icônes shell (image list)       │
│  ├─ ShellBridge    menu contextuel, DnD            │
│  └─ Tray/Settings  réglages, hotkeys (F-033)       │
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
- **DesktopSurface** (`src/platform/win/desktop_surface.{hpp,cpp}`) : **le chemin de rendu retenu (T-020, §7 piste (a))**. Surface Win32 brute hébergée sous `Progman` qui possède son propre `WNDPROC` : elle blitte sur `WM_PAINT` une image produite par Qt hors écran (`QWidget::render()` dans un `QImage`). Qt ne touche jamais au HWND hébergé, ce qui contourne le blocage d'ADR-4 finding 3. `WS_EX_LAYERED` est posé dès `CreateWindowExW` (finding 4). Expose `attach()` / `setFrame()` / `detach()`, `syncZOrder()`, un garde `WM_WINDOWPOSCHANGING` (le `hwndInsertAfter` est réécrit en `SHELLDLL_DefView`) et un watchdog 2 s. Compteurs de peinture (`paintReport()`) pour diagnostiquer un « rien à l'écran ».
- **DesktopScanner** (`src/platform/win/desktop_scanner.{hpp,cpp}`) : lit les deux dossiers Bureau fusionnés par le Shell (`FOLDERID_Desktop` + `FOLDERID_PublicDesktop`) et remplit un `core::DesktopItem` par entrée — nom d'affichage Shell, cible des `.lnk` (via `IShellLink`) et des `.url`, attributs caché/système, taille, date. T-004.
- **ShellIconCache** (`src/platform/win/shell_icon_cache.{hpp,cpp}`) : icône telle que le Shell la dessinerait, via la liste d'images système (`SHGetFileInfo` → `SHGetImageList` → `IImageList::GetIcon` → `QImage::fromHICON`), mise en cache par (chemin, taille). T-004.
- **FenceContainer** (`src/ui/fence_container.{hpp,cpp}`, T-007) : propriétaire du **cycle de vie des zones**. Une `FenceWindow` = id + titre + moniteur + `QRect` + `FenceStyle` + `FenceView` + `DesktopSurface` optionnelle + la dernière frame rendue. Deux modes : **headless** (défaut — géométrie et widget seulement, aucun bureau requis, ce qui rend les tests et la sonde possibles) et **hébergé** (`setHostOnDesktop(true)` — chaque zone reçoit sa `DesktopSurface`, `IDropTarget` OLE câblé comme en T-006-3). `removeFence()` supprime la zone, **jamais les fichiers**. Depuis T-008 il porte aussi le mapping vers/depuis le document : `toLayout()` / `applyLayout()`.
- **FenceGeometry** (`src/ui/fence_geometry.hpp`, T-007) : géométrie **pure** du cycle de vie — `FenceHandle`, `hitTest`, `resize` (avec taille minimale), `snap` (bords + centres, écran et autres zones) et `clampToBounds`. Aucun widget, aucun Win32 : c'est ce qui rend les règles de T-007 unit-testables.
- **JsonStore / LayoutStore** (`src/core/jsonstore.{hpp,cpp}`, `src/core/layout_store.{hpp,cpp}`, T-008) : seule source de vérité pour « quelle icône dans quelle zone, où ». `JsonStore` ne connaît que le format (schéma 1, écriture atomique, sérialisation déterministe) ; `LayoutStore` possède le document (chemin, chargement, écriture différée — voir §10 et ADR-7). Les fichiers, eux, restent où ils sont sur le disque.
- **RuleEngine** : pure, testable sans UI (entrée = métadonnées fichier + règles ; sortie = zone cible).
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
| Hotkeys globales | `RegisterHotKey` (Win+Espace pour Peek = F-021) |

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
- `items` est la **composition de la zone** (voir ADR-7) ; `kind` vaut `"file"` jusqu'à F-024 (T-011), une entrée `"portal"` relue étant résolue comme le dossier qu'elle désigne.
- `rules` est réservé à F-017/F-018 (T-010) et n'est pas encore écrit.
- Règle d'or : `moveFiles: false` par défaut — les règles ne **déplacent jamais un fichier sur le disque** sans opt-in explicite.

## 6. Build, packaging, qualité

- **Build** : CMake ≥ 3.24, Qt 6.8, compilateur MSVC 2022 (cible de référence, celle de la CI). **Toolchain locale de développement** (depuis la session 7, la machine de dev n'a ni MSVC ni Qt) : Qt 6.8.3 `win64_mingw` + MinGW 13.1.0, installés sans droits admin via `aqtinstall` dans `C:/Users/<user>/Qt` — voir `devlog/2026-10-13.md` session 7 pour les commandes exactes. Le code compile proprement sur les deux ; MSVC reste la cible publiée.
  - Portabilité : MinGW-w64 déclare `IID_IImageList` mais ne fournit aucun symbole dans ses bibliothèques d'import → l'IID est défini localement dans `shell_icon_cache.cpp` (valeur reprise du SDK). MSVC utilise le symbole du SDK.
  - **Manifeste applicatif obligatoire** (`src/starfish.manifest` + `src/starfish.rc`, `enable_language(RC)`) : MinGW n'a **pas** d'étape manifeste par défaut, contrairement à MSVC. Sans lui, Windows applique le comportement pré-Windows 8, `WS_EX_LAYERED` est refusé sur les fenêtres enfants et **les zones hébergées ne s'affichent jamais** — silencieusement (ADR-4, finding 4). Ne pas retirer ce fichier du `CMakeLists.txt` : la panne ne produit aucun message.
- **CI** : GitHub Actions `windows-latest` → configure/build + tests unitaires (`RuleEngine`, `LayoutStore`) à chaque PR.
- **Packaging** : zip portable (`windeployqt`) pour les nightly ; installeur (WiX ou Inno Setup) pour les releases ; publication `winget` plus tard.
- **Tests** : unitaires pour la logique pure (règles, sérialisation, verdict de z-order, ordre/fusion des entrées du bureau) ; manuels documentés pour l'intégration shell (checklist dans `devlog/`).
- **Style** : clang-format (LLVM, 2 espaces), commits en anglais, Conventional Commits.
- **Diagnostics CLI** (binaire en sous-système GUI) : `--dump`, `--list-desktop [--icons] [--icon-size N] [--icon-dump DIR]`, `--host-demo [--seconds N] [--hide-icons]`, `--render-probe [--surface] [--no-parent] [--show-desktop]`, `--fence-demo [--host] [--seconds N]`, `--persist-demo [--host] [--seconds N] [--layout FILE]`, `--context-menu-verbs PATH`. Si l'appelant a redirigé stdout/stderr (fichier ou pipe), `main.cpp` **ne détourne pas** la sortie vers `CONOUT$` et force `QT_ASSUME_STDERR_HAS_CONSOLE=1` : sans quoi Qt route `qCInfo`/`qCWarning` vers `OutputDebugString` et tous les diagnostics CLI sont perdus.
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
- `kind: "portal"` n'est pas encore produit (F-024 / T-011).

