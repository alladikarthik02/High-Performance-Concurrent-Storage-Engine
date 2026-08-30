// A temporary directory that cleans itself up, and that lives on the CONTAINER-LOCAL
// filesystem rather than the bind mount.
//
// SPEC 8: the bind mount is where wanrep B2 measured flock failing to exclude, and where
// fsync latency is least representative of anything. Every test database therefore lives
// under $LSMENG_TEST_DIR (set to /data in the Dockerfile), falling back to /tmp so the
// tests still run outside the container.
#pragma once

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "lsmeng/env.h"

namespace testing {

class TmpDir {
 public:
  explicit TmpDir(const std::string& tag) {
    const char* root = std::getenv("LSMENG_TEST_DIR");
    if (!root || !*root) root = "/tmp";
    static int counter = 0;
    path_ = std::string(root) + "/lsmeng-" + tag + "-" + std::to_string(::getpid()) + "-" +
            std::to_string(counter++);
    ::mkdir(root, 0755);
    Remove();
    ::mkdir(path_.c_str(), 0755);
  }
  ~TmpDir() { Remove(); }
  TmpDir(const TmpDir&) = delete;
  TmpDir& operator=(const TmpDir&) = delete;

  const std::string& path() const { return path_; }
  std::string file(const std::string& name) const { return path_ + "/" + name; }

 private:
  void Remove() {
    auto* env = ::lsmeng::Env::Default();
    std::vector<std::string> kids;
    if (env->GetChildren(path_, &kids).ok())
      for (const auto& k : kids) env->DeleteFile(path_ + "/" + k);
    ::rmdir(path_.c_str());
  }
  std::string path_;
};

}  // namespace testing
