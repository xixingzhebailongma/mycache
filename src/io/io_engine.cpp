#include "mycache/io/io_engine.h"

#include "mycache/common/logger.h"
#include "mycache/io/posix_io.h"
#ifdef MYCACHE_HAVE_IO_URING
#include "mycache/io/uring_io.h"
#endif

namespace mycache {

std::unique_ptr<IOEngine> CreateIOEngine(const std::string& name) {
#ifdef MYCACHE_HAVE_IO_URING
  if (name == "io_uring") {
    auto e = std::make_unique<UringIO>();
    if (e->valid()) return e;
    LOG_WARN("io_uring init failed, falling back to posix");
    return std::make_unique<PosixIO>();
  }
#else
  if (name == "io_uring") {
    LOG_WARN("io_uring requested but liburing is not compiled in, falling back to posix");
  }
#endif
  return std::make_unique<PosixIO>();
}

}  // namespace mycache
