# Flux de travail : build quotidien, build de test, versions

> Pour tout agent (Cursor, Claude Code, etc.) ou humain. Complète [`reprise.md`](reprise.md).

## Deux dossiers de build, deux rôles

| Dossier | Rôle | Qui le lance |
|---|---|---|
| `build\` (Release) | **StarFish quotidien de l'utilisateur** : `build\starfish.exe` | l'utilisateur (raccourci « StarFish (local) ») |
| `build\dev\` (Release) | build de **test** des agents : ctest, sondes | les agents |

- On ne teste **jamais** avec `build\starfish.exe` : tests et sondes passent par `build\dev`.
- Ne jamais compiler dans `build\` pendant que le StarFish de l'utilisateur tourne :
  `Start-Process build\starfish.exe -ArgumentList '--quit' -Wait -WindowStyle Hidden`, puis
  `cmake --build build --target starfish`.
- **Fin de chaque lot** : recompiler `build\` puis relancer
  `Start-Process -FilePath "$PWD\build\starfish.exe" -WorkingDirectory "$PWD\build"`, et vérifier
  que StarFish tient le bureau (icônes natives masquées) et que les zones sont toutes là.

## Une seule instance, des données partagées

- Une seule instance à la fois : la copie installée (`%LOCALAPPDATA%\Programs\StarFish`) **ou** la
  copie locale. `--quit` (de n'importe quelle copie) arrête celle qui tourne.
- Toutes les copies partagent les mêmes données : `%APPDATA%\StarFish` (layout.json, `backups\`),
  `%LOCALAPPDATA%\StarFish` (journaux) et `HKCU\Software\StarFish`. Ne jamais les effacer.
- Le démarrage avec Windows (`HKCU\...\Run\StarFish`) est un réglage de l'utilisateur : ne pas y toucher.

## Tester sans déranger

- Fichiers factices uniquement, nettoyés ensuite ; jamais déplacer les fichiers réels, pas d'entrées
  souris/clavier injectées, pas de redémarrage d'Explorer ; captures par PrintWindow des zones ou hors écran.
- **`--product-probe`** rend les icônes natives en s'arrêtant, même si une autre instance tient le
  bureau : relancer ensuite le StarFish de l'utilisateur (`--quit` puis relance).
- Espacer les sondes : l'utilisateur travaille sur la machine en même temps.

## Git

- D'abord `git status` : du travail non commité d'un autre agent ne s'écrase jamais (ne committer que ses fichiers).
- Avant commit : `ctest --test-dir build/dev`, `python tools\i18n_check.py`, `python tools\docs_size.py`
  (`STATE.md` ≤ 6000 octets) ; mettre à jour `TODO.md`, `STATE.md`, `devlog/`.
- Commits **locaux** ; `push` et tags seulement sur demande explicite. Jamais de push forcé.
- L'historique public a été réécrit : ne jamais pousser depuis un ancien clone (`StarFish-old`,
  `StarFish-clean`...) ; en cas de doute, re-cloner.

## Version officielle (sur demande seulement)

1. Monter `project(StarFish VERSION x.y.z)` dans `CMakeLists.txt`, commit, push.
2. `git tag vx.y.z` puis `git push origin vx.y.z` : `release.yml` construit le setup et publie la Release
   (détails : [`mises-a-jour.md`](mises-a-jour.md)).
3. L'utilisateur n'est pas obligé de l'installer : son quotidien reste la copie locale.
