# Flux de travail : un seul build, tests, versions

> Pour tout agent (Cursor, Claude Code, etc.) ou humain. Complète [`reprise.md`](reprise.md).

## Un seul build : `build\dev`

`build\dev\` (Release, Ninja) est **à la fois** le StarFish quotidien de l'utilisateur (lancé avec
Windows via `HKCU\...\Run\StarFish` et un raccourci du bureau) **et** le build de test des agents.
Ne pas créer d'autre dossier de build pour l'usage quotidien.

À chaque lot :

1. `Start-Process build\dev\starfish.exe -ArgumentList '--quit' -Wait -WindowStyle Hidden` (libère l'exe ;
   les icônes natives reviennent le temps du travail).
2. `cmake --build build/dev`, puis les tests (`ctest --test-dir build/dev`, sondes utiles).
3. **Toujours** relancer à la fin :
   `Start-Process -FilePath "$PWD\build\dev\starfish.exe" -WorkingDirectory "$PWD\build\dev"`, puis vérifier
   que StarFish tient le bureau (icônes natives masquées) et que les zones sont toutes là.

## Une seule instance, des données partagées

- Une seule instance à la fois ; `--quit` (de n'importe quelle copie) arrête celle qui tourne. Le setup
  d'une Release n'est pas installé sur le poste de dev.
- Toutes les copies partagent les mêmes données : `%APPDATA%\StarFish` (layout.json, `backups\`),
  `%LOCALAPPDATA%\StarFish` (journaux) et `HKCU\Software\StarFish`. Ne jamais les effacer.
- Le démarrage avec Windows (`HKCU\...\Run\StarFish`, qui pointe `build\dev\starfish.exe`) est un réglage
  de l'utilisateur : ne pas le modifier sans demande.

## Tester sans déranger

- Fichiers factices uniquement, nettoyés ensuite ; jamais déplacer les fichiers réels, pas d'entrées
  souris/clavier injectées, pas de redémarrage d'Explorer ; captures par PrintWindow des zones ou hors écran.
- **`--product-probe`** rend les icônes natives en s'arrêtant : relancer ensuite le StarFish de
  l'utilisateur (étape 3 ci-dessus).
- Espacer les sondes : l'utilisateur travaille sur la machine en même temps.

## Git

- D'abord `git status` : du travail non commité d'un autre agent ne s'écrase jamais (ne committer que ses fichiers).
- Avant commit : `ctest --test-dir build/dev`, `python tools\i18n_check.py`, `python tools\docs_size.py`
  (`STATE.md` ≤ 6000 octets) ; mettre à jour `TODO.md`, `STATE.md`, `devlog/`.
- Commits **locaux** ; `push` et tags seulement sur demande explicite. Jamais de push forcé.
- L'historique public a été réécrit : ne jamais pousser depuis un ancien clone ; en cas de doute, re-cloner.

## Version officielle (sur demande seulement)

1. Monter `project(StarFish VERSION x.y.z)` dans `CMakeLists.txt`, commit, push.
2. `git tag vx.y.z` puis `git push origin vx.y.z` : `release.yml` construit le setup et publie la Release
   (détails : [`mises-a-jour.md`](mises-a-jour.md)).
3. Le setup sert aux autres utilisateurs ; sur le poste de dev, le quotidien reste `build\dev`.
