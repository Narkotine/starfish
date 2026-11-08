# AGENTS.md — StarFish

> **Flux de travail (un seul build `build\dev` = StarFish quotidien + tests ; `--quit` puis relance à chaque lot ; push sur demande) :** [`docs/workflow-dev.md`](docs/workflow-dev.md).
>
> **Reprise rapide (nouvel agent) :** lire [`docs/reprise.md`](docs/reprise.md) - ordre de lecture, commandes, données, règles, pièges.

> Instructions pour tout agent de codage IA travaillant sur ce dépôt. Fichier canonique ; `AGENT.md` en est un alias. Concis par conception : il est injecté à chaque prompt.

## Protocole d'entrée (obligatoire, dans cet ordre)

1. **`STATE.md`** — où on en est. **Court par construction (cap 6 Ko)** : il ne raconte pas les sessions (c'est le rôle du devlog) et son détail vit dans `docs/state/` (décisions et garde-fous par domaine, inventaire, écarts, scripts d'essai), lu **à la demande**. `tools/docs_size.py` (en CI) tient le cap.
2. `README.md` — carte des documents.
3. La phase courante dans `docs/plan.md`, puis `TODO.md` pour les tâches ouvertes (les tâches ✅ sont dans `docs/todo-archive.md`).

## Protocole de sortie (obligatoire avant de terminer une session)

- [ ] Mettre à jour les statuts de `TODO.md` (⬜ à faire / 🔄 en cours / ✅ fait / ⛔ bloquée).
- [ ] Ajouter une entrée datée dans `devlog/AAAA-MM-JJ.md` (créer le fichier si absent).
- [ ] Mettre à jour `STATE.md` : phase courante, décisions, prochaines actions.
- [ ] Commiter si `git` est initialisé (Conventional Commits, en anglais).
- [ ] Après des tâches terminées : mettre à jour la feuille de route **locale** de l'utilisateur (le fichier `starfish road.txt` sur son Bureau) avec ce qui reste et les décisions en attente, **si elle est accessible** ; sinon ne pas bloquer. Ce fichier n'entre **jamais** dans le dépôt. À chaque écriture, mettre en tête la date et l'heure de la modification (fuseau de l'utilisateur).

Une session qui ne met pas à jour ces fichiers est considérée comme perdue pour la reprise.

## Conventions

- **Langue :** code, commentaires et messages de commit en anglais ; documentation et devlog en français (l'utilisateur est francophone).
- **Identifiants :** `F-xxx` fonctionnalités (inventaire dans `docs/produit.md`), `T-xxx` tâches (`TODO.md`), `ADR-x` décisions techniques (`docs/technique.md`). Toute nouvelle fonctionnalité = une ligne dans l'inventaire + une tâche.
- **Périmètre v1 :** Windows 10/11 x64 uniquement, C++17 + Qt 6.8. Consulter les ADR de `docs/technique.md` avant d'en dévier ; toute déviation = nouvel ADR.
- **Règle d'or données :** ne jamais déplacer de fichier sur le disque sans opt-in explicite (`moveFiles: true` dans une règle). StarFish gère l'affichage, pas l'emplacement des fichiers.
- **Tests :** la logique pure (`RuleEngine`, `LayoutStore`) se teste sans UI ; consigner les tests manuels shell dans le devlog.

## Portabilité

- Fichiers commités = Markdown brut + code, **aucun chemin ni outil spécifique** à un harness. Les dossiers de mémoire d'un outil (`.workbuddy-ai/`, `.cursor/`…) ne doivent jamais être référencés par la doc.
- **git est la couche de portabilité** : committer en fin de session permet à n'importe quel harness (ou humain) de reprendre depuis `HEAD`.
- Points d'entrée alternatifs maintenus : `AGENT.md` → `AGENTS.md` → `STATE.md`.
