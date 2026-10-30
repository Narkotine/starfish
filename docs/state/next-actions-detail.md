# Prochaines actions — détail et ordre

> Dernière révision : **session 58 (2026-10-30)**, après l'**ADR-21** (StarFish dessine tout le bureau). Lu quand on attaque une action de `STATE.md`.
> **Règle d'écriture** : nommer la chose avant l'identifiant (« les coins des zones (T-058) », jamais « T-058 » seul).
> L'ancienne version (ordre de la session 56, essai ADR-20, spike MSIX) est archivée dans `recaps.md`.

## 1. Le bureau dessiné, dans l'ordre (ADR-21)

1. ✅ **Lot 1 (T-089)** — dessin piloté par `IFolderView2`, vrais menus, rangement sans toucher au fichier, filet T-046.
2. **Lot 2 (T-090)** — synchronisation complète : redémarrage d'Explorer (ex-T-018), plusieurs écrans et DPI, place des éléments neufs sans position.
3. **Lot 3 (T-091)** — gestes : F2, clavier, rectangle de sélection, glisser OLE (`DoDragDrop`/`IDropTarget`), `IFileOperation` (copier, coller, corbeille, annuler), déplacer les icônes (`SelectAndPositionItems`).
4. **Lot 4 (T-092)** — menu clic droit proche de Windows 11, **modifiable**, rempli des vraies commandes, « Afficher plus d'options » = menu classique ; Windows 10 : menu classique natif.

## 2. Ensuite (retours de la session 54, inchangés)

Deux crans d'arrondi de plus (T-084) · retirer le Peek (T-053) · le bord opposé qui bouge au redimensionnement — instrumenter d'abord (T-086) · l'espace par icône en réglage (T-085) · fantôme pendant le glisser et aperçu de la zone pendant le tracé (T-087).

## 3. Tranché avec lui

- L'auto-organisation n'est pas demandée : ni écran des règles (T-032), ni règles qui déplacent des fichiers (T-031).
- Déposer un fichier sur le bureau vide : accepté tel quel (T-052).
- Ses trois zones empilées à `(80, 1200)` : on n'y touche pas.

## 4. Questions ouvertes (elles attendent son mot)

- **Portails, moitié écriture (T-036)** : déposer dans un portail voudrait dire déplacer un fichier sur le disque. À faire ?
- **Sous-dossiers (T-033)** : seul le premier niveau d'un dossier est surveillé. À faire ?
- **Raccourcis (T-030)** : en dur, `Win+Space` refusé par Windows. Configurables ?

## 5. Validations humaines (`docs/plan.md`)

- **Avec et sans Fences 6** (T-019) : `--host-demo` dans les deux cas.
- **Une zone qui montre les vraies icônes** (T-021) : clic, Ctrl+clic, double-clic.
- **Deux écrans** : bloqué par le matériel (un seul écran, 2560×1440).
- **Photo avant / après** une restauration de disposition.