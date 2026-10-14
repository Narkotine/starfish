# StarFish

Équivalent open source de **Stardock Fences 6** pour Windows : organiser le bureau en zones (« fences ») dans lesquelles déposer, trier et déplacer ses icônes.

> Même fonctionnalités, implémentation et identité différentes. Objectif : parité fonctionnelle progressive, pas un clone.

## État du projet

**Phase 3 — Zones complètes.** Le rendu des zones hébergées est réglé (T-020 : une surface Win32 brute blitte une image rendue par Qt hors écran), les icônes du bureau réel s'affichent dans une zone avec sélection, double-clic, menu contextuel Shell et glisser-déposer (T-005/T-006), et les zones se créent, se déplacent, se redimensionnent, s'aimantent et se suppriment (T-007). Reste la persistance du layout (T-008).

Build et tests **verts en local** (Qt 6.8.3 + MinGW 13.1.0, voir plus bas) : 5 suites de tests unitaires.

⚠️ **Les validations à l'écran restent à faire** (T-021 et suivantes) : elles exigent un bureau libre et un œil humain, et ne peuvent pas être conduites depuis un agent — voir `STATE.md`, « Décisions actées » 13.

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

La CI GitHub Actions (`.github/workflows/ci.yml`) construit et teste automatiquement sur `windows-latest` (MSVC). Le build local est validé depuis le 2026-10-13 avec **Qt 6.8.3 `win64_mingw` + MinGW 13.1.0**, installés sans droits administrateur via `aqtinstall` (la machine de dev n'a pas MSVC) :

```bash
python -m aqt install-qt windows desktop 6.8.3 win64_mingw -O C:/Qt
python -m aqt install-tool windows desktop tools_mingw1310 qt.tools.win64_mingw1310 -O C:/Qt
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/mingw_64
```

Détail et portabilité : `docs/technique.md` §6.

### Lancer

Sans argument, le binaire démarre et se termine immédiatement (utilisable en CI, aucune intégration bureau).

```bash
starfish --dump                 # hiérarchie Progman / SHELLDLL_DefView, puis quitte
starfish --list-desktop         # contenu du Bureau tel que le Shell le voit, puis quitte
starfish --list-desktop --icons --icon-size 48 --icon-dump out/icons
starfish --host-demo            # affiche une zone vide sur le bureau pendant 10 s
starfish --host-demo --seconds 30 --hide-icons
starfish --fence-demo           # cycle de vie des zones, rapport PASS/FAIL, puis quitte
starfish --fence-demo --host --seconds 10
```

Le binaire est en sous-système GUI : quand la sortie est redirigée (`> log.txt`), elle est respectée ; sinon la console du parent est récupérée.

`--host-demo` journalise le verdict de placement (`below icon layer` attendu) — mais **le z-order seul ne prouve rien** (ADR-4) : confirmer visuellement que la zone est au-dessus du papier peint et sous les icônes, **avec le bureau réellement visible** (une fenêtre maximisée au premier plan donne un faux négatif).

### Expérimentations

`experiments/spike_workerw.py` valide le placement des fenêtres sur le bureau (ADR-4). Nécessite le **Python système** (Tkinter absent du Python managé) :

```bash
python experiments/spike_workerw.py
```
