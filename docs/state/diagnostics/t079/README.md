# T-079 — diagnostics reconstitués de la session 44 (une zone neuve n'apparaît pas)

> **Ce ne sont pas des correctifs.** Ce sont les **diagnostics temporaires** qu'une session
> précédente avait ajoutés pour instruire le rapport utilisateur
> « *je ne vois plus les fences créées, ou nouvellement créée, même en matériel opaque* »
> (2026-10-17, 20:51:50), puis **retirés par elle-même** — annoncé d'avance, et vérifié
> (`git status` propre, build `rc=0`, `ctest` 14/14, `--fence-demo` 42/42).
> Elle ne les a **pas consignés** : ni mesure, ni conclusion, ni tâche. C'est le trou que ce dossier répare.

## Provenance et fiabilité

| | |
|---|---|
| Session source | `20261017_191505_dbfd3e` (transcription dans la base de sessions d'agents) |
| Fenêtre | diagnostics ajoutés de **21:00:02 à 21:08:46**, retirés à **21:09:26** (`git checkout --` sur les 4 fichiers) |
| Reconstruction | rejeu des appels d'édition de la transcription sur le contenu de `HEAD` — **12/12 ancrages retrouvés à l'identique** |
| Contrôles | `git apply --check` **passe** sur les 4 patchs ; comptes reproduits : **35 / 46 / 5 / 21+1** lignes (= ce que `git diff --stat` affichait avant le retrait) |

## Ce que ça instrumente

- `fence_container.cpp` (`renderFence()`) : taille, `dpr`, **alpha du centre**, alpha du coin, *poussé à la fenêtre*, per-pixel, visibilité, matériau de la frame, puis `paintReport()`.
- `desktop_surface.cpp` / `.hpp` : `DesktopSurface::screenSample()` — **l'alpha de l'écran au pixel du centre de la zone** (`::GetDC` / `::GetPixel`), et un `TMP tick screen` dans le watchdog.
- `desktop_host.cpp` (`dumpDesktopHierarchy()`) : **les hôtes** — `Progman`, boucle `WorkerW`, `SHELLDLL_DefView`, `SysListView32`, nombre d'items.

## Résultats obtenus alors (extraits de la transcription, session 44)

- **La frame est parfaite en mémoire** : centre alpha **255**, panneau `#ff1f2937`, `pushed-to-window 1`, mode per-pixel, fenêtre visible ; `corner alpha 1` = le **coin arrondi** (radius 10).
- **Les fenêtres vivent et sont bien ordonnées** : 5 `StarFishDesktopSurface` enfants de `Progman`, à leurs rects exacts, au-dessus du `DefView`, stables sur 4-5 relevés en 20 s.
- **Rien à l'écran** dans le contexte mesuré : **0/25** points du panneau par rect, les centres portant le **papier peint** (`#2a6511`, `#7cb801`, `#499003`, `#010300`, `#1c4700`), vu aussi bien de l'extérieur que du produit.
- **Mais** en lançant « comme l'utilisateur » (par le shell), l'échantillon renvoie `#0c0c0c` puis `#191919` = **les pixels de StarFish** ⇒ le contexte de lancement change le résultat.
- **Deux racines de bureau coexistaient** : `Progman 0x101bc` + `DefView 0x2016e` (celui que le produit choisit) et `Progman 0x65980` + `DefView 0x131438` (88 icônes) — mesuré, non expliqué.
- **Explorer recréé à 21:05:43**, ~14 min **après** le rapport ⇒ ne peut pas expliquer cette occurrence.

**Hypothèses restantes** : « premier push perdu » (le watchdog de 2 s le masquait avant T-050) et « bureau recréé sans réattachement ». Le test qui les départage : relancer, puis **déplacer/redimensionner** une zone — si elle réapparaît, c'est le premier push perdu.

## Comment s'en servir

```bash
# depuis la racine du dépôt -- vérifier d'abord, puis appliquer
git apply --check docs/state/diagnostics/t079/*.patch
git apply         docs/state/diagnostics/t079/*.patch
# ... instrumenter / lire les sorties ...
git checkout -- src/platform/win/desktop_host.cpp src/platform/win/desktop_surface.cpp \
                 src/platform/win/desktop_surface.hpp src/ui/fence_container.cpp
```

**Ce que ces mesures départagent** : (a) la zone n'est jamais créée (ou posée hors écran, `freeSpot`/T-067), (b) elle est peinte mais **invisible à l'œil** (alpha, taille nulle, frame périmée), (c) elle est **sous** autre chose (z-order, papier peint), (d) elle existe à l'écran mais **pas dans le document** (persistance, T-008).

⚠️ **Ne pas conclure depuis les sondes seules** : au moment du retrait, `--fence-demo` passait 42/42 et `ctest` 14/14 alors que le symptôme persistait. Mesurer **au vrai curseur**, comme pour T-071/T-069.
