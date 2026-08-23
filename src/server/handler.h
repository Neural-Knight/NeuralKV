#pragma once

#include "protocol/types.h"
#include "storage/sharded_kv.h"

namespace neuralkv {

// Applies a decoded client request to storage and builds the response.
// Owns no state beyond the storage reference; safe to reuse across
// connections since ShardedKV itself is thread-safe.
class RequestHandler {
 public:
  explicit RequestHandler(ShardedKV& kv);

  protocol::ClientResponse Handle(const protocol::ClientRequest& req);

 private:
  ShardedKV& kv_;
};

}  // namespace neuralkv
