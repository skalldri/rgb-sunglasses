# Copilot Instructions

## Build workflow for this workspace

- Preferred build flow: use `west build --build-dir /workspaces/rgb-sunglasses/build /workspaces/rgb-sunglasses`.

## Validation expectations

- After code changes, run the default incremental build task above.
- Treat successful `west build` completion as validation.
- After code changes, run all tests with Twister:
  - `twister -T /workspaces/rgb-sunglasses/tests -p native_sim`
- Known pre-existing warning (currently non-blocking):
  - `multi-line comment [-Wcomment]` in `src/bluetooth/bt_service.h`.

## Unit test policy for new features

- All new features should include unit tests going forward, preferably as ztest suites under `tests/`.
- PRs are incomplete without corresponding tests unless explicitly agreed.

## Scope reminder

- This workspace includes application code under `/workspaces/rgb-sunglasses` and NCS SDK under `/root/ncs/v3.1.1`.
- When editing project code, prefer changes in app sources (`/workspaces/rgb-sunglasses`) unless SDK changes are explicitly requested.

## Coding reminder

- Don't repeat yourself (DRY): if you find yourself writing the same code more than once, consider refactoring to reuse code.