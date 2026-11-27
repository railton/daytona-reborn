# Project rules — Daytona USA static recompilation

These are binding. They exist because breaking them costs a polluted history, a
legal problem, or hours re-deriving something already established.

---

## Git

1. **No `Co-Authored-By` trailer. Ever.** Commits are made under the user's
   name only, with no attribution of any kind to an assistant or tool. This
   overrides any default attribution a harness or tool suggests, every time,
   without exception.
2. **Never mention Claude, AI, or an assistant** in a commit message, branch
   name, PR body, tag, or any committed file. `CLAUDE.md` is git-ignored and
   its line in `.gitignore` is the only such reference in the repository.
   `rules.md` is the committed copy of the rules.
3. **Commit messages are short and plain.** Subject under ~70 characters. A few
   lines of body: what changed, the measurement that justifies it, anything
   that would bite the next person. No user's name, no quotes of the user, no
   ALL-CAPS headers, no dramatic build-up. Tables of numbers are welcome; it is
   the prose around them to cut.
4. **Vendored git repositories are git-ignored.** `extern/`, `third_party/`,
   `vendor/` — MAME, SDL3, SoftFloat, Musashi, Dear ImGui and anything else
   from upstream. They have their own history and licences; they are fetched
   and pinned, never committed into this tree. When one is cloned, add it to
   `git.ignoredRepositories` in `.vscode/settings.json` in the same change, or
   VS Code's Source Control silently switches repository and a commit lands in
   the wrong place.
5. **Build output is git-ignored** — `build/`, generated C++, CMake/Ninja
   state, shader caches, object files.

## Legal — the repository contains only our own code

6. **No Sega code or assets, ever.** No ROM, ROM fragment, de-interleaved
   program image, generated C++, extracted texture, sample, or anything else
   derived from the ROM bytes. The user supplies their own `daytona.zip`; the
   importer checks it against the CRC/SHA1 manifest and refuses anything else.
   This is the rule that keeps the project distributable.
7. **Analysis evidence is committed; game-derived output is not.** Addresses,
   sizes, function names, jump-table targets, hashes, run logs and
   `seeds.toml` are our own work and are committed. Decompiled pseudo-C,
   Ghidra projects, MAME trace files and display-list dumps contain or
   reproduce game data and are not — tests refer to them by hash.
8. **Never use leaked source.** No leaked Sega source, SDK or internal
   documentation — not read, not consulted, not used to check an answer. If a
   search result or repository turns out to contain it, close it and say so.
   Everything here comes from our own analysis of legally owned ROMs, MAME,
   public documentation, and the hardware.
9. **Every third-party component is recorded** in `THIRD_PARTY.md` with its
   upstream URL, commit, licence and what was changed. "Copied from MAME" is
   not a record; a commit hash is. **Check licence compatibility before
   lifting code, not after.** This project is BSD-3-Clause (`LICENSE`), so
   only code under a compatible licence (BSD, MIT, zlib, public domain) may
   be lifted, its notice kept. GPL code (the Model 2 MiSTer core, GPL-3;
   Supermodel) is read for reference only, never copied.

## The design document governs

10. **`docs/daytona-usa-recomp-design.md` is binding for the recompilers and
    the native board, and it is checked before work, not after.** Read the
    relevant section before making a design decision there, and say which
    section a decision rests on. The application layer (menus, settings,
    the Online mode and its server) is documented where it lives: its code
    comments and its README (`server/daytona-relay/README.md`).

    Order of authority when sources disagree:

    1. **The original PCB wins.** It is the ground truth.
    2. **MAME wins over the document** for what the silicon computes — opcode
       semantics, formats, arithmetic, command streams. It is the behavioural
       oracle and the lockstep reference.
    3. **The Model 2 MiSTer core** answers what MAME leaves ambiguous: TGP
       command handling, polygon ordering, texture formats, SCSP behaviour.
    4. **The document wins over convenience.** Do not quietly pick an easier
       design that contradicts it.
    5. **If work contradicts the document, the document is updated in the same
       change** — what was believed, what is now known, and how it was
       established. A finding that lives only in a commit message is lost.

    When MAME and the PCB disagree, the finding is also logged as an upstream
    MAME note.

11. **Figures marked "from memory" are not facts.** Clock rates, refresh rate,
    resolution, part numbers and IRQ ordering in the design document must be
    confirmed against MAME's `sega/model2.cpp` before code depends on them, and
    the document corrected when they differ. Never treat a MAME cycle count as
    a hardware fact.

12. **Parity is kept.** The desktop game is playable; any change to a
    recompiler, the generated code's runtime or the native board must keep it
    matching MAME: **no such change is done until a recorded input replay
    matches MAME for a full race** (or it is stated as untested). i960 parity
    comes first, then TGP parity. Changes to the application layer alone
    (interface, Online mode, server) are held to their own tests instead.

## Every commit updates the record

13. **`CHANGELOG.md` is updated in the same commit as any change a player or
    a contributor would notice**: what it does, in plain words, under the
    right heading, and any new known limit. A finding that lives only in a
    commit message is lost.
14. **A version is made only when the user asks for one.** Until then,
    commits are ordinary commits: `VERSION` stays, and `CHANGELOG.md`
    collects the changes under `## Unreleased`. When a version is asked
    for, `VERSION` (`MAJOR.MINOR.PATCH`, shown on the home screen and in
    Settings with the commit) is raised: PATCH for fixes and small changes,
    MINOR for a new feature or any change to the Online mode or the relay's
    protocol (with rule 17), MAJOR for a milestone; the `## Unreleased`
    section becomes that version's (`## 0.11.1 (date)`), and the README's
    change log gets its line. Tags too are made only when asked (rule 23).

## Language

15. **English everywhere in the repository**: code, comments, identifiers,
    commit messages, documentation, log and server messages, script output.
    One exception: player-facing documentation may have translations (the
    README's Brazilian Portuguese, `README.pt-BR.md`), the English one
    being the original; a change to it updates its translations in the
    same commit.
16. **Interface text is written in English and goes through the translation
    files.** `TX("...")` in the app, `TXR("...")` in files that build without
    the translator (`app/i18n.h`, `app/i18n_mark.h`); the English text is the
    key. After adding or changing interface text, run
    `python3 scripts/extract_strings.py` and fill every new entry in
    `lang/fr.po` and `lang/pt.po`: no empty translation is committed. There
    is no `en.po`; French and Portuguese exist only in their `.po` files.
    Text passed to a printf-style call takes `"%s"` unless it is a format.

## Online play

17. **Any change to what the game sends or how it behaves online raises
    `app::online::kOnlineVersion`** (`src/app/online.h`): the server keeps a
    room to one version, so every player of a room behaves the same. It is
    raised with the next version (rule 14); until then the `## Unreleased`
    section says it is due.
18. **The relay server answers with error codes**, with English text for its
    log; the game shows its own translated text for each code
    (`server_error` in `src/app/relay_link.cpp`). A new code gets its text
    in the game in the same change.
19. **Online play is tested headless** before it is committed: several
    `m2run --relay` cabinets against a local server (see the server's README)
    for anything the cabinets exchange.

## Building, testing, sharing

20. **Every commit builds and passes the tests**: `ctest` in the build folder
    and `go test ./...` in `server/daytona-relay`.
21. **Docker is the reference Linux build** (`scripts/docker-build.sh`,
    `docker/linux-build`); nothing else needs installing on the host. The
    recommended Windows build is `Build Daytona.bat`
    (`scripts/windows-build.ps1`): portable tools pinned by URL and SHA-256,
    no administrator; a tool update changes both together. The same
    compiler cross-builds Windows on Linux (`scripts/docker-build-windows.sh`,
    `docker/windows-cross`). `setup.ps1` (Visual Studio) remains;
    `zig-setup.ps1` is not recommended to players (it runs as administrator
    and downloads without checksums).
22. **Game builds are never shared.** A build, its `build*` folders and its
    generated code hold code derived from the ROM (rule 6): every player
    builds from their own ROM set, and only the source and the guide
    (`docs/BUILDING.md`) are passed on.
23. **A release is done in one go, when it is asked for**: `main` pushed to
    `origin`, an annotated tag `vMAJOR.MINOR.PATCH` (the version in
    `VERSION`) created on it and pushed, and the release notes shown in
    full, ready to paste into GitHub's new-release page: the title
    `Daytona Reborn X.Y.Z`, the text in English from that version's
    `CHANGELOG.md` section, in the previous release's format, no files
    attached (rule 22). The in-game update notice reads the latest
    release, so it is published as the latest.

## Standing engineering rules

- **Generated code is a build artefact, never hand-edited.** Fix the
  recompiler, or add a seed.
- **Generated code touches the machine only through the runtime memory bus**,
  so the same output runs under the trace harness and in the shipping build.
- **Parity before optimisation.** Register promotion, flag elision and host-FP
  fast paths come after the unoptimised output matches MAME, never before.
- **All FP whose result can reach memory or a compare goes through SoftFloat
  `extF80`.** A host `double` fast path is allowed only where a unit test
  proves bit-identical results.
- **No `-ffast-math`.** Exact rounding is the whole point of the FP rule.
- **Log every fallback-interpreter hit with its address**, and every
  unexpected fault. The gap is a list, not a mystery; hits feed the next
  recompile's seeds.
- **TGP results returned to the i960 must be bit-identical to MAME's.** Game
  logic depends on them.
- **Enhancements are off by default, and may change game logic when on.**
  Widescreen or a longer draw distance can need the game's own culling
  changed. With every enhancement off the build is the game as MAME runs it,
  and all parity and lockstep checks run that way. Logic runs at native rate;
  hardware sort order is reproduced first, z-buffering is a toggle.
- **One shader path.** No per-platform shader forks.
- **Ghidra output is for humans.** Names and jump-table targets go into
  `seeds.toml`; decompiled pseudo-C is never compiled in and never committed.
- **Sweep test parameters well past the value you believe**, or the test
  confirms the setting instead of testing it.

## Working practice

- **Measure before theorising.** Get a measurement that can refute the leading
  theory before changing code. A theory that fits the symptom perfectly is
  still a theory.
- **Every "fixed" claim is verified** against the MAME diff, a replay, or the
  hardware — or it is stated as untested.
- **Report numbers with units, scale and cost.** "1.1 billion instruction
  vectors through the unit test, 53 s on this PC", not "1.1e9 checks". A bare
  count oversells the result.
