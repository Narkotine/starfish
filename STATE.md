# STATE.md — Où on en est

> **Protocole de reprise :** `AGENTS.md` (conventions) → **ce fichier** → la phase courante de `docs/plan.md` → les tâches **ouvertes** de `TODO.md` → travailler → mettre à jour `TODO.md` + `devlog/` + ce fichier (+ commit).

> **Ce fichier est court par construction (cap 6 Ko, vérifié par `tools/docs-size.sh` en CI).** Il ne raconte pas les sessions : le récit va dans `devlog/AAAA-MM-JJ.md`. Le détail vit dans `docs/state/` et se lit **à la demande** (table ci-dessous) — une session n'a jamais besoin de tout lire.

- **Phase courante :** Phase 5 — Finitions fonctionnelles, **EN COURS**.
- **Dernière session de code livrée :** 49 (2026-10-18) — **T-082 ✅** : le matériau efface le **panneau et sa barre de titre**, les **tuiles restent opaques** (F-038, ADR-17). Une fenêtre ne porte qu'un alpha : les tuiles ont leur **seconde fenêtre** par zone — opaque, **région = union des tuiles**, **clic-traversante**, **un seul rendu** et **un seul chemin d'entrée** (souris, `setInputTarget`, OLE de dépôt restent sur le panneau). L'application **aux icônes** devient une **option par zone** (`FenceStyle::fadeIcons`, document, `false` par défaut, **aucune migration**), offerte par le menu d'une zone **sous `Material ▸`** et **seulement** sous un panneau non opaque. `ctest` 15/15 ; `--fence-demo` **36/36** headless, **54/54** `--host`. ⚠️ La **première** version avait le couple **inversé** (`SetWindowPos` insère *sous* la fenêtre nommée) : la sonde l'a dit (`overlay above the panel=0`). Détail : **§37**.
- **Dernière session :** 49 (2026-10-18) — session de **code** (T-082), ouverte par « continue dev ». Décision **technique** (le candidat (b) est **faux au-dessus d'une autre zone**) ⇒ **pas** d'arbitrage utilisateur à attendre (précédent ADR-15). Consigne de forme : **écrire bref**.
- **Dernière mise à jour :** 2026-10-18 (session 49)
- **Branche :** `main` ; remote `origin` = github.com/Narkotine/starfish (**privé**) ; **non poussé** (le push déclenche la CI et exige l'accord de l'utilisateur).

## 3 prochaines actions

> L'essai **humain** d'abord (il tranche et coûte une minute) ; le reste est du travail **ou** une décision ❓, pas les deux. Ordre, raisons et scripts clic par clic : [`docs/state/next-actions-detail.md`](docs/state/next-actions-detail.md).

1. **L'essai à l'œil de la session 49 (T-082)** — une zone en *Very translucent* : le papier peint doit traverser le **panneau** et sa **barre de titre**, les **tuiles** rester **nettes** ; puis cocher **`Material on the icons too`** dans le menu de la zone et vérifier que **tout** s'efface (comme avant T-082).
2. **T-073 ❓ — l'entrée F-001 dans le menu principal de Windows 11** : il faut une extension shell `IExplorerCommand`, donc un **paquet sparse MSIX** ⇒ **dépendance de packaging** : **spike et accord utilisateur avant toute installation**. Ensuite **T-042** (les interactions des icônes hors zone, le prix de l'ADR-3).
3. **`T-059` puis `T-080`** — déclasser la fenêtre de réglages : le matériau **par zone** a désormais deux endroits naturels (le menu d'une zone, et `fadeIcons` qui y est **par zone**). Puis l'apparence par zone (couleurs, rayon, couleur de barre de titre — absente du schéma). **`T-076` reste ouvert d'une entrée** (afficher/masquer la barre de titre) et **`T-070`** (hook souris) ferme la marche. Restent aussi, à l'œil, les points de la s46 : **coins** (T-058), **translucide**, **ressenti** de T-050/T-077.
**Sans blocage**, à prendre ensuite : `T-047`, `T-013`, `T-014b`, `T-015` (packaging v0.1) — voir `TODO.md`.

## Où lire quoi (à la demande)

| Besoin | Fichier |
|---|---|
| Décisions actées — **index** des 46, une ligne chacune | [`docs/state/decisions.md`](docs/state/decisions.md) |
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
