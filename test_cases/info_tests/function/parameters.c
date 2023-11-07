int f01(int b, int c) { return b + c; }
int f02(int a, int c) { return a + c; }
int f03(int a, int b) { return a + b; }
int f04(int b, int a, int c) { return a + b + c; }
int f05(int a, int c, int b) { return a + b + c; }
int f06(int c, int b, int a) { return a + b + c; }
int f07(int b, int c, int a) { return a + b + c; }
int f08(int c, int a, int b) { return a + b + c; }
int f09(int d, int a, int b, int c) { return a + b + c + d; }
int f10(int a, int d, int b, int c) { return a + b + c + d; }
int f11(int a, int b, int d, int c) { return a + b + c + d; }
int f12(int a, int b, int c, int d) { return a + b + c + d; }

struct S {
  int (*f01)(int b, int c);
  int (*f02)(int a, int c);
  int (*f03)(int a, int b);
  int (*f04)(int b, int a, int c);
  int (*f05)(int a, int c, int b);
  int (*f06)(int c, int b, int a);
  int (*f07)(int b, int c, int a);
  int (*f08)(int c, int a, int b);
  int (*f09)(int d, int a, int b, int c);
  int (*f10)(int a, int d, int b, int c);
  int (*f11)(int a, int b, int d, int c);
  int (*f12)(int a, int b, int c, int d);
};

struct S s;
