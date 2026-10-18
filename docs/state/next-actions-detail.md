# Prochaines actions — détail et ordre

> Lu quand on attaque une des actions de `STATE.md`. Dernière révision : **session 53 (2026-10-18)** — la liste des sessions
> 15 à 46 qu'il portait est périmée (ses tâches sont livrées, et la fenêtre de réglages a changé) ; le verbatim, lui, est au
> `devlog/`. Ce fichier ne dit plus que **ce qui reste**, dans l'ordre où le prendre.

> **Règle de lecture** : l'essai **humain** d'abord (il tranche, et il coûte une minute) ; le reste est du travail **ou** une
> décision (❓), pas les deux à la fois.

## 1. L'essai à l'œil (lui seul — 5 minutes, et il décide)

Lancer `build\starfish.exe` — **quitter d'abord l'instance en cours** : le produit est mono-instance, un second lancement
sort en silence. `--seconds 600` la referme toute seule. Sur un poste français, l'interface est en **français**.

1. **T-080 — les couleurs et les arrondis.** Clic droit **sur la barre de titre** d'une zone → `Couleurs ▸` (`Couleur du panneau...`, `Couleur de la barre de titre...`, et `Barre de titre : celle du panneau` **seulement** si une bande existe — jamais grisée) ; puis `Arrondi des coins ▸` (*Carré* / 6 / 10 (défaut) / 14 / 20). À dire : est-ce que ça plaît, et **10 px** est-il le bon défaut ?
2. **T-082 — le matériau n'efface que le panneau.** `Matériau ▸` → *Très translucide*. Attendu : le papier peint traverse le **panneau et sa barre de titre**, les **tuiles restent nettes**. Puis l'entrée `Matériau sur les icônes aussi` (option **par zone**, visible seulement sous un panneau non opaque) → tout s'efface : c'est l'ancien comportement, à la demande.
3. **T-058 — les coins, à l'œil.** Depuis ADR-15 le coin est découpé par une **région** (1 bit) : le bord **n'est pas anticrénelé**. Dire si ça se voit, et si un liseré ou une ombre manquent.
4. **T-078 — les noms longs.** Une zone dont un libellé est long élargit **toutes** ses cellules (jusqu'à 2× l'icône) ; une zone aux noms courts garde exactement la cellule d'avant. Dire si « ça respire ».
5. **T-050 / T-077 — le silence et le geste.** Laisser tourner **une minute sans rien toucher** : plus de clignotement (le watchdog ne fait plus rien au repos). Puis **redimensionner** une zone (le bord suit le curseur, sans à-coups) et **déplacer** par la barre (aucun redessin au passage).
6. **T-071 / T-069 — le bureau reste le sien.** Déplacer une **icône** (elle se repose où on la lâche) ; **glisser gauche** dans le vide (rectangle de sélection) ; **clic droit sur le bureau** (le menu de **Windows**, avec « StarFish: create a fence » dans « Afficher d'autres options » — limite de Windows 11, **T-073**) ; **clic droit sur une icône** (son menu) ; **cliquer une zone** (elle répond). Créer une zone par cette entrée → elle apparaît à la **première place libre**.
7. **T-066 — le cas qui cassait le bureau.** Ranger **une vraie icône** dans une zone, puis laisser tourner une minute : le bureau reste **le sien** (icônes visibles, cliquables, déplaçables), et l'icône rangée s'affiche **deux fois** — c'est le **prix annoncé** de T-042, pas un bug à signaler.
8. **T-067 — ses trois zones empilées.** Créer trois zones de suite doit les poser à **trois endroits différents** ; ses trois zones déjà à `(80, 1200)` y sont **toujours** (T-067 corrige la création, pas l'existant) — à décaler à la main, ou me le demander (**je ne touche pas à son document sans son accord**).

⚠️ `--claim-desktop` est un **chemin de développement** (StarFish masque les icônes et dessine le bureau lui-même) : ne pas juger le produit avec.

## 2. Ce qui attend une décision de lui (❓ — bloque du code)

- **T-073** — l'entrée de F-001 dans le menu **principal** de Windows 11 = **extension shell `IExplorerCommand`** ⇒ **paquet sparse MSIX** ⇒ **dépendance de packaging** (signature ? inscription par utilisateur ? mode développeur ?) : **spike d'abord**, et **aucune installation sans son accord**. L'entrée actuelle (menu hérité) reste valable entre-temps.
- **T-032** — l'**écran des règles** (F-017) : `docs/plan.md` dit « l'auto-organisation (F-017/F-018, T-031/T-032) **n'est pas demandée** — ne pas y investir », alors que `TODO.md` et `STATE.md` la portent. **Une des deux doit gagner** ; question posée en session 52, **rien écrit** depuis.
- **T-052** — déposer un fichier sur le **bureau vide** ne fait plus rien (le fond n'enregistre aucune cible de dépôt, volontairement). Le corriger veut dire **réimplémenter le drop du bureau**, et « déposer sur le bureau » **déplace** le fichier (famille T-031/T-036, opt-in explicite). **À trancher avant de coder.**
- **T-053** — le **Peek** n'a plus d'effet visible (une zone est déjà au-dessus de tout ce qui est dessiné sur le bureau) : **retirer** l'entrée du tray et le raccourci, ou la **requalifier** en « au-dessus des fenêtres d'application » (= T-029, modèle top-level).
- **T-048** — « des bugs quand je mets **certains** éléments dans une zone » (session 28) : **donner des exemples reproductibles** (un dossier ? un `.lnk` ? un fichier sans icône ? plusieurs fichiers d'un coup ?) avant tout code.
- **T-031 / T-033 / T-036** — même famille : l'écriture sur le disque et la surveillance récursive. Aujourd'hui **rien ne déplace jamais un fichier** (règle d'or d'`AGENTS.md`) : à faire (avec journal d'annulation) ou à requalifier.

## 3. Ce qui se mesure à la main (l'agent ne peut pas l'injecter)

- **T-070 — le spike du hook souris** (`experiments/spike_mouse_hook.cpp`, commande dans `README.md`) : **un glisser au bouton droit** et **un clic droit** sur le papier peint. C'est la mesure qui manque pour savoir si F-001 peut vivre sans **aucune** fenêtre à nous sur le bureau — le fond plein écran disparaîtrait alors, avec les trous d'alpha de T-069.
- Les **raccourcis globaux** : `Win+Shift+H` (quick hide) est **refusé** tant qu'une autre instance le tient — mesuré ce soir, `--nav-demo` **21/22** **pour cette seule raison** (**T-044**).

## 4. Validations humaines de fin de phase (déjà demandées par `docs/plan.md`)

- **T-018** — redémarrer Explorer (`taskkill /f /im explorer.exe`, puis relance) : les zones, enfants de `Progman`, doivent revenir sans crash.
- **T-019** — `--host-demo` avec **Fences 6 actif** et **arrêté** (les deux configurations n'ont été mesurées qu'avec le spike Python).
- **T-021** — `--render-probe --surface --with-desktop-items`, **bureau visible** : la grille d'icônes réelles dans une zone, la sélection (clic / Ctrl / Maj), le double-clic.
- **Passe à deux écrans** (critère de sortie de la Phase 5) : **bloquée par le matériel** — la machine n'a qu'**un** écran (mesuré le 2026-10-18 : 2560×1440 à 100 %). Il faudrait un second écran, si possible à une **échelle différente** (le cas DPI mixte est celui qui a motivé F-032).
- **Capture avant/après restauration de snapshot** (second critère de la Phase 5).
- Au passage, les sondes hébergées : `--fence-demo --host`, `--persist-demo --host`, `--nav-demo --host`, `--rules-demo --host`, `--portal-demo --host --portal-folder <un dossier à soi>`.

## 5. Sans blocage (travail, aucune décision à prendre)

- **T-014b, le reste** — les **thèmes de zones** : `iconSize`, `padding`, `gap`, `titleBar` sont **persistés et inertes** (un champ qu'un widget jette est une promesse).
- **T-081** — un **défaut général** (aujourd'hui un littéral compilé) + « appliquer à toutes les zones ».
- **T-083** — la sonde `--icons-demo` lit un **compte** de sélection là où sa question porte sur un **élément**.
- Puis **T-047** (zones prédéfinies), **T-076** (afficher/masquer la barre de titre : il faut un geste de déplacement qui survive à sa disparition), **T-063**, **T-042** (renommer / couper-coller hors zone + restitution de `DefView` après un plantage), **T-013** (pages de bureau), **T-072** (le double-clic du bureau vide cherche un porteur), **T-015** (packaging v0.1, installeur, docs utilisateur).
