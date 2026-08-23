#pragma once

#include "persistence/durable_storage.h"
#include "protocol/types.h"

namespace neuralkv {

// Applies a decoded client request to durable storage and builds the
// response. Owns no state beyond the storage reference.
class RequestHandler {
 public:
  explicit RequestHandler(persistence::DurableStorage& storage);

  protocol::ClientResponse Handle(const protocol::ClientRequest& req);

 private:
  persistence::DurableStorage& storage_;
};

}  // namespace neuralkv
