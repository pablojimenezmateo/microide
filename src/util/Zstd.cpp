#include "util/Zstd.h"

#include <algorithm>
#include <memory>

// The window-log bounds are in the static-linking section; the library is linked
// statically (third_party/zstd), so its exact version is the one compiled here.
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

namespace microide::util {
namespace {

struct CctxDeleter {
  void operator()(ZSTD_CCtx* context) const { ZSTD_freeCCtx(context); }
};
struct DctxDeleter {
  void operator()(ZSTD_DCtx* context) const { ZSTD_freeDCtx(context); }
};

// The window must reach back over the whole base, or matches against its start
// are out of range — the CLI's `--patch-from` sizes it the same way.
int WindowLogFor(std::size_t bytes) {
  int log = ZSTD_WINDOWLOG_MIN;
  while (log < ZSTD_WINDOWLOG_MAX_64 && (std::size_t{1} << log) < bytes) {
    ++log;
  }
  return log;
}

}  // namespace

std::optional<std::string> ZstdDelta(std::string_view base, std::string_view target) {
  const std::unique_ptr<ZSTD_CCtx, CctxDeleter> context(ZSTD_createCCtx());
  if (!context) {
    return std::nullopt;
  }
  ZSTD_CCtx* cctx = context.get();
  const int window = WindowLogFor(base.size() + target.size());
  if (ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 3)) ||
      ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, window)) ||
      ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 0)) ||
      ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 1)) ||
      // Long-distance matching finds the base's copy of a block wherever it sits,
      // which is the whole point for a file with an edit in the middle.
      (base.size() > (std::size_t{1} << 20) &&
       ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1))) ||
      ZSTD_isError(ZSTD_CCtx_refPrefix(cctx, base.data(), base.size()))) {
    return std::nullopt;
  }
  std::string out(ZSTD_compressBound(target.size()), '\0');
  const std::size_t written =
      ZSTD_compress2(cctx, out.data(), out.size(), target.data(), target.size());
  if (ZSTD_isError(written)) {
    return std::nullopt;
  }
  out.resize(written);
  return out;
}

std::optional<std::string> ZstdApplyDelta(std::string_view base, std::string_view delta,
                                          std::size_t max_size) {
  const unsigned long long declared = ZSTD_getFrameContentSize(delta.data(), delta.size());
  if (declared == ZSTD_CONTENTSIZE_ERROR || declared == ZSTD_CONTENTSIZE_UNKNOWN ||
      declared > max_size) {
    return std::nullopt;
  }
  const std::unique_ptr<ZSTD_DCtx, DctxDeleter> context(ZSTD_createDCtx());
  if (!context) {
    return std::nullopt;
  }
  ZSTD_DCtx* dctx = context.get();
  const int window = WindowLogFor(base.size() + static_cast<std::size_t>(declared));
  if (ZSTD_isError(ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, window)) ||
      ZSTD_isError(ZSTD_DCtx_refPrefix(dctx, base.data(), base.size()))) {
    return std::nullopt;
  }
  std::string out(static_cast<std::size_t>(declared), '\0');
  const std::size_t written =
      ZSTD_decompressDCtx(dctx, out.data(), out.size(), delta.data(), delta.size());
  if (ZSTD_isError(written) || written != out.size()) {
    return std::nullopt;
  }
  return out;
}

}  // namespace microide::util
