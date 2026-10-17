# Décisions actées — index

> Extrait de l'ancienne section « Décisions actées » de `STATE.md` : **une ligne par décision**, la numérotation d'origine est
> conservée (les docs citent « décision 18 », « décision 24 »…). Le détail est dans un fichier **par domaine**, lu à la demande.
> Le récit et les mesures restent dans `docs/technique.md`.

| # | Décision (essentiel) | Domaine |
|---|---|---|
| 1 | Cible **Windows 10/11 x64** uniquement en v1 (Fences est Windows-only). | [Contraintes](./decisions/01-contraintes.md) |
| 2 | Stack : **C++17 + Qt 6.8 (Widgets)**. | [Contraintes](./decisions/01-contraintes.md) |
| 3 | Masquage des icônes natives et rendu maison (ADR-3) ; sur Win11, masquage réversible via… | [Windows](./decisions/02-windows-plateforme.md) |
| 4 | **ADR-4** : fenêtres enfants de `Progman`, insérées sous `SHELLDLL_DefView`… | [Windows](./decisions/02-windows-plateforme.md) |
| 5 | Persistance JSON versionnée atomique ; les fichiers ne sont jamais déplacés sans opt-in. | [Layout](./decisions/04-layout-persistance.md) |
| 6 | Local-first : zéro télémétrie. | [Contraintes](./decisions/01-contraintes.md) |
| 7 | **Portage Qt (T-016)** : garde `WM_WINDOWPOSCHANGING` (aucun restack au-dessus des icônes) + watchdog 2 s au… | [Windows](./decisions/02-windows-plateforme.md) |
| 8 | **Session 7** : le flag layered doit être voulu par Qt (`setWindowOpacity(0.99)`), jamais ajouté après le… | [Windows](./decisions/02-windows-plateforme.md) |
| 9 | **T-004** : le Shell est l'autorité pour les chemins (`SHGetKnownFolderPath`) et les noms affichés… | [Windows](./decisions/02-windows-plateforme.md) |
| 10 | **Session 8 — chemin de rendu (§7 piste (a))** : une **surface Win32 brute** (`DesktopSurface`) possède le… | [Rendu](./decisions/03-rendu.md) |
| 11 | **Session 8 — manifeste applicatif obligatoire** : `starfish.exe` doit embarquer `src/starfish.manifest` (via… | [Windows](./decisions/02-windows-plateforme.md) |
| 12 | **Session 9 — T-017, les deux modes layered sont mutuellement exclusifs**… | [Rendu](./decisions/03-rendu.md) |
| 13 | **Session 10 — la validation T-021/T-017 à l'écran n'est pas faisable depuis l'agent** : le bureau de… | [Outils](./decisions/08-outils-verification.md) |
| 14 | **Session 10 — toolchain agent** : `export PATH` dans le bash agent ne suffit pas à résoudre… | [Outils](./decisions/08-outils-verification.md) |
| 15 | **Session 13 — T-007 : la géométrie des zones est pure, le propriétaire est le conteneur.** `FenceGeometry`… | [Layout](./decisions/04-layout-persistance.md) |
| 16 | **Session 14 — T-008 : le layout est la seule autorité sur le contenu d'une zone, et l'écriture est différée**… | [Layout](./decisions/04-layout-persistance.md) |
| 17 | **Session 15 — T-009 : la navigation change ce qui est *visible*, jamais ce qui est *stocké*** (détail… | [Navigation](./decisions/05-navigation-gestes.md) |
| 18 | **Session 16 — le binaire est autonome (déploiement du runtime Qt)** : `build/starfish.exe` refusait de… | [Outils](./decisions/08-outils-verification.md) |
| 19 | **Session 17 — T-010 : les règles décident quelle zone *montre* un item, et ne touchent jamais au disque**… | [Règles](./decisions/06-regles-portails.md) |
| 20 | **Session 18 — T-011 : un portail reflète un dossier, le document n'en stocke que le chemin** (détail : §13… | [Règles](./decisions/06-regles-portails.md) |
| 21 | **Session 20 — T-012 : une snapshot est une disposition *plus une enveloppe*, et l'enveloppe se détecte en la… | [Layout](./decisions/04-layout-persistance.md) |
| 22 | **Session 21 — T-038 : le produit, c'est une séquence, et la capture souris était le vrai blocage** (détail… | [Produit](./decisions/07-produit.md) |
| 23 | **Session 22 — T-014a : le multi-écran est un modèle pur, et le DPI tient en deux frontières** (détail : §16… | [Produit](./decisions/07-produit.md) |
| 24 | **Session 23 — publication sur GitHub, et le premier build MSVC (qui a trouvé un vrai bug)** (détail… | [Outils](./decisions/08-outils-verification.md) |
| 25 | **Session 24 — T-014a : la sonde lit l'environnement, et la mise à l'échelle de la peinture se mesure**… | [Produit](./decisions/07-produit.md) |
| 26 | **Session 25 — ADR-9 : l'édition manuelle est une demande produit, et le z-order est le mur** (détail : ADR-9… | [Navigation](./decisions/05-navigation-gestes.md) |
| 27 | **Session 26 — T-043/T-044 : le geste se mesure à l'écran, et une seule instance** (détail : §9 et §15 de… | [Navigation](./decisions/05-navigation-gestes.md) |
| 28 | **Session 27 — T-045 : les cellules de la grille sont locales au contenu, et une vérification bornée par un cl… | [Rendu](./decisions/03-rendu.md) |
| 29 | **Session 28 — décisions utilisateur : ADR-9 option A confirmée, « le Peek est l'affichage normal », filet ava… | [Produit](./decisions/07-produit.md) |
| 30 | **Session 29 — T-046 : le filet est une note sur disque, et l'**ordre d'écriture** est la décision** (détail… | [Layout](./decisions/04-layout-persistance.md) |
| 31 | **Session 29 (clôture) — trois rapports utilisateur, notés sans être devinés.** Le filet était livré ; en… | [Produit](./decisions/07-produit.md) |
| 32 | **Session 30 — T-039 : les icônes hors zone sont *calculées*, et le fond est un *rôle* de surface, pas une sec… | [Rendu](./decisions/03-rendu.md) |
| 33 | **Session 31 — T-040 : sortir une icône est une *possession*, pas un état ; et le contenu doit être redessiné*… | [Rendu](./decisions/03-rendu.md) |
| 34 | **Session 32 — T-041 : le fond *est* le bureau, donc les gestes du bureau vide n'ont plus besoin d'un hook glo… | [Navigation](./decisions/05-navigation-gestes.md) |
| 35 | **Session 33 — le produit ne se mettait jamais dans l'état où la fonctionnalité existe ; le geste passe au bou… | [Produit](./decisions/07-produit.md) |
| 36 | **Session 34 — T-060 : une fenêtre layered par pixel n'existe pour la souris que là où son alpha n'est pas nul… | [Rendu](./decisions/03-rendu.md) |
| 37 | **Session 34 (suite) — T-061 : le papier peint appartient au `DefView`, les icônes à son enfant ; on masque le… | [Rendu](./decisions/03-rendu.md) |
| 38 | **Session 34 (fin) — ADR-13 : plus rien n'est pris au bureau ; les icônes sont celles de l'utilisateur, et Sta… | [Navigation](./decisions/05-navigation-gestes.md) |
| 39 | **Session 36 — T-066/T-067 : le produit ne se met plus dans un état que personne n'a demandé, et une zone neuv… | [Produit](./decisions/07-produit.md) |
| 40 | **Session 37 — T-068 : une règle écrite pour le fond s'appliquait aux zones** (détail… | [Règles](./decisions/06-regles-portails.md) |
| 41 | **Session 38 — T-069 : le bureau redevient atteignable, par un trou d'alpha et non par une réponse** (détail… | [Navigation](./decisions/05-navigation-gestes.md) |
| 42 | **Session 38 (suite) — décision utilisateur : F-001 passe au hook souris, spike d'abord** (détail : **T-070**… | [Navigation](./decisions/05-navigation-gestes.md) |
| 43 | **Session 40 — T-071 / ADR-14 : le bureau reste au bureau, et F-001 passe par le menu de Windows** (détail… | [Windows](./decisions/02-windows-plateforme.md) |
| 44 | **Session 41 — T-049 : une cellule plus large que son icône, et une légende qui se replie** (détail : **§31**… | [Rendu](./decisions/03-rendu.md) |
| 45 | **Session 43 — T-074 / T-075 : l'opaque par défaut, et les coins ronds qui allaient avec** (détail : **§32**… | [Rendu](./decisions/03-rendu.md) |
| 46 | **Session 44 — T-050 / T-077 : une cadence n'est pas une raison, et une frame périmée n'est pas une frame**… | [Rendu](./decisions/03-rendu.md) |
| 47 | **Session 46 — T-079 / ADR-15 : le push par pixel ne compose pas sur un enfant de `Progman` ; les zones repassent au blit, la forme devient une région** (détail : **§34** de `docs/technique.md`). Annule la règle de T-075 (« toute zone est poussée par pixel ») | [Rendu](./decisions/03-rendu.md) |
