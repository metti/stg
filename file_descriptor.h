#ifndef STG_FILE_DESCRIPTOR_H_
#define STG_FILE_DESCRIPTOR_H_

#include <sys/stat.h>  // for mode_t

#include <utility>

namespace stg {

// RAII wrapper over file descriptor
class FileDescriptor {
 public:
  FileDescriptor() {}
  FileDescriptor(const char* filename, int flags, mode_t mode = 0);
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  FileDescriptor(FileDescriptor&& other) {
    std::swap(fd_, other.fd_);
  }
  FileDescriptor& operator=(FileDescriptor&& other) = delete;
  ~FileDescriptor() noexcept(false);

  int Value() const;

 private:
  int fd_ = -1;
};

}  // namespace stg

#endif  // STG_FILE_DESCRIPTOR_H_
