**StarFish {VERSION}: alpha.** Expect bugs. Back up `%APPDATA%\StarFish` before installing.

### Nouveautés

{CHANGELOG}

### Install

1. Download `StarFish-{VERSION}-setup.exe` below and run it. It installs for your user only, with no admin rights, into `%LOCALAPPDATA%\Programs\StarFish`.
2. Releases are not code-signed yet. If Windows SmartScreen says "Windows protected your PC", click **More info > Run anyway** (French: *Informations complémentaires > Exécuter quand même*).

### Your data

Updating never touches your fences and settings (`%APPDATA%\StarFish`). Uninstalling keeps them unless you explicitly choose to delete them.

### Integrity

SHA-256 of the setup: `{SHA256}` (also in the attached `.sha256` file).
Check it in PowerShell: `Get-FileHash .\StarFish-{VERSION}-setup.exe -Algorithm SHA256`.
