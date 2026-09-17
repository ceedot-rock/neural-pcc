# Standing push order (owner directive, 2026-09-16)

When all five SickNode phases are complete — final 12-file benchmark verified
(12/12 SHA-256), RESULTS_WORKBENCH.md written, final local commit in — push
branch `workbench` to the `github` remote (private ceedot-rock/neural-pcc)
as the LAST step, then report the push result (commit hash, branch, remote URL)
along with the final numbers.

- ONE push at completion, NOT per phase.
  AMENDED 2026-09-16: TWO authorized pushes total —
  (1) after P1's first full 12-file benchmark lands (numbers verified,
      committed locally), push branch `workbench` to the `github` remote
      and report the hash;
  (2) after all five phases complete, push again and report.
  No other pushes.
- If any phase fails or stalls such that completion is impossible, DO NOT push
  a broken state — report to the parent agent instead.
- Workers: this does not change your constraints. Keep committing locally,
  never push on your own. The coordinator performs the single final push.
