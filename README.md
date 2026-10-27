# mycache

## 构建

需要 CMake >= 3.16、C++17 编译器（googletest 在 configure 时自动拉取，需联网）。

```sh
./scripts/build.sh
```

等价手动构建：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Sanitizer 构建（可选）：

```sh
# ASan/UBSan
cmake -S . -B build-asan -DMYCACHE_ENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j

# ThreadSanitizer
cmake -S . -B build-tsan -DMYCACHE_ENABLE_TSAN=ON
cmake --build build-tsan -j
```

## 运行

```sh
./build/mycache-server conf/mycache.conf
# 或指定其它配置文件
./build/mycache-server /path/to/mycache.conf
```

用 `redis-cli` 验证：

```sh
redis-cli ping
redis-cli set foo bar
redis-cli get foo
```

端口在 `conf/mycache.conf` 的 `port` 修改（默认 6379）。

## 测试

```sh
./build/mycache_tests          # 单元测试
./build/mycache_crash_tests    # kill -9 崩溃恢复集成测试
ctest --test-dir build
```

脚本验收：

```sh
./scripts/acceptance.sh [port]
./scripts/eviction_test.sh [port]
./scripts/crash_test.sh [port]
./scripts/recovery_test.sh [port]
./scripts/rewrite_test.sh [port]
./scripts/rdb_test.sh [port]
./scripts/multi_key_test.sh [port]
./scripts/soak_test.sh [seconds] [port]
```

## 基准

```sh
./build/mycache_get_set_bench [-h host] [-p port] [-n nreq] [-d size]
./build/mycache_pipeline_bench [host] [port] [nreq]
./build/mycache_concurrency_bench [-h host] [-p port] [-c conns] [-t threads] [-n per_conn]
./build/mycache_aof_bench [n]
./build/mycache_io_bench [n]

redis-benchmark -t get,set -n 200000 -c 50 -d 64
```
