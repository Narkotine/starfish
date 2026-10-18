# STATE.md — Où on en est

> **Protocole de reprise :** `AGENTS.md` (conventions) → **ce fichier** → la phase courante de `docs/plan.md` → les tâches **ouvertes** de `TODO.md` → travailler → mettre à jour `TODO.md` + `devlog/` + ce fichier (+ commit).

> **Ce fichier est court par construction (cap 6 Ko, vérifié par `tools/docs-size.sh` en CI).** Il ne raconte pas les sessions : le récit va dans `devlog/AAAA-MM-JJ.md`. Le détail vit dans `docs/state/` et se lit **à la demande** (table ci-dessous) — une session n'a jamais besoin de tout lire.

- **Phase courante :** Phase 5 — Finitions fonctionnelles, **EN COURS**.
- **Dernière session de code livrée :** 47 (2026-10-18) — **T-076 → 🔄 / T-064 ✅ / ADR-16** : une zone a **son** menu (clic droit sur la barre de titre ou le panneau vide — `Material ▸`, `Roll up`/`Roll down`, `Rename...`, `Remove this fence` ; le clic droit sur une **tuile** garde le menu du Shell). Contenu = donnée pure, effets = les appels du tray. Sonde neuve `--fence-menu-probe` : **98 %** des pixels du rect du menu changent à l'écran, `WindowFromPoint` rend notre popup, **0** pixel différent après fermeture. `ctest` 15/15 ; `--fence-demo` 35/35 headless, 48/48 `--host`.
- **Dernière session :** 47 (2026-10-18) — session de **code**, close par une **demande consignée** (T-082) : l'essai du menu est confirmé, et le matériau efface **aussi les icônes** (voir action 1). Consigne utilisateur du soir : **écrire bref** (faits consignés, non déroulés), il est en vibe coding.
- **Dernière mise à jour :** 2026-10-18 (session 47)
- **Branche :** `main` ; remote `origin` = github.com/Narkotine/starfish (**privé**) ; **non poussé** (le push déclenche la CI et exige l'accord de l'utilisateur).

## 3 prochaines actions

> L'essai **humain** d'abord (il tranche et coûte une minute) ; le reste est du travail **ou** une décision ❓, pas les deux. Ordre, raisons et scripts clic par clic : [`docs/state/next-actions-detail.md`](docs/state/next-actions-detail.md).

1. **T-082 ❓ — le matériau doit être celui du *panneau*, pas de la zone entière** (retour utilisateur, session 47, **après un essai confirmé** : « tests ok »). Aujourd'hui, depuis ADR-15, l'alpha est celui de la **fenêtre** : la barre de titre **et les icônes** s'effacent avec le fond, alors que F-038 décrit le **panneau**. Demandé : panneau seul par défaut, application **aux icônes** en **option par zone**. **Deux candidats écrits** (deux fenêtres par zone · composition du fond dans la frame), chacun avec son prix : **à trancher avant tout code** (détail et champ de document : `TODO.md`, T-082).
2. **Les retours de la session 42, puis `T-042`** — **T-078** (les noms les plus longs, suite de `T-049`), **T-073** (l'entrée F-001 dans le menu **principal** de Windows 11 : extension shell/MSIX ⇒ spike **et** accord utilisateur avant toute installation) ; ensuite les interactions des icônes hors zone (**T-042**, le prix de l'ADR-3).
3. **`T-059` puis `T-080`** — la fenêtre de réglages peut être déclassée (le matériau **par zone** a un endroit : le menu d'une zone) ; l'apparence par zone (couleurs, rayon, couleur de barre de titre — absente du schéma) vit **dans ce même menu**. **`T-076` reste ouvert d'une entrée** (afficher/masquer la barre de titre) et **`T-070`** (hook souris) ferme la marche. Restent aussi, à l'œil, les points de la s46 : **coins** (T-058), **translucide**, **ressenti** de T-050/T-077.
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
