# CHANGELOG.md — notes de version (FR)

> Chaque version taguée **doit** avoir une section ici. Le workflow Release
> (`.github/workflows/release.yml`) en extrait le bloc pour le corps GitHub
> (`### Nouveautés`). Voir `AGENTS.md`. Ne pas y mettre de chemins personnels
> ni de noms d'utilisateur.

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
