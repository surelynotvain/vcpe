# AI policy

VCPE is developed with AI assistance. This page says how, what people remain responsible for, how the result is
checked, and what that means for you as a user or contributor.

## 1. Disclosure
AI coding assistants were used throughout VCPE's development, from version 1.0 to the current one, and are still
used. A large share of the engine's code, converter tools, tests and documentation was first written by an AI
system and then reviewed, tested and accepted by the maintainer. No attempt is made to mark individual lines as
human- or AI-written; assume any part of the engine may have been.

## 2. How AI is used
| Activity | Examples |
|---|---|
| Writing code | Engine modules, converter tools, the self-test, host tests |
| Reviewing code | Line-by-line reviews of modules, as in the 4.0 review recorded in `docs/upgrading-to-4.0.md`, where every point was checked against the code and several were rejected as wrong |
| Analysis | Reading GE command streams and disassembly, measuring memory use per module, finding regressions |
| Documentation | Headers, guides, the changelog |

## 3. What people do
- **Direction.** The maintainer decides what VCPE is for, what gets built and in what order (see the roadmap).
- **Acceptance.** Nothing enters the engine without the maintainer accepting it. AI output is a proposal.
- **Hardware.** Tests on real PSP consoles are done by people.
- **Releases and authorship.** Commits, pull requests and releases are made by people. AI systems are not listed
  as authors, co-authors or contributors, and hold no rights in the engine.
- **Responsibility.** The maintainer answers for what is released, whoever or whatever wrote it.

## 4. Verification gates
AI-written code is held to the same gates as any other change, and they are deliberately strict:

1. **Frame-exact recordings.** Games built on the engine are driven by autotest scripts in an emulator with a
   fixed time step and seed, and every frame is compared byte for byte with the previous engine version. A change
   that alters a frame is either explained and accepted, or it is made opt-in.
2. **Self-test.** One EBOOT exercises every built-in module and reports PASS/FAIL per check.
3. **Host tests.** Pure-C and Python tests for code that does not need a PSP (mixer equivalence, math accuracy,
   save recovery, input, converters).
4. **Hardware.** Behaviour an emulator does not model (the GE texture cache, audio timing, sleep and resume, real
   memory limits) is checked on a console before it becomes a default. Until then it stays opt-in and is marked
   as unverified in the changelog.
5. **Claims are verified, not trusted.** Statements an AI makes about the hardware, the SDK or the code are
   checked against the source, the disassembly or a measurement before they are acted on.

## 5. Known limits
- Passing every gate shows that a change behaves as tested, not that it is correct everywhere. AI-written code can
  be wrong in rarely used paths that no recording or test reaches.
- Emulator-based gates inherit the emulator's blind spots, which is why item 4 exists.
- If you find a defect, please report it with the engine version, the PSP model and a way to reproduce it. These
  reports are the most valuable contribution there is.

## 6. Provenance
- The engine contains no game data and no code copied from any game. Games that use VCPE supply their own
  content.
- The two pictures in `engineassets/` are third-party images (see `LICENSE`, section 9).
- AI assistants are asked to write original code. If you recognise material in VCPE that appears to be copied
  from somewhere else, open an issue and it will be checked and removed if needed.

## 7. Contributions written with AI help
They are welcome, under the same rules as the maintainer's own AI use:
- **Disclose it** in the pull request: which parts, and roughly how (generated, rewritten, reviewed).
- **Understand it.** You must be able to explain every line you submit and answer review questions yourself.
- **Test it** against the gates above that you can run, and say which ones you ran.
- **Own it.** You take the same responsibility for it as for code you typed (`LICENSE`, section 5.2).
- **Do not list an AI as an author or co-author** in commits or pull requests.
- Do not submit AI output you have not read, bulk reformatting, or speculative rewrites without measurements.

## 8. Changes to this policy
This policy may change as tools and practice change. Changes are made by pull request and noted in the changelog.
