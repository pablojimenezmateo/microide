# Responsive Shell Layout

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
