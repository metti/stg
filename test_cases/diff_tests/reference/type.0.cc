struct foo {
  int x;
};

struct boo {
  foo* ptr1;
  foo& lref1;
  foo&& rref1;

  int* ptr2;
  int& lref2;
  int&& rref2;
};

void func(boo a) {
  (void) a;
}
