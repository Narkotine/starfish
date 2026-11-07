# Installation et mises à jour

## Installateur (fait)

- Inno Setup 6, **par utilisateur, sans admin** : `%LOCALAPPDATA%\Programs\StarFish`.
- Construction : `pwsh tools/build_installer.ps1` (ou `cmake --build build --target installer`).
  Le script prépare `build\installer\stage` (exe + `starfish_*.qm` + `windeployqt`), lit la
  version dans `project(StarFish VERSION …)` et écrit `StarFish-<ver>-setup.exe` + `.sha256`.
- Avant installation/désinstallation : `starfish.exe --quit` (l'instance rend les icônes du
  bureau). Relance après installation (case à cocher ; en silencieux sauf `/NORELAUNCH`).
- Une mise à niveau ne remplace que les fichiers programme. Données jamais touchées :
  `%APPDATA%\StarFish`, `%LOCALAPPDATA%\StarFish`, `HKCU\Software\StarFish`.
- Désinstallation : garde les données par défaut ; question explicite (défaut « Non ») ou
  `/REMOVEDATA` en silencieux. La valeur `Run\StarFish` n'est supprimée que si elle pointe
  dans le dossier installé. Au lancement, StarFish repointe `Run` vers son propre exe s'il
  était inscrit : le démarrage avec Windows suit l'installation.

## Publier une version

1. Changer `project(StarFish VERSION x.y.z)` dans `CMakeLists.txt`, commit, push.
2. `git tag vx.y.z` puis `git push origin vx.y.z`.
3. Le workflow `release.yml` vérifie que le tag = la version CMake, compile (MSVC), teste, construit
   le setup (runtime VC++ copié à côté de l'exe) et publie la Release avec le setup et son `.sha256`
   (pré-version si 0.x ou suffixe). Texte : `.github/release/notes.md`.
4. Échec : corriger, supprimer le tag (`git push origin :refs/tags/vx.y.z`, `git tag -d vx.y.z`), retaguer.

## Données et compatibilité (fait)

- `layout.json` porte `schema` (1) et `writtenBy` (version de l'app). Écriture atomique (QSaveFile).
- Sauvegardes `backups/layout.v<schéma>.<horodatage>.<raison>.bak.json`, 10 dernières gardées :
  avant migration, au changement de version, sur fichier corrompu.
- Schéma plus ancien : migration en mémoire par étapes (une étape manquante = refus).
- Schéma plus récent : lecture au mieux, **lecture seule**, jamais réécrit.
- Champs inconnus conservés à la réécriture (racine, enclos, style, tuiles, widgets, règles).
- Fichier illisible : reprise depuis la dernière sauvegarde valide ; sinon lecture seule.

## Mise à jour automatique (conception)

1. **Flux** : JSON `{"schema":1,"latest":{"version","url","sha256","notes"}}`, HTTPS uniquement.
2. **Intégrité** : SHA-256 du setup vérifié avant exécution ; idéalement aussi une signature
   (Authenticode, ou signature détachée minisign avec clé publique embarquée).
3. **Application** : télécharger dans `%LOCALAPPDATA%\StarFish\updates`, vérifier, lancer
   `setup.exe /VERYSILENT /SUPPRESSMSGBOXES` (le setup fait `--quit` puis relance).
4. **Retour arrière** : les données ne bougent pas (schéma rétrocompatible + sauvegardes) ;
   réinstaller le setup précédent suffit. Garder le dernier setup téléchargé.
5. **Opt-in, sans télémétrie** : désactivé par défaut ; une simple requête GET du flux.

Implémenté (indépendant des décisions) : `core/update_feed` — lecture du flux, comparaison de
versions, vérification SHA-256, réglages `HKCU\Software\StarFish\Updates` (`Enabled`=0 par
défaut, `FeedUrl` vide). Aucun réseau, aucun jeton, aucune entrée de menu.

## Décisions pour l'utilisateur

- **Canal** : le dépôt est privé, donc pas de jeton embarqué. Recommandé : dépôt public
  séparé « releases » (ou GitHub Pages) hébergeant setup + flux.
- **Signature de code** (SmartScreen) : sans certificat, avertissement au premier lancement.
  Recommandé : Azure Trusted Signing (peu coûteux) le jour d'une diffusion publique.
- **Portée** : par utilisateur (recommandé, déjà en place) plutôt que machine.
