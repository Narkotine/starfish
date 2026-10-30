# StarFish

Équivalent open source de **Stardock Fences 6** pour Windows : organiser le bureau en zones (« fences ») dans lesquelles déposer, trier et déplacer ses icônes.

> Même fonctionnalités, implémentation et identité différentes. Objectif : parité fonctionnelle progressive, pas un clone.

## État du projet

**StarFish dessine le bureau (ADR-21, session 58).** Lancé sans argument, `starfish.exe` charge la disposition, pose les zones, et **dessine tout le bureau** à la place de la liste d'icônes native, qu'il masque (sans jamais toucher à la DefView, ADR-12) : fond d'écran, icônes aux positions que le shell garde, sélection. Ce dessin est **piloté par la vue du bureau elle-même** (`IFolderView2`, voie documentée, proxies COM système) : éléments (Bureau public et « Ce PC » compris), positions, **vrais menus** de clic droit (fond et élément, sous-menus compris) et verbes (double-clic = verbe par défaut). Une icône **rangée dans une zone quitte le bureau** sans que son fichier bouge. Si StarFish s'arrête — proprement, en plantant, ou si Explorer redémarre — **toutes les icônes reviennent** (marqueur T-046, réparé au lancement suivant ; tray « Restaurer mes icônes »). `--native-desktop` garde le bureau natif (l'icône rangée s'y affiche alors deux fois).

Suite : `TODO.md` (T-090 synchronisation complète, T-091 gestes délégués au shell, T-092 menu Windows 11 configurable). Où on en est : `STATE.md`. L'ancien récit de cette section (sessions ≤ 57, dont des pistes que l'ADR-21 renverse) est archivé dans [`docs/state/recaps.md`](docs/state/recaps.md).

## Carte des documents

| Fichier | Rôle |
|---|---|
| [`AGENTS.md`](AGENTS.md) / [`AGENT.md`](AGENT.md) | **Point d'entrée pour tout agent IA** (conventions harness-agnostiques). Lire AVANT `STATE.md`. |
| [`STATE.md`](STATE.md) | **État courant, court par construction (cap 6 Ko)** : phase, 3 prochaines actions, garde-fous transverses, index du détail. Le récit d'une session ne vit pas ici mais dans `devlog/`. |
| [`TODO.md`](TODO.md) | **Ce qui reste** : tâches ouvertes (une ligne de tableau + son détail). |
| [`docs/state/`](docs/state/) | **Le détail de l'état, lu à la demande** : décisions actées **par domaine** (`decisions.md` + `decisions/*.md`), garde-fous par domaine (`guardrails.md` + `guardrails/*.md`), inventaire de ce qui existe (`inventory.md`), écarts assumés (`gaps.md`), ordre et scripts d'essai des prochaines actions (`next-actions-detail.md`), récits de session archivés (`recaps.md`). |
| [`docs/todo-archive.md`](docs/todo-archive.md) | Tâches terminées : table + détail verbatim (ce que `TODO.md` portait avant de grossir). |
| [`devlog/`](devlog/) | Journal de développement, une entrée datée par session de travail. |
| [`docs/produit.md`](docs/produit.md) | Documentation produit : vision, personas, cas d'usage, inventaire fonctionnel complet (référence Fences 6), périmètre par version, non-objectifs. |
| [`docs/plan.md`](docs/plan.md) | Plan de mise en œuvre par phases avec critères de sortie. |
| [`docs/technique.md`](docs/technique.md) | Architecture, décisions techniques (ADR), modèle de données, intégration Windows, build/packaging. |

## Reprendre le travail (agent ou humain)

1. Lire `STATE.md` (court : où on en est, quoi faire ensuite).
2. Lire la phase courante dans `docs/plan.md`.
3. Ouvrir **à la demande** ce qui sert au travail choisi : `docs/state/decisions/<domaine>.md` **et** `docs/state/guardrails/<domaine>.md` (mêmes domaines), `docs/state/inventory.md` pour « est-ce que X existe déjà ? », `docs/state/next-actions-detail.md` pour l'ordre et les scripts d'essai. Rien de tout cela n'est à lire en entier.
4. Cocher les tâches faites dans `TODO.md`.
5. Travailler.
6. **Avant de terminer :** mettre à jour `TODO.md` (une tâche ✅ ne garde qu'une ligne ; son paragraphe part dans `docs/todo-archive.md`), ajouter une entrée dans `devlog/AAAA-MM-JJ.md`, mettre à jour `STATE.md` — **sans y recopier le récit de la session**. `python tools/docs_size.py` refuse un `STATE.md` > 6 Ko.

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
starfish --backdrop-demo        # T-039/T-041/T-055/T-060/T-062 : les icônes hors zone (calculées, jamais stockées) et **laissées au bureau par défaut** (ADR-13 : 0 pixel peint), la disposition — aux positions lues de la coquille —, la frame transparente **sans trou d'alpha**, les gestes du bureau vide (glisser droit = menu du bureau + créer une zone, double-clic gauche = quick hide), et le menu du bureau lui-même
starfish --backdrop-demo --host # idem, plus : la surface de fond réellement hébergée (plein écran, transparente, qui prend la souris pour ses gestes et n'avale aucun dépôt), **le test de toucher réel demandé à Windows** (`WM_NCHITTEST` : `HTTRANSPARENT` sur une vraie icône de l'utilisateur, `HTCLIENT` sur le bureau nu) et **la règle du test de toucher mesurée dans les deux sens** (cadre à alpha 0 : la souris traverse ; au plancher : le curseur trouve la fenêtre), plus la **lecture directe** : sur une grille de 1 523 points du bureau vide, le fond lui-même répond **1 398 fois** (le total mesuré après ADR-12)
starfish --icons-demo          # T-066 : monte le cas qui cassait le bureau (une zone qui détient une VRAIE icône) et
                                # mesure de l'extérieur que tes icônes restent les tiennes (rapport PASS/FAIL, puis quitte)
starfish --install-desktop-menu    # T-071 : ajoute « StarFish: create a fence » au menu du bureau de WINDOWS (par utilisateur)
starfish --uninstall-desktop-menu  # ...et le retire
starfish --new-fence               # T-071 : ce que lance l'entrée du menu -- demande la zone à l'instance qui tourne, ou la démarre
starfish --claim-desktop        # T-056, chemin de développement seulement : masque les icônes natives et dessine le bureau
                                # lui-même. ⚠️ Depuis T-066 le produit ne prend JAMAIS la liste tout seul (ADR-13)
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

`experiments/spike_mouse_hook.cpp` (T-070) étudie le **hook souris bas niveau** (`WH_MOUSE_LL`) comme porteur de F-001 — créer une zone en traçant au bouton droit sur le bureau nu — **sans aucune fenêtre à nous sur le bureau**. Contexte : les deux défauts des sessions 37-38 venaient de la fenêtre de fond (elle prend la souris ; `HTTRANSPARENT` ne traverse pas les processus, d'où des trous d'alpha en rustine). Le spike s'installe **sans élévation** et écrit son rapport dans `build/spike_mouse_hook.txt`. ⚠️ **Le geste doit être fait à la main** : un hook bas niveau ne voit pas les événements que le processus installeur injecte lui-même (`SendInput` du même processus → 0 événement), et un processus tiers lancé depuis le terminal de l'agent ne voyait pas la fenêtre du spike. Il mesure la classification (bureau nu / icône / nos zones), le **coût du rappel en µs** (un hook lent rend la souris pâteuse pour toute la machine et Windows l'évince en silence) et l'éviction :

```bash
$QT/Tools/mingw1310_64/bin/g++ -std=c++17 -O1 experiments/spike_mouse_hook.cpp \
    -o spike_mouse_hook.exe -luser32 -lgdi32
./spike_mouse_hook.exe --seconds 30      # puis : un glisser au bouton droit, un clic droit
```

`experiments/spike_layer_alpha_shape.cpp` mesure **le test de toucher d'une fenêtre layered par pixel** (T-060 / ADR-11) : un cadre à alpha 0 laisse passer la souris, un cadre à alpha 1 non — mesuré sur une fenêtre topmost **et** sur un enfant layered, les deux sens à chaque fois. Hors Qt, compilable avec le MinGW portable :

```bash
$QT/Tools/mingw1310_64/bin/g++ -std=c++17 -O1 experiments/spike_layer_alpha_shape.cpp \
    -o spike_layer_alpha_shape.exe -lgdi32 -luser32 -ldwmapi
```
