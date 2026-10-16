# Easy upgrade to VCPE 5.0

Run the engine's offline upgrade module against the demake's project root:

```sh
python3 /path/to/vcpe/tools/vcpe_upgrade.py plan /path/to/demake
python3 /path/to/vcpe/tools/vcpe_upgrade.py apply /path/to/demake
```

Add `--savedata` to link `vcpe_savedata`, `vcpe_save`, and `-lpsputility` as well.
`--engine /path/to/another/vcpe` selects a different engine source. No downloads run.

The tool accepts `port/Makefile` or a root `Makefile` with the normal `VCPE` and
`VCPE_MODULES` variables. It copies the engine source into
`.vcpe/engines/<version>-<content-hash>/`, verifies its SHA-256 manifest, pins the
Makefile to that snapshot, resolves common module dependencies, and tracks engine
headers for rebuilds. Existing Podman builds that mount the whole project at `/work`
can read the pinned engine directly. A command-line `VCPE=` cannot accidentally
replace the pin. Custom builds that mount only `port/` must mount the whole project.
Make paths containing whitespace or Make metacharacters are rejected with a message.

Build with the game's normal `build.sh`, then compare its regression scenarios with
the previous engine. The tool leaves game source, converters, assets and saves
alone. It adds the loader dependencies, but the game must actually call the loader:

```c
/* gfx already initialized; between displayed frames */
vcpe_loading_begin(bytes_to_read); /* 0 if unknown */
int step = vcpe_step_begin("Preparing level");
/* Read content; PAK reads report bytes themselves. */
vcpe_step_end(step, 1);
vcpe_loading_end();
```

Use that around boot loading and level/area switches. Call `vcpe_loading_tick()` in
long CPU-only work. The default screen shows the supplied GIF at its original
aspect ratio, the release credit at bottom right, and the latest eight log rows.
`vcpe_loading_log_lines(n)` changes the visible count (1..24); history retains 24.
`vcpe_loading_show_after(0)` forces it visible for testing. Existing custom draw
hooks remain supported; they can call `vcpe_loading_draw_standard()`.

Automatic adaptation remains opt-in (`vcpe_adapt_enable(1)`); batching, pipelining,
and lazy texture flushing keep their prior opt-in behavior. No gameplay/RNG changes
are silently enabled by the upgrade tool. Native savedata is also an explicit game
integration; [the savedata guide](savedata.md) includes the import call.

Inspect or undo the latest upgrade:

```sh
python3 /path/to/vcpe/tools/vcpe_upgrade.py status /path/to/demake
python3 /path/to/vcpe/tools/vcpe_upgrade.py rollback /path/to/demake
```

Backups live in `.vcpe/backups/`. Rollback restores the exact previous Makefile and
previous pin, retaining snapshots. It preserves compiled objects in the backup so
the next build recompiles with restored headers. If the Makefile has been edited
since the upgrade, rollback stops rather than overwriting those edits: review and
merge the backed-up Makefile manually. Applying the same engine again is a no-op;
modified pinned files are detected instead of silently reused.

Release behavior changes to review: the standard loader is intentionally different
visually; native utility calls invalidate volatile data, as described in savedata.md.
The original local `vcpe_save` API and file format remain compatible.
