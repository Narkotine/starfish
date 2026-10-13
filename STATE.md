# STATE.md — Où on en est

> **Protocole de reprise (obligatoire avant tout travail) :** lire `AGENTS.md` (conventions) → ce fichier → la phase courante de `docs/plan.md` → `TODO.md` → travailler → mettre à jour `TODO.md` + `devlog/` + ce fichier (+ commit git) avant de terminer.

- **Phase courante :** Phase 1 — Fondations (quasi terminée ; restent T-004 + T-016 portage C++)
- **Dernière mise à jour :** 2026-10-13 (session 5)
- **Branche de travail :** `main` (repo git local, pas encore de remote)

## Ce qui existe

- Documentation : `README.md`, `AGENTS.md` + `AGENT.md` (portabilité inter-harness), `docs/produit.md`, `docs/plan.md`, `docs/technique.md` (ADR-1→6), `TODO.md` (T-001…T-016), `devlog/`
- Squelette C++ : `CMakeLists.txt` (Qt 6.8, C++17) ; `src/main.cpp` (placeholder) ; `src/core/rules.{hpp,cpp}` (RuleEngine) ; `src/core/jsonstore.{hpp,cpp}` (Layout JSON schéma v1, atomique) ; `tests/test_rules.cpp` (7 cas QTest)
- CI : `.github/workflows/ci.yml` (windows-latest, Qt 6.8 MSVC, build + ctest) — **non validée** (aucun compilateur/Qt en local)
- **Spike placement bureau VALIDÉ** (T-003 ✅) : `experiments/spike_workerw.py` (Win32 pur, ctypes). Séquence de référence : SetParent(Progman) → WS_CHILD sans WS_POPUP → retirer `WS_EX_TOPMOST` → **ajouter `WS_EX_LAYERED` + `SetLayeredWindowAttributes(alpha=255)`** → SetWindowPos sous `SHELLDLL_DefView`. Vérifié par chaîne de z-order **+ capture** (session 5 : 77,8 % de pixels orange, icônes/fences dessinées par-dessus).
  - ⚠️ **Finding session 5** : un enfant de Progman **non-layered n'est pas composé par DWM** → invisible malgré un z-order correct (`IsWindowVisible`=True). Le « succès » de la session 4 était un **faux positif** (ancien papier peint orange ≈ couleur de la fenêtre de test). Le flag layered est **obligatoire**. Ce n'est **pas** Fences qui bloquait.
  - Diagnostics : `experiments/probe_desktop.py`, `experiments/probe_fences.py` (Fences dessine dans le `SysListView32` natif, ne crée pas de fenêtres).
- Environnement : git + cmake présents ; **pas de compilateur C++ ni de Qt 6.8** sur la machine → build local impossible pour l'instant, la CI fera foi

## Décisions actées (détail dans `docs/technique.md`)

1. Cible **Windows 10/11 x64** uniquement en v1 (Fences est Windows-only).
2. Stack : **C++17 + Qt 6.8 (Widgets)**.
3. Masquage des icônes natives et rendu maison (ADR-3) ; sur Win11, masquage réversible via `ShowWindow(SW_HIDE)` sur `SHELLDLL_DefView`.
4. **ADR-4** : fenêtres enfants de `Progman`, insérées sous `SHELLDLL_DefView`, **obligatoirement `WS_EX_LAYERED`** (`SetLayeredWindowAttributes` alpha=255) — sinon non composées par DWM (échec silencieux). Séquence de référence documentée ; pas de WorkerW, pas de Tk pour le produit (Tk restack).
5. Persistance JSON versionnée atomique ; les fichiers ne sont jamais déplacés sans opt-in.
6. Local-first : zéro télémétrie.

## 3 prochaines actions

1. **T-016** — portage C++/Qt du spike : `DesktopHost` (découverte Progman/DefView, parenting, **`WS_EX_LAYERED` + `SetLayeredWindowAttributes`**, recalcul de z-order sur `WM_SETTINGCHANGE`).
2. **T-004** — lecture du dossier Bureau + icônes shell (`SHGetFileInfo`, cache image list), en s'appuyant sur `DesktopHost`.
3. **Débloquer le build local** : installer Qt 6.8 + MSVC 2022, puis `cmake -B build` + `ctest`. Push GitHub pour valider la CI seulement avec accord explicite.

## À ne pas faire

- Ne pas pousser sur un remote GitHub sans accord explicite.
- Ne pas recréer de doc sans mettre à jour `STATE.md` en parallèle.
- Ne pas modifier l'inventaire fonctionnel sans créer/mettre à jour la `F-xxx` correspondante dans `TODO.md`.
