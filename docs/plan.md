# Plan de mise en œuvre — StarFish

> Version 0.1 — 2026-10-13. Chaque phase se termine par des **critères de sortie** vérifiables ; on ne passe à la suivante qu'une fois ces critères remplis et consignés dans `devlog/`.

## Phase 0 — Cadrage ✅ (faite le 2026-10-13)

Recherche des fonctionnalités de Fences 6, rédaction `docs/produit.md`, `docs/plan.md`, `docs/technique.md`, `TODO.md`, `STATE.md`.

**Sortie :** documentation produit/technique/plan commitées, prêtes à guider les phases suivantes.

## Phase 1 — Fondations

- T-001 Audit concurrence open source
- T-002 `git init` + squelette (CMake, Qt 6.8, arborescence `src/`, CI minimale)
- T-003 Spike : fenêtre sans bord ni fond posée sur le `WorkerW`, visible au-dessus du papier peint, sous les icônes ; masquage des icônes natives du bureau (commutable)
- T-004 Lecture du contenu du dossier Bureau + récupération des icônes shell (`SHGetFileInfo`)

**Sortie :** app qui démarre, affiche une zone vide au bon z-order ; le masquage des icônes natives est réversible (au cas où le mode « icônes natives repositionnées » serve de repli) ; devlog documente les API Win32 utilisées.

## Phase 2 — Icônes sur le bureau

- T-005 Rendu des icônes dans la zone (taille, libellé, sélection)
- T-006 Glisser-déposer interne (réordonner dans une zone, déplacer entre zones)

**Sortie :** les icônes du bureau réel sont affichées et déplaçables dans une zone ; double-clic ouvre le fichier ; menu contextuel shell (Ouvrir, Propriétés…) fonctionnel via `IContextMenu`.

## Phase 3 — Zones complètes

- T-007 Créer / déplacer / redimensionner / supprimer une zone ; aimantation
- T-008 Persistance JSON du layout (schéma versionné, écriture atomique)

**Sortie :** redémarrage de l'app → disposition restaurée à l'identique ; suppression de zone sans perte de fichiers.

## Phase 4 — Fonctions phares

- T-009 Roll-up, Peek (Win+Espace), quick hide (double-clic bureau)
- T-010 Règles d'auto-organisation (type / nom / date / cible)
- T-011 Folder portals

**Sortie :** un fichier déposé dans `~/Desktop` est automatiquement rangé selon une règle ; un portail affiche un dossier arbitraire.

## Phase 5 — Finitions fonctionnelles

- T-012 Snapshots (sauvegarde / restauration / export-import)
- **T-038 Mode produit minimal** *(ajouté après l'audit de la session 19 : le projet livrait un moteur, pas encore un produit)* — tray (F-034), fenêtre de réglages, zones hébergées manipulables à la souris, watcher branché sur le vrai dossier Bureau (F-018)
- T-013 Pages de bureau multiples
- T-014 Multi-écran, DPI mixte, i18n (en/fr), thèmes de zones

**Sortie :** passe de tests manuels multi-écrans documentée ; capture d'écran avant/après restauration de snapshot.

## Phase 6 — Qualité et release

- T-015 Packaging (portable + installeur), CI GitHub Actions, docs utilisateur, page de réglages complète

**Sortie :** binaire reproductible depuis un checkout propre ; v0.1 taguée avec notes de version.

## Risques et mitigations

| Risque | Impact | Mitigation |
|---|---|---|
| Masquage des icônes natives = perte d'interactions shell (renommer, couper/coller, menu Envoyer vers) | Élevé | Phase 2 implémente `IContextMenu` + DnD `CF_HDROP` ; repli possible : mode « repositionnement des icônes natives » (moins riche) |
| Fenêtre WorkerW instable selon les versions de Windows 10/11 | Élevé | Spike T-003 verrouille la technique ; fallback : fenêtre `PROGMAN`-child ou top-level ancrée à l'écran |
| Mises à jour Windows cassant l'ordre z / l'API WorkerW | Moyen | Tests de non-régression manuels + recalcul à chaque `WM_SETTINGCHANGE` |
| DPI mixte multi-écrans | Moyen | Per-monitor DPI v2, Qt `highDpiScaleFactor` désactivé et gestion manuelle |
| Performance avec beaucoup d'icônes + portails | Moyen | Cache d'icônes système (`IImageList`), rendu paresseux |
