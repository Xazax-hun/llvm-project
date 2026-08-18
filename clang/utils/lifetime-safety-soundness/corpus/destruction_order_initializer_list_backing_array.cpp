// DESC: a std::initializer_list at static storage duration, whose ELEMENTS are destroyed at
// shutdown by an unverified destructor. This is the "logger destroyed before its clients" bug
// the model claims to prevent by construction.
//
// Every rule keys on the declared type, and std::initializer_list is trivially destructible,
// so no annotation was demanded. But the list does not own its elements: the compiler
// synthesizes a SEPARATE backing array, also of static storage duration, and it is that
// array's `Client` elements which are destroyed at shutdown -- by __cxx_global_array_dtor.
// `~Client` was therefore arbitrary, never-verified shutdown code.
//
// So the object that actually runs code at shutdown has no declaration in the source at all;
// the declared type describes a view of it rather than the thing itself. Every sibling wrapper
// OWNS what it holds and was already rejected: vector<Client>, optional<Client>,
// unique_ptr<Client>, array<Client,2>, pair<int,Client>, a lambda capturing one,
// `static const Client &`, and thread_local Client. initializer_list was the one gap, and it
// is specific to the static-duration declaration path -- in a function-local or return-type
// position it already drew -Wlifetime-safety-unknown-ownership.
//
// An aggregate holding one is trivially destructible too, so the walk ended before reaching
// the member: that form needs no annotation anywhere, and it also passed the documented
// subobject rule for an annotated type.
// EXPECT-ASAN: heap-use-after-free
#include <initializer_list>
#include <string>

volatile char sink;

struct Client {
  ~Client();
};

// The backing array is constructed FIRST, so it is destroyed LAST -- after logbuf.
static std::initializer_list<Client> clients = {Client{}};

static std::string logbuf;

Client::~Client() { sink = (char)logbuf.find('z'); } // heap-use-after-free

int main() {
  logbuf.assign(1000, 'a');
  return 0;
}
