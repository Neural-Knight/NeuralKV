#include "cluster/cluster_config.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include <unistd.h>

namespace neuralkv::cluster {
namespace {

// Writes contents to a fresh temp file and removes it on destruction.
class TempConfigFile {
 public:
  explicit TempConfigFile(const std::string& contents) {
    char pattern[] = "/tmp/nkv_cluster_conf_XXXXXX";
    const int fd = ::mkstemp(pattern);
    if (fd < 0) {
      std::perror("mkstemp");
      std::abort();
    }
    ::close(fd);
    path_ = pattern;

    std::ofstream out(path_, std::ios::trunc);
    out << contents;
  }

  ~TempConfigFile() { ::unlink(path_.c_str()); }

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

constexpr const char* kValidConfig = R"(
node_id=1
leader_id=1
peer 1 127.0.0.1 7401
peer 2 127.0.0.1 7402
peer 3 127.0.0.1 7403
)";

TEST(ClusterConfigTest, ParsesValidConfig) {
  TempConfigFile file(kValidConfig);
  ClusterConfig config;
  ASSERT_TRUE(LoadClusterConfig(file.path(), config).ok());

  EXPECT_EQ(config.local_node_id, 1u);
  EXPECT_EQ(config.leader_node_id, 1u);
  ASSERT_EQ(config.peers.size(), 3u);
  const PeerInfo* peer2 = config.FindPeer(2);
  ASSERT_NE(peer2, nullptr);
  EXPECT_EQ(peer2->host, "127.0.0.1");
  EXPECT_EQ(peer2->port, 7402);
  EXPECT_EQ(config.FindPeer(99), nullptr);
}

TEST(ClusterConfigTest, IgnoresBlankLinesAndComments) {
  TempConfigFile file(R"(
# cluster config
node_id=2

leader_id=1
peer 1 127.0.0.1 7401
peer 2 127.0.0.1 7402
)");
  ClusterConfig config;
  ASSERT_TRUE(LoadClusterConfig(file.path(), config).ok());
  EXPECT_EQ(config.local_node_id, 2u);
}

TEST(ClusterConfigTest, MissingFileReturnsError) {
  ClusterConfig config;
  const Status status = LoadClusterConfig("/tmp/nkv_cluster_conf_does_not_exist", config);
  EXPECT_FALSE(status.ok());
}

TEST(ClusterConfigTest, LocalNodeNotInPeersReturnsError) {
  TempConfigFile file(R"(
node_id=9
leader_id=1
peer 1 127.0.0.1 7401
peer 2 127.0.0.1 7402
)");
  ClusterConfig config;
  EXPECT_FALSE(LoadClusterConfig(file.path(), config).ok());
}

TEST(ClusterConfigTest, LeaderNotInPeersReturnsError) {
  TempConfigFile file(R"(
node_id=1
leader_id=9
peer 1 127.0.0.1 7401
peer 2 127.0.0.1 7402
)");
  ClusterConfig config;
  EXPECT_FALSE(LoadClusterConfig(file.path(), config).ok());
}

TEST(ClusterConfigTest, DuplicatePeerIdReturnsError) {
  TempConfigFile file(R"(
node_id=1
leader_id=1
peer 1 127.0.0.1 7401
peer 1 127.0.0.1 7402
)");
  ClusterConfig config;
  EXPECT_FALSE(LoadClusterConfig(file.path(), config).ok());
}

TEST(ClusterConfigTest, MalformedPeerLineReturnsError) {
  TempConfigFile file(R"(
node_id=1
leader_id=1
peer 1 127.0.0.1
)");
  ClusterConfig config;
  EXPECT_FALSE(LoadClusterConfig(file.path(), config).ok());
}

TEST(ClusterConfigTest, UnknownKeyReturnsError) {
  TempConfigFile file(R"(
node_id=1
leader_id=1
peer 1 127.0.0.1 7401
bogus_key=5
)");
  ClusterConfig config;
  EXPECT_FALSE(LoadClusterConfig(file.path(), config).ok());
}

}  // namespace
}  // namespace neuralkv::cluster
