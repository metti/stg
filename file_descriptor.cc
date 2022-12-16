#include "file_descriptor.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <exception>

#include "error.h"

namespace stg {

FileDescriptor::FileDescriptor(const char* filename, int flags, mode_t mode)
    : fd_(open(filename, flags, mode)) {
  if (fd_ < 0) {
    Die() << "open failed: " << ErrnoToString(errno);
  }
}

FileDescriptor::~FileDescriptor() noexcept(false) {
  // If we're unwinding, ignore any close failure.
  if (fd_ >= 0 && close(fd_) != 0 && !std::uncaught_exception()) {
    Die() << "close failed: " << ErrnoToString(errno);
  }
  fd_ = -1;
}


int FileDescriptor::Value() const {
  Check(fd_ >= 0) << "FileDescriptor was not initialized";
  return fd_;
}

}  // namespace stg
