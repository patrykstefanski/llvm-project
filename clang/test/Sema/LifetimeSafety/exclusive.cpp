// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety -Wlifetime-safety-exclusivity -Wno-dangling -verify %s

#include "Inputs/lifetime-analysis.h"

// The three access kinds of the [[clang::lifetime_exclusive]] model:
//  - const T&: shared immutable.
//  - T&: shared mutable; may write to subobjects but must not invalidate them.
//  - T& [[clang::lifetime_exclusive]]: exclusive; may invalidate (reallocate,
//    destroy, move out of) subobjects, like a Rust `&mut`.
int my_get(const std::vector<int> &v, unsigned i);
void my_set(std::vector<int> &v, unsigned i, int val);
void my_push_back(std::vector<int> &v [[clang::lifetime_exclusive]], int val);
void my_push_back_ref(std::vector<int> &v [[clang::lifetime_exclusive]],
                      const int &val);
void sink_vec(std::vector<int> &&v);
void use(int);

//===----------------------------------------------------------------------===//
// Caller side: passing to an exclusive parameter invalidates other borrows.
//===----------------------------------------------------------------------===//
namespace CallerSide {
void ok() {
  std::vector<int> v;
  int &ref = v[5];
  std::span<int> sp(v);
  my_set(v, 5, 123); // shared mutable: nothing is invalidated.
  use(ref);
  use(sp[5]);
  use(my_get(v, 5));
}

void bug() {
  std::vector<int> v;
  int &ref = v[5];      // expected-warning {{local variable 'v' is later invalidated}} \
                        // expected-note {{expression aliases the storage of local variable 'v' because the implicit object parameter is lifetimebound}}
  std::span<int> sp(v); // expected-warning {{local variable 'v' is later invalidated}}
  my_push_back(v, 123); // expected-note 2 {{local variable 'v' is invalidated here}}
  use(ref);             // expected-note {{later used here}}
  use(sp[5]);           // expected-note {{later used here}}
  use(my_get(v, 5));    // ok: borrowed after the invalidation.
}

// A reference to the container object itself is not invalidated by an
// interior invalidation; only references into the container are.
void alias_to_object_stays_valid() {
  std::vector<int> v;
  std::vector<int> &alias = v;
  my_push_back(v, 1);
  use(my_get(alias, 0)); // ok
  my_push_back(alias, 2);
  use(my_get(v, 0)); // ok
}

// Exclusive access is incompatible with another argument of the same call
// aliasing the object (cf. the classic `v.push_back(v[0])`).
void same_call_aliasing() {
  std::vector<int> v;
  my_push_back_ref(v, v[0]); // expected-warning {{argument aliases the object that parameter 'v' of 'my_push_back_ref' requires exclusive access to}} \
                             // expected-note {{parameter 'v' of 'my_push_back_ref' requires exclusive access here}}
}

void same_call_no_aliasing() {
  std::vector<int> v, w;
  my_push_back_ref(v, w[0]); // ok: different object
  my_push_back(v, v[0]);     // ok: the element is copied before the call
}
} // namespace CallerSide

//===----------------------------------------------------------------------===//
// Callee side: invalidating through a non-exclusive parameter is a violation.
//===----------------------------------------------------------------------===//
namespace CalleeSide {
void impl_ok(std::vector<int> &v [[clang::lifetime_exclusive]], int val) {
  v.push_back(val);
}

void impl_bad(std::vector<int> &v, int val) { // expected-warning {{parameter 'v' is not marked [[clang::lifetime_exclusive]] but the object it refers to is invalidated}}
  v.push_back(val);                           // expected-note {{member function 'push_back' requires exclusive access here}}
}

void forward_ok(std::vector<int> &v [[clang::lifetime_exclusive]]) {
  my_push_back(v, 1);
}

void forward_bad(std::vector<int> &v) { // expected-warning {{parameter 'v' is not marked [[clang::lifetime_exclusive]] but the object it refers to is invalidated}}
  my_push_back(v, 1);                   // expected-note {{parameter 'v' of 'my_push_back' requires exclusive access here}}
}

// Repeated invalidation through the same exclusive parameter is fine: the
// parameter refers to the container, not into it.
void repeated(std::vector<int> &v [[clang::lifetime_exclusive]]) {
  v.push_back(1);
  v.push_back(2);
  my_push_back(v, 3);
}

// But a borrow into the container taken before the invalidation dangles.
void local_borrow(std::vector<int> &v [[clang::lifetime_exclusive]]) { // expected-warning {{parameter 'v' is later invalidated}}
  int &r = v[0];
  v.push_back(1); // expected-note {{parameter 'v' is invalidated here}}
  use(r);         // expected-note {{later used here}}
}

// An owned (by-value) container may be invalidated freely.
void owned(std::vector<int> v) {
  v.push_back(1);
  my_push_back(v, 2);
}
} // namespace CalleeSide

//===----------------------------------------------------------------------===//
// Views: a by-value view parameter refers to the caller's objects.
//===----------------------------------------------------------------------===//
namespace Views {
void ok(std::span<std::vector<int>> sp [[clang::lifetime_exclusive]]) {
  my_set(sp[1], 5, 123);
  my_push_back(sp[1], 42);
  sink_vec(std::move(sp[1]));
}

void bug(std::span<std::vector<int>> sp) { // expected-warning {{parameter 'sp' is not marked [[clang::lifetime_exclusive]] but the object it refers to is invalidated}}
  my_set(sp[1], 5, 123);                   // ok
  my_push_back(sp[1], 42);                 // expected-note {{parameter 'v' of 'my_push_back' requires exclusive access here}}
  sink_vec(std::move(sp[2]));              // (second violation of the same parameter is not repeated)
}

void move_bug(std::span<std::vector<int>> sp) { // expected-warning {{parameter 'sp' is not marked [[clang::lifetime_exclusive]] but the object it refers to is invalidated}}
  sink_vec(std::move(sp[2]));                   // expected-note {{rvalue reference parameter of 'sink_vec' requires exclusive access here}}
}
} // namespace Views

//===----------------------------------------------------------------------===//
// User containers: the attribute on the implicit object parameter replaces the
// hard-coded standard-library allowlist.
//===----------------------------------------------------------------------===//
namespace UserContainer {
struct [[gsl::Owner(int)]] MyVec {
  int &operator[](unsigned) [[clang::lifetimebound]];
  void set(unsigned i, int val);
  void push_back(int val) [[clang::lifetime_exclusive]];
  void grow_ok() [[clang::lifetime_exclusive]];
  void grow_bad();
};

void caller() {
  MyVec mv;
  int &r = mv[0]; // expected-warning {{local variable 'mv' is later invalidated}} \
                  // expected-note {{expression aliases the storage of local variable 'mv' because the implicit object parameter is lifetimebound}}
  mv.set(0, 1);   // ok
  mv.push_back(1); // expected-note {{local variable 'mv' is invalidated here}}
  use(r);          // expected-note {{later used here}}
}

void MyVec::grow_ok() { push_back(1); }

void MyVec::grow_bad() { // expected-warning {{implicit object parameter is not marked [[clang::lifetime_exclusive]] but the object it refers to is invalidated}}
  push_back(1);          // expected-note {{member function 'push_back' requires exclusive access here}}
}
} // namespace UserContainer
