namespace unchanged_n {
int var;
}

namespace removed_n {
int var;
}

namespace foo {
int var_decl;

int array_decl[5];

extern const int qualif_decl = 5;

typedef int typedef_decl;

struct help {
  int* ptr_decl;
  int& lref_decl;
  int&& rref_decl;
  typedef_decl t;
};

int func_decl(help a) {
  (void)a;
  return 0;
}

struct StructDecl {
  int x;
} struct_decl;

union UnionDecl {
  int x;
} union_decl;

enum class EnumDecl { X, Y } enum_decl;

}  // namespace foo
