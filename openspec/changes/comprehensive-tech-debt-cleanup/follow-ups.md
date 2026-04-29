## Scheduled Follow-Ups

### Release +2 cleanup: `legacy-persistence-cleanup`

- Scope: remove one-shot `.legacy` file cleanup code and any now-stale migration scaffolding.
- Timing: target the release after next, not this change.
- Guardrail: do not delete existing `.legacy` files in `comprehensive-tech-debt-cleanup`.
