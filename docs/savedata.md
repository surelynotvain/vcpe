# PSP Saved Data Utility

Link `vcpe_savedata vcpe_save vcpe_mem vcpe_gfx vcpe_sys` (and the normal graphics
dependencies) with `-lpsputility`. The easy upgrade tool does this with `--savedata`.
Call from the main thread, **between frames**, after graphics initialization:

```c
static const VcpeSavedata slot = {
    .game_id = "UCJS19051", /* use your game's unique DISC_ID, not this example */
    .slot = "SLOT00",
    .title = "My Game",
    .save_title = "Slot 1",
    .detail = "Chapter 2 - village",
    .icon0 = NULL,         /* extracts the game's embedded ICON0 from EBOOT.PBP */
    .dialog = 1           /* firmware confirmation; 0 for autosave/autoload */
};
int result = vcpe_savedata_write(&slot, &state, sizeof state, SAVE_FORMAT_VERSION);
/* Commit the game's live state only after a successful read. */
uint32_t loaded_size;
result = vcpe_savedata_read(&slot, &state, sizeof state, &loaded_size, SAVE_FORMAT_VERSION);
```

The native utility creates `ms0:/PSP/SAVEDATA/<game_id><slot>/` with `PARAM.SFO`,
`DATA.BIN`, and `ICON0.PNG` when an icon is available. Titles, slot title, details,
and icon are available to the XMB **Saved Data Utility**, including copy/delete and
the normal in-game save/load dialogs. Each call targets the specified slot; games
with several slots choose their suffix themselves. PSP Go storage uses the
firmware's savedata destination. Keep the ID, slot and format key stable across
game/engine upgrades. The game ID must be nine uppercase letters/digits/underscores;
the slot is 1..19 of the same characters. Use unique IDs across games on a card.

Supply a relative PNG path in `icon0` to override the EBOOT icon. The game controls
the icon's PSP-valid size/format (normally 144x80 PNG); malformed or oversized PBP
sections are ignored. An absent icon does not prevent a save, but provide one for
the intended XMB presentation.

Operations block while the firmware runs its UI/I/O. Exit/power callbacks continue
to run. The engine renders the utility before swapping buffers, including when
normal rendering uses pipelining. Return 0 means success; negative values include
firmware cancellation/errors and the named `VCPE_SAVEDATA_*` engine errors. Show the
failure to the player and keep their current state. The checked envelope carries
the game's save version, payload size, and CRC-32. Reads leave the destination and
size untouched on any failure. The module bounds payloads to 16 MiB and allocates
one payload-sized temporary. Firmware encryption uses a fixed format key documented
in the source; this is compatibility with the native utility, not a secret.

The firmware needs the 4 MiB volatile partition. VCPE yields it before utility
initialization and reclaims it after shutdown, bumping `vcpe_resume_gen`. Engine
texture bundles marked for resume reload are reloaded at the next frame. Game-owned
volatile data must be rebuilt when the generation changes, just as after sleep.
Ensure worker jobs aren't using that partition during the call. If reclaiming fails,
the engine shows a fatal error instead of continuing with invalid assets.

Import a legacy engine save explicitly:

```c
result = vcpe_savedata_import(&slot, "SAVE.DAT", sizeof state, SAVE_FORMAT_VERSION);
```

This reads the newest valid `vcpe_save` / `vcpe_save_write_var` slot, then writes
native savedata. It preserves both legacy files. Only offer/import when the game
decides that migration is needed; don't overwrite an existing native save every
boot. Raw custom `fwrite` files need a game-specific parser. Existing games using
`vcpe_save` keep their local two-slot behavior until they integrate this module.
Native utility saves are a separate backend; the local two-slot durability promise
does not apply to the firmware's single `DATA.BIN`.

The Blank Engine SAVES page is a standalone integration example. Emulator checks
verify utility calls, metadata, encrypted round trips, and imports. Real PSP XMB
display, firmware cancellation, pulled media, and sleep during saving still need
physical-console testing. No hardware/card was accessed for the 5.0 work.

API reference: [PSPSDK savedata definitions](https://github.com/pspdev/pspsdk/blob/master/src/utility/psputility_savedata.h).
