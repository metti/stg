struct foo {
};

void bar_2(struct foo* y) {
  (void) y;
}

void bar(const volatile struct foo* y) {
  (void) y;
}

void baz(void(*const volatile y)(const volatile struct foo*)) {
  (void) y;
}

void(*const volatile quux)(const volatile struct foo*) = &bar;
