struct foo {
  long x;
};

struct boo {
  foo* ptr1;
  foo& lref1;
  foo&& rref1;

  char* ptr2;
  char& lref2;
  char&& rref2;
};

void func(boo a) {
  (void) a;
}
