# STATE.md — Où on en est

> **Protocole de reprise (obligatoire avant tout travail) :** lire `AGENTS.md` (conventions) → ce fichier → la phase courante de `docs/plan.md` → `TODO.md` → travailler → mettre à jour `TODO.md` + `devlog/` + ce fichier (+ commit git) avant de terminer.

- **Phase courante :** Phase 2 — Icônes sur le bureau (**T-005 ✅** en code ; reste validation manuelle à l'écran T-021). Fondations Phase 1 : T-001…T-004, T-016, **T-020 ✅**, **T-017 ✅** en code (reste validation visuelle). Restent T-018/T-019 (manuel), puis T-006.
- **Dernière mise à jour :** 2026-10-13 (session 9, soir)
- **Branche de travail :** `main` (repo git local, pas encore de remote)

## Ce qui existe

- Documentation : `README.md`, `AGENTS.md` + `AGENT.md` (portabilité inter-harness), `docs/produit.md`, `docs/plan.md`, `docs/technique.md` (ADR-1→6), `TODO.md` (T-001…T-020), `devlog/`
- Squelette C++ : `CMakeLists.txt` (Qt 6.8, C++17, `enable_language(RC)`) ; `src/main.cpp` (CLI : `--dump`, `--list-desktop [--icons] [--icon-size] [--icon-dump]`, `--host-demo [--seconds] [--hide-icons]`, `--render-probe [--surface] [--no-parent] [--show-desktop]` ; sans argument → démarre et quitte, compatible CI) ; `src/core/rules.{hpp,cpp}` (RuleEngine) ; `src/core/jsonstore.{hpp,cpp}` (Layout JSON schéma v1, atomique) ; `src/core/zorder.{hpp,cpp}` (verdict de placement, pur, testé) ; `src/core/desktop_item.{hpp,cpp}` (T-004 : modèle + tri naturel + fusion des deux dossiers Bureau, pur, testé)
- **`DesktopSurface`** (`src/platform/win/desktop_surface.{hpp,cpp}`, **T-020 ✅**) : **le chemin de rendu des zones**. Fenêtre Win32 brute (son propre `WNDPROC`), créée avec `WS_EX_LAYERED` dès `CreateWindowExW`, re-parentée sous `Progman`, placée sous `SHELLDLL_DefView`. `setFrame(QImage)` publie une image ; `WM_PAINT` la blitte en GDI. Qt ne touche jamais au HWND hébergé : il ne sert qu'au rendu hors écran (`QWidget::render()`). Garde `WM_WINDOWPOSCHANGING` + watchdog 2 s. **Mesuré : 153600/153600 px (100 %) de la zone rendus à l'écran.**
- **`DesktopHost`** (`src/platform/win/desktop_host.{hpp,cpp}`, T-016) : découverte `Progman`/`SHELLDLL_DefView` (fallback WorkerW Win10), `attach()`, garde `WM_WINDOWPOSCHANGING`, watchdog 2 s, `desktopReacquired()`, `setIconsHidden()` réversible, `setWindowAlpha()`, `verifyPlacement()`, `dumpDesktopHierarchy()`. **N'est plus le chemin de rendu** (Qt ne peint pas dans un HWND hébergé), mais reste utilisé pour le placement, le masquage des icônes et les verdicts.
- **`src/starfish.manifest` + `src/starfish.rc`** : manifeste applicatif embarqué dans `starfish.exe`. **Obligatoire** — sans lui Windows applique le comportement pré-Windows 8 et `WS_EX_LAYERED` est refusé sur une fenêtre enfant, donc les zones ne s'affichent jamais (échec silencieux). Voir ADR-4 finding 4.
- **`DesktopScanner`** (`src/platform/win/desktop_scanner.{hpp,cpp}`, T-004) : `SHGetKnownFolderPath` (Desktop + Public Desktop), nom d'affichage Shell, cibles `.lnk`/`.url`, attributs caché/système, RAII COM
- **`ShellIconCache`** (`src/platform/win/shell_icon_cache.{hpp,cpp}`, T-004) : icônes via la liste d'images système (48/256 px), repli `SHGFI_ICON`, cache par (chemin, taille)
- `src/ui/fence_view.{hpp,cpp}` : zone (panneau arrondi + barre de titre + grille d'icônes). T-005 : héberge `IconGrid`, dessine les icônes du bureau (via le `IconProvider` lié à `ShellIconCache`), gère la sélection (clic / Ctrl / Maj) et le double-clic (lancement via `QDesktopServices`), émet `contentChanged()`.
- `src/ui/icon_layout.hpp` (T-005) : géométrie pure de la grille (colonnes, cellules icône+libellé, hit-test), testée sans Win32.
- `src/ui/icon_grid.{hpp,cpp}` (T-005) : modèle de la grille (items, tri `name|type|size|date`, sélection, peinture via un `IconProvider` appelé avec `item.path` ; repli placeholder si l'icône ne résout pas). `launchPath()` renvoie la cible d'un `.lnk`/`.url`.
- Tests : `tests/test_rules.cpp` (7 cas), `tests/test_zorder.cpp` (6 cas), `tests/test_desktop_item.cpp` (10 cas) — 3 exécutables ctest, verts
- CI : `.github/workflows/ci.yml` (windows-latest, Qt 6.8 MSVC, build + ctest) — **non validée** (aucun push vers un remote)
- **Toolchain locale : débloquée (session 7)** — Qt 6.8.3 `win64_mingw` + MinGW 13.1.0 installés via `aqtinstall` dans `C:/Users/<user>/Qt` (aucun droit admin requis ; MSVC absent de la machine). Build + `ctest` verts en local. Recette : voir `docs/technique.md` §6 et `devlog/2026-10-13.md` session 7.
- **Spike placement bureau VALIDÉ** (T-003) : `experiments/spike_workerw.py` (Win32 pur, ctypes). Séquence de référence : `CreateWindowExW` avec `WS_EX_LAYERED` → SetParent(Progman) → WS_CHILD sans WS_POPUP → `SetLayeredWindowAttributes(alpha=255)` → SetWindowPos sous `SHELLDLL_DefView`. Rejoué avec le bureau visible : **96,6 %** de pixels dans la zone.
  - ⚠️ **Finding session 5** : un enfant de Progman **non-layered n'est pas composé par DWM** → invisible malgré un z-order correct. Le flag layered est **obligatoire**.
  - ⚠️ **Findings session 7** : (1) Qt **retire** `WS_EX_LAYERED` juste après `attach()` → corrigé par `setWindowOpacity(0.99)` ; (2) Windows **refuse silencieusement** d'ajouter ce flag après le reparentage, et `UpdateLayeredWindow` y échoue (err 87) ; (3) **Qt ne peint plus dans la fenêtre hébergée** → T-020.
  - ⚠️ **Finding session 8 (cause racine de (2), et correctif de (3))** : le refus silencieux de `WS_EX_LAYERED` sur une fenêtre enfant vient de l'**absence de manifeste applicatif** (comportement pré-Windows 8). Et le rendu passe par une **surface Win32 brute** (`DesktopSurface`), pas par Qt. Détail : ADR-4 finding 4 et §7.
- **Outils de diagnostic (session 8)** : `experiments/spike_surface_min.cpp` (+ `.manifest`/`.rc`) — reproducer Win32 pur, aucune Qt, qui rejoue la séquence, imprime chaque retour/relecture et **compte lui-même** les pixels à l'écran ; `experiments/spike_exstyle_battery.py` — la même batterie en ctypes (A/B Python vs C++).

## Décisions actées (détail dans `docs/technique.md`)

1. Cible **Windows 10/11 x64** uniquement en v1 (Fences est Windows-only).
2. Stack : **C++17 + Qt 6.8 (Widgets)**.
3. Masquage des icônes natives et rendu maison (ADR-3) ; sur Win11, masquage réversible via `ShowWindow(SW_HIDE)` sur `SHELLDLL_DefView`.
4. **ADR-4** : fenêtres enfants de `Progman`, insérées sous `SHELLDLL_DefView`, **obligatoirement `WS_EX_LAYERED`** (`SetLayeredWindowAttributes` alpha=255) — sinon non composées par DWM (échec silencieux). Pas de WorkerW, pas de Tk pour le produit.
5. Persistance JSON versionnée atomique ; les fichiers ne sont jamais déplacés sans opt-in.
6. Local-first : zéro télémétrie.
7. **Portage Qt (T-016)** : garde `WM_WINDOWPOSCHANGING` (aucun restack au-dessus des icônes) + watchdog 2 s au lieu des broadcasts `WM_SETTINGCHANGE`/`WM_DISPLAYCHANGE` (non reçus par une fenêtre enfant).
8. **Session 7** : le flag layered doit être voulu par Qt (`setWindowOpacity(0.99)`), jamais ajouté après le reparentage (refus silencieux de Windows) ; `attach()` **vérifie** au lieu de poser.
9. **T-004** : le Shell est l'autorité pour les chemins (`SHGetKnownFolderPath`) et les noms affichés (`SHGFI_DISPLAYNAME`) ; les icônes passent par la liste d'images système, pas `SHGFI_ICON` (32 px trop petit).
10. **Session 8 — chemin de rendu (§7 piste (a))** : une **surface Win32 brute** (`DesktopSurface`) possède le HWND et blitte sur `WM_PAINT` une image produite par Qt hors écran. Qt ne peint jamais dans une fenêtre hébergée. Conséquence : ADR-2 (Qt Widgets) est intact, mais Qt n'est plus responsable du blit.
11. **Session 8 — manifeste applicatif obligatoire** : `starfish.exe` doit embarquer `src/starfish.manifest` (via `src/starfish.rc`). Sans lui, `WS_EX_LAYERED` est refusé sur les fenêtres enfants et **rien ne s'affiche**, sans le moindre message. Corollaire : le flag est **aussi** posé dès `CreateWindowExW`, pour survivre à un binaire sans manifeste.
12. **Session 9 — T-017, les deux modes layered sont mutuellement exclusifs** : `SetLayeredWindowAttributes(LWA_ALPHA)` est une porte à sens unique — après cet appel, tout `UpdateLayeredWindow` échoue (err 87), même sur notre propre fenêtre. ULW fonctionne sinon **même sur un enfant cross-process de Progman** (spikes `experiments/spike_ulw*.py`). Mode per-pixel de `DesktopSurface` : `attach()` saute l'appel d'attributs, frames poussées via ULW, repli opaque automatique. Détail : `docs/technique.md` §8.

## 3 prochaines actions

1. **T-021** (manual) — vérifier à l'écran le rendu T-005 : `starfish --render-probe --surface --with-desktop-items` (grille d'icônes réelles, sélection clic/Ctrl/Maj, double-clic → lancement) + validation visuelle **T-017** (`--render-probe --surface --pixel-alpha`, papier peint visible à travers le panneau) + **T-018/T-019** (redémarrage d'Explorer, `--host-demo` avec Fences 6 actif **et** arrêté).
2. **T-006** — glisser-déposer interne (réordonner dans/déplacer entre zones) + menu contextuel shell (`IContextMenu`) + DnD `CF_HDROP`.
3. **T-007** — cycle de vie des zones (créer / déplacer / redimensionner / supprimer + aimantation).

## À ne pas faire

- Ne pas pousser sur un remote GitHub sans accord explicite.
- Ne pas valider un placement par le seul z-order ni par une fenêtre de test de la même couleur que le fond (faux positif de la session 4), **ni par une capture d'écran sans vérifier que le bureau est visible** (faux négatif de la session 7 : navigateur maximisé ; session 8 : `Win+D` **restaure** les fenêtres déjà minimisées — ne pas le presser si le bureau est déjà visible).
- Ne pas retirer `starfish.rc` / `starfish.manifest` du build : la panne (zones invisibles) est totalement silencieuse. Idem, un manifeste XML **ne peut pas contenir `--` dans un commentaire** (Windows refuse alors de démarrer le binaire, avec un message qui n'a rien à voir).
- Ne pas tenter d'**ajouter** `WS_EX_LAYERED` après le reparentage sur un binaire sans manifeste, ni d'appeler `UpdateLayeredWindow` sur une fenêtre hébergée créée par Qt : refus silencieux / `ERROR_INVALID_PARAMETER`.
- Ne pas recréer de doc sans mettre à jour `STATE.md` en parallèle.
- Ne pas modifier l'inventaire fonctionnel sans créer/mettre à jour la `F-xxx` correspondante dans `TODO.md`.
