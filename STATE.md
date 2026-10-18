# STATE.md — Où on en est

> **Protocole de reprise :** `AGENTS.md` (conventions) → **ce fichier** → la phase courante de `docs/plan.md` → les tâches **ouvertes** de `TODO.md` → travailler → mettre à jour `TODO.md` + `devlog/` + ce fichier (+ commit).

> **Ce fichier est court par construction (cap 6 Ko, vérifié par `tools/docs-size.sh` en CI).** Il ne raconte pas les sessions : le récit va dans `devlog/AAAA-MM-JJ.md`. Le détail vit dans `docs/state/` et se lit **à la demande** (table ci-dessous) — une session n'a jamais besoin de tout lire.

- **Phase courante :** Phase 5 — Finitions fonctionnelles, **EN COURS**.
- **Dernière session de code livrée :** 50 (2026-10-18) — **T-080 ✅** : l'**apparence par zone** arrive **dans le menu d'une zone** — `Colours ▸` (panneau, barre de titre) et `Corner radius ▸` (*Square* / 6 / 10 / 14 / 20). Clé neuve `FenceStyle::titleBarColor` (`#AARRGGBB`, **vide = pas de bande**), écrite **seulement** quand elle existe ⇒ **aucune ligne** ajoutée aux documents existants, **aucune migration**. ⚠️ **Le rayon appartient à la géométrie** : `FenceView::cornerRadius()` = `min(demandé, demi-petit-côté)`, le document garde la demande, la **région** prend le possible. `ctest` **15/15** ; `--fence-demo` **44/44** headless / **62/62** `--host` (sept vérifications neuves, pixels) ; prémisse prouvée (**3 `FAIL`**). Détail : **§38** + **ADR-18**.
- **Dernière session :** 50 (2026-10-18) — session de **code** (T-080), ouverte par « rattache cette conversation au projet StarFish, puis continue le développement » ; décision **technique**, pas d'arbitrage à attendre.
- **Dernière mise à jour :** 2026-10-18 (session 50)
- **Branche :** `main` ; remote `origin` = github.com/Narkotine/starfish (**privé**) ; **non poussé** (le push déclenche la CI et exige l'accord de l'utilisateur).

## 3 prochaines actions

> L'essai **humain** d'abord (il tranche et coûte une minute) ; le reste est du travail **ou** une décision ❓, pas les deux. Ordre, raisons et scripts clic par clic : [`docs/state/next-actions-detail.md`](docs/state/next-actions-detail.md).

1. **L'essai à l'œil de la session 50 (T-080)** — clic droit sur une zone → **`Colours ▸` → `Title bar colour...`** : la barre prend la couleur, le panneau ne bouge pas ; puis **`Title bar: same as the panel`** (elle n'apparaît **que** là) la remet au panneau. Puis **`Corner radius ▸`** : deux ou trois arrondis, dont *Square* (**T-058** juge, à l'œil). Et l'essai de la s49 s'il n'est pas fait (zone *Very translucent* : papier peint à travers le **panneau**, **tuiles nettes**).
2. **T-073 ❓ — l'entrée F-001 dans le menu principal de Windows 11** : extension shell `IExplorerCommand` ⇒ **paquet sparse MSIX** ⇒ **dépendance de packaging** : **spike et accord utilisateur avant toute installation**. Ensuite **T-042** (le prix de l'ADR-3).
3. **`T-059` puis `T-058` / `T-081`** — déclasser la fenêtre de réglages : le matériau **et** l'apparence ont désormais leur endroit (**le menu d'une zone**). Puis les **coins** (T-058, à l'œil) et le **défaut général + « appliquer à toutes »** (T-081 : une clé, un défaut, rien qui migre — la forme de T-080). **`T-076`** reste ouvert d'une entrée (barre de titre affichable/masquable), **`T-070`** ferme la marche.
**Sans blocage**, à prendre ensuite : `T-047`, `T-013`, `T-014b`, `T-015` (packaging v0.1) — voir `TODO.md`.

## Où lire quoi (à la demande)

| Besoin | Fichier |
|---|---|
| Décisions actées — **index** des 50, une ligne chacune | [`docs/state/decisions.md`](docs/state/decisions.md) |
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
- **Ne pas réordonner les deux fenêtres d'une zone** (T-082) : `SetWindowPos` insère **sous** la fenêtre nommée, donc c'est **le panneau** qui est posé **sous son overlay** (`setStackedBelow`). L'inverse laisse les tuiles effacées derrière leur panneau (mesuré).
- **Écrire court** : une session = une entrée `devlog/` + ses lignes ici + un commit. Rien de plus. Et **détail technique non déroulé** (consigne utilisateur du 2026-10-17 — vibe coding) : on consigne les faits (mesures, fichiers, pièges), **une ligne par fait**, sans justifications pas à pas ; la conclusion d'abord.
- **Ne pas confondre le rayon *demandé* et le rayon *dessiné* (T-080)** : la vue borne (`min(demandé, demi-petit-côté)`) — et c'est **ce** nombre que la **région** doit recevoir.
