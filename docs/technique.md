# Documentation technique — StarFish

> Version 0.1 — 2026-10-13. Les décisions sont tracées sous forme d'ADR ; toute évolution doit mettre à jour l'ADR concerné et `STATE.md`.

## 1. Contraintes de plateforme

- Windows 10 (22H2+) et Windows 11, x64 en priorité.
- Pas de service Windows obligatoire : app desktop standard lancée au démarrage (option).
- Fonctionne sans droits administrateur.

## 2. Architecture (composants)

```
┌────────────────────────────────────────────────────┐
│ Application (Qt, thread UI)                        │
│  ├─ DesktopHost    héberge les zones sur WorkerW   │
│  ├─ FenceView      rendu + interaction d'une zone  │
│  ├─ IconGrid       grille d'icônes d'une zone      │
│  ├─ RuleEngine     évaluation des règles (F-017)   │
│  ├─ LayoutStore    lecture/écriture JSON atomique  │
│  ├─ ShellBridge    icônes, menu contextuel, DnD    │
│  └─ Tray/Settings  réglages, hotkeys (F-033)       │
└────────────────────────────────────────────────────┘
     │ dépend de
     ▼
  Windows API : WorkerW, SHELL32 (SHGetFileInfo,
  SHChangeNotify), ole32 (IContextMenu, CF_HDROP),
  per-monitor DPI v2, RegisterHotKey
```

- **LayoutStore** : seule source de vérité pour « quelle icône dans quelle zone, où ». Format JSON versionné (voir §5). Les fichiers, eux, restent où ils sont sur le disque.
- **RuleEngine** : pure, testable sans UI (entrée = métadonnées fichier + règles ; sortie = zone cible).
- **ShellBridge** : isole tout l'accès aux API Windows pour rendre le reste du code testable/portable.

## 3. Décisions techniques

### ADR-1 — Windows uniquement en v1
Fences est Windows-only et la valeur du produit est l'intégration shell. Linux (KDE) pourra être étudié après la v1. *Statut : accepté.*

### ADR-2 — Stack : C++17 + Qt 6.8 (module Widgets)
- Pour : rendu custom avancé (formes de zones, fondu, teinte), DnD mature (`QMimeData`, `QDrag`), packaging Windows simple (`windeployqt`), per-monitor DPI, licence LGPL compatible open source.
- Contre : verbosité, build lourd.
- Alternative écartée : PySide6 (itération rapide, mais perf/packaging/empreinte mémoire moins bons pour un daemon de bureau) ; WPF/WinUI (verrou Microsoft, moins portable) ; egui/Rust (écosystème DnD/windowing moins mature).

### ADR-3 — On masque les icônes natives et on dessine les nôtres
Requis par folder portals (F-024), teinte d'icônes (F-015) et pages de bureau (F-030). Conséquence : il faut réimplémenter les interactions shell (double-clic, menu contextuel via `IContextMenu`, glisser-déposer `CF_HDROP`, renommage F2). **Repli possible** : mode dégradé qui repositionne les icônes natives dans la zone (pas de portals ni de teinte) — à garder en tête si T-003/T-005 révèlent un blocage shell.

### ADR-4 — Positionnement : fenêtres enfants de Progman, sous SHELLDLL_DefView

Technique initialement envisagée (WorkerW) **invalide sur Windows 11 récent** — findings du spike du 2026-10-13 (`experiments/spike_workerw.py`) :
- Win11 garde `SHELLDLL_DefView` **enfant direct de Progman** ; le détournement WorkerW (message 0x052C, re-parenting de la DefView) propre à Win10 ne se produit pas (0x052C ne crée rien ici).
- Les `WorkerW` présents (15 sur la machine de dev) sont des fenêtres 136×39 d'outils tiers ; `SetParent` vers elles échoue.
- La machine de dev a **Fences 6 installé et actif** → toute la validation était faite « sous Fences », ce qui est représentatif.

**Stratégie retenue :** `SetParent(fenêtre_zone, Progman)` puis `SetWindowPos(hwnd, SHELLDLL_DefView, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW)` → la fenêtre s'insère juste **sous** la couche d'icônes, donc au-dessus du papier peint et sous les icônes. Validation spike : `SetParent` ok=True, `SetWindowPos` ok=True.
**Séquence Win32 de référence** (validée par le spike du 2026-10-13, dans cet ordre) :
1. Créer une fenêtre `WS_POPUP` sans décoration.
2. `SetParent(hwnd, Progman)` ; style : ajouter `WS_CHILD`, retirer `WS_POPUP`.
3. **Effacer `WS_EX_TOPMOST`** (obligatoire : sinon la fenêtre reste au-dessus de la DefView quel que soit `insert_after`).
4. `SetWindowPos(hwnd, SHELLDLL_DefView, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW)`.
5. Vérifier par la **chaîne de z-order des enfants de Progman** (`EnumChildWindows`) : la fenêtre doit apparaître **après** `SHELLDLL_DefView`/`SysListView32`. (Hit-tests non fiables : `WindowFromPoint` répond `Shell_LightDismissOverlay` ; une DefView désactivée est ignorée des hit-tests.)
⚠️ Ne pas utiliser `overrideredirect` Tk ni de framework qui restack : Tk replace ses fenêtres au-dessus après `SetWindowPos`.
**Comportement constaté avec Fences 6 actif** : nos fenêtres se placent sous les fences dessinées par Fences — placement conforme au produit de référence.
**Bonus mesuré :** masquer les icônes natives = `ShowWindow(SW_HIDE)` sur la `SHELLDLL_DefView` (réversible, pas d'écriture registre sur ce build). Le basculement registre `HideIcons` reste prévu en secours.
Z-order à recalculer sur `WM_SETTINGCHANGE`, changement de résolution et redémarrage d'Explorer.

### ADR-5 — Persistance JSON versionné
Exemple (§) : lisible, diff-able, exportable (F-028). Écriture atomique (fichier temporaire + `ReplaceFile`). Emplacement : `%APPDATA%/StarFish/`.

### ADR-6 — Local-first
Aucun accès réseau fonctionnel, aucune télémétrie, aucun compte (F-037). Les folder portals pointent vers des dossiers locaux ou réseau de l'utilisateur, jamais vers un service StarFish.

## 4. Points d'intégration Windows (à valider en Phase 1)

| Besoin | API / technique |
|---|---|
| Masquer les icônes natives | Basculer `HideIcons` (`HKCU\...\Explorer\Advanced`) ou styler la fenêtre `SHELLDLL_DefView` ; rendre le basculement instantané et réversible |
| Z-order bureau | `Progman` → `WorkerW` ; attacher les fenêtres de zone |
| Icônes système | `SHGetFileInfo` + cache `IImageList` (`SHGetImageList`) |
| Notifications de changement du bureau | `SHChangeNotify` / `ReadDirectoryChangesW` sur le dossier Bureau |
| Menu contextuel | `IContextMenu` (shell) — phase 2 |
| Glisser-déposer sortant | `CF_HDROP` via OLE (phase 2) |
| DPI | Per-monitor DPI v2 (`SetProcessDpiAwarenessContext`) |
| Hotkeys globales | `RegisterHotKey` (Win+Espace pour Peek = F-021) |

## 5. Modèle de données (ébauche)

```json
{
  "schema": 1,
  "fences": [
    {
      "id": "uuid",
      "title": "Travail",
      "monitor": "\\\\.\\DISPLAY1",
      "rect": { "x": 0, "y": 0, "w": 480, "h": 320 },
      "style": {
        "bg": "#1F2937CC", "radius": 10, "padding": 12,
        "iconSize": 48, "gap": 8, "titleBar": true,
        "iconTint": null
      },
      "sort": { "by": "name", "asc": true },
      "rolledUp": false,
      "items": [
        { "kind": "file",  "path": "C:\\Users\\x\\Desktop\\a.pdf" },
        { "kind": "portal","path": "C:\\Users\\x\\Documents\\Factures" }
      ]
    }
  ],
  "rules": [
    { "id": "uuid", "name": "Images → Médias",
      "match": { "type": ["jpg", "png"] },
      "targetFence": "uuid", "moveFiles": false }
  ]
}
```

Règle d'or : `moveFiles: false` par défaut — les règles ne **déplacent jamais un fichier sur le disque** sans opt-in explicite.

## 6. Build, packaging, qualité

- **Build** : CMake ≥ 3.24, Qt 6.8, compilateur MSVC 2022 (ou MinGWw64 en secours).
- **CI** : GitHub Actions `windows-latest` → configure/build + tests unitaires (`RuleEngine`, `LayoutStore`) à chaque PR.
- **Packaging** : zip portable (`windeployqt`) pour les nightly ; installeur (WiX ou Inno Setup) pour les releases ; publication `winget` plus tard.
- **Tests** : unitaires pour la logique pure (règles, sérialisation) ; manuels documentés pour l'intégration shell (checklist dans `devlog/`).
- **Style** : clang-format (LLVM, 2 espaces), commits en anglais, Conventional Commits.

## 7. Ce qui reste à trancher

- Stratégie de renommage inline (F2) vs menu shell uniquement.
- Comportement Peek quand aucune zone n'est définie.
- Format exact des hotkeys configurables (JSON dans les réglages).
