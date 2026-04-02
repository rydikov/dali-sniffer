# AGENTS

## Frontend Source Of Truth

- `webui/` contains the source frontend files that should be analyzed and edited.
- `main/web_dist/` contains compiled frontend assets produced from `webui/`.
- Do not analyze `main/web_dist/` as the primary implementation source when working on UI tasks.
- Do not make manual edits in `main/web_dist/`.
- If a frontend change is needed, update files in `webui/` and rebuild the frontend so generated assets in `main/web_dist/` are refreshed by the build process.
