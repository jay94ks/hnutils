# asyncpp reference guidance

- Treat `repo` as read-only upstream source.
- Do not commit source edits inside the submodule.
- Store required source changes as numbered patch files in `patches`, for
  example `0001-short-description.patch`.
- Record each patch's purpose, application order, and expected upstream commit
  in this directory's `README.md`.
- Put integration and usage notes in `docs`.
- When updating the submodule revision, verify all recorded patches still
  apply and update the documentation.
