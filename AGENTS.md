# Development Rules

This repository follows the architecture and implementation contracts under `docs/`.

## Architecture authority

Treat the approved architecture/specification documents as authoritative for all production implementation.

Do not change frozen protocol IDs, state semantics, scheduling semantics, ownership rules, failure semantics, log-ordering semantics, persistence ordering, or other architecture contracts merely for implementation convenience.

If implementation exposes a genuine contradiction or missing architectural decision, do not silently resolve it in production code. Stop the affected work, document the issue, and report it for architecture review.

## Milestone discipline

Follow `docs/IMPLEMENTATION_PLAN.md` milestone by milestone.

Work on only the currently approved Milestone.

Do not begin work belonging to the next Milestone until the user explicitly approves progression to it.

Within the current Milestone:

- implement only work required by that Milestone and necessary supporting code;
- run all acceptance tests required by that Milestone;
- fix failures before declaring the Milestone complete;
- review the implementation against the relevant architecture documents;
- do not weaken tests or architecture constraints merely to make validation pass.

## Milestone completion

A Milestone is complete only when its documented acceptance criteria are satisfied with concrete evidence.

At completion, stop implementation and report:

- files added, modified, or removed;
- functionality implemented;
- tests and validation commands executed;
- their results;
- architecture/specification documents checked;
- any deviations, compromises, assumptions, or unresolved issues;
- whether every acceptance criterion for the current Milestone is satisfied.

Do not start the next Milestone after this report. Wait for explicit user approval.