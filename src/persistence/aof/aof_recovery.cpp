#include "mycache/persistence/aof/aof_recovery.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <vector>

#include "mycache/common/logger.h"
#include "mycache/persistence/aof/aof_format.h"
#include "mycache/protocol/resp_parser.h"
#include "mycache/storage/db/shard.h"
#include "mycache/storage/object/string_object.h"

namespace mycache {

namespace {

// 从 fd 精确读取 len 字节；返回实际读取的字节数，<0 表示出错。
ssize_t ReadFull(int fd, void* buf, size_t len) {
  size_t done = 0;
  char* p = static_cast<char*>(buf);
  while (done < len) {
    ssize_t n = ::read(fd, p + done, len - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) break;
    done += static_cast<size_t>(n);
  }
  return static_cast<ssize_t>(done);
}

// 从缓冲区头部解析记录头（小端）。
aof::RecordHeader DecodeHeader(const char* buf) {
  aof::RecordHeader h;
  const auto* b = reinterpret_cast<const unsigned char*>(buf);
  h.len = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
          (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
  h.crc = static_cast<uint32_t>(b[4]) | (static_cast<uint32_t>(b[5]) << 8) |
          (static_cast<uint32_t>(b[6]) << 16) | (static_cast<uint32_t>(b[7]) << 24);
  return h;
}

}  // namespace

AofRecoveryResult AofRecovery::Replay(
    const std::string& path,
    const std::function<bool(const std::string&, const std::vector<std::string>&)>& apply) {
  AofRecoveryResult r;

  int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) {
      r.ok = true;  // 还没有 AOF 文件，视为空。
      return r;
    }
    r.error = std::string("open failed: ") + std::strerror(errno);
    return r;
  }

  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    r.error = std::string("fstat failed: ") + std::strerror(errno);
    ::close(fd);
    return r;
  }
  const size_t size = static_cast<size_t>(st.st_size);

  // 读入整个文件（学习项目：AOF 体积可控，一次性载入简化尾部判断）。
  std::vector<char> buf(size);
  ssize_t got = ReadFull(fd, buf.data(), size);
  if (got < 0 || static_cast<size_t>(got) != size) {
    r.error = "failed to read AOF file";
    ::close(fd);
    return r;
  }

  size_t pos = 0;       // 下一个待处理记录的起始偏移
  bool truncated = false;

  if (size == 0) {
    r.ok = true;
    ::close(fd);
    return r;
  }

  // ---- 文件头 ----
  if (size < aof::kMagicSize) {
    // 首次写入即中断，只有部分文件头：整文件截断为空。
    truncated = true;
    pos = 0;
  } else if (std::memcmp(buf.data(), aof::kMagic, aof::kMagicSize) != 0) {
    r.error = "not a mycache AOF file (bad magic)";
    ::close(fd);
    return r;
  } else {
    pos = aof::kMagicSize;
  }

  // ---- 逐条记录 ----
  while (pos + sizeof(aof::RecordHeader) <= size) {
    aof::RecordHeader h = DecodeHeader(buf.data() + pos);
    const size_t payload_off = pos + sizeof(aof::RecordHeader);

    // 记录头声称的载荷长度超出文件剩余字节（包括 kill -9 写坏的半条记录，或头部
    // 本身就是垃圾）：当作尾部不完整记录，截断丢弃。
    if (payload_off + h.len > size) {
      truncated = true;
      break;
    }

    const char* payload = buf.data() + payload_off;
    if (aof::Crc32(payload, h.len) != h.crc) {
      if (payload_off + h.len == size) {
        // 最后一条记录校验失败：当作尾部损坏，截断丢弃。
        truncated = true;
        break;
      }
      r.error = "AOF checksum mismatch in middle of file";
      ::close(fd);
      return r;
    }

    // 解析载荷为一条 RESP 命令。
    RespValue cmd;
    size_t consumed = 0;
    ParseResult pr = RespParser().Parse(payload, h.len, &consumed, &cmd);
    if (pr != ParseResult::kOk || cmd.type != RespType::kArray || cmd.array.empty() ||
        consumed != h.len) {
      r.error = "invalid RESP command in AOF record";
      ::close(fd);
      return r;
    }

    cmd.Materialize();  // AOF recovery 从文件解析：物化 view 后读 str

    std::string name = cmd.array[0].str;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    std::vector<std::string> args;
    args.reserve(cmd.array.size() - 1);
    for (size_t i = 1; i < cmd.array.size(); ++i) {
      if (cmd.array[i].type != RespType::kBulkString) {
        r.error = "non-bulk-string argument in AOF command";
        ::close(fd);
        return r;
      }
      args.push_back(cmd.array[i].str);
    }

    if (!apply(name, args)) {
      r.error = "cannot replay AOF command '" + name + "'";
      ::close(fd);
      return r;
    }
    r.commands_replayed++;
    pos = payload_off + h.len;
  }

  if (pos < size) truncated = true;  // 尾部残留（不足一个完整记录头）

  if (truncated) {
    r.bytes_truncated = size - pos;
    if (::ftruncate(fd, static_cast<off_t>(pos)) != 0) {
      r.error = std::string("ftruncate failed: ") + std::strerror(errno);
      ::close(fd);
      return r;
    }
    LOG_WARN("aof recovery: truncated %llu trailing bytes at offset %llu",
             static_cast<unsigned long long>(r.bytes_truncated),
             static_cast<unsigned long long>(pos));
  }

  r.ok = true;
  ::close(fd);
  return r;
}

bool ApplyAofCommand(Shard* shard, const std::string& name,
                     const std::vector<std::string>& args) {
  if (args.empty()) return false;

  if (name == "set" && args.size() == 2) {
    shard->Set(args[0], args[1]);
    return true;
  }
  if (name == "del") {
    shard->Del(args);
    return true;
  }
  if (name == "hset" && args.size() == 3) {
    shard->HSet(args[0], args[1], args[2]);
    return true;
  }
  if (name == "hdel") {
    std::vector<std::string> fields(args.begin() + 1, args.end());
    shard->HDel(args[0], fields);
    return true;
  }
  if (name == "pexpireat" && args.size() == 2) {
    long long when = 0;
    if (!StringToLongLong(args[1].data(), args[1].size(), &when)) return false;
    shard->ExpireAt(args[0], static_cast<int64_t>(when));
    return true;
  }
  if (name == "lpush" && args.size() >= 2) {
    std::vector<std::string> values(args.begin() + 1, args.end());
    shard->LPush(args[0], values);
    return true;
  }
  if (name == "rpush" && args.size() >= 2) {
    std::vector<std::string> values(args.begin() + 1, args.end());
    shard->RPush(args[0], values);
    return true;
  }
  if (name == "lpop" && args.size() == 1) {
    std::string out;
    shard->LPop(args[0], &out);
    return true;
  }
  if (name == "rpop" && args.size() == 1) {
    std::string out;
    shard->RPop(args[0], &out);
    return true;
  }
  return false;
}

}  // namespace mycache
