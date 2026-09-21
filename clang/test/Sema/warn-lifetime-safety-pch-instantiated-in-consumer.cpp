// A specialization THIS translation unit instantiates from a template that came
// from a PCH must be analyzed here: the producer could not analyze it, because at
// produce time no instantiation exists yet.
//
// It is reachable through no DeclContext -- an implicitly-instantiated
// specialization lives in its template's folding set -- so a traversal rooted at
// the translation unit finds it only by going through the template, and
// `noload_decls()` deliberately leaves a PCH's template in lazy storage. Sema
// records what it instantiated instead; see Sema::InstantiatedVarDefinitions.
//
// RUN: rm -rf %t && split-file %s %t
//
// Producing the PCH reports nothing: the dependent pattern says nothing until T
// is known, and no instantiation exists yet.
// RUN: %clang_cc1 -std=c++20 -Wlifetime-safety-soundness -x c++-header \
// RUN:   -I%S/Inputs -emit-pch -o %t/tpl.pch -verify=produce %t/tpl.h
//
// RUN: %clang_cc1 -std=c++20 -Wlifetime-safety-soundness -fsyntax-only \
// RUN:   -I%S/Inputs -include-pch %t/tpl.pch -verify=consume %t/main.cpp
//
// The same code reached by a plain #include reports identically, which is the
// behaviour being restored -- including that each translation unit instantiating
// it reports, exactly as it does without a PCH.
// RUN: %clang_cc1 -std=c++20 -Wlifetime-safety-soundness -fsyntax-only \
// RUN:   -I%t -I%S/Inputs -verify=consume %t/direct.cpp

//--- tpl.h
#include "lifetime-analysis.h"

std::string g_mut = "a mutable global string";

struct [[gsl::Pointer]] Watch {
  std::string_view v;
};

// THE CASE THIS TEST IS FOR: a static data member of a class template, whose
// initializer is reached only by the file-variable-initializer sweep. The
// initializer does not depend on T, but the pattern is still skipped -- it says
// nothing until T is known -- so the producer reports nothing and only a consumer
// that instantiates can.
template <class T> struct A {
  static inline Watch w = Watch{g_mut};
};

// A function template is NOT affected: Sema instantiates the body in the consumer
// and the per-function path runs on it, with or without a PCH. Kept so the two
// cannot be confused if this regresses again.
template <class T> std::string_view viewOfGlobal() { return g_mut; }

// The expectations for the two lines above live in main.cpp and direct.cpp, as
// `@tpl.h:` directives. They cannot live here: in the PCH-consuming run this
// header is never parsed -- it arrives through the PCH -- and -verify reports a
// fatal error for directives in a file it did not parse.
// produce-no-diagnostics

//--- main.cpp
volatile char sink;

void use() {
  // consume-warning@tpl.h:15 {{'Watch' borrows from a mutable global or static object}}
  // consume-warning@+2 {{cannot track static variable 'w' here}}
  // consume-warning@+1 {{cannot track this value here}}
  sink = *A<int>::w.v.data();
  // consume-warning@tpl.h:21 {{'std::string_view' (aka 'basic_string_view<char>') borrows from a mutable global or static object}}
  // consume-note@+2 {{in instantiation of function template specialization 'viewOfGlobal<int>' requested here}}
  // consume-warning@+1 {{cannot track this value here}}
  sink = *viewOfGlobal<int>().data();
}

//--- direct.cpp
#include "tpl.h"
volatile char sink;

void use() {
  // consume-warning@tpl.h:15 {{'Watch' borrows from a mutable global or static object}}
  // consume-warning@+2 {{cannot track static variable 'w' here}}
  // consume-warning@+1 {{cannot track this value here}}
  sink = *A<int>::w.v.data();
  // consume-warning@tpl.h:21 {{'std::string_view' (aka 'basic_string_view<char>') borrows from a mutable global or static object}}
  // consume-note@+2 {{in instantiation of function template specialization 'viewOfGlobal<int>' requested here}}
  // consume-warning@+1 {{cannot track this value here}}
  sink = *viewOfGlobal<int>().data();
}
