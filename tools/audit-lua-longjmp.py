#!/usr/bin/env python3
"""AST audit: no Lua longjmp over live C++ locals (tech debt 020/058).

The project links the C build of Lua, so every raise-capable Lua C API call
(`lua_error`, `luaL_error`, entry-validation `luaL_check*` / `luaL_arg*` /
`luaL_opt*`, and uncaught `lua_call`) is a C `longjmp` back to the enclosing
protected call. It does NOT run the destructors of C++ automatic objects still
alive on the stack — undefined behaviour plus a leak whenever a `std::string`,
`std::vector`, `std::filesystem::path`, … local (or by-value parameter, or a
lifetime-extended temporary bound to a reference) is in scope at the raise.

The regex architecture lint (`CheckPluginLuaErrorDoesNotLongjmpOverCppLocals`)
bans the `luaL_error` spelling wholesale but cannot see *scopes*: it can't
verify that the sanctioned idiom (`lua_error_util::PushMessage` inside a block,
`lua_error` after the block closes — see src/plugin/LuaError.h) is actually
applied, nor that the ~30 allowed entry-position `luaL_check*` calls really run
before any non-trivial local is constructed. This script checks exactly that
property on the real clang AST:

  for every raise-capable call in an audited TU:
      collect every VarDecl/ParmVarDecl whose scope encloses the call and whose
      declaration precedes it; flag the call if any such object (or a
      MaterializeTemporaryExpr lifetime-extended into a reference local) has a
      non-trivially-destructible type.

Audited TUs: every `src/plugin/*.cpp` (which pulls in the .inc wrappers and
headers — diagnostics are reported at the .inc/.h line they occur on) plus the
self-contained sandbox `src/editor/SyntaxDefinitionLoader.cpp`.

Requirements (not wired into ctest on purpose — this is an on-demand audit):
  * clang python bindings:  pip install clang==18.*   (pure python, PEP 668
    environments: use a venv)
  * libclang shared library (auto-detected from /usr/lib/llvm-*/lib)
  * build/compile_commands.json:
        cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

Usage:  tools/audit-lua-longjmp.py [--build-dir build] [-v]
Exit codes: 0 clean, 1 violations found, 2 environment/setup problem.
"""

from __future__ import annotations

import argparse
import glob
import json
import re
import shlex
import sys
from pathlib import Path

try:
    import clang.cindex as ci
except ImportError:
    print("error: python clang bindings missing — pip install clang==18.* "
          "(use a venv on PEP 668 systems)", file=sys.stderr)
    sys.exit(2)

# ---------------------------------------------------------------------------
# Raise-capable Lua C API calls.
#
# `lua_error` is the sanctioned raise; the luaL_* family raises on argument
# mismatch (that is its purpose); `lua_call` propagates callee errors without a
# protective frame (the codebase mandates lua_pcall). `luaL_argcheck` is a
# macro over luaL_argerror so the expansion is caught via luaL_argerror.
# Deliberately NOT audited: allocation OOM raises (lua_push*/lua_newtable/...)
# — auditing those would flag every interop line; the host installs no panic
# recovery so OOM aborts rather than unwinding — and the metamethod-capable
# field reads (lua_getfield/gettable/geti), which a separate architecture lint
# forces through the pcall-protected lua_interop::GetFieldProtected.
# ---------------------------------------------------------------------------
RAISING_CALLS = re.compile(
    r"^(lua_error|luaL_error|lua_call|"
    r"luaL_check\w+|luaL_opt\w+|luaL_argerror|luaL_typeerror|luaL_argexpected)$")

# Types the invariant is about, matched on canonical spelling as a fallback for
# when libclang has no instantiated definition to inspect structurally.
KNOWN_NONTRIVIAL = re.compile(
    r"std::(__cxx11::)?(basic_string\b|vector\b|map\b|multimap\b|set\b|multiset\b|"
    r"unordered_\w+|deque\b|list\b|forward_list\b|function\b|unique_ptr\b|"
    r"shared_ptr\b|weak_ptr\b|optional\b|variant\b|any\b|filesystem::path\b|"
    r"basic_[io]?f?stream\b|basic_string_stream\b|basic_ostringstream\b|"
    r"basic_istringstream\b|basic_stringstream\b|promise\b|future\b|thread\b|"
    r"jthread\b|regex\b|basic_regex\b)")

CK = ci.CursorKind
TK = ci.TypeKind


def _register_missing_enum_ids():
    """The pip bindings can lag libclang (e.g. RequiresExpr): pre-register
    placeholder kinds so `.kind` on modern-C++ cursors doesn't raise."""
    for enum in (ci.CursorKind, ci.TypeKind):
        for kind_id in range(1, 1200):
            try:
                enum.from_id(kind_id)
            except ValueError:
                try:
                    enum(kind_id)
                except ValueError:
                    pass


_register_missing_enum_ids()

TRIVIAL_TYPE_KINDS = {
    TK.BOOL, TK.CHAR_U, TK.UCHAR, TK.CHAR16, TK.CHAR32, TK.USHORT, TK.UINT,
    TK.ULONG, TK.ULONGLONG, TK.UINT128, TK.CHAR_S, TK.SCHAR, TK.WCHAR,
    TK.SHORT, TK.INT, TK.LONG, TK.LONGLONG, TK.INT128, TK.FLOAT, TK.DOUBLE,
    TK.LONGDOUBLE, TK.NULLPTR, TK.POINTER, TK.BLOCKPOINTER,
    TK.MEMBERPOINTER, TK.ENUM, TK.FUNCTIONPROTO, TK.FUNCTIONNOPROTO,
    TK.VOID,
}


def is_trivially_destructible(t: ci.Type, memo: dict, depth: int = 0) -> bool:
    """Conservative structural triviality check over the canonical type."""
    t = t.get_canonical()
    key = t.spelling
    if key in memo:
        return memo[key]
    memo[key] = True  # break recursion cycles optimistically
    result = _is_trivially_destructible_impl(t, memo, depth)
    memo[key] = result
    return result


def _is_trivially_destructible_impl(t: ci.Type, memo: dict, depth: int) -> bool:
    kind = t.kind
    if kind in TRIVIAL_TYPE_KINDS:
        return True
    if kind in (TK.LVALUEREFERENCE, TK.RVALUEREFERENCE):
        return True  # references do not destroy their referent
    if kind in (TK.CONSTANTARRAY, TK.INCOMPLETEARRAY, TK.VARIABLEARRAY):
        return is_trivially_destructible(t.element_type, memo, depth + 1)
    if kind == TK.RECORD or kind == TK.ELABORATED:
        if KNOWN_NONTRIVIAL.search(t.spelling):
            return False
        decl = t.get_declaration()
        if decl.kind == CK.NO_DECL_FOUND:
            return False  # cannot see it: assume the worst
        children = list(decl.get_children())
        if not children and decl.kind in (CK.CLASS_DECL, CK.STRUCT_DECL,
                                          CK.CLASS_TEMPLATE):
            # No visible definition/instantiation to inspect: conservative.
            return False
        for c in children:
            if c.kind == CK.DESTRUCTOR:
                # A user-provided or virtual destructor is non-trivial.
                if c.is_virtual_method() or not c.is_default_method():
                    return False
        for c in children:
            if c.kind == CK.CXX_BASE_SPECIFIER:
                if not is_trivially_destructible(c.type, memo, depth + 1):
                    return False
            elif c.kind == CK.FIELD_DECL:
                if not is_trivially_destructible(c.type, memo, depth + 1):
                    return False
        return True
    # Unexposed / dependent / atomic / other exotica: assume the worst.
    return False


def callee_name(call: ci.Cursor) -> str:
    ref = call.referenced
    if ref is not None and ref.spelling:
        return ref.spelling
    return call.spelling or ""


def find_call_paths(body: ci.Cursor):
    """Yield (call_cursor, ancestor_chain) for every raise-capable call.

    Does not descend into lambda bodies: a lambda raises from its own stack
    frame (it is audited as its own function entry), so the enclosing
    function's locals are not what the longjmp unwinds over."""
    chain = []

    def walk(node):
        if node.kind == CK.CALL_EXPR:
            name = callee_name(node)
            if name and RAISING_CALLS.match(name):
                yield node, list(chain)
        if node.kind == CK.LAMBDA_EXPR and node is not body:
            return
        chain.append(node)
        for child in node.get_children():
            yield from walk(child)
        chain.pop()

    yield from walk(body)


def has_nontrivial_materialized_temporary(node: ci.Cursor, memo: dict) -> bool:
    """Reference initializers: lifetime-extended temporaries live in the scope."""
    stack = [node]
    while stack:
        n = stack.pop()
        # MaterializeTemporaryExpr surfaces as UNEXPOSED_EXPR in libclang;
        # approximate: any subexpression producing a prvalue of non-trivial
        # RECORD type (call result / functional cast / temporary object)
        # bound into this reference initializer counts as a live object.
        # (CXXTemporaryObjectExpr surfaces as CALL_EXPR in libclang.)
        if n.kind == CK.CALL_EXPR or n.kind == CK.CXX_FUNCTIONAL_CAST_EXPR:
            if n.kind == CK.CALL_EXPR and n.referenced is not None:
                # A call returning a reference produces no temporary, but
                # libclang strips the reference from the expression type —
                # consult the callee's declared return type instead.
                ret = n.referenced.result_type.get_canonical()
                if ret.kind in (TK.LVALUEREFERENCE, TK.RVALUEREFERENCE):
                    stack.extend(n.get_children())
                    continue
            t = n.type.get_canonical()
            if t.kind == TK.RECORD and not is_trivially_destructible(t, memo):
                return True
        stack.extend(n.get_children())
    return False


def live_locals_at(call: ci.Cursor, chain: list, func: ci.Cursor, memo: dict):
    """Collect (cursor, why) for non-trivially-destructible objects alive at
    the call: enclosing-scope locals declared before it + by-value params +
    lifetime-extended temporaries in enclosing reference locals."""
    call_offset = call.extent.start.offset
    call_file = str(call.location.file) if call.location.file else ""
    findings = []

    # By-value parameters of the enclosing function.
    for p in func.get_children():
        if p.kind == CK.PARM_DECL:
            if not is_trivially_destructible(p.type, memo):
                findings.append((p, "by-value parameter still alive at raise"))

    # DeclStmts in every scope on the ancestor chain, declared before the call.
    for scope in chain:
        if scope.kind not in (CK.COMPOUND_STMT, CK.FOR_STMT, CK.IF_STMT,
                              CK.SWITCH_STMT, CK.WHILE_STMT,
                              CK.CXX_FOR_RANGE_STMT):
            continue
        if scope.kind == CK.CXX_FOR_RANGE_STMT:
            # Range-for over a temporary container: the hidden __range binding
            # lifetime-extends it across the whole loop body.
            for child in scope.get_children():
                if child.kind == CK.COMPOUND_STMT:
                    continue  # the body — handled via the chain
                if has_nontrivial_materialized_temporary(child, memo):
                    findings.append(
                        (scope, "range-for lifetime-extends a non-trivial "
                                "temporary container"))
                    break
        for stmt in scope.get_children():
            if stmt.kind != CK.DECL_STMT:
                continue
            if (str(stmt.location.file) == call_file
                    and stmt.extent.start.offset >= call_offset):
                continue  # declared after the raise: not yet constructed
            for var in stmt.get_children():
                if var.kind != CK.VAR_DECL:
                    continue
                vt = var.type.get_canonical()
                if vt.kind in (TK.LVALUEREFERENCE, TK.RVALUEREFERENCE):
                    if has_nontrivial_materialized_temporary(var, memo):
                        findings.append(
                            (var, "reference local lifetime-extends a "
                                  "non-trivial temporary"))
                    continue
                if not is_trivially_destructible(var.type, memo):
                    findings.append((var, "local still alive at raise"))
    return findings


def load_compile_commands(build_dir: Path):
    path = build_dir / "compile_commands.json"
    if not path.exists():
        print(f"error: {path} not found — configure with "
              "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", file=sys.stderr)
        sys.exit(2)
    entries = {}
    for entry in json.loads(path.read_text()):
        file = str(Path(entry["directory"], entry["file"]).resolve())
        if file not in entries:  # core TUs appear once via the object library
            entries[file] = entry
    return entries


def clang_args(entry) -> list:
    if "arguments" in entry:
        argv = list(entry["arguments"])
    else:
        argv = shlex.split(entry["command"])
    args, skip = [], False
    for a in argv[1:]:  # drop the compiler itself
        if skip:
            skip = False
            continue
        if a in ("-o", "-c"):
            skip = a == "-o"
            continue
        if a == entry["file"] or a.endswith(entry["file"]):
            continue
        args.append(a)
    args.append("-Wno-everything")  # we want the AST, not gcc-flag warnings
    return args


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build", type=Path)
    parser.add_argument("-v", "--verbose", action="store_true")
    opts = parser.parse_args()

    lib = sorted(glob.glob("/usr/lib/llvm-*/lib/libclang-*.so.1")) or \
        sorted(glob.glob("/usr/lib/*/libclang-*.so.1"))
    if not lib:
        print("error: libclang shared library not found", file=sys.stderr)
        return 2
    ci.Config.set_library_file(lib[-1])

    repo = Path(__file__).resolve().parent.parent
    entries = load_compile_commands(opts.build_dir)
    audited = [e for f, e in sorted(entries.items())
               if "/src/plugin/" in f or f.endswith("SyntaxDefinitionLoader.cpp")]
    if not audited:
        print("error: no audited TUs in compile_commands.json", file=sys.stderr)
        return 2

    index = ci.Index.create()
    memo = {}
    violations = []
    calls_checked = 0
    for entry in audited:
        file = str(Path(entry["directory"], entry["file"]).resolve())
        rel = str(Path(file).relative_to(repo)) if file.startswith(str(repo)) else file
        tu = index.parse(file, args=clang_args(entry))
        fatal = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Fatal]
        if fatal:
            print(f"error: failed to parse {rel}: {fatal[0].spelling}",
                  file=sys.stderr)
            return 2
        for cursor in tu.cursor.walk_preorder():
            if cursor.kind not in (CK.FUNCTION_DECL, CK.CXX_METHOD,
                                   CK.FUNCTION_TEMPLATE, CK.LAMBDA_EXPR):
                continue
            if not cursor.location.file:
                continue
            loc = str(cursor.location.file)
            if "/src/" not in loc:
                continue  # system/third-party headers
            body = next((c for c in cursor.get_children()
                         if c.kind == CK.COMPOUND_STMT), None)
            if body is None:
                continue
            for call, chain in find_call_paths(body):
                calls_checked += 1
                name = callee_name(call)
                if opts.verbose:
                    print(f"  check {name} at {call.location.file}:"
                          f"{call.location.line}")
                for var, why in live_locals_at(call, chain, cursor, memo):
                    violations.append(
                        f"{call.location.file}:{call.location.line}: "
                        f"`{name}` can longjmp over `{var.spelling}` "
                        f"({var.type.spelling}) declared at "
                        f"{var.location.file}:{var.location.line} — {why}")

    print(f"audited {len(audited)} TUs, {calls_checked} raise-capable call "
          f"sites")
    if violations:
        print(f"\n{len(violations)} VIOLATION(S):")
        for v in violations:
            print(f"  {v}")
        return 1
    print("clean: no raise-capable Lua call has a live non-trivially-"
          "destructible C++ object in scope")
    return 0


if __name__ == "__main__":
    sys.exit(main())
