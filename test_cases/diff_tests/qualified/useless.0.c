struct foo {
};

void bar(struct foo y) {
  (void) y;
}

void bar_1(const volatile struct foo* y) {
  (void) y;
}

void baz(void(*y)(struct foo)) {
  (void) y;
}

void(*quux)(struct foo) = &bar;
