int f01(int a, int b, int c) { return a + b + c; }
int f02(int a, int b, int c) { return a + b + c; }
int f03(int a, int b, int c) { return a + b + c; }
int f04(int a, int b, int c) { return a + b + c; }
int f05(int a, int b, int c) { return a + b + c; }
int f06(int a, int b, int c) { return a + b + c; }
int f07(int a, int b, int c) { return a + b + c; }
int f08(int a, int b, int c) { return a + b + c; }
int f09(int a, int b, int c) { return a + b + c; }
int f10(int a, int b, int c) { return a + b + c; }
int f11(int a, int b, int c) { return a + b + c; }
int f12(int a, int b, int c) { return a + b + c; }

struct S {
  int f01(int a, int b, int c);
  int f02(int a, int b, int c);
  int f03(int a, int b, int c);
  int f04(int a, int b, int c);
  int f05(int a, int b, int c);
  int f06(int a, int b, int c);
  int f07(int a, int b, int c);
  int f08(int a, int b, int c);
  int f09(int a, int b, int c);
  int f10(int a, int b, int c);
  int f11(int a, int b, int c);
  int f12(int a, int b, int c);
};

int S::f01(int a, int b, int c) { return a + b + c; }
int S::f02(int a, int b, int c) { return a + b + c; }
int S::f03(int a, int b, int c) { return a + b + c; }
int S::f04(int a, int b, int c) { return a + b + c; }
int S::f05(int a, int b, int c) { return a + b + c; }
int S::f06(int a, int b, int c) { return a + b + c; }
int S::f07(int a, int b, int c) { return a + b + c; }
int S::f08(int a, int b, int c) { return a + b + c; }
int S::f09(int a, int b, int c) { return a + b + c; }
int S::f10(int a, int b, int c) { return a + b + c; }
int S::f11(int a, int b, int c) { return a + b + c; }
int S::f12(int a, int b, int c) { return a + b + c; }

struct S s;
