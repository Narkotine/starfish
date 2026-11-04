# STATE.md — Où on en est

> **Protocole de reprise :** `AGENTS.md` (conventions) → **ce fichier** → la phase courante de `docs/plan.md` → les tâches **ouvertes** de `TODO.md` → travailler → mettre à jour `TODO.md` + `devlog/` + ce fichier (+ commit).

> **Ce fichier est court par construction (cap 6 Ko, vérifié par `tools/docs-size.sh` en CI).** Il ne raconte pas les sessions : le récit va dans `devlog/AAAA-MM-JJ.md`. Le détail vit dans `docs/state/` et se lit **à la demande** (table ci-dessous) — une session n'a jamais besoin de tout lire.

- **Phase courante :** Phase 5 — Finitions fonctionnelles, **EN COURS**.
- **Dernière session de code livrée :** 82 (2026-11-04) - **menu clic droit, entrées à nous** : l'**aperçu du tracé reste affiché pendant que le menu est ouvert** (`showFencePreview` / `clearFencePreview` + garde dans `onDesktopMenu`), **plus d'infobulle à la création d'une zone**, une **pool de 33 icônes** nommées pour les entrées personnalisées (`core::menuGlyphPool`, Segoe Fluent Icons) avec son sélecteur, **6 entrées prêtes à l'emploi** (`core::menuPresets`) et une entrée peut porter une **action** (`"action": "theme"` → bascule clair/sombre, `platform::win::windows_theme`). `ctest` **20/20**, `--product-probe` **38/38**, `--ui-probe` hors écran **76/76**, i18n **322**. **F-042**, **T-139/T-140** ✅.
- **ADR-21 (session 58) : StarFish dessine le bureau**, piloté par la vue du shell (`desktop_shell_view`, `IFolderView2`) : vrais menus, verbes, rangement sans bouger le fichier, filet T-046. Lots 1-4 livrés (T-089 → T-092) ; restent des validations humaines (deux écrans, vrai redémarrage d'Explorer, vraie souris).
- **Sessions 59-81 (récits dans `devlog/`) :** apps empaquetées, icône, Réglages + éditeur du menu, redimensionnement, renommage du titre, superposition, `--quit` (59-61) · « Démarrer avec Windows », image de glisser (62) · rectangle fluide, zone translucide composée (T-098), curseurs, superposition cohérente (63-64) · liste de l'utilisateur, menus, Apparence…, curseurs, langue, aperçu du tracé (65) · apparence, `crash.log`, barre de titre 4 côtés, onglet, survols, double-clic bureau, contour uniforme, modèles (66-70) · grain sous les icônes, déroulé au survol, largeur stable, suppr, enroulement, renommage, raccourcis conservés, lancement shell, noms, +N, splash, horloge (71-79) · barre d'insertion, grille du bureau, témoin des moteurs (80) · **widget Notes** (81).
- **Dernière mise à jour :** 2026-11-04 (session 82)
- **Branche :** `main` ; remote `origin` = github.com/Narkotine/starfish (**privé**) ; poussé jusqu'à la session 80 (`2fcc7cd`) ; sessions 81-82 **locales**.

## 3 prochaines actions

1. **À essayer à la vraie souris** : le widget Notes (« Ajouter un widget → Notes », double-clic pour écrire, `Ctrl+Entrée` / `Échap`) ; le tracé d'une zone qui garde son aperçu pendant le menu ; l'éditeur du menu clic droit (icône, « Ajouter depuis la liste… », mode clair/sombre) ; la grille du bureau et la barre d'insertion de la session 80.
2. **Reste humain sur l'ADR-21** : deux écrans, vrai redémarrage d'Explorer, vrai glisser OLE, le menu dessiné à la vraie souris et au clavier. Menu : Réglages → Menu clic droit (fichier `%LOCALAPPDATA%\StarFish\context-menu.json`).
3. ❓ Questions ouvertes : **T-141** (alignement des icônes d'une zone sur la grille du bureau : trois modèles, rien codé), `T-095`, `T-096`, `T-013`, `T-031`, `T-036`, `T-015`.
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
