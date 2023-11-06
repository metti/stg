namespace unchanged_n {
int var;
}

namespace added_n {
int var;
}

// Difference: every int below changed to long
namespace foo {
long var_decl;

long array_decl[5];

extern const long qualif_decl = 5;

typedef long typedef_decl;

struct help {
  long* ptr_decl;
  long& lref_decl;
  long&& rref_decl;
  typedef_decl t;
};

long func_decl(help a) {
  (void)a;
  return 0;
}

struct StructDecl {
  long x;
} struct_decl;

union UnionDecl {
  long x;
} union_decl;

enum class EnumDecl { X, Y, Z } enum_decl;

}  // namespace foo
