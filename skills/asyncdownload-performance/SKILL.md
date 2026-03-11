---
name: asyncdownload-performance
description: Guide the AsyncDownload performance optimization workflow. Use when taking over benchmark analysis, profiler analysis, choosing the next optimization target, running a full measure-change-verify loop, or updating docs/performance after a performance iteration.
---

# AsyncDownload Performance

Use this skill when working on AsyncDownload performance. The goal is not only to change code, but to preserve a repeatable optimization loop with stable baselines, profiler evidence, and updated project docs.

## What This Skill Owns

- Recover performance context from `docs/performance`
- Confirm the optimization goal before changing code
- Keep one optimization theme per thread
- Update the right performance docs after each completed loop

## Thread Rule

Default to one optimization theme per thread.

Stay in the current thread only while the work still belongs to the same main bottleneck or the same optimization experiment.

Recommend a new thread when either condition is true:
- One optimization loop has closed: baseline understood, code changed, regression rerun, profiler rerun if needed, docs updated
- The main target changes to a different bottleneck

You cannot create a new thread automatically. When a new thread is appropriate, end with a short handoff that includes:
- why the current loop is closed
- the next recommended optimization target
- the docs that the next thread must read first
- a suggested first user prompt for the next thread

## Start Of Every Performance Task

1. Read the entry doc first:
   [performance_playbook_zh.md](D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_playbook_zh.md)

2. Follow the reading order and current official references from the playbook instead of relying on memory.

3. Confirm the optimization goal with the user before changing code.

Do not assume all performance work is about throughput. Confirm which of these is primary:
- higher throughput
- lower memory
- fewer queue-full pauses
- smoother stress-case behavior
- hotspot migration in profiler

If the user does not specify a goal, propose one concise default goal based on the current baseline and ask for confirmation before the first code change.

## Design Guardrail

Performance changes must be evaluated against the component's design philosophy and correctness boundaries, not only against benchmark results.

Before changing code, think through whether the optimization could affect:
- recovery semantics
- persistence guarantees
- queue and backpressure meaning
- range scheduling invariants
- data ordering assumptions
- public or de facto component contracts
- previously established design constraints in docs or code structure

Do not treat a faster benchmark as sufficient proof that the change is acceptable.

If a change may alter component behavior, weaken an existing guarantee, or reinterpret an existing parameter or contract, explicitly surface that risk to the user before committing to the direction.

If you are not confident whether the change stays within the intended design philosophy, pause and ask the user to confirm the tradeoff before implementing it.

## Source Of Truth Rule

Do not duplicate evolving performance conclusions inside this skill.

Treat `docs/performance` as the source of truth for:
- the current official benchmark suite
- the current historical comparison suite
- the current benchmark commands
- the current profiler commands
- the current main comparison cases
- the current hotspot interpretation
- the current formal baseline numbers

Use this skill for process and workflow discipline, not for frozen technical conclusions.

If the playbook or baseline docs conflict with this skill on a project-specific detail, follow `docs/performance` and update the skill later if its process instructions are now stale.

## Benchmark Use

Use the benchmark tooling and suite that the current docs define.

Use benchmark results to answer:
- did throughput improve
- did memory regress
- did queue-full pauses regress
- did the main comparison cases change ranking

## Profiler Use

- Profiler is a behavior baseline, not the official throughput baseline
- Compare hotspot structure, not absolute MB/s

Use profiler results to answer:
- which code path is hot now
- whether the hot path migrated after the last optimization
- whether `gap` is relevant
- whether `queue_full_pause` is still the dominant control path

## Recommended Optimization Loop

1. Recover current baseline and current hotspot story from `docs/performance`
2. Confirm the optimization goal with the user
3. Choose the benchmark cases and profiler cases that match that goal, using the current docs as the authoritative guide
4. Make one focused code change
5. Rebuild and rerun `Release` benchmark
6. Rerun profiler if the change targets a hotspot or changes behavior shape
7. Re-check whether the change preserved the intended design boundaries
8. Decide whether the change is a keeper
9. Update the required docs
10. If the loop is closed, recommend a new thread for the next bottleneck

Prefer one focused optimization per loop. Do not combine unrelated bottlenecks in one batch.

## Doc Update Rules

After every completed performance loop, update docs before treating the loop as closed.

Treat performance documentation as a global system, not as isolated files.

Do not update only the document you were already looking at. Actively judge whether the current change also affects neighboring docs.

Always update:
- [performance_optimization_history_zh.md](D:/git_repository/coding_with_agents/AsyncDownload/docs/performance/performance_optimization_history_zh.md)

Update benchmark baseline docs when:
- the official comparison suite changes
- the official baseline numbers change materially
- the interpretation of the main cases changes

Update profiler baseline docs when:
- a new profiler baseline is intentionally established
- hotspot interpretation materially changes

Update the regression guide when:
- the main suite changes
- the main comparison cases change
- the validation policy changes

Update the playbook when:
- the workflow changes
- the document responsibilities change
- a new long-lived performance doc is added

When updating any performance doc, also ask:
- did this change affect the current official baseline interpretation
- did this change affect regression or profiler workflow guidance
- did this change affect optimization history framing
- did this change affect the playbook's document map or maintenance rules

If the answer may be yes, update the related docs in the same loop instead of leaving them inconsistent.

Do not rely on `build/` directories as the long-term source of truth. Persist the important numbers and conclusions in `docs/performance`.

## Stable Boundaries

This skill should stay stable across optimization iterations.

Keep these categories in the skill:
- how to start a performance task
- when to confirm goals
- when to escalate design-tradeoff uncertainty to the user
- when to recommend a new thread
- what the optimization loop looks like
- which kinds of docs must be updated
- which system is the source of truth

Do not keep these categories in the skill:
- current official suite names
- current official case names
- current baseline numbers
- current hotspot conclusions
- current preferred bottleneck to optimize next

Those belong in `docs/performance`.
