struct Func {
  // change
  int change_return_type();

  // add or remove
  int add_par();
  int remove_par(int remove);
  int change_par_type(int par);
  int rename_old();

  // no diff
  int change_par_name(int remove);

  int x;
} var;

int Func::change_return_type() { return 0; }
int Func::add_par() { return 0; }
int Func::remove_par(int remove) { return remove; }
int Func::change_par_type(int par) { return par; }
int Func::rename_old() { return 0; }
int Func::change_par_name(int remove) { return remove; }
