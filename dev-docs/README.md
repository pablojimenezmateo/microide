# Developer documentation

Internal documentation for building, profiling, and maintaining microide lives here. The
top-level [`docs/`](../docs/) directory is reserved for the public [GitHub Pages](https://pablojimenezmateo.github.io/microide/) site.

## Layout

| Path | Contents |
|------|----------|
| [`project/`](project/) | Active work, implementation guide, tech debt, editor essentials, release notes |
| [`performance/`](performance/) | Perf harness, tracing, profiling, findings; [`investigations/`](performance/investigations/) for historical deep dives |
| [`platform/`](platform/) | Linux build, host bring-up, platform audit |
| [`plugins/`](plugins/) | Plugin runtime research |
| [`design/`](design/) | Active design contracts (e.g. text-surface unification) |
| [`archive/`](archive/) | Shipped plans, superseded notes, out-of-scope decisions |

## Policy and handbook

- Repo policy: [`AGENTS.md`](../AGENTS.md)
- Implementation handbook: [`guidelines/`](../guidelines/)
- Plugin trust model: [`guidelines/plugin-trust-model.md`](../guidelines/plugin-trust-model.md)
- Product specs: [`openspec/specs/`](../openspec/specs/)

## Source-of-truth order

When guidance conflicts:

1. `AGENTS.md`
2. `openspec/specs/`
3. `dev-docs/project/active-work.md`
4. `dev-docs/project/implementation-guide.md`
5. Focused docs under `dev-docs/`
6. `guidelines/`
