# STATE.md — Où on en est

> **Protocole de reprise (obligatoire avant tout travail) :** lire `AGENTS.md` (conventions) → ce fichier → la phase courante de `docs/plan.md` → `TODO.md` → travailler → mettre à jour `TODO.md` + `devlog/` + ce fichier (+ commit git) avant de terminer.

- **Phase courante :** Phase 1 — Fondations (quasi terminée ; restent T-004 + portage C++ du spike)
- **Dernière mise à jour :** 2026-10-13 (session 3)
- **Branche de travail :** `main` (repo git local, pas encore de remote)

## Ce qui existe

- Documentation : `README.md`, `AGENTS.md` + `AGENT.md` (portabilité inter-harness), `docs/produit.md`, `docs/plan.md`, `docs/technique.md` (ADR-1→6), `TODO.md`, `devlog/`
- Squelette C++ : `CMakeLists.txt` (Qt 6.8, C++17) ; `src/main.cpp` (placeholder) ; `src/core/rules.{hpp,cpp}` (RuleEngine : extensions + glob, première règle gagnante) ; `src/core/jsonstore.{hpp,cpp}` (Layout JSON schéma v1, écriture atomique) ; `tests/test_rules.cpp` (7 cas QTest)
- CI : `.github/workflows/ci.yml` (windows-latest, Qt 6.8 MSVC, build + ctest) — **non validée** (aucun compilateur/Qt en local)
- Spike validé : `experiments/spike_workerw.py` → placement sur le bureau via Progman + z-order sous DefView (ADR-4 réécrit)
- Environnement : git + cmake présents ; **pas de compilateur C++ ni de Qt 6.8** sur la machine → build local impossible pour l'instant, la CI fera foi

## Décisions actées (détail dans `docs/technique.md`)

1. Cible **Windows 10/11 x64** uniquement en v1 (Fences est Windows-only).
2. Stack : **C++17 + Qt 6.8 (Widgets)**.
3. Masquage des icônes natives et rendu maison (ADR-3) ; sur Win11, masquage réversible via `ShowWindow(SW_HIDE)` sur `SHELLDLL_DefView` (finding spike).
4. **ADR-4 réécrit** : plus de WorkerW (invalide sur Win11 récent) ; fenêtres enfants de `Progman`, insérées sous `SHELLDLL_DefView`. Validé par spike.
5. Persistance JSON versionnée atomique ; les fichiers ne sont jamais déplacés sans opt-in.
6. Local-first : zéro télémétrie.

## 3 prochaines actions

1. **T-004** — lecture du dossier Bureau + icônes shell (`SHGetFileInfo`, cache image list).
2. **Débloquer le build local** : installer Qt 6.8 + MSVC 2022, puis `cmake -B build` + `ctest`. En parallèle, pousser sur GitHub pour valider la CI (**demander l'autorisation** avant tout push externe).
3. **T-003 (suite)** — porter le spike en C++/Qt : `DesktopHost` qui regroupe découverte Progman/DefView, parenting et recalcul de z-order sur `WM_SETTINGCHANGE`.

## À ne pas faire

- Ne pas pousser sur un remote GitHub sans accord explicite.
- Ne pas recréer de doc sans mettre à jour `STATE.md` en parallèle.
- Ne pas modifier l'inventaire fonctionnel sans créer/mettre à jour la `F-xxx` correspondante dans `TODO.md`.
