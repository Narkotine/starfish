# StarFish

Équivalent open source de **Stardock Fences 6** pour Windows : organiser le bureau en zones (« fences ») dans lesquelles déposer, trier et déplacer ses icônes.

> Même fonctionnalités, implémentation et identité différentes. Objectif : parité fonctionnelle progressive, pas un clone.

## État du projet

**Un mode produit existe (T-038).** Lancé **sans argument**, `starfish.exe` *est* l'application : il charge la disposition, pose les zones sur le bureau, affiche une **icône de notification** (« New fence », afficher/masquer les zones, Peek, masquer les icônes du bureau, **« Restore my icons »**, réglages, quitter) et une **fenêtre de réglages** (renommer, enrouler, supprimer une zone), surveille le **vrai dossier Bureau** pour les règles, et réécrit le document en sortant. Les zones se déplacent et se redimensionnent **à la souris** : la capture souris manquait (`SetCapture`), ce qui rendait tout déplacement impossible — c'était l'écart n°6 de l'audit. Sonde `--product-probe` : **16/16 PASS**. ⚠️ L'interface est en **anglais** pour l'instant (i18n = **T-014b** / F-036 — v0.2).

**StarFish possède la couche d'icônes (T-039).** C'est la réponse au mur du z-order : une zone est posée **sous** `SHELLDLL_DefView`, donc au centre d'une zone le clic part à la liste du bureau — masquer les icônes natives n'est pas cosmétique, c'est la condition pour qu'une zone serve à quelque chose. Une fois la couche masquée, quelqu'un doit dessiner les icônes qui n'étaient dans aucune zone : c'est le **fond de bureau** (`src/ui/desktop_backdrop.{hpp,cpp}`), une surface **plein écran, transparente (per-pixel) et traversée par les clics**, placée **sous les zones**. Ce qu'il dessine est **calculé** — *toutes les entrées du bureau moins celles qu'une zone détient* (`src/ui/backdrop_layout.hpp`, **pur**) — et **jamais stocké** : le document ne bouge pas d'un octet, et supprimer une zone **rend** ses icônes sans une ligne de code de restauration. La disposition est **en colonnes**, comme le bureau Windows. ⚠️ Le fond est décoratif : il peint les icônes, il ne les **possède** pas encore (ni clic, ni renommage, ni menu contextuel — T-042), et deux prix sont assumés (déposer un fichier sur le bureau vide → T-052 ; le Peek n'a plus d'effet visible → T-053). Sonde `--backdrop-demo` : **15/15 PASS** headless, **21/21** en `--host` (z-order sous `DefView`, push per-pixel confirmé, 6 % de la frame peinte, et **strictement moins** quand une zone réclame des icônes).

**Le filet de sécurité est posé (T-046).** Masquer les icônes natives est un changement de toute la session, donc une fin brutale aurait laissé le bureau **sans icônes** : désormais chaque masquage laisse un **marqueur** (`%APPDATA%/StarFish/icons-hidden.marker`) écrit **avant** de masquer, le **lancement suivant restaure inconditionnellement** si le marqueur est là, et le tray offre une sortie de secours **« Restore my icons »**. Sonde `--crash-demo` : **20/20 PASS** — elle masque les vraies icônes, **tue le processus qui les a masquées**, et vérifie que le lancement suivant les rend ; en neutralisant la réparation (test temporaire), elle tombe à `rc=1` avec exactement deux `FAIL` (prémisse prouvée).

**Phase 5 — Finitions fonctionnelles, entamée.** T-012 (snapshots) est faite : une **snapshot** est une disposition nommée (`SnapshotManager`, `src/core/snapshot.{hpp,cpp}`), la **restaurer** c'est appliquer la disposition aux zones, et l'export/import réutilise le document schéma 1 — aucun second format à tenir synchronisé. `--snapshot-demo` le prouve de bout en bout (**11/11 PASS**), y compris le retour dans une zone réelle (`FenceContainer::applyLayout`). **T-014a (multi-écran + DPI, F-032) est faite côté code** : les écrans sont un modèle **pur** (`src/ui/screen_layout.hpp` — rects **physiques** d'écran virtuel, DPR, restauration), une zone appartient à l'écran de **son centre** et y reste entière (jamais dans la zone morte entre deux écrans de tailles différentes), l'aimantation cible **chaque** écran donc le bord interne du moniteur voisin, un écran **débranché ramène ses zones** au lieu de les perdre, et le DPI est franchi à **deux** frontières (frame rendu en pixels physiques, souris reçue divisée par le ratio). `tests/test_screen_layout.cpp` : **20 cas**, `ctest` **12/12**. La sonde `--screen-demo` (**19/19**) **lit** l'arrangement réel de la machine et **mesure** la mise à l'échelle de la peinture (prémisse prouvée) ; il reste la **passe de tests à deux écrans** que la phase réclame — **bloquée par le matériel** (la machine de dev n'a qu'un écran, lu par la sonde : 2560×1440 à 100 %). Restent **T-014b** (i18n en/fr + thèmes, P1/v0.2) et **T-013** (pages de bureau, P2).

**Phase 4 — Fonctions phares, terminée.** L'auto-organisation est en place (T-010) : des règles déclaratives (type, motif de nom, fenêtre d'âge, zone cible) rangent automatiquement les fichiers qui apparaissent dans un dossier surveillé — vérifié pour de vrai par `--rules-demo`, un fichier créé dans un dossier surveillé étant signalé en 300 ms puis rangé dans la bonne zone (28/28 PASS). **Aucun fichier n'est jamais déplacé sur le disque** : une règle décide quelle zone *montre* un item, point. Roll-up, quick hide et Peek sont faits (T-009) : enrouler une zone ne change que ce qu'elle **montre** (le rect stocké reste le corps déplié, donc dérouler restaure la zone exactement), quick hide masque et rappelle toutes les zones, et Peek les fait passer au-dessus de la couche d'icônes — le tout mesuré par `--nav-demo` (22/22 PASS headless, 29/29 en `--host`). Les **folder portals** sont faits (T-011) : une zone peut refléter un dossier arbitraire, le document ne stockant que le chemin du dossier — le miroir suit le dossier (ajouts **et** suppressions) sans jamais réécrire le document, et **sans jamais déplacer un fichier** (`--portal-demo`, 29/29 PASS headless, 31/31 en `--host`). Voir la **Phase 5** ci-dessus (T-014a / T-014b / T-013 à suivre).

**Phase 3 — Zones complètes. TERMINÉE.** Le rendu des zones hébergées est réglé (T-020 : une surface Win32 brute blitte une image rendue par Qt hors écran), les icônes du bureau réel s'affichent dans une zone avec sélection, double-clic, menu contextuel Shell et glisser-déposer (T-005/T-006), les zones se créent, se déplacent, se redimensionnent, s'aimantent et se suppriment (T-007), et la disposition survit à un redémarrage (T-008 : document JSON versionné dans `%APPDATA%/StarFish/layout.json`, écriture atomique différée, restauration vérifiée par la sonde `--persist-demo` — 26/26 PASS).

Build et tests **verts en local** (Qt 6.8.3 + MinGW 13.1.0, voir plus bas) : **12 suites de tests unitaires** (`ctest` 12/12).

⚠️ **Les validations à l'écran restent à faire** (T-021 et suivantes) : elles exigent un bureau libre et un œil humain, et ne peuvent pas être conduites depuis un agent — voir `STATE.md`, « Décisions actées » 13. Trois écarts à Fences 6 sont par ailleurs **assumés et documentés** : quick hide n'a pas encore son geste « double-clic sur le bureau vide » (T-028), Peek surélève les zones dans le bureau, pas au-dessus des fenêtres d'application (T-029), et les règles ne déplacent jamais un fichier sur le disque (T-031).

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

La CI GitHub Actions (`.github/workflows/ci.yml`) construit et teste automatiquement sur `windows-latest` (MSVC) : elle est **validée depuis le 2026-10-15** (build MSVC + `ctest` **10/10**), et vérifie en plus que le **manifeste applicatif** est bien embarqué dans le binaire — c'est une ressource porteuse (ADR-4), son absence étant silencieuse. Le build local est validé depuis le 2026-10-13 avec **Qt 6.8.3 `win64_mingw` + MinGW 13.1.0**, installés sans droits administrateur via `aqtinstall` (la machine de dev n'a pas MSVC) :

```bash
python -m aqt install-qt windows desktop 6.8.3 win64_mingw -O C:/Qt
python -m aqt install-tool windows desktop tools_mingw1310 qt.tools.win64_mingw1310 -O C:/Qt
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/mingw_64
```

Détail et portabilité : `docs/technique.md` §6.

Le build déploie le runtime Qt à côté du binaire (`POST_BUILD` → `windeployqt`) : `build\starfish.exe` est **autonome**, lançable depuis n'importe quel shell ou par double-clic, sans Qt ni MinGW dans le `PATH`.

### Lancer

**Sans argument, StarFish démarre l'application elle-même** (zones sur le bureau, icône de notification, fenêtre de réglages). `--seconds N` la referme au bout de N secondes, ce qui est la façon la plus simple de l'essayer ; `--no-tray`, `--no-settings`, `--no-watch`, `--no-host` retirent une pièce à la fois. Un `--product-probe` exécute la même séquence **sans bureau ni tray**, donc dans n'importe quelle session. En CI, les points d'entrée non interactifs restent `ctest` et les sondes `--xxx-demo`.

```bash
starfish                    # l'application : zones sur le bureau, icône de notification, réglages
starfish --seconds 30       # ...et se referme au bout de 30 s
starfish --product --no-watch     # l'application, sans surveiller le dossier Bureau
starfish --product-probe    # la séquence produit sans bureau ni tray, rapport PASS/FAIL, puis quitte
starfish --dump                 # hiérarchie Progman / SHELLDLL_DefView, puis quitte
starfish --list-desktop         # contenu du Bureau tel que le Shell le voit, puis quitte
starfish --list-desktop --icons --icon-size 48 --icon-dump out/icons
starfish --host-demo            # affiche une zone vide sur le bureau pendant 10 s
starfish --host-demo --seconds 30 --hide-icons
starfish --fence-demo           # cycle de vie des zones, rapport PASS/FAIL, puis quitte
starfish --fence-demo --host --seconds 10
starfish --persist-demo         # aller-retour de persistance + redémarrage simulé, puis quitte
starfish --persist-demo --host --seconds 10   # les zones restaurées, hébergées pour de vrai
starfish --persist-demo --layout C:/tmp/layout.json
starfish --nav-demo             # roll-up / quick hide / Peek, rapport PASS/FAIL, puis quitte
starfish --nav-demo --host --seconds 10       # les mêmes, avec les fenêtres réellement hébergées
starfish --rules-demo           # règles d'auto-organisation + dossier surveillé, rapport PASS/FAIL, puis quitte
starfish --rules-demo --host --seconds 15     # idem, et l'on voit une zone se remplir toute seule
starfish --rules-demo --watch C:/tmp/depot   # surveille un dossier au choix (les fichiers créés sont supprimés ensuite)
starfish --portal-demo          # portail de dossier (F-024) : la zone montre le contenu d'un dossier, rapport PASS/FAIL, puis quitte
starfish --portal-demo --host --seconds 15    # idem, et l'on voit un vrai dossier s'afficher dans une zone
starfish --portal-demo --portal-folder C:/tmp/depot   # reflète un dossier au choix (la sonde ne supprime que ses propres fichiers)
starfish --snapshot-demo        # snapshots (F-027) + export/import (F-028), rapport PASS/FAIL, puis quitte
starfish --snapshot-demo --layout C:/tmp/layout.json --snapshot-id avant --snapshot-note "avant la modif"
starfish --crash-demo           # T-046 : masque les icônes natives, TUE le processus qui les a masquées, et vérifie que le lancement suivant les rend
starfish --backdrop-demo        # T-039 : les icônes hors zone (calculées, jamais stockées), la disposition en colonnes, la frame transparente
starfish --backdrop-demo --host # idem, et il héberge réellement la surface de fond (plein écran, transparente, traversée par les clics)
```

`--rules-demo` et `--portal-demo` créent leurs fichiers de démonstration sous un nom dérivé du PID du processus et les suppriment en fin de sonde : elles ne peuvent donc pas écraser un fichier existant, et rien d'autre que ce qu'elles ont créé n'est jamais touché — y compris si `--watch` ou `--portal-folder` pointe sur un vrai dossier. Un portail **ne déplace jamais** un fichier : il décide quelle zone *montre* le contenu d'un dossier.

Le binaire est en sous-système GUI : quand la sortie est redirigée (`> log.txt`), elle est respectée ; sinon la console du parent est récupérée.

Les sondes écrivent leur rapport PASS/FAIL dans `build/` **relativement au répertoire courant** : lance-les **depuis la racine du dépôt**, sinon le rapport atterrit dans `build/build/` et l'ancien reste en place.

`--host-demo` journalise le verdict de placement (`below icon layer` attendu) — mais **le z-order seul ne prouve rien** (ADR-4) : confirmer visuellement que la zone est au-dessus du papier peint et sous les icônes, **avec le bureau réellement visible** (une fenêtre maximisée au premier plan donne un faux négatif).

### Expérimentations

`experiments/spike_workerw.py` valide le placement des fenêtres sur le bureau (ADR-4). Nécessite le **Python système** (Tkinter absent du Python managé) :

```bash
python experiments/spike_workerw.py
```
