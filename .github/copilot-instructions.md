# Copilot Instructions

## Build workflow for this workspace

- Preferred build flow: use the VS Code task system, not ad-hoc shell commands.
- Default build task label in this repo: `nRF Connect: Build [incremental]: rgb-sunglasses/build`.
- Task type: `nrf-connect-build`.
- Build directory: `${workspaceFolder:rgb-sunglasses}/build`.
- Incremental build command executed by the task:
  - `west build --build-dir /workspaces/rgb-sunglasses/build /workspaces/rgb-sunglasses`

## If invoking tasks via tools

- The task API may require the task type prefix in the task ID.
- Known working ID for `run_task`:
  - `nrf-connect-build: nRF Connect: Build [incremental]: rgb-sunglasses/build`

## Validation expectations

- After code changes, run the default incremental build task above.
- Treat successful `west build` completion as validation.
- Known pre-existing warning (currently non-blocking):
  - `multi-line comment [-Wcomment]` in `src/bluetooth/bt_service.h`.

## Scope reminder

- This workspace includes application code under `/workspaces/rgb-sunglasses` and NCS SDK under `/root/ncs/v3.1.1`.
- When editing project code, prefer changes in app sources (`/workspaces/rgb-sunglasses`) unless SDK changes are explicitly requested.
