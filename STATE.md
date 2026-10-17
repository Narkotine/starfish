# STATE.md — Où on en est

> **Protocole de reprise :** `AGENTS.md` (conventions) → **ce fichier** → la phase courante de `docs/plan.md` → les tâches **ouvertes** de `TODO.md` → travailler → mettre à jour `TODO.md` + `devlog/` + ce fichier (+ commit).

> **Ce fichier est court par construction (cap 6 Ko, vérifié par `tools/docs-size.sh` en CI).** Il ne raconte pas les sessions : le récit va dans `devlog/AAAA-MM-JJ.md`. Le détail vit dans `docs/state/` et se lit **à la demande** (table ci-dessous) — une session n'a jamais besoin de tout lire.

- **Phase courante :** Phase 5 — Finitions fonctionnelles, **EN COURS**.
- **Dernière session de code livrée :** 44 (2026-10-17) — **T-050 ✅ + T-077 ✅** : la surface ne travaille plus quand rien n'a changé (le watchdog demande avant de réparer ; plus de frame périmée poussée à la nouvelle position). `ctest` 14/14 ; `--fence-demo` 30/30 headless, 42/42 `--host`.
- **Dernière session :** 45 (2026-10-17) — **session de documentation** : ce fichier passe de 230 Ko à 4 Ko, le détail part dans `docs/state/`, les tâches terminées dans `docs/todo-archive.md`, et `tools/docs_size.py` (en CI) tient les caps. **Les 3 prochaines actions n'ont pas bougé.**
- **Dernière mise à jour :** 2026-10-17 (session 45)
- **Branche :** `main` ; remote `origin` = github.com/Narkotine/starfish (**privé**) ; **CI verte**.

## 3 prochaines actions

> L'essai **humain** d'abord (il tranche et coûte une minute) ; le reste est du travail **ou** une décision ❓, pas les deux. Ordre, raisons et scripts clic par clic : [`docs/state/next-actions-detail.md`](docs/state/next-actions-detail.md).

1. **`T-079` (P0) — une zone neuve n'apparaît pas.** Retour utilisateur de la session 44 resté **non consigné** alors, **toujours présent** : « je ne vois plus les fences créées, ou nouvellement créée, même en matériel opaque ». Son diagnostic est **reconstitué et prêt à réappliquer** : `docs/state/diagnostics/t079/` (lire son `README.md` d'abord).
2. **Essai à la main (utilisateur)** — six vérifications en attente : **`T-079`** (une zone neuve doit apparaître), `T-050`/`T-077` (plus de clignotement au repos, redimensionnement stable), `T-071` (le bureau est redevenu le bureau), `T-069` (le bureau est atteignable), `ADR-13` (rien n'est plus pris au bureau), `T-057` (matériaux : papier peint à travers, souris partout).
3. **Les retours de la session 42, puis `T-042` → `T-064` → `T-059`** — `T-076` (un menu à nous au clic droit sur une zone), `T-078` (les noms les plus longs, suite de `T-049`), `T-073` (l'entrée F-001 dans le menu principal de Windows 11) ; ensuite les interactions des icônes hors zone (le prix de l'ADR-3), puis le matériau par clic droit **avant** de nettoyer la fenêtre de réglages.

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
- Ne pas confondre « le produit va bien » et « la sonde est verte » : une sonde mesure ce qu'elle a été écrite pour mesurer (le reste est dans `docs/state/guardrails/08-outils-verification.md`).
- **Écrire court** : une session = une entrée `devlog/` + ses lignes ici + un commit. Rien de plus.
