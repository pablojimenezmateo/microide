## 1. Canonical Edit Delta

- [x] 1.1 Make `TextViewport` derive and retain the last applied edit from history-backed mutations, including undo and redo.
- [x] 1.2 Keep the applied-edit contract conservative by falling back cleanly when a precise contiguous delta is unavailable.

## 2. Workspace Fast Path Adoption

- [x] 2.1 Route keyboard, text-input, and editor-owned single-edit workspace paths through `RequestActiveEditableLastChangeRedraw()`.
- [x] 2.2 Preserve compare and merge derived-state updates while removing unnecessary whole-buffer redraw and LSP diff work from their edit entry points.

## 3. Regression Coverage And Verification

- [x] 3.1 Add focused `TextViewport` regression coverage for applied-edit metadata and undo or redo delta behavior.
- [x] 3.2 Run targeted build and editor tests, then capture any remaining known gaps or follow-up risks.
