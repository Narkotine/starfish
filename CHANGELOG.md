# CHANGELOG.md — notes de version (FR)

> Chaque version taguée **doit** avoir une section ici. Le workflow Release
> (`.github/workflows/release.yml`) en extrait le bloc pour le corps GitHub
> (`### Nouveautés`). Voir `AGENTS.md`. Ne pas y mettre de chemins personnels
> ni de noms d'utilisateur.

## Non publié

### Correctifs
- Bouton : plus de titre dessiné sur le disque (replié ou au survol) ; le nom reste enregistré pour les menus.
- Bouton : les noms des raccourcis apparaissent tout seuls dès la fin de l'ouverture (plus besoin de sortir le curseur ou de cliquer) ; le fondu redessine la surface hébergée à chaque image.
- Bouton : les noms des raccourcis restent cachés pendant l'ouverture et le repli (plus d'ellipse « … » qui change à chaque image) ; ils n'apparaissent qu'une fois le disque entièrement ouvert, avec le texte déjà coupé à la taille finale, et un court fondu. Option par bouton (Apparence…, « Afficher les noms des raccourcis », activée par défaut) pour ne jamais les afficher.
- Bouton : à l'ouverture (et au repli), les icônes glissent vers leur place finale sans sauter ni vibrer (la disposition des anneaux est calculée une fois, puis interpolée en flottant avec le rayon du disque ; le dessin reste dans le cercle).
- Bouton : par défaut il n’est plus déplaçable (à la souris, plus de curseur de déplacement sur le disque ; les icônes à l’intérieur se cliquent et se lancent). Pour le bouger : clic droit « Déplacer le bouton » (forme compacte seulement, contour coloré comme le mode redimensionnement d’un widget) ; Échap, un lâcher, un clic ailleurs ou ~10 s d’inactivité terminent le mode. Grisé si le Bouton est verrouillé.
- Bouton : le disque ne dérive plus vers le haut-gauche à l’approche du curseur (l’ancre est mémorisée, plus reconstruite avec QRect::center() / x - d/2, ce qui reculait d’un pixel à chaque cycle si le diamètre est pair ; le diamètre physique est calé sur le facteur DPI pour que le dessin reste centré).
- Français : l’interface ne dit plus « fence » (menu « Apparence de la zone », réglages, infobulles). `tools/i18n_check.py` échoue si une traduction FR contient encore le mot.
- Bouton : le disque replié ne glisse plus au bord de l’écran (l’ancre reste fixe ; le cercle déployé est seulement décalé pour tenir dans l’écran, puis y revient) ; l’animation reste un cercle (plus d’ellipse) ; un Bouton verrouillé ne bouge pas, y compris quand le curseur est sur le bord.

### Nouveautés
- **Habillage du bouton** : un GIF animé, dont les images suivent l'ouverture (pas d'horloge propre : l'image est choisie d'après la progression). « Simple », « Obturateur (démo) » (iris six lames, fourni) ou « GIF animé… » (fichier *.gif, chemin mémorisé par bouton ; fichier manquant → Simple). « Position de l'image » : derrière ou devant les icônes (devant, les icônes restent cliquables une fois ouvert).
- **Apparence du bouton** : « Diamètre » du disque replié (48–256 px, pas de 8, par défaut ≈ 2× la taille des icônes) et « Nombre d'anneaux » (Automatique, ou 1 à 8). Un nombre fixe répartit plus d'icônes sur les anneaux extérieurs (proportionnel à la circonférence) et agrandit le rayon si besoin, sans chevauchement.
- **Apparence du bouton** : le clic droit « Apparence… » ouvre un dialogue dédié (matériau, transparence, effet, couleur de fond, bordure, taille des icônes, « Afficher les noms des raccourcis », etc.), séparé de l'apparence d'une zone classique.
- Architecture des formes de zone (`docs/state/decisions/zone-shapes.md`) : classique vs formes plug-in (Bouton, pétale plus tard) ; physique optionnelle plus tard.
- **Bouton** : zone circulaire libre (`shape=button`), menu bureau « Nouveau bouton », survol/clic pour déployer, raccourcis en anneaux concentriques (départ en haut, sens horaire ; couches supplémentaires et rayon agrandi s’il le faut, sans chevauchement).

## 0.1.3 — 9 novembre 2026

### Correctifs
- Une zone vide n'affichait pas la barre d'insertion au-dessus d'un widget glissé (le message « zone vide » coupait le dessin trop tôt).
- Le menu au dépôt depuis l'Explorateur (session 98) s'ouvrait au clic droit et n'avait pas le style StarFish : corrigé en session 99 (voir ci-dessous).

### Nouveautés
- **Mode Redimensionner** d'un widget (clic droit → Redimensionner) : contour couleur d'accent, bords droit / bas / coin, cases entières, fin 10 s après le dernier geste, Échap ou clic ailleurs ; aucun en zone verrouillée.
- **Menu au dépôt depuis l'Explorateur** (réglage Paramètres → Zones, désactivé par défaut) : un glisser au clic gauche depuis l'extérieur, sur une zone ou le bureau StarFish, propose Déplacer ici / Copier ici / Créer des raccourcis ici / Annuler, dans le style du menu clic droit du bureau. Ctrl et Alt restent directs. Les glissers internes à StarFish ne changent pas.
- **Verrouiller un widget** (taille et place) : Verrouiller / Déverrouiller en tête du menu ; pas de glisser ni de Redimensionner ; le reste reste utilisable.
- **Widgets de zone et widgets du bureau séparés** : un widget de zone ne va que de zone en zone ; un widget du bureau reste sur le bureau (peut passer derrière les zones) et n'entre jamais dans une zone.
- **Widget Image du bureau** redimensionnable au pixel (minimum 32 px, sans dépasser le bureau).

## 0.1.2 — 8 novembre 2026

### Correctifs
- Une nouvelle icône posée par Windows sous un widget du bureau est déplacée à la place libre la plus proche (les icônes déjà là ne bougent pas).
- Zone verrouillée : plus de poignée de déplacement au survol, plus de cadenas visible.
- Glisser une icône sur une zone : le texte sous l'image dit « Insérer » (plus le « Copier » du shell).

### Nouveautés
- Taille libre des widgets (Configurer / Autre taille…, jusqu'à 8 × 6 cases).
- Grille du bureau affichée quand un widget sort d'une zone.
- Vitesse d'enroulement aussi dans Apparence… de chaque zone.
- Un seul bouton de remise à zéro (À propos) : « Retirer toutes les zones et tous les widgets ».
- Widget Image (Ajuster / Remplir) ; glisser un widget entre zone et bureau (retiré en 0.1.3 : les deux familles sont séparées).
- Verrouillage des zones (Verrouiller / Déverrouiller).
- Langue appliquée sans redémarrage.
- Entrée prête « Gestionnaire des tâches » ; l'éditeur du menu bureau liste toutes les entrées prêtes.
