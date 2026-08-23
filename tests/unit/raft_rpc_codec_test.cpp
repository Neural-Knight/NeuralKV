#include "raft/rpc_codec.h"

#include <gtest/gtest.h>

#include <string>

namespace neuralkv::raft {
namespace {

TEST(RaftRpcCodecTest, RoundTripsRequestVoteRequest) {
  const RequestVoteRequest req{
      .term = 7, .candidate_id = 3, .last_log_index = 42, .last_log_term = 6};
  std::string body;
  ASSERT_TRUE(EncodeRequestVoteRequest(req, body).ok());

  RequestVoteRequest decoded;
  ASSERT_TRUE(DecodeRequestVoteRequest(body, decoded).ok());
  EXPECT_EQ(decoded.term, 7u);
  EXPECT_EQ(decoded.candidate_id, 3u);
  EXPECT_EQ(decoded.last_log_index, 42u);
  EXPECT_EQ(decoded.last_log_term, 6u);
}

TEST(RaftRpcCodecTest, RoundTripsRequestVoteResponseGranted) {
  const RequestVoteResponse resp{.term = 9, .vote_granted = true};
  std::string body;
  ASSERT_TRUE(EncodeRequestVoteResponse(resp, body).ok());

  RequestVoteResponse decoded;
  ASSERT_TRUE(DecodeRequestVoteResponse(body, decoded).ok());
  EXPECT_EQ(decoded.term, 9u);
  EXPECT_TRUE(decoded.vote_granted);
}

TEST(RaftRpcCodecTest, RoundTripsRequestVoteResponseDenied) {
  const RequestVoteResponse resp{.term = 2, .vote_granted = false};
  std::string body;
  ASSERT_TRUE(EncodeRequestVoteResponse(resp, body).ok());

  RequestVoteResponse decoded;
  ASSERT_TRUE(DecodeRequestVoteResponse(body, decoded).ok());
  EXPECT_EQ(decoded.term, 2u);
  EXPECT_FALSE(decoded.vote_granted);
}

TEST(RaftRpcCodecTest, RoundTripsAppendEntriesRequestWithEntries) {
  const AppendEntriesRequest req{
      .term = 4,
      .leader_id = 1,
      .prev_log_index = 5,
      .prev_log_term = 3,
      .leader_commit = 5,
      .entries = {LogEntry{.term = 4, .index = 6, .op = persistence::WalOp::kSet, .key = "k1", .value = "v1"},
                  LogEntry{.term = 4, .index = 7, .op = persistence::WalOp::kDelete, .key = "k2", .value = ""}}};
  std::string body;
  ASSERT_TRUE(EncodeAppendEntriesRequest(req, body).ok());

  AppendEntriesRequest decoded;
  ASSERT_TRUE(DecodeAppendEntriesRequest(body, decoded).ok());
  EXPECT_EQ(decoded.term, 4u);
  EXPECT_EQ(decoded.leader_id, 1u);
  EXPECT_EQ(decoded.prev_log_index, 5u);
  EXPECT_EQ(decoded.prev_log_term, 3u);
  EXPECT_EQ(decoded.leader_commit, 5u);
  ASSERT_EQ(decoded.entries.size(), 2u);
  EXPECT_EQ(decoded.entries[0].term, 4u);
  EXPECT_EQ(decoded.entries[0].index, 6u);
  EXPECT_EQ(decoded.entries[0].op, persistence::WalOp::kSet);
  EXPECT_EQ(decoded.entries[0].key, "k1");
  EXPECT_EQ(decoded.entries[0].value, "v1");
  EXPECT_EQ(decoded.entries[1].op, persistence::WalOp::kDelete);
  EXPECT_EQ(decoded.entries[1].key, "k2");
  EXPECT_TRUE(decoded.entries[1].value.empty());
}

TEST(RaftRpcCodecTest, RoundTripsAppendEntriesRequestWithNoEntries) {
  const AppendEntriesRequest req{
      .term = 1, .leader_id = 2, .prev_log_index = 0, .prev_log_term = 0, .leader_commit = 0, .entries = {}};
  std::string body;
  ASSERT_TRUE(EncodeAppendEntriesRequest(req, body).ok());

  AppendEntriesRequest decoded;
  ASSERT_TRUE(DecodeAppendEntriesRequest(body, decoded).ok());
  EXPECT_TRUE(decoded.entries.empty());
}

TEST(RaftRpcCodecTest, RoundTripsAppendEntriesResponse) {
  const AppendEntriesResponse resp{.term = 3, .success = true};
  std::string body;
  ASSERT_TRUE(EncodeAppendEntriesResponse(resp, body).ok());

  AppendEntriesResponse decoded;
  ASSERT_TRUE(DecodeAppendEntriesResponse(body, decoded).ok());
  EXPECT_EQ(decoded.term, 3u);
  EXPECT_TRUE(decoded.success);
}

TEST(RaftRpcCodecTest, DecodeRejectsTruncatedBody) {
  RequestVoteRequest decoded;
  EXPECT_FALSE(DecodeRequestVoteRequest(std::string("\x00\x00"), decoded).ok());
}

TEST(RaftRpcCodecTest, DecodeRejectsTrailingBytes) {
  const RequestVoteResponse resp{.term = 1, .vote_granted = true};
  std::string body;
  ASSERT_TRUE(EncodeRequestVoteResponse(resp, body).ok());
  body.push_back('\xFF');

  RequestVoteResponse decoded;
  EXPECT_FALSE(DecodeRequestVoteResponse(body, decoded).ok());
}

}  // namespace
}  // namespace neuralkv::raft
