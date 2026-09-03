// DESC: a [[clang::lifetimebound]] parameter's borrow is captured into a caller-owned OWNER
// out-param. `set_prefix(Cache &c [[noescape]], const char *p [[lifetimebound]])` stores `p`
// into a private cursor of `c` and also returns it. Every annotation is literally true -- `c`
// does not escape, and the function really does return `p` -- but lifetimebound admits only
// the RETURN relationship. The caller, reading the declaration, discards the result and lets
// `tag` die while keeping `c`, which now points into a freed buffer.
//
// The undeclared-capture question was asked for the implicit object, driven by the escape
// facts. A parameter's object has no escape fact -- the store is all there is to see -- so the
// identical capture one parameter over was silent, while `d_ = p` inside a member function was
// reported. The other annotations on the source were already covered: noescape forbids the
// store outright and an unannotated parameter is demanded to be annotated, so lifetimebound
// slipped exactly between them.
//
// Note the honest author cannot declare this relationship for an owner destination:
// '[[clang::lifetime_capture_by(c)]]' naming a [[gsl::Owner]] is itself refused, since an owner
// is meant to own its contents. That is why the diagnostic's advice is to hold the borrow in a
// [[gsl::Pointer]] or to store an owned copy.
//
// The object starts out holding an immortal borrow so the lost-borrow sentinel has nothing to
// say; without that the analysis refuses the function rather than diagnosing it.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

static const char kInit[] = "immortal";

class [[gsl::Owner(char)]] Cache {
  const char *d_ = "";
public:
  explicit Cache(const char *init [[clang::lifetimebound]]) : d_(init) {}
  char read() const { return d_[0]; }

  // Caches the prefix and hands it back for chaining. The lifetimebound is truthful; the
  // store into `c` is the part no annotation here declares.
  friend const char *set_prefix(Cache &c [[clang::noescape]],
                                const char *p [[clang::lifetimebound]]) {
    c.d_ = p;
    return p;
  }
};

int main() {
  Cache c(kInit);
  {
    std::string tag(4096, 'T');
    set_prefix(c, tag.c_str()); // result discarded: the caller sees only a return relationship
  }                            // `tag` dies here, and `c` still points into its buffer
  sink = c.read();
  return 0;
}
