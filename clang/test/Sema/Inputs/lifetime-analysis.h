
// These declarations stand in for the standard library, so they must be treated as
// library-owned code: several checks now ask whether a declaration in namespace `std`
// was written by the LIBRARY (a system header) rather than by the user, because a user
// specialization of a standard template is legal C++ and lands in `std` too. Without
// this the stubs would be judged as user code and behave unlike the real libc++ they
// model.
#pragma clang system_header

namespace __gnu_cxx {
template <typename T>
struct basic_iterator {
  basic_iterator operator++();
  basic_iterator operator++(int);
  basic_iterator operator--();
  basic_iterator operator--(int);
  basic_iterator operator+(int);
  basic_iterator operator-(int);
  T& operator*() const;
  T* operator->() const;
};

template<typename T>
bool operator==(basic_iterator<T>, basic_iterator<T>);
template<typename T>
bool operator!=(basic_iterator<T>, basic_iterator<T>);
template<typename T>
basic_iterator<T> operator+(int, basic_iterator<T>);
template<typename T>
basic_iterator<T> operator-(int, basic_iterator<T>);
}

namespace std {
template<typename T> struct remove_reference       { typedef T type; };
template<typename T> struct remove_reference<T &>  { typedef T type; };
template<typename T> struct remove_reference<T &&> { typedef T type; };

template< class InputIt, class T >
InputIt find( InputIt first, InputIt last, const T& value );

// Algorithms that CALL what they are handed: the functor's operator(), and the
// iterators' operator*, operator++ and operator!=, all run from inside the library.
template< class InputIt, class UnaryFunc >
UnaryFunc for_each( InputIt first, InputIt last, UnaryFunc f );
template< class InputIt, class Pred >
bool any_of( InputIt first, InputIt last, Pred p );

template< class ForwardIt1, class ForwardIt2 >
ForwardIt1 search( ForwardIt1 first, ForwardIt1 last,
                   ForwardIt2 s_first, ForwardIt2 s_last );

template<typename T>
typename remove_reference<T>::type &&move(T &&t) noexcept;

template<typename T>
T &&forward(typename remove_reference<T>::type &t) noexcept;

template <typename C>
auto data(const C &c) -> decltype(c.data());

template <typename C>
auto begin(C &c) -> decltype(c.begin());
template <typename C>
auto end(C &c) -> decltype(c.end());

template<typename T, int N>
T *begin(T (&array)[N]);

using size_t = decltype(sizeof(0));
using nullptr_t = decltype(nullptr);

template<typename T>
struct initializer_list {
  const T* ptr; size_t sz;
};
template<typename T> class allocator {};

template <typename Iterator>
struct reverse_iterator {
  reverse_iterator operator++();
  reverse_iterator operator++(int);
  reverse_iterator operator--();
  reverse_iterator operator--(int);
  reverse_iterator operator+(int) const;
  reverse_iterator operator-(int) const;
  decltype(*Iterator()) operator*() const;
};

template <typename Iterator>
reverse_iterator<Iterator> operator+(int, reverse_iterator<Iterator>);
template <typename Iterator>
reverse_iterator<Iterator> operator-(int, reverse_iterator<Iterator>);

template <typename T, typename Alloc = allocator<T>>
struct vector {
  using iterator = __gnu_cxx::basic_iterator<T>;
  using const_iterator = __gnu_cxx::basic_iterator<const T>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;
  iterator begin();
  iterator end();
  const_iterator cbegin() const;
  const_iterator cend() const;
  reverse_iterator rbegin();
  reverse_iterator rend();
  const_reverse_iterator crbegin() const;
  const_reverse_iterator crend() const;
  const T *data() const;
  vector();
  ~vector();
  vector(initializer_list<T> __l,
         const Alloc& alloc = Alloc());

  template<typename InputIterator>
	vector(InputIterator first, InputIterator __last);

  T& operator[](unsigned);
  const T& operator[](unsigned) const;

  T &  at(int n) &;
  T && at(int n) &&;

  void push_back(const T&);
  void push_back(T&&);
  template<typename... Args> T& emplace_back(Args&&... args);
  size_t size() const;
  const T& back() const;
  void pop_back();
  iterator insert(iterator, T&&);
  void resize(size_t);
  void erase(iterator);
  void clear();
};

template<class T>
void swap( T& a, T& b );

template<typename A, typename B>
struct pair {
  A first;
  B second;
};

template<class Key,class T>
struct flat_map {
  using iterator = __gnu_cxx::basic_iterator<std::pair<const Key, T>>;
  T& operator[](const Key& key);
  iterator begin();
  iterator end();
  iterator find(const Key& key);
  iterator erase(iterator);
};

template<class Key,class T>
struct unordered_map {
  using iterator = __gnu_cxx::basic_iterator<std::pair<const Key, T>>;
  T& operator[](const Key& key);
  iterator begin();
  iterator end();
  iterator find(const Key& key);
  iterator erase(iterator);
};

template<class Key>
struct set {
  using iterator = __gnu_cxx::basic_iterator<const Key>;
  iterator begin();
  iterator end();
  void insert(const Key& key);
  iterator erase(iterator);
  void extract(iterator);
  void clear();
};

template<class Key>
struct multiset {
  using iterator = __gnu_cxx::basic_iterator<const Key>;
  iterator begin();
  iterator end();
  void insert(const Key& key);
  void clear();
};

template<class Key, class T>
struct map {
  using iterator = __gnu_cxx::basic_iterator<std::pair<const Key, T>>;
  T& operator[](const Key& key);
  iterator begin();
  iterator end();
  void insert(const std::pair<const Key, T>& value);
  template<class... Args>
  void emplace(Args&&... args);
  iterator erase(iterator);
  void clear();
};

template<class Key, class T>
struct multimap {
  using iterator = __gnu_cxx::basic_iterator<std::pair<const Key, T>>;
  iterator begin();
  iterator end();
  void insert(const std::pair<const Key, T>& value);
  void clear();
};

template<typename T>
struct basic_string_view {
  basic_string_view();
  basic_string_view(const T *);
  const T *begin() const;
  const T *end() const;
  const T *data() const;
  int size() const;
};
using string_view = basic_string_view<char>;

template <typename T>
basic_string_view<T>::basic_string_view(const T *) {}

template<typename T>
struct span {
  span();
  span(const vector<T>&);
  const T *begin() const;
  const T *end() const;
};

template<class _Mystr> struct iter {
    iter& operator-=(int);

    iter operator-(int _Off) const {
        iter _Tmp = *this;
        return _Tmp -= _Off;
    }
};

template<typename T>
struct basic_string {
  basic_string();
  basic_string(const basic_string<T> &);
  basic_string(basic_string<T> &&);
  basic_string(const T *);
  ~basic_string();
  basic_string& operator=(const basic_string&);
  basic_string& operator+=(const basic_string&);
  basic_string& operator+=(const T*);
  basic_string& operator+=(T);
  void push_back(T);

  template<class StringViewLike> basic_string& insert(size_t index, const StringViewLike&);

  void clear();
  const T *c_str() const;
  operator basic_string_view<T> () const;
  using const_iterator = iter<T>;
  const T *data() const;
};
using string = basic_string<char>;

template <class T>
basic_string<T> operator+(const basic_string<T> &, const basic_string<T> &);
template <class T> basic_string<T> operator+(const basic_string<T> &, const T *);
template <class T> basic_string<T> operator+(const T *, const basic_string<T> &);
template <class T> basic_string<T> operator+(const basic_string<T> &, T);

template<typename T>
struct unique_ptr {
  unique_ptr();
  explicit unique_ptr(T*);
  unique_ptr(unique_ptr<T>&&);
  unique_ptr& operator=(unique_ptr<T>&&);
  unique_ptr& operator=(std::nullptr_t);
  void reset();
  ~unique_ptr();
  T* release();
  // const-qualified, as in libc++: `const` applies to the unique_ptr, not to
  // what it owns, so a const unique_ptr still hands out mutable access to its
  // pointee. Modelling these as non-const made a const unique_ptr
  // undereferenceable and hid that distinction.
  T &operator*() const;
  T *operator->() const;
  T *get() const;
};

template<typename T, typename... Args>
unique_ptr<T> make_unique(Args&&... args) {
  return unique_ptr<T>(new T(args...));
}

template <class T>
void destroy_at(T *);

template<typename T>
struct shared_ptr {
  shared_ptr();
  explicit shared_ptr(T*);
  shared_ptr(const shared_ptr<T>&);
  shared_ptr(shared_ptr<T>&&);
  
  template<typename U>
  shared_ptr(unique_ptr<U>&& up) : ptr_(up.get()) { up.release(); }

  ~shared_ptr();
  T &operator*();
  T *operator->();
  T *get() const;
  T* ptr_;
};

template<typename T>
struct optional {
  optional();
  optional(const T&);

  ~optional();

  template<typename U = T>
  optional(U&& t);

  template<typename U>
  optional(optional<U>&& __t);

  T *operator->();
  T &operator*() &;
  T &&operator*() &&;
  T &value() &;
  T &&value() &&;
};
template<typename T>
optional<__decay(T)> make_optional(T&&);


template<typename T>
struct stack {
  T &top();
};

struct any {
  // FIXME: CFG based analysis should be able to catch bugs without need of ctor and dtor.
  any();
  ~any();
};

template<typename T>
T any_cast(const any& operand);

template<typename T>
struct reference_wrapper {
  template<typename U>
  reference_wrapper(U &&);
};

template<typename T>
reference_wrapper<T> ref(T& t) noexcept;

template <typename T>
struct [[gsl::Pointer]] iterator {
  T& operator*() const;
};

struct false_type {
    static constexpr bool value = false;
    constexpr operator bool() const noexcept { return value; }
};
struct true_type {
    static constexpr bool value = true;
    constexpr operator bool() const noexcept { return value; }
};

template<class T> struct is_pointer : false_type {};
template<class T> struct is_pointer<T*> : true_type {};
template<class T> struct is_pointer<T* const> : true_type {};

template<class> class function;
template<class R, class... Args>
class function<R(Args...)> {
public:
  template<class F> function(F) {}
  function(const function&) {}
  function(function&&) {}
  template<class F> function& operator=(F) { return *this; }
  function& operator=(const function&) { return *this; }
  function& operator=(function&&) { return *this; }
  R operator()(Args...) const;
  ~function();
};

// Minimal variadic stand-in, recognized by name. Its alternatives arrive as a single
// template Pack, which is the path a check following template arguments has to look
// inside. The value lives in a type-erased buffer the analysis does not expand, so a
// view alternative is untracked; the destructor is non-trivial so the type is also
// relevant to destruction-order questions.
template <class... Ts> class variant {
  char buf[32];

public:
  variant();
  ~variant();
};

// A container that is TRIVIALLY DESTRUCTIBLE whenever its element is -- it keeps the
// elements in its own storage, so nothing needs destroying -- while still CONSTRUCTING
// them inside the library. That combination is what leaves a temporary of it with no
// CXXBindTemporaryExpr, which is the node the construction question used to hang on, so
// creating one ran every element's constructor unasked. libc++'s std::optional is the
// real instance: trivially destructible exactly when its value type is. Deliberately not
// an aggregate -- an aggregate's element constructors appear in the caller's AST and are
// reported precisely, which is the case that already worked.
template <class T, unsigned long N> struct fixed_vector {
  fixed_vector();
  explicit fixed_vector(unsigned long);
  T &operator[](unsigned long);
  const T &operator[](unsigned long) const;
  unsigned long size() const;
};

// A non-template POLYMORPHIC library base that users derive from -- std::pmr::memory_resource
// and std::streambuf are the real instances. A derived type reaches the library only as the
// implicit object argument of a call to an inherited member: it appears in no template
// argument, so a walk over those never sees it, and the conversion such a call inserts means
// the receiver's reported type is this base rather than the derived type as written.
struct polymorphic_base {
  virtual ~polymorphic_base();
  int run();
  virtual int hook() = 0;
};

// Minimal stand-in for the template users most often specialize for their own types.
// A user specialization of it is legal C++ and lands in namespace `std`, which is why
// trust has to key on whether the LIBRARY wrote a declaration, not on its namespace.
template <class T> struct hash {
  unsigned long operator()(const T &) const;
};

// Minimal owning pointer with a DELETER argument: the library CALLS that argument
// rather than destroying it, which is what makes it a hook. (The unique_ptr above
// takes no deleter.)
template <class T, class D> struct owner_with_deleter {
  ~owner_with_deleter();
};

}

void *operator new(std::size_t, void *) noexcept;
void *operator new[](std::size_t, void *) noexcept;
