# StarFish

Équivalent open source de **Stardock Fences 6** pour Windows : organiser le bureau en zones (« fences ») dans lesquelles déposer, trier et déplacer ses icônes.

> Même fonctionnalités, implémentation et identité différentes. Objectif : parité fonctionnelle progressive, pas un clone.

## État du projet

**Phase 0 — Cadrage (documentation).** Aucune ligne de code pour l'instant.

- Prix de référence du produit original : abonnement annuel à partir de ~10 $ USD (Stardock, consulté le 2026-10-13). StarFish sera gratuit, open source, local-first, sans télémétrie.

## Carte des documents

| Fichier | Rôle |
|---|---|
| [`AGENTS.md`](AGENTS.md) / [`AGENT.md`](AGENT.md) | **Point d'entrée pour tout agent IA** (conventions harness-agnostiques). Lire AVANT `STATE.md`. |
| [`STATE.md`](STATE.md) | État courant, prochaines actions, protocole de reprise (résilient aux resets de contexte). |
| [`TODO.md`](TODO.md) | Suivi des tâches : à faire / en cours / fait. |
| [`devlog/`](devlog/) | Journal de développement, une entrée datée par session de travail. |
| [`docs/produit.md`](docs/produit.md) | Documentation produit : vision, personas, cas d'usage, inventaire fonctionnel complet (référence Fences 6), périmètre par version, non-objectifs. |
| [`docs/plan.md`](docs/plan.md) | Plan de mise en œuvre par phases avec critères de sortie. |
| [`docs/technique.md`](docs/technique.md) | Architecture, décisions techniques (ADR), modèle de données, intégration Windows, build/packaging. |

## Reprendre le travail (agent ou humain)

1. Lire `STATE.md` (où on en est, quoi faire ensuite).
2. Lire la phase courante dans `docs/plan.md`.
3. Cocher les tâches faites dans `TODO.md`.
4. Travailler.
5. **Avant de terminer :** mettre à jour `TODO.md`, ajouter une entrée dans `devlog/AAAA-MM-JJ.md`, mettre à jour `STATE.md`.

Conventions de nommage : `F-xxx` = fonctionnalité, `T-xxx` = tâche, `ADR-x` = décision technique. Docs et devlog en français ; code, commentaires et commits en anglais.

## Licence

Prévue : MIT (à confirmer à la première release).

## Build

Cible v1 : Windows, Qt 6.8 (MSVC 2022 x64), CMake ≥ 3.24, Ninja.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

La CI GitHub Actions (`.github/workflows/ci.yml`) construit et teste automatiquement sur `windows-latest`. Le build local n'est **pas encore vérifié** (pas de compilateur ni de Qt sur la machine de dev au 2026-10-13) — la validation passe par la CI au premier push.

### Expérimentations

`experiments/spike_workerw.py` valide le placement des fenêtres sur le bureau (ADR-4). Nécessite le **Python système** (Tkinter absent du Python managé) :

```bash
python experiments/spike_workerw.py
```
