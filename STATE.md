# STATE.md — Où on en est

> **Protocole de reprise :** `AGENTS.md` (conventions) → **ce fichier** → la phase courante de `docs/plan.md` → les tâches **ouvertes** de `TODO.md` → travailler → mettre à jour `TODO.md` + `devlog/` + ce fichier (+ commit).

> **Ce fichier est court par construction (cap 6 Ko, vérifié par `tools/docs-size.sh` en CI).** Il ne raconte pas les sessions : le récit va dans `devlog/AAAA-MM-JJ.md`. Le détail vit dans `docs/state/` et se lit **à la demande** (table ci-dessous) — une session n'a jamais besoin de tout lire.

- **Phase courante :** Phase 5 — Finitions fonctionnelles, **EN COURS**.
- **Dernière session de code livrée :** 57 (2026-10-20) — **T-088 / F-040 / ADR-20 : la case « StarFish dessine les icônes du bureau » est RETIRÉE**, avec `core::Preferences` et le mode lui-même — sa demande, après l'avoir essayée : « oui, enlève cette option ». Son défaut : décochée en cours de session, la case rendait la liste d'icônes native mais **gardait notre fond hébergé**, donc **le clic droit du bureau restait celui de StarFish** jusqu'à la fermeture. Le produit ne reprend plus **jamais** la couche d'icônes (le mode survit derrière `--claim-desktop`) ; le prix de F-039 revient, dit tel quel : une icône rangée s'affiche **deux fois**. `ctest` **16/16**, `--product-probe` **33/33**, `--backdrop-demo` **43/43**. **§43**.
- **Livré juste avant :** 56 — **T-042 (1ʳᵉ tranche)** les icônes du fond répondent à la souris (⛔ suspendu par l'ADR-20) ; 55 — T-088 / ADR-19 la case (⛔ retirée) ; 52 — i18n (F-036) ; 51 — T-059.
- **Dernière mise à jour :** 2026-10-20 (session 57)
- **Branche :** `main` ; remote `origin` = github.com/Narkotine/starfish (**privé**) ; **poussé** au 2026-10-19 (sessions 55-56, accord de l'utilisateur, `bf33643`) — la session 57 est **locale** : CI non déclenchée.

## 3 prochaines actions

> L'essai **humain** d'abord (il tranche et coûte une minute) ; le reste est du travail **ou** une décision ❓, pas les deux. Ordre, raisons et scripts clic par clic : [`docs/state/next-actions-detail.md`](docs/state/next-actions-detail.md).

1. **Son essai — plus rien n'a changé sur son bureau** : lancer `build/starfish.exe` comme d'habitude suffit à le vérifier (bureau normal, clic droit de Windows ; la fenêtre de réglages n'a **plus aucune case**). Puis son retour sur les petits réglages en attente : `T-084` (arrondis 30/40), `T-053` (retirer le Peek), `T-085` (l'espace par icône en **réglage**), `T-086` (**instrumenter** avant de corriger), `T-087` (fantôme du transport). Scripts : [`docs/state/next-actions-detail.md`](docs/state/next-actions-detail.md).
2. **T-073 — le spike du paquet MSIX** : accord **obtenu** (« ok pour dépendances ») ⇒ mesurer ce que l'extension shell impose (signature, inscription, mode développeur) ; **rien installé sans son accord**.
3. ❓ **Trois questions posées en clair, sans réponse** : `T-036` (les portails en **écriture**), `T-033` (suivre les **sous-dossiers**), `T-030` (les **raccourcis**). Et `T-070` (le hook souris) attend **un geste à la main**.
**Sans blocage** : le reste de `T-014b` (les **thèmes** : `iconSize`/`padding`/`gap`/`titleBar` persistés et **inertes**), `T-081`, `T-083` (sonde `--icons-demo`), `T-013`, `T-072`, `T-015` — voir `TODO.md`. **⛔ suspendues** : `T-042` (le mode qu'elle sert n'est plus atteignable) et `T-088` (retirée, à l'archive).
## Où lire quoi (à la demande)

| Besoin | Fichier |
|---|---|
| Décisions actées — **index** des 52, une ligne chacune | [`docs/state/decisions.md`](docs/state/decisions.md) |
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
