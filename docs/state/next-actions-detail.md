# Prochaines actions — détail et ordre

> Dernière révision : **session 56 (2026-10-19)**. Lu quand on attaque une des actions de `STATE.md`.
> **Règle d'écriture** (leçon de la session 54) : **nommer la chose avant l'identifiant** — « les coins des zones
> (T-058) », jamais « T-058 » seul. Les identifiants ne servent qu'au suivi, pas à la compréhension.

## 1. Ce que son essai à l'œil a validé (session 54)

Plus de clignotements au repos, les zones se redimensionnent et se déplacent sans redessin inutile (**T-050/T-077**) ;
le bureau reste le sien — icônes déplaçables, rectangle de sélection, menu de Windows (**T-071/T-069**) ; ranger une
vraie icône dans une zone ne casse plus rien — elle s'affiche deux fois, c'est le prix annoncé (**T-066**) ; une zone
translucide laisse voir le papier peint **à travers son fond et sa barre de titre** mais garde ses icônes nettes
(**T-082**) ; les coins ne gênent pas (**T-058**) ; les couleurs et l'échelle d'arrondi d'une zone plaisent (**T-080**).

## 2. Ce que son essai a ouvert — à faire, dans cet ordre

0. **Essayer la case : « StarFish dessine les icônes du bureau » (T-088 + T-042, 1ʳᵉ tranche — session 56)** — c'est **sa** demande, et elle marche maintenant de bout en bout : **clic sur l'icône de la zone de notification → « Réglages... » → la case** ; puis **glisser une icône du bureau dans une zone** ⇒ elle y est **rangée** et **quitte le bureau** ; **double-clic** sur une icône restée libre ⇒ elle **s'ouvre**. Où sont les limites : les icônes **hors** des zones ne répondent **encore** ni au **clic droit**, ni à **F2**, ni au **couper/coller**, et il n'y a pas de **sélection rectangulaire** (reste de T-042) — ni de fantôme pendant le transport (**T-087**). Pour revenir en arrière : **décocher la case**, ou « **Restaurer mes icônes** » dans le menu de la zone de notification. La case est **décochée** par défaut, donc **rien ne change** s'il ne l'essaie pas.

1. **Deux crans d'arrondi de plus (T-084)** — le menu « Arrondi des coins » d'une zone propose *Carré*, 6, 10, 14, 20 ;
   il passe à sept entrées en ajoutant **30** et **40**. Petit.
2. **Retirer le Peek (T-053)** — il a tranché : « si plus d'effet visible, l'enlever ». Partent : l'entrée de la zone de
   notification, le raccourci `Alt+Win+Space` (le `Win+Space` d'origine est de toute façon refusé par Windows) et la
   fonction elle-même, avec ce que la sonde `--nav-demo` en mesurait. **Le quick hide reste** (`Win+Shift+H`, le
   double-clic sur le bureau vide cherche encore un porteur : **T-072**). « Voir au-dessus des fenêtres » (**T-029**)
   devient sans objet.
3. **Ranger et ne pas bouger : le bord opposé (T-086)** — ⚠️ son retour est **approximatif** (« occasionnellement,
   selon je pense position de la fence ») : le bord qu'on **tire** suit bien le curseur, mais l'**opposé** se déplace
   parfois. Non reproductible à volonté ⇒ **on instrumente d'abord** (géométrie demandée / obtenue à chaque pas,
   écran du centre, aimantation, bord d'écran), et on ne corrige qu'ensuite. Candidats nommés, aucun prouvé.
4. **L'espace par icône devient un réglage (T-085)** — son mot : « ça devrait être un réglage. -> élargissement
   automatique. Sinon manuel, à quel point veut-on de l'espace pour chaque icône ». Aujourd'hui c'est **automatique et
   subi** : la cellule s'élargit toute seule pour le plus long nom (plancher 1,5× l'icône, plafond 2×). Il veut
   pouvoir choisir : **automatique** (ce qui existe) ou **manuel** (une largeur décidée). Champ par zone, comme la
   couleur de barre ; le défaut général reste **T-081**.
5. **Voir ce qu'on tient, et ce qu'on va obtenir (T-087)** — deux idées de lui, notées telles quelles : (a) pendant un
   glisser de tuile, rien ne suit le curseur (seul un repère dit où ça tombera) — il voudrait un **fantôme** de ce
   qu'il tient ; (b) en traçant le rectangle au bouton droit pour créer une zone, rien ne se voit avant le lâcher —
   il voudrait une **proposition graphique** de la zone pendant le tracé.
6. **« StarFish : créer une zone » dans le vrai menu de Windows (T-073)** — accord donné (voir §3). La voie est une
   **extension shell** livrée dans un **paquet MSIX**, donc le travail commence par un **spike** : d'abord **mesurer**
   ce que ça impose (signature, inscription, mode développeur), et ne rien installer sans son accord.

## 3. Ce qui a été tranché avec lui (session 54)

- **T-073 — « ok pour dépendances »** : la dépendance de packaging est **acceptée**, le spike peut être conduit.
  L'**installation** sur sa machine reste soumise à son accord, comme l'ADR le prévoit.
- **L'auto-organisation n'est pas demandée** — il l'a redit. L'**écran des règles** (T-032) ne se fera **pas**, et la
  même réponse vaut pour les règles qui **déplaceraient** des fichiers (T-031), qui n'étaient de toute façon pas
  implémentées.
- **Déposer un fichier sur le bureau vide (T-052)** : « ok » ⇒ le comportement est **accepté tel quel**. Le corriger
  voudrait dire déplacer le fichier — réversible s'il change d'avis.
- **Bugs « avec certains éléments » dans une zone (T-048)** : il n'a **plus d'exemple** en tête ⇒ **abandonnée** (à
  rouvrir si un cas précis revient). Rien n'est mesurable sans un cas.
- **Ses trois zones empilées à `(80, 1200)`** : « touche à rien » ⇒ elles **restent en place**, on n'y touche pas.

## 4. Questions ouvertes, posées en clair (elles attendent son mot)

- **Portails : la moitié écriture (T-036)** — un « portail » est une zone qui **montre** le contenu d'un dossier. On
  peut y **déposer** un fichier : il devient une tuile, mais il n'entre **pas** dans le dossier. L'écriture voudrait
  dire **déplacer** un fichier sur le disque. À faire, ou à laisser ?
- **Suivre les sous-dossiers (T-033)** — StarFish ne surveille que le **premier niveau** d'un dossier : un fichier
  ajouté dans `Bureau\Sous-dossier` ne réveille personne (ni une règle, ni un portail). À faire ?
- **Le hook souris (T-070)** — la voie de Fences 6 : ne plus poser **aucune** fenêtre sur le bureau et **observer** le
  geste. Le gel de la souris est le risque (un hook lent rend la souris pâteuse pour toute la machine, et Windows
  l'évince en silence) : le spike existe, mais **le geste doit être fait à la main** (un glisser au bouton droit, un
  clic droit sur le papier peint). L'agent ne peut pas l'injecter.
- **Les raccourcis (T-030)** : aujourd'hui en dur, et `Win+Space` est refusé par Windows. Faut-il les rendre
  configurables ?

## 5. Validations humaines de fin de phase (déjà demandées par `docs/plan.md`)

- **Redémarrer Explorer** (T-018) : tuer puis relancer `explorer.exe` — les zones, qui sont enfants du bureau, doivent
  revenir toutes seules, sans plantage.
- **Avec et sans Fences 6** (T-019) : lancer `--host-demo` dans les deux cas (les deux configurations n'ont été
  mesurées qu'avec un prototype Python).
- **Une zone qui montre les vraies icônes** (T-021) : bureau visible ; clic, Ctrl+clic, double-clic.
- **Deux écrans** (critère de sortie de la phase) : **bloqué par le matériel** — un seul écran (2560×1440 mesuré le
  2026-10-18). Il faudrait un second écran, si possible à une échelle différente.
- **Photo avant / photo après** une restauration de disposition enregistrée (second critère de la phase).
- Au passage, les sondes qui affichent de vraies zones sur le bureau quelques secondes : `--fence-demo --host`,
  `--persist-demo --host`, `--nav-demo --host`, `--rules-demo --host`,
  `--portal-demo --host --portal-folder <un dossier à soi>`.

## 6. Sans blocage (travail, aucune décision à prendre)

- **Les thèmes de zone (reste de T-014b)** : la taille d'icône, la marge intérieure, l'écart entre icônes et la barre
  de titre sont **enregistrés mais sans effet** (un champ qu'un widget jette est une promesse).
- **Un défaut général pour les zones neuves (T-081)** : aujourd'hui un littéral dans le code ; il faut un réglage, ou
  un bloc `defaults` dans le fichier de disposition, plus une action « appliquer à toutes les zones ».
- **La sonde `--icons-demo` mesure un compte au lieu d'un élément (T-083)** : elle compare le **nombre** d'icônes
  sélectionnées ; si l'icône cliquée l'était déjà, le nombre ne bouge pas et la vérification tombe alors que le clic
  est bien livré. Lire l'**index** ou le **nom** sélectionné.
- Puis : **des zones prédéfinies** (haut/milieu/bas, gauche/droite — T-047), **afficher/masquer la barre de titre**
  (T-076 : il faut un geste pour déplacer une zone quand sa barre disparaît), **renommer et couper/coller les icônes
  hors zone** (T-042, avec la restitution des icônes du bureau après un plantage), **plusieurs pages de bureau**
  (T-013), **le double-clic sur le bureau vide** qui cherche un nouveau porteur (T-072), **le paquet et l'installeur
  v0.1** (T-015).
