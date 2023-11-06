struct nested {
  int x;
};

struct containing {
  struct nested inner;
};

struct referring {
  struct nested * inner;
};

void register_ops6(struct containing y) { (void) y; }
void register_ops7(struct containing* y) { (void) y; }
void register_ops8(struct referring y) { (void) y; }
void register_ops9(struct referring* y) { (void) y; }
