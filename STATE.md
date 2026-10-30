# STATE.md — Où on en est

> **Protocole de reprise :** `AGENTS.md` (conventions) → **ce fichier** → la phase courante de `docs/plan.md` → les tâches **ouvertes** de `TODO.md` → travailler → mettre à jour `TODO.md` + `devlog/` + ce fichier (+ commit).

> **Ce fichier est court par construction (cap 6 Ko, vérifié par `tools/docs-size.sh` en CI).** Il ne raconte pas les sessions : le récit va dans `devlog/AAAA-MM-JJ.md`. Le détail vit dans `docs/state/` et se lit **à la demande** (table ci-dessous) — une session n'a jamais besoin de tout lire.

- **Phase courante :** Phase 5 — Finitions fonctionnelles, **EN COURS**.
- **Dernière session de code livrée :** 58 (2026-10-30) - **ADR-21 : StarFish dessine le bureau**, piloté par la vue du shell (`desktop_shell_view`, `IFolderView2`) : vrais menus, verbes, rangement sans bouger le fichier, filet T-046. **Lot 1 (T-089)** dessin + menus ; **lot 2 (T-090)** Explorer qui redémarre, liste re-masquée, écrans/DPI ; décisions : entrée de menu repointée, éléments virtuels rangeables, flèche des zones = réglage (oui par défaut) ; **lot 3 (T-091)** F2, clavier, rectangle, glisser OLE, `IFileOperation` + annuler ; **lot 4 (T-092)** menu clic droit façon Windows 11 dessiné par StarFish, configurable. `ctest` 17/17, `--drawn-desktop-probe` **46/46**. **§44**.
- **Session 59 :** apps empaquetées dans le menu, dépôt étranger sous le curseur, icône StarFish (étoile de mer), **refonte Réglages + menu du tray** style Windows 11 (clair/sombre) avec **éditeur du menu clic droit**. `--drawn-desktop-probe` 48/48, `--product-probe` 34/34, `--ui-probe` 10/10.
- **Session 60 :** retours utilisateur - redimensionnement stable (T-086 corrigé, cause = aimantation de déplacement), renommage sur le titre, tri à l'ajout (désactivé), écart titre/icônes 12 px (`FenceMetrics`, T-097), flèches du bureau (réglage), superposition bureau/zones interdite par défaut (icônes déplacées). `ctest` 18/18, `--ui-probe` 13/13 hors écran.
- **Livré juste avant :** 57 - ADR-20 (renversé par l'ADR-21) ; 56 - T-042 1ʳᵉ tranche ; 52 - i18n.
- **Dernière mise à jour :** 2026-10-30 (session 60)
- **Branche :** `main` ; remote `origin` = github.com/Narkotine/starfish (**privé**) ; poussé au 2026-10-30 (`61df2cc`, session 59) ; session 60 **locale**.

## 3 prochaines actions

1. **Session 60 à essayer** : redimensionner (coin haut-gauche fixe), double-clic sur un titre, Réglages  Zones (3 nouveaux interrupteurs), zone posée sur des icônes du bureau. Puis **Son essai** : `build/starfish.exe` - bureau dessiné ; ranger une icône dans une zone (elle quitte le bureau), clic droit fond / icône, double-clic, quitter par le tray (tout revient). Raccourci d'urgence : tray « Restaurer mes icônes ».
2. **Plan ADR-21 livré** (T-089..T-092). Reste humain : deux écrans, vrai redémarrage d'Explorer (T-090) ; vrai glisser souris, focus clavier (T-091) ; le menu dessiné à la vraie souris et au clavier (T-092). Menu : Réglages → Menu clic droit (fichier `%LOCALAPPDATA%\StarFish\context-menu.json`).
3. ⛔ Questions ouvertes : `T-036`, `T-033`, `T-030`.
## Où lire quoi (à la demande)

| Besoin | Fichier |
|---|---|
| Décisions actées — **index** des 56, une ligne chacune | [`docs/state/decisions.md`](docs/state/decisions.md) |
| Détail d'une décision, par domaine (contraintes · windows · rendu · layout · navigation · règles · produit · outils) | `docs/state/decisions/*.md` |
| Pièges déjà payés, à ne pas rejouer — **mêmes domaines** | `docs/state/guardrails.md` → `docs/state/guardrails/*.md` |
| « Est-ce que X existe déjà ? » — inventaire composant par composant | [`docs/state/inventory.md`](docs/state/inventory.md) |
| Écarts assumés entre le plan et le code | [`docs/state/gaps.md`](docs/state/gaps.md) |
| Prochaines actions : ordre, raisons, scripts d'essai | [`docs/state/next-actions-detail.md`](docs/state/next-actions-detail.md) |
| Tâches terminées (table + détail) | [`docs/todo-archive.md`](docs/todo-archive.md) |
| Récits de session archivés | [`docs/state/recaps.md`](docs/state/recaps.md) |
| Architecture, ADR, mesures | [`docs/technique.md`](docs/technique.md) |

## Garde-fous transverses

- Ne pas pousser sur le remote sans accord explicite — et **tout push déclenche la CI**.
- Ne pas déplacer un fichier sur le disque : StarFish gère l'**affichage**, pas l'emplacement (`moveFiles` reste sans effet, T-031).
- Ne pas juger un placement par le z-order seul, ni par une capture sans vérifier que le bureau est visible : validation **au vrai curseur**.
- **Ne pas confondre « le produit va bien » et « la sonde est verte » : une sonde mesure ce qu'elle a été écrite pour mesurer (le reste est dans `docs/state/guardrails/08-outils-verification.md`).**
- **Ne pas réordonner les deux fenêtres d'une zone** (T-082) : `SetWindowPos` insère **sous** la fenêtre nommée, donc c'est **le panneau** qui est posé **sous son overlay** (`setStackedBelow`) — l'inverse laisse les tuiles effacées derrière lui (mesuré).
- **Écrire court** : une session = une entrée `devlog/` + ses lignes ici + un commit. Rien de plus. **Détail non déroulé** (consigne utilisateur du 2026-10-17 — vibe coding) : les faits (mesures, fichiers, pièges), **une ligne par fait**, la conclusion d'abord.
- **Ne pas confondre le rayon *demandé* et le rayon *dessiné* (T-080)** : la vue borne (`min(demandé, demi-petit-côté)`) — et c'est **ce** nombre que la **région** doit recevoir.
