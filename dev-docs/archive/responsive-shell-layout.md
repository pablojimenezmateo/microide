# Responsive Shell Layout

> **Status: archived (2026-05-23).** Shipped. The durable contract lives in
> [`openspec/specs/responsive-shell-layout/spec.md`](../../openspec/specs/responsive-shell-layout/spec.md).
> Shell layout guidance: [`guidelines/ui-shell.md`](../../guidelines/ui-shell.md).
> This file is kept as a quick ASCII layout reference only.

## Regular

```text
+ Menu: File Edit Selection View Go Run Git Search Terminal Preferences Help ------+
+ Project tabs ------------------------------------------------------------------+
+ File tabs ---------------------------------------------------------------------+
+ Sidebar ----------------+ Breadcrumb -----------------------------------------+
| Project tree / tools    | Editor / compare / merge surface                    |
|                         |                                                       |
+ Bottom panel tabs -------------------------------------------------------------+
+ Status: project branch indent                    Ln/Col LSP mode regular ------+
```

## Compact

```text
+ [menu] ------------------------------------------------------------ window -----+
+ Project tabs ------------------------------------------------------------------+
+ File tabs ---------------------------------------------------------------------+
+ Sidebar icons ---------+ Breadcrumb ------------------------------------------+
| Tools                  | Editor / compare / merge surface                    |
|                        |                                                       |
+ Bottom panel tabs + -----------------------------------------------------------+
+ Status: project branch                         Ln/Col LSP mode ----------------+
```

Compact mode keeps every top-level menu reachable through the overflow menu and drops
nonessential status-bar segments before durable action surfaces.
