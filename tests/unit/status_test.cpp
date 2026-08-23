#include <gtest/gtest.h>

#include <string>

#include "common/result.h"
#include "common/status.h"

namespace neuralkv {
namespace {

TEST(StatusTest, OkHasNoMessageAndOkCode) {
  Status status = Status::Ok();
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(status.code(), ErrorCode::kOk);
  EXPECT_TRUE(status.message().empty());
}

TEST(StatusTest, ErrorCarriesCodeAndMessage) {
  Status status = Status::Error(ErrorCode::kNotFound, "key missing");
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), ErrorCode::kNotFound);
  EXPECT_EQ(status.message(), "key missing");
}

TEST(ResultTest, SuccessPathHoldsValue) {
  Result<int> result(42);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), 42);
}

TEST(ResultTest, ErrorPathHoldsStatus) {
  Result<std::string> result(Status::Error(ErrorCode::kIOError, "disk full"));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), ErrorCode::kIOError);
  EXPECT_EQ(result.status().message(), "disk full");
}

TEST(ResultTest, MoveOnlyValueIsPreserved) {
  Result<std::string> result(std::string("payload"));
  ASSERT_TRUE(result.ok());
  std::string moved = std::move(result).value();
  EXPECT_EQ(moved, "payload");
}

}  // namespace
}  // namespace neuralkv
