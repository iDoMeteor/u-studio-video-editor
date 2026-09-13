# u Studio Video Editor — v2 Planning Docs

**Status:** proposal, 2026-09-12. Written against the code as it stood in
`src/` on that date (single-track milestone; the team's same-day edits are folded into doc 00) and verified against
the installed toolchain (Fedora 44, GTK 4.22.4, libadwaita 1.9.2, MLT 7.40.0,
GCC 16.1.1, meson 1.11.2).

These docs are a **separate planning set**. They do not modify `README.md`
or anything in `src/`. Nothing here is binding until the team has read it and
either accepted or amended it; see "How to use this set" below.

## Reading order

| # | Doc | Read it if you… |
|---|-----|-----------------|
| 00 | [v1 review](00-v1-review.md) | want to know what was inspected, what holds up, and what breaks under v2 load. Includes verified facts from probes run on this machine. |
| 01 | [Goals, scope, principles](01-goals-scope-principles.md) | need the one-page definition of what v2 is and is not. |
| 02 | [Architecture](02-architecture.md) | are touching module boundaries, threading, or ownership. **Read before writing code.** |
| 03 | [Project model](03-project-model.md) | are working on the timeline data model, ids, time units, invariants. |
| 04 | [Commands and undo](04-commands-and-undo.md) | are implementing any user-visible edit. |
| 05 | [Playback engine](05-playback-engine.md) | are replacing the pull loop with an MLT consumer, or touching audio. |
| 06 | [Timeline UI](06-timeline-ui.md) | are building the multi-track timeline widget. |
| 07 | [Media bin and assets](07-media-bin-and-assets.md) | are working on import, probing, thumbnails, waveforms, proxies. |
| 08 | [Effects and compositing](08-effects-and-compositing.md) | are adding filters, transitions, keyframes, track compositing. |
| 09 | [Persistence and formats](09-persistence-and-formats.md) | are working on save/load, autosave, kdenlive import. |
| 10 | [Export and rendering](10-export-and-rendering.md) | are building the render pipeline. |
| 11 | [Build, test, CI, packaging](11-build-test-ci-packaging.md) | are restructuring meson, adding tests, or packaging. |
| 12 | [Roadmap and milestones](12-roadmap-and-milestones.md) | need to know what ships in what order and how "done" is judged. |
| 13 | [Risks and open questions](13-risks-and-open-questions.md) | are deciding anything the docs leave open. |
| 14 | [Conventions](14-conventions.md) | are writing C++ or GTK code in this repo. |
| — | [ADRs](adr/) | want the short rationale behind each load-bearing decision. |

## How to use this set

- **Disagree in place.** Add a `> REVIEW:` blockquote under the paragraph you
  disagree with, with your name and a sentence. The doc owner resolves it and
  removes the note. Don't fork the doc.
- **ADRs are the contract.** If a code change contradicts an ADR, either the
  ADR gets superseded (new ADR, old one marked "superseded by") or the change
  doesn't land. Everything else in these docs is guidance and can drift.
- **Milestone docs are the schedule.** [12-roadmap-and-milestones.md](12-roadmap-and-milestones.md)
  has acceptance criteria per milestone. A milestone is done when its criteria
  pass, not when its features are "in".
- **Keep the v1 README honest.** When v2 lands a milestone, update
  `README.md` at the repo root; these docs stay as the design record.

## Coordination note for the current team

The current work in `src/` is not blocked by anything here. Milestone M0 in the
roadmap is the only step that moves existing files, and it is designed to be
done in one short, announced window (see
the M0 section of [12-roadmap-and-milestones.md](12-roadmap-and-milestones.md)).
Until M0 lands, keep working in the current layout.
