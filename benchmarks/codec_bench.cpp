#include <benchmark/benchmark.h>

#include <span>
#include <string>
#include <vector>

#include "protocol/codec.h"
#include "protocol/constants.h"
#include "protocol/types.h"

namespace {

using neuralkv::protocol::ClientRequest;
using neuralkv::protocol::DecodeClientRequest;
using neuralkv::protocol::EncodeClientRequest;
using neuralkv::protocol::kFrameHeaderSize;
using neuralkv::protocol::Opcode;
using neuralkv::protocol::TryParseFrame;

ClientRequest MakeSetRequest() {
  return ClientRequest{
      .request_id = 1,
      .opcode = Opcode::kSet,
      .key = std::string(16, 'k'),
      .value = std::string(256, 'v'),
  };
}

}  // namespace

static void BM_EncodeRequest_Set(benchmark::State& state) {
  const ClientRequest req = MakeSetRequest();
  std::vector<uint8_t> out;
  for (auto _ : state) {
    benchmark::DoNotOptimize(EncodeClientRequest(req, out));
  }
}
BENCHMARK(BM_EncodeRequest_Set);

static void BM_DecodeRequest_Set(benchmark::State& state) {
  const ClientRequest req = MakeSetRequest();
  std::vector<uint8_t> encoded;
  EncodeClientRequest(req, encoded);
  const std::span<const uint8_t> payload(encoded.data() + kFrameHeaderSize,
                                          encoded.size() - kFrameHeaderSize);

  for (auto _ : state) {
    ClientRequest decoded;
    benchmark::DoNotOptimize(DecodeClientRequest(payload, decoded));
  }
}
BENCHMARK(BM_DecodeRequest_Set);

static void BM_EncodeDecode_RoundTrip(benchmark::State& state) {
  const ClientRequest req = MakeSetRequest();
  std::vector<uint8_t> encoded;

  for (auto _ : state) {
    EncodeClientRequest(req, encoded);
    ClientRequest decoded;
    benchmark::DoNotOptimize(
        DecodeClientRequest(std::span(encoded).subspan(kFrameHeaderSize), decoded));
  }
}
BENCHMARK(BM_EncodeDecode_RoundTrip);

static void BM_ParseFrame_Complete(benchmark::State& state) {
  const ClientRequest req = MakeSetRequest();
  std::vector<uint8_t> frame;
  EncodeClientRequest(req, frame);

  for (auto _ : state) {
    state.PauseTiming();
    std::vector<uint8_t> buffer = frame;
    state.ResumeTiming();

    ClientRequest decoded;
    benchmark::DoNotOptimize(TryParseFrame(buffer, &decoded, nullptr));
  }
}
BENCHMARK(BM_ParseFrame_Complete);

BENCHMARK_MAIN();
