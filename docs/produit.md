# Documentation produit — StarFish

> Équivalent open source de Stardock Fences 6. Version du document : 0.1 — 2026-10-13.

## 1. Vision

Sur Windows, le bureau est un espace unique et plat : toutes les icônes (apps, fichiers, dossiers) s'accumulent dans une grille sans structure. StarFish propose des **zones délimitées (« fences »)** posées sur le bureau : on y dépose ses icônes, on les trie, on les déplace de zone en zone, et on retrouve un bureau organisé.

Promesse : **la parité fonctionnelle de Fences 6, en open source, gratuit, local-first et léger.**

## 2. Problème

- Le bureau windowsien n'offre aucun regroupement visuel ni tri automatique.
- Les solutions payantes (Fences, etc.) sont en abonnement ; les alternatives gratuites sont partielles.
- Les utilisateurs veulent du **contrôle total** (où sont stockées leurs données de disposition, aucune télémétrie).

## 3. Cible et positionnement

| Critère | StarFish | Fences 6 |
|---|---|---|
| Prix | Gratuit, open source (MIT prévu) | Abonnement dès ~10 $/an |
| Données | 100 % locales, JSON lisible | Propriétaire |
| Télémétrie | Aucune | Oui (Stardock) |
| Plateforme v1 | Windows 10/11 x64 | Windows 10/11 (+ARM) |
| Déploiement entreprise | Non visé en v1 | Oui (édition Business) |

Sources : pages officielles Stardock (produit + notes de version Fences 6, consultées le 2026-10-13).

## 4. Personas

- **Power user** : 100+ icônes, plusieurs projets en cours, veut des zones par contexte (dev, perso, admin) et un tri automatique.
- **Étudiant** : cours/devoirs, une zone couleur par matière, anti-distraction.
- **Gamer** : lanceurs de jeux regroupés, zone enroulable, Peek pendant le stream.
- **Accessibilité** : dossiers profonds ramenés sur le bureau via folder portals, icônes plus grandes.
- **IT / admin (hors scope v1)** : déploiement de gabarits — noté pour plus tard.

## 5. Cas d'usage de référence

1. L'utilisateur **trace un rectangle au bouton droit** sur le bureau vide : le menu du bureau s'ouvre avec « Créer une zone ici (300 × 200) » en tête, il valide, et la zone existe exactement là où il l'a dessinée.
2. L'utilisateur glisse une zone sur la droite du bureau, y dépose ses icônes de travail, les trie par nom.
3. Nouveau fichier téléchargé sur le bureau → une règle le range automatiquement dans la zone « Downloads ».
4. Zone « Jeux » enroulée (roll-up) : seule la barre de titre reste visible.
5. Win+Espace (Peek) : toutes les zones passent au premier plan au-dessus des fenêtres.
6. Un portail pointe vers `~/Documents/Factures` : son contenu s'affiche comme des icônes de bureau.
7. Changement de résolution/moniteur → snapshot permet de restaurer la disposition.
8. L'utilisateur choisit un **matériau translucide** pour ses zones : le papier peint les traverse, et le bureau reste lisible sous elles.

## 6. Inventaire fonctionnel

Référence : Fences 6 (fonctionnalités consolidées des versions 4/5/6). Priorités : **P0** = MVP v0.1, **P1** = v0.2, **P2** = v1.0 et après.

### A. Zones (fences)

| ID | Fonctionnalité | Prio |
|---|---|---|
| F-001 | Créer une zone : **entrée « StarFish: create a fence » dans le menu clic droit *de Windows*** sur le bureau (**T-071** — le geste du bouton droit a été **abandonné** : il exigeait une fenêtre à nous sur tout le bureau, qui coûtait les clics et les glissers des icônes ainsi que les deux boutons du bureau nu). La zone se pose à la **première place libre**. Reste aussi la zone de notification | P0 |
| F-002 | Renommer la zone (barre de titre) | P0 |
| F-003 | Déplacer et redimensionner une zone (aimantation aux bords d'écran et aux autres zones) | P0 |
| F-004 | Apparence : **couleur de fond**, opacité, **coins arrondis**, **style de barre de titre** — détail des gestes : **T-080** (le **par zone**) et **T-081** (le **défaut général** + « appliquer à toutes »), demandés en session 46 | P0 |
| F-005 | Réglages fins : padding interne, espacement entre icônes, marges | P1 |
| F-006 | Verrouiller position et taille d'une zone | P1 |
| F-007 | Supprimer une zone (les icônes reviennent sur le bureau, jamais supprimées du disque) | P0 |
| F-008 | Onglets : regrouper plusieurs zones sous forme d'onglets (nouveauté Fences 6) | P1 |
| F-009 | Couleur de zone auto-adaptée au fond d'écran (type Chameleon) | P2 |
| F-038 | **Matériaux de zone** : choix parmi des presets (opaque / translucide / très translucide) — transparence partielle du panneau, le papier peint traverse la zone. **Livré (T-057, session 35)** : les presets sont `opaque` (255) / `translucent` (204, le défaut du schéma) / `very-translucent` (128), l'alpha du fond (persisté depuis T-008) est enfin vif, et le choix s'expose au tray (toutes les zones) et dans les réglages (une zone) ; l'**acrylique** (flou) reste ouvert et à cadrer par un spike, les matériaux DWM visant une fenêtre de premier niveau | P1 | ; ⚠️ **retour utilisateur (session 47)** : la translucidité porte aujourd'hui sur la **fenêtre entière** (barre de titre **et icônes** comprise) alors que cette ligne décrit le **panneau** — **T-082** (panneau seul par défaut, application aux icônes en option **par zone**)

### B. Icônes

| ID | Fonctionnalité | Prio |
|---|---|---|
| F-010 | Déposer des icônes dans une zone (glisser-déposer) | P0 |
| F-011 | Sortir une icône d'une zone (retour au bureau) | P0 |
| F-012 | Tri dans une zone : nom, type, taille, date | P1 |
| F-013 | Alignement sur grille / réarrangement automatique | P0 |
| F-014 | Taille des icônes et densité réglables (par zone) | P1 |
| F-015 | Teinte de couleur appliquée aux icônes (nouveauté Fences 6) | P2 |
| F-016 | Fondu des icônes dans le fond d'écran (Chameleon fade) | P2 |
| F-041 | **Widgets dans une zone** : un bloc qui occupe plusieurs cellules de la grille et garde ses réglages dans le document (`kind: "widget"`, `settings`) — il se déplace, se range et se redimensionne comme une tuile, sans jamais toucher au disque. Livrés : **horloge** (**T-134**, s79 : heure, date, secondes, 12/24 h, taille) et **notes** (s81 : texte libre sur plusieurs lignes, écrit dans la zone au double-clic, enregistré pendant la frappe). À venir : widgets web (`%APPDATA%\StarFish\widgets\<id>\manifest.json`), météo, calendrier, CPU/RAM (**T-135**) | P1 |

### C. Automatisation

| ID | Fonctionnalité | Prio |
|---|---|---|
| F-017 | Règles d'auto-organisation : par type, nom (motif), date, emplacement cible | P1 |
| F-018 | Application des règles aux nouveaux éléments du bureau (surveillance du dossier) | P1 |
| F-019 | Création rapide de zones prédéfinies (haut / milieu / bas, gauche / droite) | P1 |

### D. Navigation

| ID | Fonctionnalité | Prio |
|---|---|---|
| F-020 | Roll-up : enrouler une zone sur sa barre de titre (molette ou double-clic) | P0 |
| F-021 | Peek : raccourci (Win+Espace) pour mettre toutes les zones au premier plan | P0 |
| F-022 | Quick hide : double-clic sur le bureau vide pour masquer/afficher | P0 |
| F-023 | Navigation clavier dans les zones (accessibilité) | P1 |

### E. Folder portals

| ID | Fonctionnalité | Prio |
|---|---|---|
| F-024 | Portail vers un dossier quelconque (miroir en lecture/écriture) | P1 |
| F-025 | Taille d'icônes distincte pour les portails | P2 |
| F-026 | Navigation dans les sous-dossiers d'un portail (navigation améliorée Fences 6) | P2 |

### F. Dispositions (layouts)

| ID | Fonctionnalité | Prio |
|---|---|---|
| F-027 | Snapshots : sauvegarder et restaurer une disposition | P0 |
| F-028 | Export / import d'une disposition (fichier) | P1 |
| F-029 | Disposition distincte par moniteur | P2 |

### G. Pages de bureau

| ID | Fonctionnalité | Prio |
|---|---|---|
| F-030 | Pages de bureau multiples, bascule par glissement sur le bureau vide | P2 |
| F-031 | Association page ↔ écran | P2 |

### H. Système et plateforme

| ID | Fonctionnalité | Prio |
|---|---|---|
| F-032 | Multi-moniteur + awareness DPI (résolutions mixtes) | P0 |
| F-033 | Raccourcis globaux configurables | P1 |
| F-034 | Icône de zone de notification + fenêtre de réglages | P0 |
| F-035 | Lancement au démarrage (optionnel) | P1 |
| F-036 | Interface au moins en anglais et français | P1 |
| F-037 | Aucune télémétrie, aucun compte, données locales uniquement | P0 |
| F-039 | **Le bureau reste celui de l'utilisateur : StarFish ne masque aucune icône native — et ne pose plus aucune fenêtre sur le bureau** (ADR-13, **T-066**). Le produit se pose **sur** le bureau normal — son papier peint, ses icônes à leurs places, leur clic, leur menu, leur glisser — **au lancement, en fonctionnement et à la fermeture**, y compris dans le cas qui a fait défaut : **une zone qui détient une icône du bureau**. Conséquence assumée et visible tant que **T-042** n'est pas écrit : cette icône s'affiche **deux fois** (sur le bureau **et** comme tuile dans la zone) — on ne peut pas laisser ses icônes à l'utilisateur *et* les faire disparaître de là où elles sont sans les déplacer, et StarFish ne déplace **jamais** un fichier. Reprendre la couche d'icônes (`--claim-desktop`) reste un **chemin de développement** ; dans le produit, **aucun réglage** ne le fait : la case de **T-088 / F-040** a été **retirée** en session 57 (**ADR-20**, sa demande) | P0 |
| F-040 | **StarFish dessine le bureau — ⛔ RETIRÉ (session 57, ADR-20)** : la case livrée en session 55 (T-088 / ADR-19) et le mode qu'elle ouvrait n'existent **plus** — il changeait le **clic droit du bureau** (menu reconstruit par StarFish au lieu de celui de Windows) et ne le rendait **pas** toujours en décochant, sans parler des icônes muettes. Aucune fonctionnalité du produit ne reprend la couche d'icônes ; `--claim-desktop` reste un chemin de développement. La préférence (`%APPDATA%/StarFish/preferences.json`) n'est plus lue. Détail : §43 de `docs/technique.md` | — |
| F-042 | **Entrées personnalisées du menu clic droit, avec icône** : un programme, ses arguments et une **icône choisie dans une pool de 33** (celles du menu lui-même, Segoe Fluent Icons, nommées et traduites) ; **entrées prêtes à l'emploi** (Paramètres Windows, Applications installées, Apparence et thèmes, Mode clair ou sombre, Réseau et Internet, Windows Update) ajoutées d'un clic. Une entrée peut aussi porter une **action** de StarFish au lieu d'un programme — aujourd'hui la bascule clair/sombre (**T-139**) | P1 |

Spécifiquement **hors périmètre** : édition entreprise (déploiement par GPO/comptes centralisés), skins avancés, support ARM (réévaluable plus tard).

## 7. Périmètre par version

- **v0.1 (MVP)** : noyau desktop (F-032, F-037, **F-039**) + zones basiques (F-001…F-004, F-007, F-013) + icônes drag & drop (F-010, F-011) + roll-up/Peek/quick hide (F-020…F-022) + snapshots (F-027) + tray/réglages minimaux (F-034).
- **v0.2** : apparence fine (F-005, F-006, **F-038**), tri (F-012, F-014), règles (F-017…F-019), portails (F-024), raccourcis (F-033, F-035), i18n (F-036).
- **v1.0** : onglets (F-008), teinte/fondu (F-015, F-016), dispositions par moniteur (F-029), export/import (F-028), accessibilité clavier (F-023).
- **Plus tard** : pages de bureau (F-030, F-031), Chameleon (F-009), navigation portails (F-025, F-026).

## 8. Non-objectifs

- Remplacer l'Explorateur Windows ou le shell.
- Synchroniser les dispositions entre machines (pas de cloud en v1).
- Gérer les thèmes Windows (Start11, WindowBlinds… c'est l'écosystème Stardock, pas le nôtre).
- Déplacer les fichiers de l'utilisateur sans son accord explicite : par défaut, StarFish ne change **jamais** l'emplacement des fichiers sur le disque ; seules les positions d'affichage sont gérées.

## 9. Concurrence

Audit réalisé le 2026-10-13 (sources : dépôts GitHub + blogs, non vérifiés par test).

| Projet | Stack | Licence | Maturité | Enseignements pour StarFish |
|---|---|---|---|---|
| **Fences 6 (Stardock)** | propriétaire | payant (~10 $/an) | mature, ~20 M+ téléchargements | référence fonctionnelle ; abonnement = notre différenciateur principal |
| **NoFences** (Twometer/NoFences) | C# / WPF | MIT | 690 ★, 131 forks ; README en statut 🚧 WIP, dernier commit janv. 2024, v1.8 (mai 2022) | prouve la demande et la faisabilité (fenêtre floutée au-dessus du bureau) ; projet en veille → fenêtre d'opportunité pour un maintenu |
| **openFences** (weiweigogo) | C++17, Win32 + Direct2D, nlohmann/json | MIT | actif (août 2026), exe unique ~780 Ko | concurrent direct le plus sérieux : acrylique, config `%APPDATA%` JSON, i18n zh/en, masquage des icônes natives avec restauration par le tray ; périmètre encore maigre (pas de portals, règles, pages, Peek) |
| Outils « save/restore layout » gratuits (DesktopOK, etc.) | divers | freeware | matures | restauration de positions uniquement, aucun concept de zone → ne couvrent pas le besoin |

**Différenciation StarFish :** parité fonctionnelle Fences 6 visée (portals, règles, Peek, pages, snapshots — absents ou partiels chez les concurrents OSS), local-first JSON documenté (ADR-5), développement actif et structuré (STATE/devlog/AGENTS), i18n EN/FR, base Qt permettant plus tard une cible Linux.
