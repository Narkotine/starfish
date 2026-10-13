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
│  ├─ FenceView      rendu + interaction d'une zone  │
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

- **DesktopHost** (`src/platform/win/desktop_host.{hpp,cpp}`) : seul point d'entrée vers le placement bureau (parentage sous `Progman`, z-order sous `SHELLDLL_DefView`, flag layered, masquage des icônes natives). Implémenté en Phase 1 (T-016) ; le rendu effectif des zones est encore ouvert (ADR-4, finding 3).
- **DesktopScanner** (`src/platform/win/desktop_scanner.{hpp,cpp}`) : lit les deux dossiers Bureau fusionnés par le Shell (`FOLDERID_Desktop` + `FOLDERID_PublicDesktop`) et remplit un `core::DesktopItem` par entrée — nom d'affichage Shell, cible des `.lnk` (via `IShellLink`) et des `.url`, attributs caché/système, taille, date. T-004.
- **ShellIconCache** (`src/platform/win/shell_icon_cache.{hpp,cpp}`) : icône telle que le Shell la dessinerait, via la liste d'images système (`SHGetFileInfo` → `SHGetImageList` → `IImageList::GetIcon` → `QImage::fromHICON`), mise en cache par (chemin, taille). T-004.
- **LayoutStore** : seule source de vérité pour « quelle icône dans quelle zone, où ». Format JSON versionné (voir §5). Les fichiers, eux, restent où ils sont sur le disque.
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

**Séquence Win32 de référence** (validée session 5, dans cet ordre) :
1. Créer une fenêtre `WS_POPUP` sans décoration.
2. `SetParent(hwnd, Progman)` ; style : ajouter `WS_CHILD`, retirer `WS_POPUP`.
3. Ex-style : retirer `WS_EX_TOPMOST` ; **ajouter `WS_EX_LAYERED`** puis `SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)`.
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

2. **Windows refuse d'ajouter `WS_EX_LAYERED` après le reparentage, silencieusement.** Sur une fenêtre `WS_CHILD` de `Progman` (fenêtre d'un autre processus), `SetWindowLongPtrW(hwnd, GWL_EXSTYLE, …)` retourne 0, laisse `GetLastError` à 0 et **ne change pas le style**. `GWL_STYLE` continue de fonctionner — d'où le diagnostic trompeur « z-order correct, rien à l'écran ». À l'inverse, le **retrait** du flag passe. Conséquence : `attach()` ne doit jamais tenter d'*ajouter* le flag ; il le vérifie (`qCCritical` si absent) et laisse Qt le poser. `UpdateLayeredWindow` échoue aussi sur cette fenêtre (`ERROR_INVALID_PARAMETER`, 87) — la voie T-017 « transparence par pixel » est donc **bloquée en l'état** sur une fenêtre enfant cross-process.

3. **Bloqueur ouvert — Qt ne peint pas dans la fenêtre hébergée.** Le placement est désormais exact (`--dump` pendant la démo : `DefView[0] / SysListView32[1] / SysHeader32[2] / Qt683QWindowIcon[3] 80,80 480x320 / WorkerW[4]`, layered, alpha 255), mais **rien n'apparaît** : capture avant/après identique (`diff bbox = None`). Mesures : `PrintWindow` renvoie une image vide (`centrePixel = 0x0`) alors que `QWidget::grab()` produit bien un 480×320 peint ; `repaint()`, `hide()/show()` et `WA_Mapped` (vrai) ne changent rien. Autrement dit Qt ne fait plus le blit du backing store vers le HWND dès que celui-ci est enfant d'un parent étranger. Le spike Win32 pur, lui, rend correctement avec la même séquence (**96,6 %** de pixels dans la zone, contrôle rejoué en session 7) : la technique de placement reste valide, c'est le chemin de rendu Qt qui lâche.
   **Piste recommandée** : dissocier surface et rendu — une fenêtre Win32 brute hébergée (séquence prouvée) qui blitte sur `WM_PAINT` une image produite par Qt (offscreen `QImage`/`QWidget::grab()`). À trancher (voir §7 et T-020).

### ADR-5 — Persistance JSON versionné
Exemple (§) : lisible, diff-able, exportable (F-028). Écriture atomique (fichier temporaire + `ReplaceFile`). Emplacement : `%APPDATA%/StarFish/`.

### ADR-6 — Local-first
Aucun accès réseau fonctionnel, aucune télémétrie, aucun compte (F-037). Les folder portals pointent vers des dossiers locaux ou réseau de l'utilisateur, jamais vers un service StarFish.

## 4. Points d'intégration Windows (à valider en Phase 1)

| Besoin | API / technique |
|---|---|
| Masquer les icônes natives | Basculer `HideIcons` (`HKCU\...\Explorer\Advanced`) ou styler la fenêtre `SHELLDLL_DefView` ; rendre le basculement instantané et réversible — **implémenté** : `DesktopHost::setIconsHidden()` (`ShowWindow` sur DefView, aucune écriture registre) |
| Z-order bureau | Enfant de `Progman`, **`WS_EX_LAYERED` obligatoire**, inséré sous `SHELLDLL_DefView` (ADR-4) — **implémenté** : `DesktopHost::attach()` ; ⚠️ rendu effectif encore ouvert (ADR-4, finding 3) |
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

```json
{
  "schema": 1,
  "fences": [
    {
      "id": "uuid",
      "title": "Travail",
      "monitor": "\\\\.\\DISPLAY1",
      "rect": { "x": 0, "y": 0, "w": 480, "h": 320 },
      "style": {
        "bg": "#1F2937CC", "radius": 10, "padding": 12,
        "iconSize": 48, "gap": 8, "titleBar": true,
        "iconTint": null
      },
      "sort": { "by": "name", "asc": true },
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

Règle d'or : `moveFiles: false` par défaut — les règles ne **déplacent jamais un fichier sur le disque** sans opt-in explicite.

## 6. Build, packaging, qualité

- **Build** : CMake ≥ 3.24, Qt 6.8, compilateur MSVC 2022 (cible de référence, celle de la CI). **Toolchain locale de développement** (depuis la session 7, la machine de dev n'a ni MSVC ni Qt) : Qt 6.8.3 `win64_mingw` + MinGW 13.1.0, installés sans droits admin via `aqtinstall` dans `C:/Users/<user>/Qt` — voir `devlog/2026-10-13.md` session 7 pour les commandes exactes. Le code compile proprement sur les deux ; MSVC reste la cible publiée.
  - Portabilité : MinGW-w64 déclare `IID_IImageList` mais ne fournit aucun symbole dans ses bibliothèques d'import → l'IID est défini localement dans `shell_icon_cache.cpp` (valeur reprise du SDK). MSVC utilise le symbole du SDK.
- **CI** : GitHub Actions `windows-latest` → configure/build + tests unitaires (`RuleEngine`, `LayoutStore`) à chaque PR.
- **Packaging** : zip portable (`windeployqt`) pour les nightly ; installeur (WiX ou Inno Setup) pour les releases ; publication `winget` plus tard.
- **Tests** : unitaires pour la logique pure (règles, sérialisation, verdict de z-order, ordre/fusion des entrées du bureau) ; manuels documentés pour l'intégration shell (checklist dans `devlog/`).
- **Style** : clang-format (LLVM, 2 espaces), commits en anglais, Conventional Commits.
- **Diagnostics CLI** (binaire en sous-système GUI) : `--dump`, `--list-desktop [--icons] [--icon-size N] [--icon-dump DIR]`, `--host-demo [--seconds N] [--hide-icons]`. Si l'appelant a redirigé stdout/stderr (fichier ou pipe), `main.cpp` **ne détourne pas** la sortie vers `CONOUT$` et force `QT_ASSUME_STDERR_HAS_CONSOLE=1` : sans quoi Qt route `qCInfo`/`qCWarning` vers `OutputDebugString` et tous les diagnostics CLI sont perdus.

## 7. Ce qui reste à trancher

- **Chemin de rendu des zones hébergées** (bloquant Phase 1, T-020) : Qt ne blitte plus dans la fenêtre native une fois celle-ci enfant de `Progman` (ADR-4, finding 3). Options : (a) surface Win32 brute hébergée qui blitte sur `WM_PAINT` une image rendue par Qt (offscreen) — piste recommandée ; (b) faire peindre Qt dans le HWND via un `QWindow`/`QPaintEngine` natif ; (c) créer la surface hors de Qt et n'utiliser Qt que pour le rendu offscreen. À trancher avec la conséquence sur ADR-2 (Qt Widgets).
- **Transparence par pixel** (T-017) : `UpdateLayeredWindow` échoue sur une fenêtre enfant cross-process (err 87) — le fond translucide d'une zone devra passer par la surface retenue en T-020.
- Stratégie de renommage inline (F2) vs menu shell uniquement.
- Comportement Peek quand aucune zone n'est définie.
- Format exact des hotkeys configurables (JSON dans les réglages).
