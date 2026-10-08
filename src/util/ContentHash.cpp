#include "util/ContentHash.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <new>

#include "util/Hex.h"

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace microide::util {
namespace {

// BLAKE3 (the hash mode only; no keyed hash, no key derivation, no XOF past 32
// bytes), from the specification: https://github.com/BLAKE3-team/BLAKE3-specs.
// Portable and self-contained. Pinned by the upstream test vectors in
// tests/ContentHashTests.cpp.
constexpr std::uint32_t kIv[8] = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
                                  0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};
constexpr std::size_t kBlockLen = 64;
constexpr std::size_t kChunkLen = 1024;
constexpr std::uint32_t kChunkStart = 1u << 0;
constexpr std::uint32_t kChunkEnd = 1u << 1;
constexpr std::uint32_t kParent = 1u << 2;
constexpr std::uint32_t kRoot = 1u << 3;
// Deep enough for 2^54 chunks, which is the spec's own bound.
constexpr std::size_t kMaxDepth = 54;

constexpr std::uint32_t Rotr(std::uint32_t value, int bits) {
  return (value >> bits) | (value << (32 - bits));
}

inline std::uint32_t LoadLe32(const std::uint8_t* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

#define MICROIDE_BLAKE3_G(a, b, c, d, mx, my) \
  do {                                        \
    s[a] = s[a] + s[b] + (mx);                \
    s[d] = Rotr(s[d] ^ s[a], 16);             \
    s[c] = s[c] + s[d];                       \
    s[b] = Rotr(s[b] ^ s[c], 12);             \
    s[a] = s[a] + s[b] + (my);                \
    s[d] = Rotr(s[d] ^ s[a], 8);              \
    s[c] = s[c] + s[d];                       \
    s[b] = Rotr(s[b] ^ s[c], 7);              \
  } while (0)

// The message schedule: round r reads word kSchedule[r][i] where the reference
// implementation permutes the block between rounds. Precomputed so a round is
// straight-line code with no copies.
constexpr std::uint8_t kSchedule[7][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8},
    {3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1},
    {10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6},
    {12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4},
    {9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7},
    {11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13},
};

// The compression function, keeping only the 8 words every caller in hash mode
// needs (a chaining value, or the first 32 output bytes of the root).
void Compress(const std::uint32_t cv[8], const std::uint8_t block[kBlockLen],
              std::uint8_t block_len, std::uint64_t counter, std::uint32_t flags,
              std::uint32_t out[8]) {
  std::uint32_t m[16];
  for (int i = 0; i < 16; ++i) {
    m[i] = LoadLe32(block + 4 * i);
  }
  std::uint32_t s[16] = {
      cv[0],  cv[1],  cv[2],  cv[3],
      cv[4],  cv[5],  cv[6],  cv[7],
      kIv[0], kIv[1], kIv[2], kIv[3],
      static_cast<std::uint32_t>(counter), static_cast<std::uint32_t>(counter >> 32),
      block_len, flags,
  };
  for (const auto& r : kSchedule) {
    MICROIDE_BLAKE3_G(0, 4, 8, 12, m[r[0]], m[r[1]]);
    MICROIDE_BLAKE3_G(1, 5, 9, 13, m[r[2]], m[r[3]]);
    MICROIDE_BLAKE3_G(2, 6, 10, 14, m[r[4]], m[r[5]]);
    MICROIDE_BLAKE3_G(3, 7, 11, 15, m[r[6]], m[r[7]]);
    MICROIDE_BLAKE3_G(0, 5, 10, 15, m[r[8]], m[r[9]]);
    MICROIDE_BLAKE3_G(1, 6, 11, 12, m[r[10]], m[r[11]]);
    MICROIDE_BLAKE3_G(2, 7, 8, 13, m[r[12]], m[r[13]]);
    MICROIDE_BLAKE3_G(3, 4, 9, 14, m[r[14]], m[r[15]]);
  }
  for (int i = 0; i < 8; ++i) {
    out[i] = s[i] ^ s[i + 8];
  }
}

#undef MICROIDE_BLAKE3_G

// The state a ContentHasher keeps in its opaque storage.
struct Blake3State {
  // The chunk being filled.
  std::uint32_t chunk_cv[8];
  std::uint64_t chunk_counter = 0;
  std::uint8_t block[kBlockLen];
  std::uint8_t block_len = 0;
  std::uint8_t blocks_compressed = 0;
  // Completed subtrees, one per set bit of the chunk count.
  std::uint8_t stack_len = 0;
  std::uint32_t stack[kMaxDepth][8];

  Blake3State() { ResetChunk(0); }

  void ResetChunk(std::uint64_t counter) {
    std::memcpy(chunk_cv, kIv, sizeof(chunk_cv));
    chunk_counter = counter;
    std::memset(block, 0, sizeof(block));
    block_len = 0;
    blocks_compressed = 0;
  }

  std::size_t ChunkLen() const { return kBlockLen * blocks_compressed + block_len; }
  std::uint32_t StartFlag() const { return blocks_compressed == 0 ? kChunkStart : 0; }

  void ChunkUpdate(const std::uint8_t* input, std::size_t length) {
    while (length > 0) {
      if (block_len == kBlockLen) {
        Compress(chunk_cv, block, kBlockLen, chunk_counter, StartFlag(), chunk_cv);
        ++blocks_compressed;
        std::memset(block, 0, sizeof(block));
        block_len = 0;
      }
      const std::size_t take = std::min(kBlockLen - block_len, length);
      std::memcpy(block + block_len, input, take);
      block_len = static_cast<std::uint8_t>(block_len + take);
      input += take;
      length -= take;
    }
  }

  void PushChunk(const std::uint32_t cv[8], std::uint64_t total_chunks) {
    std::uint32_t merged[8];
    std::memcpy(merged, cv, sizeof(merged));
    // Merge one parent per trailing zero bit of the new chunk count.
    while ((total_chunks & 1) == 0) {
      std::uint8_t parent_block[kBlockLen];
      --stack_len;
      for (int i = 0; i < 8; ++i) {
        StoreLe32(parent_block + 4 * i, stack[stack_len][i]);
        StoreLe32(parent_block + 32 + 4 * i, merged[i]);
      }
      Compress(kIv, parent_block, kBlockLen, 0, kParent, merged);
      total_chunks >>= 1;
    }
    std::memcpy(stack[stack_len], merged, sizeof(merged));
    ++stack_len;
  }

  static void StoreLe32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value);
    out[1] = static_cast<std::uint8_t>(value >> 8);
    out[2] = static_cast<std::uint8_t>(value >> 16);
    out[3] = static_cast<std::uint8_t>(value >> 24);
  }

  void Update(const std::uint8_t* input, std::size_t length) {
    while (length > 0) {
      // A full chunk is finalized only once more input proves it is not the last.
      if (ChunkLen() == kChunkLen) {
        std::uint32_t cv[8];
        Compress(chunk_cv, block, block_len, chunk_counter, StartFlag() | kChunkEnd, cv);
        const std::uint64_t total_chunks = chunk_counter + 1;
        PushChunk(cv, total_chunks);
        ResetChunk(total_chunks);
      }
      const std::size_t take = std::min(kChunkLen - ChunkLen(), length);
      ChunkUpdate(input, take);
      input += take;
      length -= take;
    }
  }

  ContentHash Finish() const {
    // The output node: the current chunk, then folded with each stacked subtree
    // from the top down; ROOT goes on whichever node ends up last.
    std::uint32_t cv_in[8];
    std::uint8_t node_block[kBlockLen];
    std::uint8_t node_len = block_len;
    std::uint64_t node_counter = chunk_counter;
    std::uint32_t node_flags = StartFlag() | kChunkEnd;
    std::memcpy(cv_in, chunk_cv, sizeof(cv_in));
    std::memcpy(node_block, block, sizeof(node_block));
    for (std::size_t remaining = stack_len; remaining > 0; --remaining) {
      std::uint32_t child[8];
      Compress(cv_in, node_block, node_len, node_counter, node_flags, child);
      for (int i = 0; i < 8; ++i) {
        StoreLe32(node_block + 4 * i, stack[remaining - 1][i]);
        StoreLe32(node_block + 32 + 4 * i, child[i]);
      }
      std::memcpy(cv_in, kIv, sizeof(cv_in));
      node_len = kBlockLen;
      node_counter = 0;
      node_flags = kParent;
    }
    std::uint32_t out[8];
    Compress(cv_in, node_block, node_len, node_counter, node_flags | kRoot, out);
    ContentHash hash;
    for (int i = 0; i < 8; ++i) {
      StoreLe32(hash.bytes.data() + 4 * i, out[i]);
    }
    return hash;
  }
};

static_assert(sizeof(Blake3State) <= 1920, "grow ContentHasher::state_");
static_assert(alignof(Blake3State) <= 8, "raise ContentHasher::state_'s alignment");

Blake3State* State(unsigned char* storage) {
  return std::launder(reinterpret_cast<Blake3State*>(storage));
}
const Blake3State* State(const unsigned char* storage) {
  return std::launder(reinterpret_cast<const Blake3State*>(storage));
}

}  // namespace

std::optional<ContentHash> ContentHash::FromRaw(std::string_view raw) {
  if (raw.size() != kBytes) {
    return std::nullopt;
  }
  ContentHash hash;
  std::memcpy(hash.bytes.data(), raw.data(), kBytes);
  return hash;
}

std::string ContentHash::Hex() const {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out(kBytes * 2, '\0');
  for (std::size_t i = 0; i < kBytes; ++i) {
    out[2 * i] = kDigits[bytes[i] >> 4];
    out[2 * i + 1] = kDigits[bytes[i] & 0x0F];
  }
  return out;
}

std::optional<ContentHash> ContentHash::FromHex(std::string_view hex) {
  if (hex.size() != kBytes * 2) {
    return std::nullopt;
  }
  ContentHash hash;
  for (std::size_t i = 0; i < kBytes; ++i) {
    const auto byte = ParseHexByte(hex[2 * i], hex[2 * i + 1]);
    if (!byte) {
      return std::nullopt;
    }
    hash.bytes[i] = *byte;
  }
  return hash;
}

ContentHasher::ContentHasher() {
  new (state_) Blake3State();
}

void ContentHasher::Update(std::string_view bytes) {
  State(state_)->Update(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
}

ContentHash ContentHasher::Finish() const {
  return State(state_)->Finish();
}

ContentHash HashContent(std::string_view bytes) {
  ContentHasher hasher;
  hasher.Update(bytes);
  return hasher.Finish();
}

std::optional<ContentHash> HashFileContent(const std::filesystem::path& path,
                                           std::uint64_t* size_out) {
#if defined(__unix__) || defined(__APPLE__)
  // O_NONBLOCK so opening a FIFO cannot hang; the fstat below then refuses it.
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    return std::nullopt;
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    ::close(fd);
    return std::nullopt;
  }
  ContentHasher hasher;
  std::uint64_t total = 0;
  char buffer[64 * 1024];
  for (;;) {
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got > 0) {
      hasher.Update(std::string_view(buffer, static_cast<std::size_t>(got)));
      total += static_cast<std::uint64_t>(got);
      continue;
    }
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got < 0) {
      ::close(fd);
      return std::nullopt;
    }
    break;
  }
  ::close(fd);
  if (size_out != nullptr) {
    *size_out = total;
  }
  return hasher.Finish();
#else
  (void)path;
  (void)size_out;
  return std::nullopt;
#endif
}

}  // namespace microide::util
