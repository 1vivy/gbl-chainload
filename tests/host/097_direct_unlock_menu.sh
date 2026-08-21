#!/usr/bin/env bash
# tests/host/097_direct_unlock_menu.sh — source-level regression gate for the
# physical-button bootloader-unlock action exposed by gbl-chainload's menu.
set -euo pipefail

python3 - <<'PY'
from pathlib import Path

root = Path("edk2/QcomModulePkg")
draw = (root / "Include/Library/DrawUI.h").read_text()
menu = (root / "Library/BootLib/FastbootMenu.c").read_text()
keys = (root / "Library/BootLib/MenuKeysDetection.c").read_text()
unlock = (root / "Library/BootLib/UnlockMenu.c").read_text()

assert draw.count("BOOTLOADERUNLOCK") == 1, "unlock action enum missing/duplicated"

row = menu.index('{{"Unlock bootloader"}')
action = menu.index("BOOTLOADERUNLOCK", row)
escape = menu.index('{{"Escape"}', row)
assert row < action < escape, "unlock menu row is not wired to its action"

case = keys.index("case BOOTLOADERUNLOCK:")
end = keys.index("\n  }\n}", case)
body = keys[case:end]
required = (
    "GblFastbootReadOemUnlockAllowed (&Allowed)",
    "if (EFI_ERROR (Status) || !Allowed)",
    "if (IsUnlocked ())",
    "ExitMenuKeysDetection ();",
    "DisplayUnlockMenu (UNLOCK, TRUE)",
)
for token in required:
    assert token in body, f"unlock dispatcher missing: {token}"
assert body.index("ExitMenuKeysDetection ();") < body.rindex(
    "DisplayUnlockMenu (UNLOCK, TRUE)"
), "fastboot menu event must close before opening the confirmation page"

assert unlock.count("This build does not erase personal data automatically.") == 2, (
    "unlock warning must describe the fork's no-auto-wipe behavior for both layouts"
)

print("PASS: direct unlock menu is permission-gated and confirmation-backed")
PY
