const unsigned long l = 3;

char foo(long arr[sizeof(l)]) {
  (void) arr;
  return 0;
}

char bar(long baz[l]) {
  (void) baz;
  return 1;
}

char quux(unsigned int m, double d[m]) {
  (void) d;
  return 2;
}

char spong(unsigned int m, double d[l]) {
  (void) m;
  (void) d;
  return 3;
}

char wibble(unsigned int a, unsigned int b, char arr[b]) {
  (void) a;
  (void) arr;
  return 4;
}
