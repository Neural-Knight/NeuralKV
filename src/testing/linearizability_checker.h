#pragma once

// Test-only, single-key linearizability checker (not linked into production).
// Simplified vs. a full Knossos/Elle-style search: validates each GET against
// every SET's real-time interval instead of searching a total order over the whole history.

#include <cstdint>
#include <string>
#include <vector>

namespace neuralkv::testing {

struct LinearizabilityOp {
  enum class Kind { kSet, kGet };

  int client_id = 0;
  Kind kind = Kind::kGet;
  std::string value;  // value written (SET) or value observed (GET)
  bool ok = false;    // did the operation complete successfully?
  // Real-time interval as sequence numbers from one shared counter the
  // caller increments at invocation and again at completion — exact,
  // no clock skew, since every op in one test runs in the same process.
  uint64_t start = 0;
  uint64_t end = 0;
};

class LinearizabilityChecker {
 public:
  void Record(LinearizabilityOp op) { history_.push_back(std::move(op)); }

  // Returns an empty string if history is consistent; otherwise a
  // description of the first violation found.
  std::string Check() const {
    std::vector<const LinearizabilityOp*> sets;
    for (const LinearizabilityOp& op : history_) {
      if (op.kind == LinearizabilityOp::Kind::kSet && op.ok) sets.push_back(&op);
    }

    for (const LinearizabilityOp& g : history_) {
      if (g.kind != LinearizabilityOp::Kind::kGet || !g.ok) continue;

      bool ever_written = false;
      for (const LinearizabilityOp* s : sets) {
        if (s->value == g.value) {
          ever_written = true;
          break;
        }
      }
      if (!ever_written) {
        return "client " + std::to_string(g.client_id) + " GET returned a value never written: '" +
               g.value + "'";
      }

      // g.value is valid if some write W wrote it, W is a real-time candidate for
      // g, and no other write W2 is provably newer than W and already in effect
      // by the time g started — the general form of "a client can't see its own write undone by nothing."
      bool valid = false;
      for (const LinearizabilityOp* w : sets) {
        if (w->value != g.value) continue;
        const bool is_candidate = w->end <= g.start || (w->start < g.end && w->end > g.start);
        if (!is_candidate) continue;

        bool superseded = false;
        for (const LinearizabilityOp* w2 : sets) {
          if (w2 == w) continue;
          if (w->end <= w2->start && w2->end <= g.start) {
            superseded = true;
            break;
          }
        }
        if (!superseded) {
          valid = true;
          break;
        }
      }
      if (!valid) {
        return "client " + std::to_string(g.client_id) + " GET returned stale value '" + g.value + "'";
      }
    }
    return "";
  }

 private:
  std::vector<LinearizabilityOp> history_;
};

}  // namespace neuralkv::testing
