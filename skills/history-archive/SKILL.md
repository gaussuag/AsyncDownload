---
name: history-archive
description: Append concise, structured per-thread change records to .agents/history/{thread_key}.json. Use when finishing code changes, documentation updates, configuration edits, policy or AGENTS rule changes, or any task that should leave a durable thread-specific archive record in the workspace.
---

# History Archive

Use this skill to persist a concise, thread-specific change log after a task reaches a clear outcome.

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
   Keep `reasoning` to one or two sentences describing the main implementation decision.
   Record each touched file once with `created`, `modified`, or `deleted`.
   Record `verification` as a compact summary such as `build pass`, `tests pass`, or `manual check`.
   Set `status` to `success`, `partial`, or `failed`.

4. Write the archive entry with the bundled script.
   Run `scripts/upsert_history.py` from the repository root or pass `--base-dir`.
   The script creates `.agents/history/{thread_key}.json` when missing and appends the new record under the current `YYYY-MM-DD` key.
   If the script reports invalid JSON, stop and tell the user instead of overwriting the file.

5. Report the result at the end of the reply.
   When the write succeeds, end the reply with this status card:

---
代码改动已存档
存档文件：`.agents/history/{thread_key}.json`
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
  --reasoning "Load DownloadOptions from JSON so repeat runs can reuse a checked config file." \
  --change src/main.cpp:modified \
  --change tests/download/download_resume_integration_test.cpp:modified \
  --verification "scripts\\build.bat pass; AsyncDownload_tests.exe pass" \
  --status success
```

The script prints a small JSON object to stdout with the archive path, date, time, and changed files so the calling agent can build the final status card without re-reading the archive file.
