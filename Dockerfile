# lsmeng - reproducible build/test environment.
#
# WHY A CONTAINER AT ALL:
#   Every correctness claim in docs/SPEC.md is about the filesystem: fsync ordering,
#   atomic rename, whether a directory fsync is needed, flock semantics, pread vs read,
#   torn-tail recovery after kill -9. All of those are OS- and filesystem-specific, and
#   macOS/APFS and Linux/ext4 disagree on several. Pinning the OS is correctness, not
#   tidiness. (SPEC 8)
#
# WHY UBUNTU 24.04:
#   glibc 2.39 + GCC 13 - full C++20 without a toolchain scavenger hunt, and a mainstream
#   LTS a reviewer can reproduce exactly.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Toolchain, plus the tools we will actually need when this breaks at 2am:
#   gdb                  - a deadlock's thread stacks, a corrupted SST entry
#   valgrind             - memcheck/helgrind/DRD/massif/cachegrind        (SPEC 10.4)
#   linux-tools-generic  - perf. Expected to lack hardware counters under the LinuxKit
#                          VM; T0 measures whether that prediction holds  (SPEC 10.4)
#   xxd / binutils/file  - hexdump-adjacent forensics on our own file formats
#   python3              - benchmark plotting and corpus generation (not a build dep)
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      g++ \
      cmake \
      ninja-build \
      gdb \
      valgrind \
      linux-tools-generic \
      binutils \
      file \
      xxd \
      python3 \
      git \
      ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work

# SPEC 8: database directories used by tests and benchmarks live on a container-local
# volume, NOT the bind mount. The bind mount is where wanrep B2 measured flock failing to
# exclude, and where fsync latency is least representative of anything. Only source and
# results are bind-mounted; /data is container-local.
RUN mkdir -p /data
ENV LSMENG_TEST_DIR=/data

# Root inside a throwaway, network-less dev container. Documented so it reads as a
# decision rather than an oversight: we need to kill -9 our own children in crash tests
# and to drop caches for honest I/O benchmarks.
CMD ["/bin/bash"]
