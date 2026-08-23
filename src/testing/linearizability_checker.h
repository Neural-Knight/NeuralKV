#pragma once

// Test-only, single-key linearizability checker. Not linked into any
// production binary. This is deliberately simplified — a real
// exhaustive checker (Knossos/Elle-style) searches for a total order
// over the whole history; this instead uses real-time interval bounds
// (exact, since the caller assigns them from one shared counter, not
// wall-clock) to check each GET independently: its returned value is
// valid if some SET wrote it, that SET's interval doesn't rule it out
// as a candidate (either it definitely completed before the GET
// started, or it overlaps the GET so either real-time order is
// possible), and no *other* SET is provably both newer than it and
// already in effect before the GET started. That last condition is
// what actually catches staleness — a client missing its own prior SET
// is just the special case where the provably-newer write happens to
// be that same client's. A GET that's still valid by every SET's real
// interval alone is accepted; this will not catch every violation a
// full search-based checker would on histories with many overlapping
// concurrent writers, but it's sound for the cases it flags and cheap
// enough to run over hundreds of ops per test.

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

      // g.value is valid if some write W wrote it, W's interval is a
      // candidate for g (W definitely completed before g started, or W
      // overlaps g so either order is real-time-consistent), and no
      // OTHER write W2 is provably newer than W *and* provably already
      // in effect by the time g started (W.end <= W2.start, and
      // W2.end <= g.start) — that combination would mean W was
      // definitely superseded before g ran, so g couldn't still be
      // seeing it. This is the general form of "a client shouldn't see
      // its own write undone by nothing": that's just the case where W2
      // happens to be the client's own later write.
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
