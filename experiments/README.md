# experiments/

Scripts de validation technique (*spikes*) — pas du code produit.

## `spike_workerw.py`

Valide l'ADR-4 de `docs/technique.md` : fenêtre sans bord placée sur le bureau,
**au-dessus du papier peint et sous les icônes natives**.

```bash
# Python système requis (les scripts utilisent ctypes ; pas de dépendance tierce)
python experiments/spike_workerw.py --seconds 6
python experiments/spike_workerw.py --capture-only experiments/out/desktop.png
```

Affiche une fenêtre de test orange étiquetée pendant N secondes, puis la ferme,
en enregistrant une capture d'écran pendant qu'elle est visible. Ne touche ni au
registre ni aux icônes du bureau.

**Séquence de référence (validée) :** `SetParent(Progman)` → style `WS_CHILD`
(sans `WS_POPUP`) → ex-style : retirer `WS_EX_TOPMOST`, **ajouter
`WS_EX_LAYERED`** + `SetLayeredWindowAttributes(alpha=255)` → `SetWindowPos`
sous `SHELLDLL_DefView`. **Le flag layered est obligatoire** : sans lui la
fenêtre est « visible » au sens Win32 mais DWM ne la compose pas (échec
silencieux — voir ADR-4).

Diagnostics optionnels : `--no-layered` (reproduit la panne), `--above`,
`--hide-icons`, `--no-parent`, `--child-direct`, `--parent-workerw`,
`--show-desktop`, `--dump`.

## `probe_desktop.py`

Diagnostic lecture seule de la hiérarchie du bureau : visibilité de
`Progman`/`DefView`, vrais enfants de `Progman` (`GW_CHILD`/`GW_HWNDNEXT`),
WorkerW plein écran, hit-tests.

## `probe_fences.py`

Diagnostic lecture seule de la façon dont une application tierce (Fences) occupe
le bureau : classes/styles/parents des fenêtres Fences, chaîne d'enfants de
`Progman` et de `DefView`. A servi à établir que Fences dessine ses zones **dans
le `SysListView32` natif** (pas de fenêtres dédiées).
