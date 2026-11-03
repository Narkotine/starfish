# Reprise - guide pour un nouvel agent

> Une page pour reprendre StarFish (Cursor, Claude Code, etc.). Le détail est ailleurs : ce fichier dit **où** et **quoi ne pas casser**.

## 1. Lire, dans cet ordre

1. `AGENTS.md` (conventions, protocoles d'entrée / sortie).
2. `STATE.md` (où on en est ; cap **6000 octets**).
3. `TODO.md` (tâches ouvertes ; les terminées sont dans `docs/todo-archive.md`).
4. Le dernier `devlog/AAAA-MM-JJ.md` (récit des dernières sessions).
5. À la demande : `docs/state/` (décisions, garde-fous, inventaire), `docs/technique.md` (ADR).

## 2. Construire et tester (PowerShell, à la racine du dépôt)

```powershell
$env:PATH="C:\Users\<user>\Qt\Tools\mingw1310_64\bin;C:\Users\<user>\Qt\6.8.3\mingw_64\bin;$env:PATH"
Start-Process build\dev\starfish.exe -ArgumentList '--quit' -Wait -WindowStyle Hidden; Start-Sleep 2  # libère build\starfish.exe
cmake --build build/dev      # build de travail (Release, Ninja)
cmake --build build          # build que l'utilisateur lance (Release, Ninja)
ctest --test-dir build/dev ; ctest --test-dir build          # 19/19 chacun
```

- **Sonde UI** (hors écran, dev) → `build\ui-probe.txt`, compter `^PASS` / `^FAIL` (sensible à la casse) :
  `$env:STARFISH_UI_PROBE_OFFSCREEN='1'; Start-Process "$PWD\build\dev\starfish.exe" -ArgumentList '--ui-probe' -Wait -WindowStyle Hidden`
- **Sonde produit** (release) → `build\product_probe.txt` (37/37) : `build\starfish.exe --product-probe` (lancé caché, stderr redirigé, attendre la fin). Elle compte les interrupteurs des Réglages (18) et la liste des combos autorisés : tout nouvel interrupteur / combo s'y ajoute (`src/main.cpp`).
- Autres sondes : `--drawn-desktop-probe`, `--context-menu-probe`, `--render-probe`. **Ne pas lancer** `--icons-demo`, `--fence-demo --host`, `--snapshot-demo`.
- Docs : `python tools\docs_size.py` (caps STATE 6000, TODO 50000, README 45000). i18n : `cmake --build build/dev --target starfish_lupdate`, remplir `src/i18n/starfish_fr.ts`, puis `python tools\i18n_check.py`.
- Captures de sonde : `build\captures\` (rendu hors écran ou PrintWindow des zones, **jamais** une capture de l'écran).
- Espacer les lancements de sondes ; ne lancer que celles qui concernent le changement.

## 3. Quitter et relancer

- Quitter proprement : `starfish.exe --quit` (n'importe quel exe ; l'instance en cours restaure les icônes du bureau).
- Relancer **le build que l'utilisateur utilisait** (en général la release) :
  `Start-Process -FilePath "$PWD\build\starfish.exe" -WorkingDirectory "$PWD\build"`
- Vérifier : `listVisible=False marker=True running=True` (liste d'icônes native masquée, marqueur présent, processus vivant).
- Raccourci d'urgence côté utilisateur : tray → « Restaurer mes icônes ».

## 4. Où sont les données

| Quoi | Où |
|---|---|
| Zones, contenu, styles (`layout.json`, sauvegardes `layout.sNN-backup.json`) | `%APPDATA%\StarFish\` |
| Copies de raccourcis des zones (coffre) | `%APPDATA%\StarFish\shortcuts\` |
| Instantanés, marqueur `icons-hidden.marker` | `%APPDATA%\StarFish\` |
| Menu clic droit (`context-menu.json`, `context-menu-keys.txt`) | `%LOCALAPPDATA%\StarFish\` |
| Journaux : `startup.log` (chronologie), `crash.log` ; cache splash | `%LOCALAPPDATA%\StarFish\` |
| Réglages globaux | registre `HKCU\Software\StarFish` |
| Démarrage avec Windows | valeur `StarFish` de `HKCU\...\CurrentVersion\Run` (**ne pas y toucher** sans demande) |

## 5. Règles non négociables

- **Jamais de push** sans accord explicite de l'utilisateur ; tout push déclenche la CI (`.github/workflows/ci.yml`). Commits **locaux**, Conventional Commits en anglais ; docs en français, courtes.
- **Ne pas redémarrer l'Explorateur.** Pas d'entrée simulée (pas de `SendInput`, souris ou clavier injectés).
- **Ne pas toucher aux fichiers ni aux fenêtres de l'utilisateur** : tests avec des fichiers factices dans un dossier temporaire, nettoyés ensuite. Ne pas ajouter de widget / tuile à ses vraies zones sans le dire.
- Ne jamais déplacer un fichier sur le disque (StarFish gère l'affichage, pas l'emplacement).
- Masquer `SysListView32`, **jamais** `SHELLDLL_DefView`. Pas de message porteur de pointeur vers un autre processus. Ne pas déclencher le vrai `SHDoDragDrop` depuis une sonde.
- Pas de nouvelle entrée au menu du tray.
- Fin de session : TODO + devlog + STATE (≤ 6000 octets) + commit.

## 6. Architecture en bref

- **StarFish dessine tout le bureau** (ADR-21) : fond d'écran + icônes libres dans une surface hébergée sous Progman/WorkerW, pilotée par la vraie vue du shell (`desktop_shell_view`, `IFolderView2`) : menus, verbes, positions et grille d'Explorer restent la vérité. La liste native est masquée.
- **Zones** : `FenceView` (QWidget jamais affiché) rendue en **image** dans une `DesktopSurface` (fenêtre enfant du bureau, alpha uniforme ; `UpdateLayeredWindow` refusé sur un enfant d'un autre processus). Zone translucide = panneau + **overlay** des tuiles opaques (T-082), le panneau posé **sous** l'overlay. Tout changement de contenu doit émettre `contentChanged` pour re-rendre l'image.
- **Glisser** : OLE (`createDesktopDropTarget`, `ShellDropTarget`) ; insertion par espace (`IconLayout::insertionIndex`, barre s80) ; grille du bureau (`OverlapPlanner::gridSlots`).
- **Widgets** : un `DesktopItem` avec `widgetType`, `span`, `widgetSettings`, chemin `widget:<uuid>` (jamais un fichier) ; placés en premier ajustement dans l'ordre du flux (`computeSpanned`) ; JSON `kind:"widget"` ; moteurs déclarés via `widgets::registerEngine`.
- **Lancement** : `openShellItem(item.path)` = `ShellExecuteEx` sur le **.lnk lui-même** (`SEE_MASK_INVOKEIDLIST`), jamais sur la cible résolue (arguments perdus).
- **Identité des fichiers** : `fileId` = n° de volume + index NTFS ; un renommage hors StarFish garde la tuile.
- Code : `src/app` (ProductApp, sondes), `src/ui` (zones, grille, réglages, backdrop), `src/platform/win` (Win32/COM), `src/core` (modèle, JSON, règles, planificateur), `tests/`.

## 7. Pièges déjà payés

- Double-clic qui perdait les arguments d'un raccourci → lancer le .lnk (s76).
- Chemins `C:/` (Qt) vs `C:\` (shell) : passer par `shellNameOf()` avant toute action shell (s61).
- `slots` est une macro Qt : ne pas l'utiliser comme nom de variable.
- Un widget `WA_DontShowOnScreen` montré reçoit de vraies peintures : `setUpdatesEnabled(false)` dans les sondes (s77).
- Lier un test qui touche `IconGrid` : ajouter `fence_widgets.cpp` + `i18n.cpp` dans `tests/CMakeLists.txt` (s79).
- Rayon demandé ≠ rayon dessiné : la région reçoit le rayon borné (T-080).
- `build\starfish.exe` verrouillé à l'édition de liens = il tourne : `--quit` d'abord.
- Splash sauté au relancement quand une fenêtre agrandie est au premier plan : voulu (s78).
- STATE dépasse vite 6000 octets : condenser, ne pas monter le cap.
- PowerShell : pas de `\"` dans `python -c "..."` ; écrire les scripts dans un fichier.
- Machine parfois « injoignable » : attendre et réessayer.

## 8. À vérifier à la vraie souris (non couvert par les sondes)

- s80 : barre d'insertion (même zone, autre zone, depuis le bureau / l'Explorateur), grille du bureau et case cible pendant un glisser, interrupteur.
- s79 : horloge (glisser, menu, tailles) ; s76 : lancement des raccourcis avec arguments, glisser hors zone au point de lâcher.
- ADR-21 : deux écrans / DPI, vrai redémarrage d'Explorer (par l'utilisateur), focus clavier, menu dessiné au clavier.
