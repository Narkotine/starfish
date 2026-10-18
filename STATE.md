# STATE.md — Où on en est

> **Protocole de reprise :** `AGENTS.md` (conventions) → **ce fichier** → la phase courante de `docs/plan.md` → les tâches **ouvertes** de `TODO.md` → travailler → mettre à jour `TODO.md` + `devlog/` + ce fichier (+ commit).

> **Ce fichier est court par construction (cap 6 Ko, vérifié par `tools/docs-size.sh` en CI).** Il ne raconte pas les sessions : le récit va dans `devlog/AAAA-MM-JJ.md`. Le détail vit dans `docs/state/` et se lit **à la demande** (table ci-dessous) — une session n'a jamais besoin de tout lire.

- **Phase courante :** Phase 5 — Finitions fonctionnelles, **EN COURS**.
- **Dernière session de code livrée :** 55 (2026-10-18) — **T-088 ✅ (F-040) / ADR-19 : le rachat des icônes devient une case** — « StarFish draws the desktop icons » dans Réglages, **son prix dessous**, persistée (`%APPDATA%/StarFish/preferences.json`, jamais dans le document ni dans le registre), « Restaurer mes icônes » offert dans le tray, et le rachat ne se déclenche **que si une zone détient une icône**. Défaut : **non**. `ctest` **17/17**, `--product-probe` **36/36**, i18n 67/67. **§41**.
- **Livré juste avant :** 52 — **T-014b i18n ✅ (F-036)** (`--lang` pour forcer) ; 51 — **T-059 ✅** (la fenêtre de réglages n'est pas le produit) ; 50 — **T-080 ✅ / ADR-18** (apparence par zone).
- **Dernière session avant :** 54 — **son essai à l'œil** : validés (clignotements, bureau rendu, icônes intactes, matériau, coins, couleurs/arrondis) ; ouverts : `T-084` à `T-087` ; tranchés : `T-073`, `T-032`, `T-052`, `T-053` (le Peek part), `T-048` (abandonnée).
- **Dernière mise à jour :** 2026-10-18 (session 55)
- **Branche :** `main` ; remote `origin` = github.com/Narkotine/starfish (**privé**) ; **poussé** au 2026-10-18 (session 53, accord de l'utilisateur) — tout push déclenche la CI.

## 3 prochaines actions

> L'essai **humain** d'abord (il tranche et coûte une minute) ; le reste est du travail **ou** une décision ❓, pas les deux. Ordre, raisons et scripts clic par clic : [`docs/state/next-actions-detail.md`](docs/state/next-actions-detail.md).

1. **Son essai — une case neuve l'attend** : **Réglages** (clic sur l'icône de la zone de notification) → cocher « **StarFish dessine les icônes du bureau** », ranger une icône dans une zone, regarder le bureau. Les icônes **hors zone** deviennent muettes tant que la case est cochée (ni clic, ni lancement, ni menu) : c'est ce que la case dit sous elle, et **`T-042`** est ce qui leur rendrait la vie. Puis son retour : `T-084` (arrondis 30/40), `T-053` (retirer le Peek), `T-085` (l'espace par icône en **réglage**), `T-086` (**instrumenter** avant de corriger), `T-087` (fantôme + aperçu). Ordre, raisons et scripts : [`docs/state/next-actions-detail.md`](docs/state/next-actions-detail.md).
2. **T-073 — le spike du paquet MSIX** : accord **obtenu** (« ok pour dépendances ») ⇒ mesurer ce que l'extension shell impose (signature, inscription, mode développeur) ; **rien installé sans son accord**.
3. ❓ **Trois questions posées en clair, sans réponse** : `T-036` (les portails en **écriture**), `T-033` (suivre les **sous-dossiers**), `T-030` (les **raccourcis**). Et **T-070** (le hook souris) attend **un geste à la main**.
**Sans blocage** : le reste de `T-014b` (les **thèmes** : `iconSize`/`padding`/`gap`/`titleBar` persistés et **inertes**), `T-081`, `T-083` (sonde `--icons-demo`), `T-042`, `T-013`, `T-072`, `T-015` — voir `TODO.md`.
## Où lire quoi (à la demande)

| Besoin | Fichier |
|---|---|
| Décisions actées — **index** des 51, une ligne chacune | [`docs/state/decisions.md`](docs/state/decisions.md) |
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
