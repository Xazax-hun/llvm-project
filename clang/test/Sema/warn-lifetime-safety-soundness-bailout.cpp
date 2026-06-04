// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-bailout -lifetime-safety-max-cfg-blocks=1 -verify=bailout %s
// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -lifetime-safety-max-cfg-blocks=1 -verify=bailout %s
// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-bailout -verify=nobailout %s

// nobailout-no-diagnostics

// When the analysis skips a function (here forced via a tiny CFG-block limit),
// the bailout soundness warning surfaces the gap so it is not silent under
// the "safe programming model". Without a limit (third RUN) there is no bailout
// and no diagnostics. The warning is also reachable via the -soundness
// umbrella (second RUN).

void use(int *);

void big(int c) { // bailout-warning {{lifetime safety analysis skipped this function because its control-flow graph is too large; lifetime mistakes here may not be detected}}
  int x;
  int *p = &x;
  if (c)
    use(p);
  else
    use(p);
}
