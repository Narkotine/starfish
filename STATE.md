# STATE.md — Où on en est

> **Protocole de reprise (obligatoire avant tout travail) :** lire `AGENTS.md` (conventions) → ce fichier → la phase courante de `docs/plan.md` → `TODO.md` → travailler → mettre à jour `TODO.md` + `devlog/` + ce fichier (+ commit git) avant de terminer.

- **Phase courante :** Phase 1 — Fondations (T-001, T-002, T-003, T-004, T-016 ✅ ; restent **T-020 bloquante** — rendu des zones hébergées — puis T-018/T-019)
- **Dernière mise à jour :** 2026-10-13 (session 7)
- **Branche de travail :** `main` (repo git local, pas encore de remote)

## Ce qui existe

- Documentation : `README.md`, `AGENTS.md` + `AGENT.md` (portabilité inter-harness), `docs/produit.md`, `docs/plan.md`, `docs/technique.md` (ADR-1→6), `TODO.md` (T-001…T-020), `devlog/`
- Squelette C++ : `CMakeLists.txt` (Qt 6.8, C++17) ; `src/main.cpp` (CLI : `--dump`, `--list-desktop [--icons] [--icon-size] [--icon-dump]`, `--host-demo [--seconds] [--hide-icons]` ; sans argument → démarre et quitte, compatible CI) ; `src/core/rules.{hpp,cpp}` (RuleEngine) ; `src/core/jsonstore.{hpp,cpp}` (Layout JSON schéma v1, atomique) ; `src/core/zorder.{hpp,cpp}` (verdict de placement, pur, testé) ; `src/core/desktop_item.{hpp,cpp}` (T-004 : modèle + tri naturel + fusion des deux dossiers Bureau, pur, testé)
- **`DesktopHost`** (`src/platform/win/desktop_host.{hpp,cpp}`, T-016) : découverte `Progman`/`SHELLDLL_DefView` (fallback WorkerW Win10), `attach()`, garde `WM_WINDOWPOSCHANGING`, watchdog 2 s, `desktopReacquired()`, `setIconsHidden()` réversible, `setWindowAlpha()`, `verifyPlacement()`, `dumpDesktopHierarchy()`
- **`DesktopScanner`** (`src/platform/win/desktop_scanner.{hpp,cpp}`, T-004) : `SHGetKnownFolderPath` (Desktop + Public Desktop), nom d'affichage Shell, cibles `.lnk`/`.url`, attributs caché/système, RAII COM
- **`ShellIconCache`** (`src/platform/win/shell_icon_cache.{hpp,cpp}`, T-004) : icônes via la liste d'images système (48/256 px), repli `SHGFI_ICON`, cache par (chemin, taille)
- `src/ui/fence_view.{hpp,cpp}` : zone vide (panneau arrondi + barre de titre) pour la démo Phase 1 ; les icônes arrivent en T-005
- Tests : `tests/test_rules.cpp` (7 cas), `tests/test_zorder.cpp` (6 cas), `tests/test_desktop_item.cpp` (10 cas) — 3 exécutables ctest
- CI : `.github/workflows/ci.yml` (windows-latest, Qt 6.8 MSVC, build + ctest) — **non validée** (aucun push vers un remote)
- **Toolchain locale : débloquée (session 7)** — Qt 6.8.3 `win64_mingw` + MinGW 13.1.0 installés via `aqtinstall` dans `C:/Users/<user>/Qt` (aucun droit admin requis ; MSVC absent de la machine). Build + `ctest` verts en local. Recette : voir `docs/technique.md` §6 et `devlog/2026-10-13.md` session 7.
- **Spike placement bureau VALIDÉ** (T-003) : `experiments/spike_workerw.py` (Win32 pur, ctypes). Séquence de référence : SetParent(Progman) → WS_CHILD sans WS_POPUP → retirer `WS_EX_TOPMOST` → **ajouter `WS_EX_LAYERED` + `SetLayeredWindowAttributes(alpha=255)`** → SetWindowPos sous `SHELLDLL_DefView`. Rejoué en session 7 avec le bureau visible : **96,6 %** de pixels dans la zone.
  - ⚠️ **Finding session 5** : un enfant de Progman **non-layered n'est pas composé par DWM** → invisible malgré un z-order correct. Le flag layered est **obligatoire**.
  - ⚠️ **Findings session 7** (détail ADR-4) : (1) Qt **retire** `WS_EX_LAYERED` juste après `attach()` → corrigé par `setWindowOpacity(0.99)` ; (2) Windows **refuse silencieusement** d'ajouter ce flag après le reparentage, et `UpdateLayeredWindow` y échoue (err 87) ; (3) **Qt ne peint plus dans la fenêtre hébergée** → T-020.
  - Diagnostics : `experiments/probe_desktop.py`, `experiments/probe_fences.py`.

## Décisions actées (détail dans `docs/technique.md`)

1. Cible **Windows 10/11 x64** uniquement en v1 (Fences est Windows-only).
2. Stack : **C++17 + Qt 6.8 (Widgets)**.
3. Masquage des icônes natives et rendu maison (ADR-3) ; sur Win11, masquage réversible via `ShowWindow(SW_HIDE)` sur `SHELLDLL_DefView`.
4. **ADR-4** : fenêtres enfants de `Progman`, insérées sous `SHELLDLL_DefView`, **obligatoirement `WS_EX_LAYERED`** (`SetLayeredWindowAttributes` alpha=255) — sinon non composées par DWM (échec silencieux). Pas de WorkerW, pas de Tk pour le produit.
5. Persistance JSON versionnée atomique ; les fichiers ne sont jamais déplacés sans opt-in.
6. Local-first : zéro télémétrie.
7. **Portage Qt (T-016)** : garde `WM_WINDOWPOSCHANGING` (aucun restack au-dessus des icônes) + watchdog 2 s au lieu des broadcasts `WM_SETTINGCHANGE`/`WM_DISPLAYCHANGE` (non reçus par une fenêtre enfant).
8. **Session 7** : le flag layered doit être voulu par Qt (`setWindowOpacity(0.99)`), jamais ajouté après le reparentage (refus silencieux de Windows) ; `attach()` **vérifie** au lieu de poser. Transparence **par pixel** non supportée (`UpdateLayeredWindow` échoue sur un enfant cross-process).
9. **T-004** : le Shell est l'autorité pour les chemins (`SHGetKnownFolderPath`) et les noms affichés (`SHGFI_DISPLAYNAME`) ; les icônes passent par la liste d'images système, pas `SHGFI_ICON` (32 px trop petit).

## 3 prochaines actions

1. **T-020 (bloquante)** — trancher et implémenter le chemin de rendu des zones hébergées : Qt ne blitte plus dans le HWND une fois enfant de `Progman` (le placement, lui, est correct). Piste recommandée : surface Win32 brute hébergée qui blitte sur `WM_PAINT` une image rendue par Qt hors écran (`docs/technique.md` §7).
2. **T-018 / T-019** — validations manuelles désormais exécutables en local : redémarrage d'Explorer, et `--host-demo` avec Fences 6 actif **et** arrêté.
3. **T-005** — rendu des icônes du bureau dans une zone (dépend de T-020 ; `DesktopScanner` + `ShellIconCache` sont prêts).

## À ne pas faire

- Ne pas pousser sur un remote GitHub sans accord explicite.
- Ne pas valider un placement par le seul z-order ni par une fenêtre de test de la même couleur que le fond (faux positif de la session 4), **ni par une capture d'écran sans vérifier que le bureau est visible** (faux négatif de la session 7 : navigateur maximisé).
- Ne pas tenter d'**ajouter** `WS_EX_LAYERED` après le reparentage, ni d'appeler `UpdateLayeredWindow` sur une fenêtre hébergée : refus silencieux / `ERROR_INVALID_PARAMETER`.
- Ne pas recréer de doc sans mettre à jour `STATE.md` en parallèle.
- Ne pas modifier l'inventaire fonctionnel sans créer/mettre à jour la `F-xxx` correspondante dans `TODO.md`.
