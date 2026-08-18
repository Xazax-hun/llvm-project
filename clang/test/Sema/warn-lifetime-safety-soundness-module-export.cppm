// RUN: %clang_cc1 -std=c++20 -emit-module-interface -Wlifetime-safety-soundness -verify -o %t.pcm %s

// A namespace-scope variable's dynamic initializer is code that only the
// file-scope-initializer sweep reaches. In a module interface unit an `export`-ed
// declaration is nested inside an ExportDecl, and that node was absent from the
// set of contexts the sweep descended into -- so every exported namespace-scope
// initializer in the TU was skipped. Nothing was refused; the code was simply
// never analyzed, which is why the identical unexported declarations below are
// reported and the exported ones used not to be.

export module lifetime_export;

volatile char sink;

struct [[gsl::Pointer]] View {
  const char *p;
};

// A borrow of a heap allocation that the initializer then frees: the store into a
// namespace-scope view outlives the allocation.
export {
int *g_exported = new int[16];
// expected-warning@+2 {{deleting a pointer whose allocation was not seen}}
// expected-warning@+1 2 {{cannot track global variable 'g_exported' here}}
int g_exported_use = (delete[] g_exported, g_exported[3]);
}

// A single-declaration `export` (no braces) nests an ExportDecl just the same.
export int *g_single = new int[16];
// expected-warning@+2 {{deleting a pointer whose allocation was not seen}}
// expected-warning@+1 2 {{cannot track global variable 'g_single' here}}
export int g_single_use = (delete[] g_single, g_single[3]);

// `export namespace` nests an ExportDecl around a NamespaceDecl, so both kinds of
// context have to be descended into, not just the one.
export namespace app {
int *g_ns = new int[16];
// expected-warning@+2 {{deleting a pointer whose allocation was not seen}}
// expected-warning@+1 2 {{cannot track global variable 'g_ns' here}}
int g_ns_use = (delete[] g_ns, g_ns[3]);
} // namespace app

// Control: an unexported declaration in the same unit. This was always analyzed,
// so it pins the diagnostics above to the export nesting rather than to modules.
int *g_internal = new int[16];
// expected-warning@+2 {{deleting a pointer whose allocation was not seen}}
// expected-warning@+1 2 {{cannot track global variable 'g_internal' here}}
int g_internal_use = (delete[] g_internal, g_internal[3]);

// Negative: an exported initializer that borrows nothing stays clean.
export int g_count = 42;
export const char *g_literal = "literals never dangle";
