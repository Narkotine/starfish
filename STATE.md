# STATE.md — Où on en est

> **Protocole de reprise (obligatoire avant tout travail) :** lire `AGENTS.md` (conventions) → ce fichier → la phase courante de `docs/plan.md` → `TODO.md` → travailler → mettre à jour `TODO.md` + `devlog/` + ce fichier (+ commit git) avant de terminer.

- **Phase courante :** Phase 1 — Fondations (T-001, T-002, T-003, T-016 ✅ ; restent T-004, le déblocage du build local et les validations manuelles T-018/T-019)
- **Dernière mise à jour :** 2026-10-13 (session 6)
- **Branche de travail :** `main` (repo git local, pas encore de remote)

## Ce qui existe

- Documentation : `README.md`, `AGENTS.md` + `AGENT.md` (portabilité inter-harness), `docs/produit.md`, `docs/plan.md`, `docs/technique.md` (ADR-1→6), `TODO.md` (T-001…T-019), `devlog/`
- Squelette C++ : `CMakeLists.txt` (Qt 6.8, C++17) ; `src/main.cpp` (CLI : `--dump`, `--host-demo`, `--seconds`, `--hide-icons` ; sans argument → démarre et quitte, compatible CI) ; `src/core/rules.{hpp,cpp}` (RuleEngine) ; `src/core/jsonstore.{hpp,cpp}` (Layout JSON schéma v1, atomique) ; `src/core/zorder.{hpp,cpp}` (verdict de placement, pur, testé)
- **`DesktopHost`** (`src/platform/win/desktop_host.{hpp,cpp}`, T-016 ✅) : découverte `Progman`/`SHELLDLL_DefView` (fallback WorkerW Win10), `attach()` = séquence de référence ADR-4 (frameless top-level → `SetParent` → `WS_CHILD` sans `WS_POPUP` → `WS_EX_LAYERED` + `SetLayeredWindowAttributes(255)` → `SetWindowPos` sous DefView), garde `WM_WINDOWPOSCHANGING`, watchdog 2 s (les fenêtres enfants ne reçoivent pas les broadcasts), `desktopReacquired()`, `setIconsHidden()` réversible, `verifyPlacement()`, `dumpDesktopHierarchy()`
- `src/ui/fence_view.{hpp,cpp}` : zone vide (panneau arrondi + barre de titre) pour la démo Phase 1 ; les icônes arrivent en T-005
- Tests : `tests/test_rules.cpp` (7 cas), `tests/test_zorder.cpp` (6 cas) — 2 exécutables ctest
- CI : `.github/workflows/ci.yml` (windows-latest, Qt 6.8 MSVC, build + ctest) — **non validée** (aucun compilateur/Qt en local)
- **Spike placement bureau VALIDÉ** (T-003 ✅) : `experiments/spike_workerw.py` (Win32 pur, ctypes). Séquence de référence : SetParent(Progman) → WS_CHILD sans WS_POPUP → retirer `WS_EX_TOPMOST` → **ajouter `WS_EX_LAYERED` + `SetLayeredWindowAttributes(alpha=255)`** → SetWindowPos sous `SHELLDLL_DefView`. Vérifié par chaîne de z-order **+ capture** (session 5 : 77,8 % de pixels orange).
  - ⚠️ **Finding session 5** : un enfant de Progman **non-layered n'est pas composé par DWM** → invisible malgré un z-order correct. Le « succès » de la session 4 était un **faux positif**. Le flag layered est **obligatoire**.
  - Diagnostics : `experiments/probe_desktop.py`, `experiments/probe_fences.py`.
- Environnement : git + cmake présents ; **pas de compilateur C++ ni de Qt 6.8** sur la machine → build local impossible pour l'instant, la CI fera foi

## Décisions actées (détail dans `docs/technique.md`)

1. Cible **Windows 10/11 x64** uniquement en v1 (Fences est Windows-only).
2. Stack : **C++17 + Qt 6.8 (Widgets)**.
3. Masquage des icônes natives et rendu maison (ADR-3) ; sur Win11, masquage réversible via `ShowWindow(SW_HIDE)` sur `SHELLDLL_DefView`.
4. **ADR-4** : fenêtres enfants de `Progman`, insérées sous `SHELLDLL_DefView`, **obligatoirement `WS_EX_LAYERED`** (`SetLayeredWindowAttributes` alpha=255) — sinon non composées par DWM (échec silencieux). Pas de WorkerW, pas de Tk pour le produit.
5. Persistance JSON versionnée atomique ; les fichiers ne sont jamais déplacés sans opt-in.
6. Local-first : zéro télémétrie.
7. **Portage Qt (T-016)** : garde `WM_WINDOWPOSCHANGING` (aucun restack au-dessus des icônes) + watchdog 2 s au lieu des broadcasts `WM_SETTINGCHANGE`/`WM_DISPLAYCHANGE` (non reçus par une fenêtre enfant). Transparence **par pixel** non supportée (nécessite `UpdateLayeredWindow`, T-017).

## 3 prochaines actions

1. **T-004** — lecture du dossier Bureau + icônes shell (`SHGetFileInfo`, cache image list), en s'appuyant sur `DesktopHost`.
2. **Débloquer le build local** : installer Qt 6.8 + MSVC 2022, puis `cmake -B build` + `ctest`. Push GitHub pour valider la CI seulement avec accord explicite.
3. **Validations manuelles** : `starfish --host-demo` (contrôle visuel du placement), `--hide-icons`, puis T-018 (redémarrage d'Explorer) et T-019 (avec/sans Fences).

## À ne pas faire

- Ne pas pousser sur un remote GitHub sans accord explicite.
- Ne pas valider un placement par le seul z-order ni par une fenêtre de test de la même couleur que le fond (faux positif de la session 4).
- Ne pas recréer de doc sans mettre à jour `STATE.md` en parallèle.
- Ne pas modifier l'inventaire fonctionnel sans créer/mettre à jour la `F-xxx` correspondante dans `TODO.md`.
