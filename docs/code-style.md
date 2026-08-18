# C++ formatting

TermForge formats every Git-tracked `*.cpp` and `*.hpp` file with
clang-format **20.x**. The checked-in [`.clang-format`](../.clang-format) is
LLVM-derived, but the formatter major is part of the policy: later majors may
produce a different tree from the same configuration.

Run the same check as CI:

```bash
tools/format.sh --check
```

Apply the policy to the complete tracked C++ scope:

```bash
tools/format.sh --fix
```

The script uses `clang-format-20` by default. When the versioned binary has a
different local name, point to it explicitly; the script still verifies that
it is major version 20:

```bash
CLANG_FORMAT=clang-format tools/format.sh --check
```

## Selected style

The policy starts from LLVM and makes a small set of deliberate choices for
TermForge's protocol-heavy C++23 code:

| Area | Selection | Reason |
|---|---|---|
| Layout | 80 columns, two-space indentation, attached braces | Keeps terminal-wire assembly and its commentary reviewable side by side. |
| Pointers and references | Bind `*` and `&` to the type | Matches the existing public API and reads naturally in nested template types. |
| Access and switch labels | Access labels outdented by one space; case labels indented | Makes class sections and switch bodies visually distinct without a large horizontal step. |
| Control flow | Simple guard clauses and lookup cases may stay on one line; loops, blocks and compound branches expand | Preserves compact validation code without compressing stateful flow. |
| Declarations | Arguments may bin-pack; return types stay with names when practical | Avoids very tall declarations while keeping the symbol easy to scan. |
| Initializers | Standard compact C++ braced initialization | Keeps value construction distinct from compound blocks. |
| Includes | Sort within existing blocks; do not merge system and project groups | Produces deterministic ordering without destroying meaningful dependency groups. |
| Namespaces and comments | Add namespace-end comments and reflow prose | Keeps long translation units navigable and comments inside the same width budget. |
| Qualifiers | Leave qualifier order unchanged | Formatting must never perform a semantic rewrite. |

Examples of the compact control-flow boundary:

```cpp
if (payload.empty()) return refusal();

if (needs_reply) {
  queue_reply();
} else {
  commit_locally();
}
```

Formatting changes belong in a dedicated mechanical commit. Feature and bug
fix commits must not hide semantic edits inside a full-tree formatter pass.
