struct nested {
  int x;
};

struct containing {
  struct nested inner;
};

struct referring {
  struct nested * inner;
};

void register_ops6(containing) { }
void register_ops7(containing*) { }
void register_ops8(referring) { }
void register_ops9(referring*) { }
