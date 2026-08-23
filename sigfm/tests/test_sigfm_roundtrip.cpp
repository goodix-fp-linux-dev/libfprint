// Standalone host-side round-trip test for the vendored SIGFM library.
// No hardware/sensor needed. Verifies the OpenCV/SIGFM integration itself
// (extraction, serialization, deserialization, scoring) is sound before it
// is ever touched by real capture data.
//
// Uses the driver's real sensor dimensions (108x88, GOODIX533C_SENSOR_WIDTH
// x GOODIX533C_SENSOR_HEIGHT) and structured synthetic input (a grid of
// Gaussian-like blobs), not flat grey -- SIFT finds zero keypoints on a
// flat image, which would make a self-match round trip pass vacuously.
//
// Build (run from the repo root, so the -I. below reaches sigfm/sigfm.hpp):
//   g++ -std=c++17 -I. sigfm/tests/test_sigfm_roundtrip.cpp sigfm/sigfm.cpp \
//       $(pkg-config --cflags opencv5 2>/dev/null || pkg-config --cflags opencv4) \
//       -lopencv_core -lopencv_imgproc -lopencv_flann \
//       $(pkg-config --exists opencv5 && echo -lopencv_features || echo -lopencv_features2d) \
//       -o /tmp/sigfm_roundtrip_test && /tmp/sigfm_roundtrip_test
//
// Not wired into the meson build (matching the upstream goodix53x5-libfprint
// repo's own sigfm/tests, which are likewise standalone/manually invoked
// rather than a `meson test` target) -- this exercises the SIGFM library in
// isolation, independent of whether any particular driver is selected.

#include "sigfm/sigfm.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("  FAIL: %s\n", msg);                                      \
      failures++;                                                            \
    } else {                                                                 \
      std::printf("  ok:   %s\n", msg);                                      \
    }                                                                        \
  } while (0)

static const int kWidth = 108;   // GOODIX533C_SENSOR_WIDTH
static const int kHeight = 88;   // GOODIX533C_SENSOR_HEIGHT

// A grid of soft Gaussian blobs at pseudo-random offsets/amplitudes -- gives
// SIFT plenty of local structure to key on, unlike a flat or purely linear
// gradient image (which either has zero keypoints or keypoints only at the
// border).
static std::vector<unsigned char> make_structured_frame(unsigned seed)
{
  std::vector<unsigned char> img(kWidth * kHeight);
  std::vector<std::array<double, 4>> blobs; // x, y, sigma, amplitude

  unsigned state = seed;
  auto next = [&state]() {
    state = state * 1103515245u + 12345u;
    return (double) ((state >> 8) & 0xFFFF) / 65535.0;
  };

  for (int i = 0; i < 24; i++) {
    double x = 6.0 + next() * (kWidth - 12.0);
    double y = 6.0 + next() * (kHeight - 12.0);
    double sigma = 2.5 + next() * 4.0;
    double amp = 60.0 + next() * 120.0;
    blobs.push_back({x, y, sigma, amp});
  }

  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < kWidth; x++) {
      double v = 90.0; // mid-grey baseline
      for (const auto &b : blobs) {
        double dx = x - b[0];
        double dy = y - b[1];
        double d2 = dx * dx + dy * dy;
        v += b[3] * std::exp(-d2 / (2.0 * b[2] * b[2]));
      }
      int iv = (int) std::lround(v);
      if (iv < 0) iv = 0;
      if (iv > 255) iv = 255;
      img[y * kWidth + x] = (unsigned char) iv;
    }
  }
  return img;
}

int main()
{
  std::printf("SIGFM round-trip test (%dx%d synthetic structured frame)\n\n",
              kWidth, kHeight);

  std::vector<unsigned char> frame = make_structured_frame(0xC0FFEE);

  // 1. Extract.
  SigfmImgInfo *info = sigfm_extract(frame.data(), kWidth, kHeight);
  CHECK(info != nullptr, "sigfm_extract() succeeds on structured input");
  if (info == nullptr) {
    std::printf("\n%d TEST(S) FAILED (cannot continue)\n", ++failures);
    return 1;
  }

  int keypoints = sigfm_keypoints_count(info);
  std::printf("  keypoints extracted: %d\n", keypoints);
  CHECK(keypoints > 0, "structured frame yields at least one SIFT keypoint");

  // 2. Serialize.
  int serialized_len = 0;
  unsigned char *serialized = sigfm_serialize_binary(info, &serialized_len);
  CHECK(serialized != nullptr && serialized_len > 0,
        "sigfm_serialize_binary() produces a non-empty buffer");
  std::printf("  serialized size: %d bytes\n", serialized_len);

  // 3. Deserialize.
  SigfmImgInfo *roundtrip = sigfm_deserialize_binary(serialized, serialized_len);
  CHECK(roundtrip != nullptr, "sigfm_deserialize_binary() succeeds");

  if (roundtrip != nullptr) {
    CHECK(sigfm_keypoints_count(roundtrip) == keypoints,
          "deserialized keypoint count matches original");

    // 4. Score the deserialized copy against the original -- a perfect
    // self-match (identical keypoints/descriptors) should score very high,
    // comfortably above GOODIX533C_SIGFM_BEST_MIN (150).
    int score = sigfm_match_score(info, roundtrip);
    std::printf("  self-match score (original vs. round-tripped): %d\n", score);
    CHECK(score >= 150, "round-tripped template scores >= GOODIX533C_SIGFM_BEST_MIN (150) against itself");
  }

  // 5. Also sanity-check sigfm_copy_info() and a genuinely independent
  // extraction of the *same* pixel buffer -- two independent SIFT passes
  // over identical input should also match each other highly.
  SigfmImgInfo *copy = sigfm_copy_info(info);
  CHECK(copy != nullptr, "sigfm_copy_info() succeeds");
  if (copy != nullptr) {
    int score = sigfm_match_score(info, copy);
    std::printf("  self-match score (original vs. sigfm_copy_info()): %d\n", score);
    CHECK(score >= 150, "copied info scores >= GOODIX533C_SIGFM_BEST_MIN against original");
    sigfm_free_info(copy);
  }

  SigfmImgInfo *independent = sigfm_extract(frame.data(), kWidth, kHeight);
  CHECK(independent != nullptr, "second independent sigfm_extract() call succeeds");
  if (independent != nullptr) {
    int score = sigfm_match_score(info, independent);
    std::printf("  self-match score (original vs. independent re-extract): %d\n", score);
    CHECK(score >= 150, "independently re-extracted frame scores >= GOODIX533C_SIGFM_BEST_MIN");
    sigfm_free_info(independent);
  }

  // Cross-check against a *different* structured frame (different seed):
  // real impostor rejection depends on preprocessing/descriptors differing,
  // which this synthetic generator does provide across seeds, so this
  // should score noticeably lower than the self-match cases above (though
  // not necessarily below the accept gate -- that is not this test's
  // contract, see test_sigfm_match.cpp upstream for the geometry contract).
  std::vector<unsigned char> other_frame = make_structured_frame(0xDEADBEEF);
  SigfmImgInfo *other = sigfm_extract(other_frame.data(), kWidth, kHeight);
  if (other != nullptr && roundtrip != nullptr) {
    int score = sigfm_match_score(roundtrip, other);
    std::printf("  cross-match score (round-tripped vs. different frame): %d\n", score);
    sigfm_free_info(other);
  }

  if (roundtrip != nullptr)
    sigfm_free_info(roundtrip);
  free(serialized);
  sigfm_free_info(info);

  if (failures == 0) {
    std::printf("\nALL TESTS PASSED\n");
    return 0;
  }
  std::printf("\n%d TEST(S) FAILED\n", failures);
  return 1;
}
