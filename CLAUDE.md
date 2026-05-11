# gbl-chainload — Claude instructions

These rules apply to **every** Claude session in this project. They override
default and auto-mode behavior. They are also enforced at the tool layer by
`.claude/hooks/block-non-hlos-flash.py` (PreToolUse Python hook) and declared
in `.claude/settings.json` `autoMode.hard_deny`.

## Safety: never flash non-HLOS images

**Do not autonomously run** any of:

- `fastboot flash <X>` where `<X>` is **not** one of
  `system`, `vendor`, `product`, `system_ext`, `odm`, `userdata`, `cache`,
  `metadata` (with optional `_a` / `_b` slot suffix).
- `fastboot oem unlock` / `fastboot oem lock`
- `fastboot --set-active <slot>`
- `fastboot erase <non-HLOS partition>`

These are device-bricking commands. To test gbl-chainload revisions, use
**only**:

```
fastboot stage dist/<artifact>.efi
fastboot oem boot-efi
```

That path is a one-shot RAM load that survives a power cycle without
touching any persistent partition. If a non-HLOS flash is genuinely needed,
surface the proposed command and let the user run it themselves in a real
shell (`! <command>` from the input box).

The PreToolUse hook will block these patterns regardless of mode, but the
rule is documented here so the model doesn't waste turns trying to work
around it.

## Workflow: PRs only

Every change in this repo lands via a feature branch + PR against `main`.

- No direct commits to `main`.
- No pushes to `main`.
- Iterative work stays a commit series on the same feature branch — the
  PR grows new commits as feedback comes in.
- Hot-fix-style "tiny" / "obvious" changes are not an exception.

This applies regardless of mode. Auto mode does not opt out of either of
these rules.
