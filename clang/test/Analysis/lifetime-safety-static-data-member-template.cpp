// RUN: %clang_cc1 -fsyntax-only -std=c++20 -verify %s
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// expected-no-diagnostics

// The lifetime-safety TU sweeps in AnalysisBasedWarnings run for every C++
// translation unit and visit template instantiations, so they reach the
// implicitly-instantiated definition of an out-of-line static data member
// template. That declaration is a TSK_ImplicitInstantiation which nonetheless
// reports a (non-null, empty) template-argument-list-as-written, because
// Sema::InstantiateVariableDefinition calls setTemplateArgsAsWritten
// unconditionally and that setter always allocates. RecursiveASTVisitor used to
// treat "has args as written" as "is not an implicit instantiation" and assert
// on the combination, so merely compiling this crashed clang.
struct B {
  template <typename T> static T v;
};
template <typename T> T B::v = T();
float fsvar = B::v<float>;
