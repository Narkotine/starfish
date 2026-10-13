# TODO — Suivi des tâches

> Légende statut : ⬜ à faire · 🔄 en cours · ✅ fait · ⛔ bloquée. IDs liés aux phases de `docs/plan.md`. Toute tâche terminée doit être consignée dans `devlog/`.

| ID | Tâche | Phase | Prio | Statut |
|---|---|---|---|---|
| T-001 | Audit des alternatives open source (compléter §9 de `docs/produit.md`) | 1 | P1 | ✅ |
| T-002 | `git init`, CMake + Qt 6.8, arborescence `src/`, `.gitignore`, CI minimale | 1 | P0 | ✅ |
| T-003 | Placement bureau : fenêtre sous la couche d'icônes (spike, ADR-4) | 1 | P0 | ✅ |
| T-016 | Portage C++/Qt du spike : `DesktopHost` (Progman + z-order, **`WS_EX_LAYERED` obligatoire**, `WM_SETTINGCHANGE`) | 1 | P0 | ✅ |
| T-004 | Lecture du dossier Bureau + icônes shell (`SHGetFileInfo`, cache image list) | 1 | P0 | ✅ |
| T-005 | Rendu des icônes du bureau dans une zone (taille, libellé, sélection, double-clic) | 2 | P0 | ⬜ |
| T-006 | Glisser-déposer interne (dans/entre zones) + menu contextuel shell + DnD `CF_HDROP` | 2 | P0 | ⬜ |
| T-007 | Cycle de vie des zones : créer / déplacer / redimensionner / supprimer + aimantation | 3 | P0 | ⬜ |
| T-008 | Persistance JSON (schéma v1, écriture atomique) + restauration au démarrage | 3 | P0 | ⬜ |
| T-009 | Roll-up (F-020), Peek Win+Espace (F-021), quick hide (F-022) | 4 | P0 | ⬜ |
| T-010 | Moteur de règles d'auto-organisation (F-017/F-018) | 4 | P1 | ⬜ |
| T-011 | Folder portals (F-024) | 4 | P1 | ⬜ |
| T-012 | Snapshots sauvegarde/restauration + export/import (F-027/F-028) | 5 | P0 | ⬜ |
| T-013 | Pages de bureau multiples (F-030/F-031) | 5 | P2 | ⬜ |
| T-014 | Multi-écran / DPI mixte / i18n en-fr / thèmes de zones | 5 | P1 | ⬜ |
| T-015 | Packaging portable + installeur, CI complète, docs utilisateur, release v0.1 | 6 | P1 | ⬜ |

## Tâches découvertes en cours de route

_(à ajouter ici au fil de l'eau, puis à répartir par phase)_

- **T-017** (Phase 2, P1) — Transparence par pixel des zones : `UpdateLayeredWindow` (rendu Qt → `QImage` → push) pour laisser voir le papier peint à travers un fond `#RRGGBBAA`. Aujourd'hui `DesktopHost::setWindowAlpha()` n'offre qu'une opacité uniforme de la fenêtre entière. ⚠️ Mesuré en session 7 : `UpdateLayeredWindow` échoue (`ERROR_INVALID_PARAMETER`) sur une fenêtre enfant cross-process créée par Qt. **À re-tenter en session 8+ sur `DesktopSurface`**, qui possède sa propre fenêtre (T-020) : c'est désormais le chemin de rendu, et la fenêtre n'est plus créée par Qt.
- **T-018** (Phase 1, P1) — Valider manuellement le redémarrage d'Explorer : les zones sont enfants de `Progman` et meurent avec lui ; vérifier que `desktopReacquired()` + `attach()` les recréent sans crash Qt (`taskkill /f /im explorer.exe`, relance).
- **T-019** (Phase 1, P2) — Vérifier que le comportement de `--host-demo` tient avec Fences 6 actif **et** arrêté (les deux configurations n'ont été mesurées qu'avec le spike Python).
- **T-020** (Phase 1, P0) — **✅ Fait (session 8).** Rendu des zones hébergées. Constat mesuré (session 7) : le placement est exact mais **rien n'est dessiné** — Qt ne blitte plus son backing store dans le HWND dès que celui-ci est enfant de `Progman` (`PrintWindow` vide, `grab()` correct, `repaint()`/`hide()/show()` sans effet). Le spike Win32 pur, lui, rend correctement : la technique de placement n'était pas en cause.
  **Résolu par la piste (a) de `docs/technique.md` §7** : `DesktopSurface` (`src/platform/win/desktop_surface.{hpp,cpp}`), surface Win32 brute hébergée qui possède le HWND et le `WNDPROC` et blitte sur `WM_PAINT` une image produite par Qt hors écran (`QWidget::render()`). Qt ne touche plus jamais au HWND hébergé.
  Mesure (`starfish --render-probe --surface`, bureau visible) : **153600 / 153600 pixels** du rectangle de la zone changent à l'écran (bbox = exactement `80,80 480x320`), relecture `PrintWindow` = `0xff1f2937` (le panneau Qt), z-order `[3]` sous `DefView [0]`. Vérifié aussi à l'œil (`experiments/out/t020_surface_on_desktop.png`).
  **Cause racine annexe, importante pour toute la suite** : `WS_EX_LAYERED` est refusé **silencieusement** sur une fenêtre enfant par un binaire **sans manifeste applicatif** (comportement pré-Windows 8). Corrigé par `src/starfish.manifest` + `src/starfish.rc`. Détail et tableau de mesures → ADR-4 finding 4.
