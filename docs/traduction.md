# Traduction de StarFish

StarFish utilise le système de traduction de Qt (Qt Linguist) :

- Le code est écrit en **anglais** (langue source, toujours disponible). Chaque texte affiché passe par `tr()` (classes `Q_OBJECT`), `QCoreApplication::translate("Contexte", "…")` (code sans `QObject`) ou `QT_TR_NOOP` (tableaux statiques traduits à l'affichage).
- Chaque langue = un fichier source `src/i18n/starfish_<code>.ts` (code ISO 639-1 : `fr`, `de`, `es`…).
- CMake (`src/CMakeLists.txt`) compile **tous** les `starfish_*.ts` en `.qm` avec `lrelease`, posés à côté de `starfish.exe` (et des tests).
- Au démarrage, `core::i18n::install()` charge le `.qm` de la langue choisie (Réglages → Général → Langue, clé HKCU `language`, ou `--lang`) puis le `qtbase_<code>.qm` de Qt (boîtes de dialogue Qt). Langue « Automatique » = celle de Windows si elle est livrée, sinon l'anglais.
- La liste des langues (Réglages) est **construite à partir des `.qm` trouvés** (`core::i18n::availableLanguages()`), avec le nom de la langue dans sa propre langue.

## Ajouter une langue

1. Copier `src/i18n/starfish_fr.ts` en `src/i18n/starfish_<code>.ts` et changer l'attribut `language` de la balise `<TS>` (ex. `language="de_DE"`).
2. Mettre à jour les entrées depuis le code : `cmake --build build --target starfish_lupdate` (tous les `.ts`).
3. Traduire avec Qt Linguist (`linguist src/i18n/starfish_<code>.ts`) ou à la main (`<translation>`). Une entrée vide reste en anglais.
4. Reconstruire : `cmake --build build`. La langue apparaît dans Réglages → Langue (appliquée au prochain démarrage).

## Vérifier

- `python tools/i18n_check.py` : chaque texte du code a son entrée dans le catalogue français.
- `starfish --lang <code>` force une langue pour un essai.
