# experiments/

Scripts de validation technique (*spikes*) — pas du code produit.

## `spike_workerw.py`

Valide l'ADR-4 de `docs/technique.md` : fenêtre sans bord positionnée sur le
`WorkerW` entre le papier peint et les icônes du bureau.

```bash
# Python système requis (Tkinter absent du Python managé)
python experiments/spike_workerw.py
```

Affiche une fenêtre de test étiquetée pendant 15 secondes, puis la ferme.
Ne touche ni au registre ni aux icônes du bureau (option `--test-hide-icons`
pour afficher la procédure de masquage réversible — jamais exécutée
automatiquement).

Sortie attendue (à coller dans le devlog) : inventaire `Progman` / `WorkerW` /
`SHELLDLL_DefView` avec rôles, puis le handle cible.
