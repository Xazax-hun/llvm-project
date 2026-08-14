// DESC: shutdown code that is not the destructor of anything.
// '__attribute__((destructor))' registers a function to run during static destruction
// directly, so the variable-level rule -- which keys on the type of a static-duration
// variable -- never reaches it. There is no annotation to lie about and no object to
// ban; the function simply ran at shutdown, read an already-destroyed global, and
// nothing looked at it.
//
// It needs no annotation to be checked: declaring a function a shutdown handler IS the
// declaration that it runs then, so it is held to the same rules as a verified
// destructor. (`std::atexit` is the same shape, but is currently caught incidentally by
// -Wlifetime-safety-unannotated-indirection on its function-pointer parameter.)
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

extern std::string g_str;

// Runs during shutdown. The priority makes it run after g_str is destroyed.
__attribute__((destructor(101))) static void late() { sink = g_str[0]; }

std::string g_str = "0123456789012345678901234567890123456789012345678901234567890123456789";

int main() { return 0; }
