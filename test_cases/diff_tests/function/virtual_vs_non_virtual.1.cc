struct VirtualToNormal {
  void print();
} virtual_to_normal;
void VirtualToNormal::print() {}

struct NormalToVirtual {
  virtual void print();
} normal_to_virtual;
void NormalToVirtual::print() {}
