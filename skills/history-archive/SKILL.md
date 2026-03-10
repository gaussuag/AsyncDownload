---
name: history-archive
description: Append structured per-thread archive records under .agents/history/{thread_key}/ with an index.json plus detailed session markdown files. Use when finishing code changes, documentation updates, configuration edits, policy or AGENTS rule changes, or any task that should preserve enough context to resume the thread later.
---

# History Archive

Use this skill to persist a concise index record and a fuller session archive after a task reaches a clear outcome.

## Workflow

1. Resolve `thread_key`.
   Use the value the user already provided in the current thread.
   If no `thread_key` is available, ask once before writing anything.
   Do not invent a key and do not read or write another thread's archive unless the user explicitly asks.

2. Decide whether to archive.
   Archive after code, docs, config, or rule changes, and after any task closeout that produced a concrete result.
   Skip automatic archiving when no files changed and there is no meaningful outcome to preserve.

3. Build one concise record for the current batch of work.
   Keep `purpose` short and user-facing.
   Keep `request_snapshot` close to the user's task in one sentence.
   Keep `summary` focused on what was completed and what information was added or changed.
   Record the source files that informed the work in `sources`.
   Record each touched file once with `created`, `modified`, or `deleted`.
   Record `verification` as a compact summary such as `build pass`, `tests pass`, or `manual check`.
   Record `next_step` so a later thread can resume from the right place.
   Set `status` to `success`, `partial`, or `failed`.

4. Write both archive layers with the bundled script.
   Run `scripts/upsert_history.py` from the repository root or pass `--base-dir`.
   The script writes:
   - `.agents/history/{thread_key}/index.json`
   - `.agents/history/{thread_key}/sessions/{timestamp}.md`
   The markdown file is the detailed session archive.
   The JSON file is the searchable index and stores the markdown path in `session_archive`.
   If an old `.agents/history/{thread_key}.json` exists, the script migrates it into the new layout.
   If the script reports invalid JSON, stop and tell the user instead of overwriting the file.

5. Report the result at the end of the reply.
   When the write succeeds, end the reply with this status card:

---
代码改动已存档
索引文件：`.agents/history/{thread_key}/index.json`
详细存档：`.agents/history/{thread_key}/sessions/{timestamp}.md`
本次记录：`{YYYY-MM-DD} {HH:mm:ss} - {purpose}`
改动文件：`{file1}, {file2}, ...`
存储结果：`success`
---

   If the write fails, say that the archive failed and include the reason. Do not claim success.

## Script

Use `scripts/upsert_history.py` to perform deterministic writes.

Example:

```bash
python skills/history-archive/scripts/upsert_history.py \
  --thread-key asyncdownload-cli-config-20260310-a \
  --purpose "Add CLI config loading" \
  --request-snapshot "Load download options from a JSON config file and reflect the behavior in CLI output and tests." \
  --summary "Add --config loading, extend summary output, and cover the flow with an integration test." \
  --source src/main.cpp \
  --source include/asyncdownload/types.hpp \
  --source tests/download/download_resume_integration_test.cpp \
  --change src/main.cpp:modified \
  --change tests/download/download_resume_integration_test.cpp:modified \
  --verification "scripts\\build.bat pass; AsyncDownload_tests.exe pass" \
  --next-step "No immediate follow-up required." \
  --status success
```

The script prints a small JSON object to stdout with the index path, detailed session archive path, date, time, and changed files so the calling agent can build the final status card without re-reading the archive files.
