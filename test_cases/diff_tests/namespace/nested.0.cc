namespace foo {
struct str {
  int x;
};
str var_foo;
}  // namespace foo

namespace n1 {
foo::str var_foo;
namespace n2 {
foo::str var_foo;
namespace n3 {
struct str {
  int x;
};
str var_n;
foo::str var_foo;
}  // namespace n3
}  // namespace n2
}  // namespace n1

foo::str var_foo;
n1::n2::n3::str var_n;
