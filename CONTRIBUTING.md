# Contributing to VCPE

Human contributions are more than welcome. This page explains what helps most and how to submit it, including the
engine changes the [license](LICENSE) asks you to send back.

## What helps most
1. **Hardware results.** Run the self-test or your game on a real console and report the model (PSP-1000, 2000,
   3000, Street, Go), the firmware, and what you saw.
2. **Bug reports** with the engine version, the PSP model or emulator version, and the shortest way to reproduce.
3. **Fixes and optimizations**, with measurements (frame time, `vcpe_perf_line`, `vcpe_mem_report`).
4. **Documentation** of modules and data formats.
5. **Reviews** of existing code: questions and doubts are useful even without a fix.

## Sending engine changes back (license section 4.1)
If you ship something built with a modified VCPE, open a pull request with those modifications within 30 days.
- Base it on the release you modified, and say which one.
- Include every changed engine file and any build flags the change needs.
- Describe what it changes and how you tested it.
- It doesn't have to be polished. A change that can't be merged as it is still meets the license and may be
  reworked by the maintainer.
- If a pull request is not possible for you, attach a patch to an issue.

## Pull request checklist
- [ ] One logical change per pull request.
- [ ] Code style matches the surrounding code (C99, 4-space indent, `vcpe_` prefix, comments that explain why).
- [ ] New behaviour that could change what a game draws or hears is **opt-in**, and documented in the module's
      header.
- [ ] Memory cost stated for new static buffers (the PSP-1000 has about 20 MB for a game).
- [ ] Tested: which builds, which emulator or console, which tests.
- [ ] `CHANGELOG.md` entry under the next version.
- [ ] AI assistance disclosed as described in [AI_POLICY.md](AI_POLICY.md), section 7. No AI listed as author or
      co-author.
- [ ] No game code, game data, decompiled code or assets you can't license (license section 5.2).

## Rules of the engine
- Games built on an earlier version must still build and draw the same frames unless the change says otherwise.
- Avoid heap allocation and blocking file I/O inside a frame. Where the engine has to (a sprite page that is not
  cached yet), it is bounded, counted in the stats, and can be moved to a loading screen.
- The GE may still be reading memory from the previous frame (pipelining): free or change textures and vertex
  data only after `vcpe_gfx_gpu_wait()`.
- Raw `sceGu*` texture binds are followed by `vcpe_unbind()`; raw drawing with batching on is preceded by
  `vcpe_gfx_flush()`.
