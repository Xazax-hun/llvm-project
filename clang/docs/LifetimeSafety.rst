========================
Lifetime Safety Analysis
========================

.. contents::
   :local:

Introduction
============

Clang Lifetime Safety Analysis is a C++ language extension which warns about
potential dangling pointer defects in code. The analysis aims to detect
when a pointer, reference or view type (such as ``std::string_view``) refers to an object
that is no longer alive, a condition that leads to use-after-free bugs and
security vulnerabilities. Common examples include pointers to stack variables
that have gone out of scope, pointers to heap objects that have been
freed, fields holding views to stack-allocated objects (dangling-field),
returning pointers/references to stack variables (return stack address) or
iterators into container elements invalidated by container operations (e.g.,
``std::vector::push_back``)

The analysis design is inspired by `Polonius, the Rust borrow checker <https://github.com/rust-lang/polonius>`_,
but adapted to C++ idioms and constraints, such as the lack of exclusivity enforcement (alias-xor-mutability). 
Further details on the analysis method can be found in the `RFC on Discourse <https://discourse.llvm.org/t/rfc-intra-procedural-lifetime-analysis-in-clang/86291/>`_.

This is compile-time analysis; there is no run-time overhead. 
It tracks pointer validity through intra-procedural data-flow analysis. While it does
not require lifetime annotations to get started, in their absence, the analysis
treats function calls optimistically, assuming no lifetime effects, thereby potentially missing dangling pointer issues. As more functions are annotated
with attributes like `clang::lifetimebound <https://clang.llvm.org/docs/AttributeReference.html#lifetimebound>`_, `gsl::Owner <https://clang.llvm.org/docs/AttributeReference.html#gsl-owner>`_, and
`gsl::Pointer <https://clang.llvm.org/docs/AttributeReference.html#gsl-pointer>`_, the analysis can see through these lifetime contracts and enforce
lifetime safety at call sites with higher accuracy. This approach supports
gradual adoption in existing codebases. 

.. note::
  This analysis is designed for bug finding, not verification. It may miss some
  lifetime issues and can produce false positives. It does not guarantee the
  absence of all lifetime bugs.

Getting Started
----------------

.. code-block:: c++

  #include <string>
  #include <string_view>

  void simple_dangle() {
    std::string_view v;
    {
      std::string s = "hello";
      v = s;  // warning: object whose reference is captured does not live long enough
    }         // note: destroyed here
    std::cout << v; // note: later used here
  }

This example demonstrates
a basic use-after-scope bug. The ``std::string_view`` object ``v`` holds a
reference to ``s``, a ``std::string``. The lifetime of ``s`` ends at the end of
the inner block, causing ``v`` to become a dangling reference.
The analysis flags the assignment ``v = s`` as defective because ``s`` is
destroyed while ``v`` is still alive and points to ``s``, and adds a note
to where ``v`` is used after ``s`` has been destroyed.

Running The Analysis
--------------------

To run the analysis, compile with the ``-Wlifetime-safety-permissive`` flag, e.g.

.. code-block:: bash

  clang -c -Wlifetime-safety-permissive example.cpp

This flag enables a core set of lifetime safety checks. For more fine-grained
control over warnings, see :ref:`warning_flags`.

Lifetime Annotations
====================

While lifetime analysis can detect many issues without annotations, its
precision increases significantly when types and functions are annotated with
lifetime contracts. These annotations clarify ownership semantics and lifetime
dependencies, enabling the analysis to reason more accurately about pointer
validity across function calls.

Owner and Pointer Types
-----------------------

Lifetime analysis distinguishes between types that own the data they point to
(Owners) and types that are non-owning views or references to data owned by
others (Pointers). This distinction is made using GSL-style attributes:

*   ``[[gsl::Owner]]``: For types that manage the lifetime of a resource,
    like ``std::string``, ``std::vector``, ``std::unique_ptr``.
*   ``[[gsl::Pointer]]``: For non-owning types that borrow resources,
    like ``std::string_view``, or raw pointers (which are
    implicitly treated as pointers).

Many common STL types, such as ``std::string_view`` and container iterators,
are automatically recognized as Pointers or Owners. You can annotate your own
types using these attributes:

.. code-block:: c++

  #include <string>
  #include <string_view>

  // Owner type
  struct [[gsl::Owner]] MyObj {
    std::string Data = "Hello";
  };

  // View type
  struct [[gsl::Pointer]] View {
    std::string_view SV;
    View() = default;
    View(const MyObj& O) : SV(O.Data) {}
    void use() const {}
  };

  void test() {
    View v;
    {
      MyObj o;
      v = o; // warning: object whose reference is captured does not live long enough
    }        // note: destroyed here
    v.use(); // note: later used here
  }

Without these annotations, the analysis may not be able to determine whether a
type is owning or borrowing, which can affect analysis precision. For more
details on these attributes, see the Clang attribute reference for
`gsl::Owner <https://clang.llvm.org/docs/AttributeReference.html#gsl-owner>`_ and
`gsl::Pointer <https://clang.llvm.org/docs/AttributeReference.html#gsl-pointer>`_.

.. note::
  Types with mixed ownership semantics (owning some data while holding views to
  other data) or types with multiple view fields with different lifetimes should
  not be annotated.  The analysis does not yet support expressing such nuanced 
  lifetime relationships.
  Future enhancements, such as named lifetimes, may provide better support for
  these patterns.

LifetimeBound
-------------

The ``[[clang::lifetimebound]]`` attribute can be applied to function parameters
or to the implicit ``this`` parameter of a method (by placing it after the
method declarator). It indicates that the returned value becomes invalid when
the attributed parameter or ``this`` object is destroyed.
This is crucial for functions that return views or references to their
arguments.

.. code-block:: c++

  #include <string>
  #include <string_view>

  struct MyOwner {
    std::string s;
    std::string_view getView() const [[clang::lifetimebound]] { return s; }
  };

  void test_lifetimebound() {
    std::string_view sv;
    sv = MyOwner().getView(); // getView() is called on a temporary MyOwner
                             // warning: object whose reference is captured does not live long enough
                             // note: destroyed here
    (void)sv;                // note: later used here
  }

Without ``[[clang::lifetimebound]]`` on ``getView()``, the analysis would not
know that the value returned by ``getView()`` depends on the temporary
``MyOwner`` object, and it would not be able to diagnose the dangling ``sv``.

A C++23 explicit object parameter ("deducing this") is treated as the object for
this purpose: ``[[clang::lifetimebound]]`` on the ``self`` parameter
(``auto getView(this const MyOwner &self [[clang::lifetimebound]])``) binds the
result to the object just as it would for an implicit ``this``.

The analysis also tracks record types returned from functions and constructors
with ``[[clang::lifetimebound]]`` annotated parameters:

.. code-block:: c++

  #include <string>

  struct StringView {
    StringView();
    StringView(const std::string &s [[clang::lifetimebound]]);
  };

  StringView getStringView(const std::string &s [[clang::lifetimebound]]);

  void test() {
    StringView a, b;
    {
      std::string s = "temp";
      StringView tmp(s);    // warning: object whose reference is captured does not live long enough
      a = tmp;
      b = getStringView(s); // warning: object whose reference is captured does not live long enough
    }                       // note: destroyed here
    (void)a;                // note: later used here
    (void)b;                // note: later used here
  }

For more details, see `lifetimebound <https://clang.llvm.org/docs/AttributeReference.html#lifetimebound>`_.

NoEscape
--------

.. _Wlifetime-safety-noescape:

The ``[[clang::noescape]]`` attribute can be applied to function parameters of
pointer or reference type. It indicates that the function will not allow the
parameter to escape its scope, for example, by returning it or assigning it to
a field or global variable. This is useful for parameters passed to callbacks
or visitors that are only used during the call and not stored.

For more details, see `noescape <https://clang.llvm.org/docs/AttributeReference.html#noescape>`_.

Checks Performed
================


.. raw:: html

   <style>
   /* Align text to left and add red/green colors */
   table.colored-code-table td, table.colored-code-table th { text-align: left !important; }
   table.colored-code-table td:first-child, table.colored-code-table th:first-child { background-color: #ffeaea !important; }
   table.colored-code-table td:nth-child(2), table.colored-code-table th:nth-child(2) { background-color: #eafaea !important; }
   table.colored-code-table td .highlight, table.colored-code-table td pre { background-color: transparent !important; border: none !important; }

   div.bad-code { background-color: #ffeaea !important; padding: 5px; border-left: 4px solid #ff6b6b; text-align: left !important; }
   div.bad-code .highlight, div.bad-code pre { background-color: transparent !important; border: none !important; }

   div.good-code { background-color: #eafaea !important; padding: 5px; border-left: 4px solid #51cf66; text-align: left !important; }
   div.good-code .highlight, div.good-code pre { background-color: transparent !important; border: none !important; }
   </style>

Use after scope
---------------

This check warns when a pointer or reference is used after the stack variable
it refers to has gone out of scope.

.. list-table::
   :widths: 50 50
   :header-rows: 1
   :class: colored-code-table

   * - Use after scope
     - Correct
   * -
       .. code-block:: c++

         void foo() {
           int* p;
           {
             int i = 0;
             p = &i;  // warning: 'p' does not live long enough
           }          // note: destroyed here
           (void)*p;  // note: later used here
         }
     -
       .. code-block:: c++

         void foo() {
           int i = 0;
           int* p;
           {
             p = &i; // OK!
           }
           (void)*p;
          }

Use after free
--------------

This check warns when a pointer or reference is used after the object it refers
to has been freed.

.. list-table::
   :widths: 50 50
   :header-rows: 1
   :class: colored-code-table

   * - Use after free
     - Correct
   * -
       .. code-block:: c++

         void foo() {
           int *p = new int(0); // warning: allocated object does not live long enough
           delete p;            // note: freed here
           (void)*p;            // note: later used here
         }
     -
       .. code-block:: c++

         void foo() {
           int *p = new int(0);
           (void)*p;
           delete p; // OK!
         }

Return of stack address
-----------------------

This check warns when a function returns a pointer or reference to a
stack-allocated variable, which will be destroyed when the function returns,
leaving the caller with a dangling pointer.

.. list-table::
   :widths: 50 50
   :header-rows: 1
   :class: colored-code-table

   * - Return of stack address
     - Correct
   * -
       .. code-block:: c++

        #include <string>
        #include <string_view>

        std::string_view bar() {
          std::string s = "on stack";
          std::string_view result = s;
          // warning: address of stack variable 's' is returned later
          return result; // note: returned here
        }
     -
       .. code-block:: c++

        #include <string>
        #include <string_view>

        std::string bar() {
          std::string s = "on stack";
          std::string_view result = s;
          return result; // OK!
        }


Dangling field
--------------

This check warns when a constructor or method assigns a pointer to a
stack-allocated variable or temporary to a field of the class.

.. list-table::
   :widths: 50 50
   :header-rows: 1
   :class: colored-code-table


   * - Dangling field
     - Correct
   * -
       .. code-block:: c++

          #include <string>
          #include <string_view>

          // Constructor finishes, leaving 'field' dangling.
          struct DanglingField {
            std::string_view field; // note: this field dangles
            DanglingField(std::string s) {
              field = s; // warning: stack variable 's' escapes to a field
            }
          };
     -
       .. code-block:: c++

          // Make the field an owner.
          struct DanglingField {
            std::string field;
            DanglingField(std::string s) {
              field = s;
            }
          };
          // Or take a string_view parameter.
          struct DanglingField {
            std::string_view field;
            DanglingField(std::string_view s [[clang::lifetimebound]]) {
              field = s;
            }
          };
         };


Use after invalidation (experimental)
-------------------------------------

This check warns when a pointer, reference or view is used after an operation
that may have invalidated it. This includes references to container elements
used after a container operation, and pointers to objects managed by owners such
as ``std::unique_ptr`` after operations like ``reset``. For example, adding
elements to ``std::vector`` may cause reallocation, invalidating all existing
iterators, pointers and references to its elements.

.. note::
  Invalidation checking is highly experimental and may produce false positives.

.. list-table::
   :widths: 50 50
   :header-rows: 1
   :class: colored-code-table


   * - Use after invalidation (experimental)
     - Correct
   * -
       .. code-block:: c++

        #include <vector>

        void baz(std::vector<int>& v) {
          int* p = &v[0]; // warning: 'v' is later invalidated
          v.push_back(4); // note: invalidated here
          *p = 10;        // note: later used here
        }
     -
       .. code-block:: c++

        #include <vector>

        void baz(std::vector<int>& v) {
          v.push_back(4);
          int* p = &v[0]; // OK!
          *p = 10;
        }

The analysis also treats explicit destruction as invalidation. Explicit
destructor calls and ``std::destroy_at`` invalidate pointers, references and
views into the destroyed object.

.. code-block:: c++

  #include <memory>
  #include <string>

  void explicit_destruction() {
    std::string s = "hello";
    const char *p = s.data(); // warning: object whose reference is captured is later invalidated
    std::destroy_at(&s);      // note: invalidated here
    (void)*p;                 // note: later used here
  }

  void unique_ptr_reset() {
    std::unique_ptr<int> u(new int(0));
    int *p = u.get(); // warning: object whose reference is captured is later invalidated
    u.reset();        // note: invalidated here
    (void)*p;         // note: later used here
  }

Annotation Inference and Suggestions
====================================

In addition to detecting lifetime violations, the analysis can suggest adding
``[[clang::lifetimebound]]`` to function parameters or methods when it detects
that a pointer/reference to a parameter or ``this`` escapes via the return
value. This helps improve API contracts and allows the analysis to perform
more accurate checks in calling code.

To enable annotation suggestions, use ``-Wlifetime-safety-suggestions``.

.. code-block:: c++

  #include <string_view>

  // The analysis will suggest adding [[clang::lifetimebound]] to 'a'.
  std::string_view return_view(std::string_view a) { 
                            // ^^^^^^^^^^^^^^^^^^
                            // warning: parameter 'a' should be marked [[clang::lifetimebound]]
    return a;               // note: param returned here
  }

Translation-Unit-Wide Analysis and Inference
--------------------------------------------

By default, lifetime analysis is intra-procedural for error checking.
However, for annotation inference to be effective, lifetime information needs
to propagate across function calls. You can enable experimental
translation-unit-wide analysis using:

*   ``-flifetime-safety-inference``: Enables inference of ``lifetimebound``
    attributes across functions in a TU.
*   ``-fexperimental-lifetime-safety-tu-analysis``: Enables TU-wide analysis
    for better inference results.

.. _warning_flags:

Warning flags
=============

Lifetime safety warnings are organized into hierarchical groups, allowing users to
enable categories of checks incrementally. For example, ``-Wlifetime-safety``
enables all dangling pointer checks, while ``-Wlifetime-safety-permissive``
enables only the high-confidence subset of these checks.

*  ``-Wlifetime-safety-all``: Enables all lifetime safety warnings, including
    dangling pointer checks, annotation suggestions, and annotation validations.

*  ``-Wlifetime-safety``: Enables dangling pointer checks from both the ``permissive`` and ``strict`` groups listed below.

  * ``-Wlifetime-safety-permissive``: Enables high-confidence checks for dangling pointers. **Recommended for initial adoption.**

    * ``-Wlifetime-safety-use-after-scope``: Warns when a pointer to a stack variable is used after the variable's lifetime has ended.
    * ``-Wlifetime-safety-use-after-free``: Warns when a pointer to an object is used after it's been freed.
    * ``-Wlifetime-safety-return-stack-addr``: Warns when a function returns a pointer or reference to one of its local stack variables.
    * ``-Wlifetime-safety-dangling-field``: Warns when a class field is assigned a pointer to a temporary or stack variable whose lifetime is shorter than the class instance.
  
  * ``-Wlifetime-safety-strict``: Enables stricter and experimental checks. These may produce false positives in code that uses move semantics heavily, as the analysis might conservatively assume a use-after-free even if ownership was transferred.

    *   ``-Wlifetime-safety-use-after-scope-moved``: Same as ``-Wlifetime-safety-use-after-scope`` but for cases where the variable may have been moved from before its destruction.
    *   ``-Wlifetime-safety-return-stack-addr-moved``: Same as ``-Wlifetime-safety-return-stack-addr`` but for cases where the variable may have been moved from.
    *   ``-Wlifetime-safety-dangling-field-moved``: Same as ``-Wlifetime-safety-dangling-field`` but for cases where the variable may have been moved from.
    *   ``-Wlifetime-safety-dangling-global-moved``: Same as ``-Wlifetime-safety-dangling-global`` but for cases where the variable may have been moved from.
    *   ``-Wlifetime-safety-invalidation``: Warns when a pointer, reference, iterator or view is used after an operation that may invalidate it, such as container mutation or explicit destruction/deallocation (e.g., ``std::unique_ptr::reset``, ``std::destroy_at``, a ``delete`` expression, or a ``free`` / ``realloc`` / ``reallocf`` / ``cfree`` / ``::operator delete`` / ``__builtin_operator_delete`` call) (Experimental).

*   ``-Wlifetime-safety-suggestions``: Enables suggestions to add ``[[clang::lifetimebound]]`` to function parameters and ``this`` parameters.

  * ``-Wlifetime-safety-intra-tu-suggestions``: Suggestions for functions local to the translation unit.
  * ``-Wlifetime-safety-cross-tu-suggestions``: Suggestions for functions visible across translation units (e.g., in headers).

* ``-Wlifetime-safety-validations``: Enables checks that validate existing lifetime annotations.

  * ``-Wlifetime-safety-noescape``: Warns when a parameter marked with ``[[clang::noescape]]`` escapes the function.
  * ``-Wlifetime-safety-lifetimebound-violation``: Warns when the analysis cannot verify that the return value can be lifetime bound to a parameter marked with ``[[clang::lifetimebound]]``.
  * ``-Wlifetime-safety-immortal-violation``: Warns when a function marked ``[[clang::lifetime_immortal]]`` returns a borrow of non-immortal storage (the implicit object ``this``, a parameter, or a local/temporary) or a borrow the analysis cannot prove is immortal -- e.g. one laundered through an untracked borrow-returning call such as ``std::string_view::substr`` (its result is an untracked borrow even when it still views a local). An immortal function must return only provably-immortal storage; delegating to another ``[[clang::lifetime_immortal]]`` function is provable and stays silent. Unlike ``[[clang::lifetimebound]]``, the immortal promise is otherwise trusted unverified, so a caller may keep the result past the borrowed object's lifetime; the body is verified here.

* ``-Wlifetime-safety-soundness``: The completeness group for the :ref:`safe programming model <safe_programming_model>`. Enabling it (typically as errors over a region of code) makes the analysis never silently fail to catch a lifetime mistake. It bundles three kinds of warning:

  * The strict bug-detection warnings -- ``-Wlifetime-safety-strict`` (which itself includes ``-Wlifetime-safety-permissive``) -- so the actual use-after-free, use-after-scope, return-stack-address, dangling-field/global, and invalidation findings are reported, not just the modeling-gap warnings below. (Reporting a construct as "unmodeled" would be of little use if the bugs the analysis *can* prove were not also surfaced.)
  * The annotation-validation warnings -- ``-Wlifetime-safety-noescape``, ``-Wlifetime-safety-lifetimebound-violation``, and ``-Wlifetime-safety-immortal-violation`` -- since an annotation the function body does not honor is itself a hole that callers trust.
  * The modeling-gap warnings, which fire wherever the analysis cannot fully model a construct:

  * ``-Wlifetime-safety-bailout``: Warns when the analysis skips a whole function (e.g. its control-flow graph is too large or could not be built).
  * ``-Wlifetime-safety-lost-loan``: Warns when a tracked pointer-like value ends up holding no borrow -- or an untracked (``Unknown``) borrow from an unmodeled borrow-returning call such as ``std::string_view::substr`` -- indicating a borrow was lost to an unmodeled construct. This fires both where the value is *used* and where it *escapes* the function (returned, or stored to a field or global) without a local use, so an untracked borrow cannot leave the function silently and defeat the downstream annotation checks. A default/empty construction (``return {};``, ``nullptr``) borrows nothing and is not flagged.
  * ``-Wlifetime-safety-indirect-call``: Warns on calls through a function or member-function pointer, which cannot carry lifetime annotations.
  * ``-Wlifetime-safety-unannotated-indirection``: Warns on a parameter that can hold a borrow but carries none of ``[[clang::lifetimebound]]``, ``[[clang::noescape]]`` or ``[[clang::lifetime_capture_by]]`` (both at the definition and at call sites). Also warns on an instance member function (including a conversion operator) that returns an indirection without marking its implicit object ``[[clang::lifetimebound]]`` (or the function ``[[clang::lifetime_immortal]]``). Finally, warns when a borrow escapes to global or static storage that the relevant annotation does not describe: a borrow of the implicit object (``this``) or one of its fields, or a parameter whose ``[[clang::lifetimebound]]``/``[[clang::lifetime_capture_by]]`` annotation describes a return/capture relationship rather than a global capture.
  * ``-Wlifetime-safety-multilevel-indirection``: Warns on declarations that use more than one level of indirection (e.g. ``int **``), and on expressions that form one (e.g. ``&p`` where ``p`` is itself a pointer or view -- taking the address of an indirection), which the analysis cannot fully model.
  * ``-Wlifetime-safety-move-silencing``: Warns when an owner is moved (e.g. ``std::move`` or ``std::unique_ptr::release``), an ownership transfer the analysis does not model.
  * ``-Wlifetime-safety-assumed-invalidation``: Warns when a borrow into an owner may be invalidated by an operation conservatively assumed to mutate it: a non-const member call (other than a recognized standard-library accessor such as ``operator[]``, ``at`` or ``data``), passing the owner to a non-const pointer/reference parameter or mutable view, calling a lambda that captures the owner by reference, or passing an owner together with a value that aliases it -- a view that borrows it, the same owner bound to two reference/pointer parameters, or a borrow of the receiver (``this``) passed as an argument to a call that reallocates the receiver -- so the callee may mutate it through one handle while another still refers to it.
  * ``-Wlifetime-safety-naked-delete``: Warns on a ``delete``/``free`` of a pointer whose allocation the analysis did not see (deallocations inside destructors are exempt).
  * ``-Wlifetime-safety-unknown-ownership``: Warns on a value of a user-defined type that can hold a borrow but is annotated neither ``[[gsl::Owner]]`` nor ``[[gsl::Pointer]]``.
  * ``-Wlifetime-safety-exception``: Warns on a ``throw`` or a ``try``/``catch``. Exception control flow (stack unwinding, running destructors and resuming in a handler) is not modeled, so a borrow that dangles only along an exception path can be missed.
  * ``-Wlifetime-safety-owner-of-indirection``: Warns on a value of a ``[[gsl::Owner]]`` container whose element type is an indirection -- a pointer, reference, ``[[gsl::Pointer]]``, or a complete unannotated user type that itself holds a borrow (e.g. ``std::vector<int*>``, or ``std::unique_ptr<Box>`` where ``Box`` has a ``std::string_view`` member). The element type is taken from the container's template arguments or, for a non-template owner, from the ``[[gsl::Owner(T)]]`` attribute's optional type argument ``T`` -- so a self-contradictory ``struct [[gsl::Owner(std::string_view)]]`` (an "owner" of a view) is rejected too. The analysis does not track borrows held by individual elements, and an element type that holds a borrow must itself be annotated ``[[gsl::Owner]]``/``[[gsl::Pointer]]`` before it can be modeled. This is checked for locals, parameters, call results, and **data members** (a borrow stored through such a member, e.g. ``this->views[i] = local``, would otherwise be dropped and dangle silently). (An *incomplete* element type is not flagged: it cannot be shown to hold a borrow, and flagging it would reject the common ``std::unique_ptr<ForwardDeclared>`` pimpl idiom.)
  * ``-Wlifetime-safety-pointer-of-indirection``: Warns on a value of a ``[[gsl::Pointer]]`` view whose pointee/element type is itself an indirection -- a pointer, reference, ``[[gsl::Pointer]]``, or a complete unannotated borrow-holding user type (e.g. ``std::span<int*>``); the view tracks only its outer level, so a borrow held one level deeper is not modeled. The pointee is taken from the ``[[gsl::Pointer(T)]]`` attribute's optional type argument ``T`` when present, else a ``value_type``/``element_type`` typedef, ``operator*``/``operator->``, or a sole template argument. Checked for locals, parameters, call results, and data members.
  * ``-Wlifetime-safety-view-on-mutable-global``: Warns when a view (a ``[[gsl::Pointer]]`` such as ``std::string_view`` or ``std::span``) or a raw pointer/reference borrows from a mutable global or static owner -- whether the owner itself (``std::string_view sv = g_str;``) or its contents (``g_table[i]``, ``&g_vec[0]``, ``g.data()``) -- which can be mutated from anywhere the intra-procedural analysis cannot see, invalidating the borrow. The check is loan-based: the borrow surfaces as a loan rooted at the global owner regardless of how it was reached, so a ``[[clang::lifetime_immortal]]`` annotation on an accessor does not exempt it (the owner's reallocatable buffer is not immortal). A pointer *at* the object (``&g``) is not a borrow into its contents and stays valid.
  * ``-Wlifetime-safety-const-subversion``: Warns on a ``mutable`` data member, a ``const_cast``, or a const member function that mutates an owner through a pointer member, smart or raw (``const`` does not propagate through a pointer) -- including when the pointee is not itself an owner but transitively contains one (e.g. a pimpl ``Impl`` holding a ``std::vector``). The analysis assumes a const member function does not invalidate borrows into the object; each of these can subvert that assumption.
  * ``-Wlifetime-safety-owner-capture``: Warns on ``[[clang::lifetime_capture_by(this)]]`` applied to a parameter of a ``[[gsl::Owner]]`` type. An owner is meant to own its contents, not to capture a borrow into its (opaque) members, which cannot be tracked once the owner is passed elsewhere; use a ``[[gsl::Pointer]]`` (view) type to hold a borrow.
  * ``-Wlifetime-safety-self-referential``: Warns when a view/pointer data member is bound to a sibling member of the same object (e.g. a ``std::string_view`` member pointing into a ``std::string`` member of the same struct), including across the base/derived boundary -- a view member in a base subobject bound to a member of the derived class, or captured into the receiver via ``[[clang::lifetime_capture_by(this)]]``. The object becomes self-referential: mutating or moving it invalidates the view, and that relationship is invisible once the object is passed elsewhere. (The base/derived case is especially dangerous: destruction order destroys the derived members before the base destructor runs.)
  * ``-Wlifetime-safety-global-capture``: Warns on a parameter annotated ``[[clang::lifetime_capture_by(global)]]`` or ``[[clang::lifetime_capture_by(unknown)]]``. The analysis cannot track a borrow captured into global or static storage, or into an unspecified location (it does not know where it is stored), so such a capture may dangle undetected; the construct is rejected.

.. _safe_programming_model:

Safe Programming Model
======================

The checks under :ref:`-Wlifetime-safety <warning_flags>` are *best-effort*:
when the analysis cannot model a construct it silently tracks nothing, so a
clean compile is not by itself a guarantee that no lifetime mistake slipped
through. The ``-Wlifetime-safety-soundness`` group closes that gap. It is a
superset of the bug-detection checks: it includes everything in
``-Wlifetime-safety-strict`` (and therefore ``-Wlifetime-safety-permissive``),
the annotation-validation checks (``-Wlifetime-safety-noescape`` and
``-Wlifetime-safety-lifetimebound-violation``), and a family of *modeling-gap*
checks that each fire wherever the analysis cannot fully account for a construct
(an unsupported indirection, an unmodeled call, an analysis bailout, an
unannotated or unknown-ownership type, and so on). Enabling this one group is
therefore sufficient -- the actual dangling-pointer findings are reported
alongside the places the analysis gives up.

By enabling these checks **as errors** over a region of code, you opt that code
into a "safe programming model" in which the analysis never silently fails to
catch a lifetime mistake. The model requires, among other things, that every
indirection be annotated, that only a single level of indirection be used, that
user-defined types declare their ownership with ``[[gsl::Owner]]`` or
``[[gsl::Pointer]]``, that an owner not capture a borrow into its members (no
``[[clang::lifetime_capture_by(this)]]`` on a ``[[gsl::Owner]]`` -- use a view
type instead), that containers not store indirections (e.g. no
``std::vector<int*>``) and views not point at indirections (e.g. no
``std::span<int*>``), that const member functions not be subverted -- no
``mutable`` fields, no ``const_cast``, and no mutation of an owner through the
pointee of a pointer member (smart or raw) -- (so const member functions can be
assumed not to invalidate borrows), that a call not be passed an owner together with an
aliasing handle to it (a view of it, or the same owner as two reference
parameters), that a view/pointer member not be bound to a sibling member of the
same object, that views not be formed over mutable global state, and that
exceptions not be used (their unwinding control flow is not modeled).

Opting in is done with the standard diagnostic pragmas, which apply to a region
of code:

.. code-block:: c++

  #pragma clang diagnostic push
  #pragma clang diagnostic error "-Wlifetime-safety-soundness"

  // ... code that must adhere to the safe programming model ...

  #pragma clang diagnostic pop

When a region calls a function that is not annotated (for example, a library
function), prefer providing an annotated redeclaration. Where that is not
possible, the same pragma machinery provides a per-construct **opt-out**:

.. code-block:: c++

  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wlifetime-safety-soundness"
  legacy_call(p); // intentionally not modeled
  #pragma clang diagnostic pop

Because diagnostic pragma regions are resolved by source location, the opt-in
and opt-out behave identically under translation-unit-wide analysis
(``-fexperimental-lifetime-safety-tu-analysis``), even though the checks run at
the end of the translation unit.

Limitations
===========

Move Semantics
--------------
The analysis does not currently track ownership transfers through move operations.
Instead, it uses scope-based lifetime tracking: when an owner goes out of scope,
the analysis assumes the resource is destroyed, even if ownership was transferred
via ``std::move()`` or ``std::unique_ptr::release()``.

This means that if a pointer or view is created from an owner, and that owner is
later moved-from and goes out of scope, the analysis will issue a
``-Wlifetime-safety-*-moved`` warning. This warning indicates that the pointer
may be dangling, even though the resource may still be alive under a new owner.
These are often false positives when ownership has been safely transferred.

To avoid these warnings and ensure correctness, follow the
**"move-first-then-alias"** pattern: create views or raw pointers *after* the
ownership transfer, sourcing them from the new owner rather than the original
owner that will go out of scope.

For example:

.. list-table::
   :widths: 50 50
   :header-rows: 1
   :align: left
   :class: colored-code-table

   * - Anti-Pattern: Aliasing Before Move
     - Good Practice: Move-First-Then-Alias
   * -
       .. code-block:: c++

         #include <memory>

         void use(int*);

         void bar() {
           std::unique_ptr<int> b;
           int* p;
           {
             auto a = std::make_unique<int>(42);
             p = a.get(); // warning!
             b = std::move(a);
           }
           use(p);
         }
     -
       .. code-block:: c++

         #include <memory>

         void use(int*);

         void bar() {
           std::unique_ptr<int> b;
           int* p;
           {
             auto a = std::make_unique<int>(42);
             b = std::move(a);
             p = b.get(); // OK!
           }
           use(p);
         }

The same principle applies when moving ownership using ``std::unique_ptr::release()``:

.. code-block:: c++
  :class: bad-code

  #include <memory>
  #include <utility>

  void use(int*);
  void take_ownership(int*);

  void test_aliasing_before_release() {
    int* p;
    {
      auto u = std::make_unique<int>(1);
      p = u.get();
      //  ^ warning: 'u' does not live long enough!
      take_ownership(u.release());
    } 
    use(p);  
  }

``std::unique_ptr`` with custom deleters
----------------------------------------
The analysis assumes standard ownership semantics for owner types like
``std::unique_ptr``: when a ``unique_ptr`` goes out of scope, it is assumed
that the owned object is destroyed and its memory is deallocated.
However, ``std::unique_ptr`` can be used with a custom deleter that modifies
this behavior. For example, a custom deleter might keep the memory alive
by transferring it to a memory pool, or simply do nothing, allowing
another system to manage the lifetime.

Because the analysis relies on scope-based lifetime for owners, it does not
support custom deleters that extend the lifetime of the owned object beyond
the lifetime of the ``std::unique_ptr``. In such cases, the analysis will
assume the object is destroyed when the ``std::unique_ptr`` goes out of scope,
leading to false positive warnings if pointers to the object are used afterward.

.. code-block:: c++

  #include <memory>

  void use(int*);

  struct NoOpDeleter {
    void operator()(int* p) const {
      // Do not delete p, memory is managed elsewhere.
    }
  };

  void test_custom_deleter() {
    int* p;
    {
      std::unique_ptr<int, NoOpDeleter> u(new int(42));
      p = u.get();  // warning: object whose reference is captured does not live long enough
    }               // note: destroyed here
    // With NoOpDeleter, p would still be valid here.
    // But analysis assumes standard unique_ptr semantics and memory being freed.
    use(p);         // note: later used here
  }

Owner annotations are trusted, not verified
--------------------------------------------
The analysis trusts a ``[[gsl::Owner]]`` annotation: it assumes such a type
genuinely owns and manages its own storage. It does **not** verify that the
type actually does so. A type annotated ``[[gsl::Owner]]`` that in fact only
*borrows* its contents (for example, a wrapper that stores a ``std::string_view``
member but is labelled an owner) will be treated as owning storage, and a borrow
taken from it will be assumed valid for the owner's lifetime.

A consequence is that the owner-of-indirection check
(``-Wlifetime-safety-owner-of-indirection``) only rejects an owner whose element
type is *unannotated*. ``std::unique_ptr<Box>`` where ``Box`` has a view member
is rejected (``Box`` must declare its ownership), but
``std::unique_ptr<AnnotatedOwner>`` is accepted on the owner's word, even if
``AnnotatedOwner`` does not really manage the storage its members refer to.
Annotate a type ``[[gsl::Owner]]`` only when it truly owns its resources.

Dangling Fields
---------------
The lifetime analysis is intra-procedural. It analyzes one function or method at
a time.
This means if a field is assigned a pointer to a local variable or temporary
inside a constructor or method, and that local's lifetime ends before the method
returns, the analysis will issue a ``-Wlifetime-safety-dangling-field`` warning.
It must do so even if no *other* method of the class ever accesses this field,
because it cannot see how other methods are implemented or used.

.. code-block:: c++

  #include <string>
  #include <string_view>

  struct MyWidget {
    std::string_view name_; // note: this field dangles
    MyWidget(std::string name) : name_(name) {} // warning: address of stack memory escapes to a field
    const char* data() { return name_.data(); } // Potential use-after-free if called
  };

In this case, ``name_`` dangles after the constructor finishes.
Even if ``data()`` is never called, the analysis flags the dangling assignment
in the constructor because it represents a latent bug.
The recommended approach is to ensure fields only point to objects that outlive
the field itself, for example by storing an owned object (e.g., ``std::string``)
or ensuring the borrowed object (e.g., one passed by ``const&``) has a
sufficient lifetime.


Performance
===========

Lifetime analysis relies on Clang's CFG (Control Flow Graph). For functions
with very large or complex CFGs, analysis time can sometimes be significant. To mitigate
this, the analysis allows to skip functions where the number of CFG blocks exceeds
a certain threshold, controlled by the ``-flifetime-safety-max-cfg-blocks=N`` language
option.
