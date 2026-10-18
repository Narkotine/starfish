# STATE.md — Où on en est

> **Protocole de reprise :** `AGENTS.md` (conventions) → **ce fichier** → la phase courante de `docs/plan.md` → les tâches **ouvertes** de `TODO.md` → travailler → mettre à jour `TODO.md` + `devlog/` + ce fichier (+ commit).

> **Ce fichier est court par construction (cap 6 Ko, vérifié par `tools/docs-size.sh` en CI).** Il ne raconte pas les sessions : le récit va dans `devlog/AAAA-MM-JJ.md`. Le détail vit dans `docs/state/` et se lit **à la demande** (table ci-dessous) — une session n'a jamais besoin de tout lire.

- **Phase courante :** Phase 5 — Finitions fonctionnelles, **EN COURS**.
- **Dernière session de code livrée :** 47 (2026-10-18) — **T-076 → 🔄 / T-064 ✅ / ADR-16** : une zone a **son** menu (clic droit sur la barre de titre ou le panneau vide — `Material ▸`, `Roll up`/`Roll down`, `Rename...`, `Remove this fence` ; le clic droit sur une **tuile** garde le menu du Shell). Contenu = donnée pure, effets = les appels du tray. Sonde neuve `--fence-menu-probe` : **98 %** des pixels du rect du menu changent à l'écran, `WindowFromPoint` rend notre popup, **0** pixel différent après fermeture. `ctest` 15/15 ; `--fence-demo` 35/35 headless, 48/48 `--host`.
- **Dernière session :** 47 (2026-10-18) — session de **code**. Consigne utilisateur du soir : **écrire bref** (faits consignés, non déroulés), il est en vibe coding.
- **Dernière mise à jour :** 2026-10-18 (session 47)
- **Branche :** `main` ; remote `origin` = github.com/Narkotine/starfish (**privé**) ; **non poussé** (le push déclenche la CI et exige l'accord de l'utilisateur).

## 3 prochaines actions

> L'essai **humain** d'abord (il tranche et coûte une minute) ; le reste est du travail **ou** une décision ❓, pas les deux. Ordre, raisons et scripts clic par clic : [`docs/state/next-actions-detail.md`](docs/state/next-actions-detail.md).

1. **Essai à la main (utilisateur)** — le **menu d'une zone** : clic droit sur la **barre de titre** ou sur le **panneau vide** → `Material ▸` (le panneau doit changer sous les yeux), `Roll up`/`Roll down`, `Rename...`, `Remove this fence` ; et un clic droit sur une **tuile** → le menu **de Windows**, inchangé. Toujours en attente depuis la s46 : les **coins** (région 1 bit, non anticrénelés — **T-058**), le **translucide** (papier peint à travers) et le **ressenti** de `T-050`/`T-077` (plus de clignotement au repos, redimensionnement stable).
2. **Les retours de la session 42, puis `T-042`** — **T-078** (les noms les plus longs, suite de `T-049`), **T-073** (l'entrée F-001 dans le menu **principal** de Windows 11 : extension shell/MSIX ⇒ spike **et** accord utilisateur avant toute installation) ; ensuite les interactions des icônes hors zone (**T-042**, le prix de l'ADR-3).
3. **`T-059` puis `T-080`, maintenant que `T-064` est livré** — la fenêtre de réglages peut être déclassée (le matériau **par zone** a un endroit : le menu) ; l'apparence par zone (couleurs, rayon, couleur de barre de titre — absente du schéma) vit **dans ce même menu**. **`T-076` reste ouvert d'une entrée** : masquer/afficher la barre de titre, qui demande un geste de déplacement survivant à sa disparition. **`T-070`** (le hook souris) ferme la marche : il ne sert plus qu'au chemin de développement.
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
- **Écrire court** : une session = une entrée `devlog/` + ses lignes ici + un commit. Rien de plus. Et **détail technique non déroulé** (consigne utilisateur du 2026-10-17 — vibe coding) : on consigne les faits (mesures, fichiers, pièges), **une ligne par fait**, sans justifications pas à pas ; la conclusion d'abord.
