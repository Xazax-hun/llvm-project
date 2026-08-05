// DESC: a std::string_view MEMBER holding a borrow into a std::string reached
// through a pointer member; the string is reallocated in the same method, and the
// view is read from another. Same root cause as
// member_cached_borrow_read_in_other_method.cpp -- the seed loan for the pointer
// member has no issuing expression and no placeholder parameter, so the field
// escape at exit was the only causing fact and had no anchor to report against.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct [[gsl::Owner]] Doc {
  Doc() : s(new std::string(200, 'a')) {}
  ~Doc() { delete s; }

  void refresh() {
    view = *s;           // view member borrows *s
    s->append(200, 'b'); // reallocates -> view dangles
  }

  char first() const { return view[0]; }

private:
  std::string *s;
  std::string_view view;
};

int main() {
  Doc d;
  d.refresh();
  sink = d.first(); // heap-use-after-free
  return 0;
}
