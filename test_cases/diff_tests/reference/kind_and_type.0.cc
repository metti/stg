struct foo {
  int x;
};

struct boo {
  foo& lref_to_ptr;
  foo* ptr_to_lref;

  foo&& rref_to_ptr;
  foo* ptr_to_rref;

  foo& lref_to_rref;
  foo&& rref_to_lref;
};

void func(boo a) {
  (void) a;
}
