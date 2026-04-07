#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define FIXTURE_TRACE(label) do { (void)(label); } while (false)

namespace microide::fixtures {

struct Record {
  int id = 0;
  std::string label;
  double weight = 0.0;
  bool active = false;
};

template <typename T>
constexpr T ClampValue(T value, T lower, T upper) {
  return value < lower ? lower : (value > upper ? upper : value);
}

constexpr std::string_view kBanner = R"FIXTURE(
microide generated fixture
with a raw string for syntax tests
)FIXTURE";

// block 001 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock001 = {1, 2, 3, 4, 5, 6};

Record MakeRecord001(std::string_view seed, int salt) {
  const double scale = static_cast<double>((1 % 7) + 1) / 3.0;
  const bool active = ((salt + 1) % 2) == 0;
  return Record{
      .id = salt + 1,
      .label = std::string(seed) + "-block-001",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock001(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 1) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 1;
    } else {
      total -= (1 % 5);
    }
  }
  return total;
}

std::string DescribeBlock001(const Record& record) {
  std::ostringstream stream;
  stream << "block=001"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 002 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock002 = {2, 3, 4, 5, 6, 7};

Record MakeRecord002(std::string_view seed, int salt) {
  const double scale = static_cast<double>((2 % 7) + 1) / 3.0;
  const bool active = ((salt + 2) % 2) == 0;
  return Record{
      .id = salt + 2,
      .label = std::string(seed) + "-block-002",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock002(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 2) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 2;
    } else {
      total -= (2 % 5);
    }
  }
  return total;
}

std::string DescribeBlock002(const Record& record) {
  std::ostringstream stream;
  stream << "block=002"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 003 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock003 = {3, 4, 5, 6, 7, 8};

Record MakeRecord003(std::string_view seed, int salt) {
  const double scale = static_cast<double>((3 % 7) + 1) / 3.0;
  const bool active = ((salt + 3) % 2) == 0;
  return Record{
      .id = salt + 3,
      .label = std::string(seed) + "-block-003",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock003(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 3) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 3;
    } else {
      total -= (3 % 5);
    }
  }
  return total;
}

std::string DescribeBlock003(const Record& record) {
  std::ostringstream stream;
  stream << "block=003"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 004 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock004 = {4, 5, 6, 7, 8, 9};

Record MakeRecord004(std::string_view seed, int salt) {
  const double scale = static_cast<double>((4 % 7) + 1) / 3.0;
  const bool active = ((salt + 4) % 2) == 0;
  return Record{
      .id = salt + 4,
      .label = std::string(seed) + "-block-004",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock004(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 4) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 4;
    } else {
      total -= (4 % 5);
    }
  }
  return total;
}

std::string DescribeBlock004(const Record& record) {
  std::ostringstream stream;
  stream << "block=004"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 005 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock005 = {5, 6, 7, 8, 9, 10};

Record MakeRecord005(std::string_view seed, int salt) {
  const double scale = static_cast<double>((5 % 7) + 1) / 3.0;
  const bool active = ((salt + 5) % 2) == 0;
  return Record{
      .id = salt + 5,
      .label = std::string(seed) + "-block-005",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock005(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 5) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 5;
    } else {
      total -= (5 % 5);
    }
  }
  return total;
}

std::string DescribeBlock005(const Record& record) {
  std::ostringstream stream;
  stream << "block=005"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 006 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock006 = {6, 7, 8, 9, 10, 11};

Record MakeRecord006(std::string_view seed, int salt) {
  const double scale = static_cast<double>((6 % 7) + 1) / 3.0;
  const bool active = ((salt + 6) % 2) == 0;
  return Record{
      .id = salt + 6,
      .label = std::string(seed) + "-block-006",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock006(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 6) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 6;
    } else {
      total -= (6 % 5);
    }
  }
  return total;
}

std::string DescribeBlock006(const Record& record) {
  std::ostringstream stream;
  stream << "block=006"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 007 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock007 = {7, 8, 9, 10, 11, 12};

Record MakeRecord007(std::string_view seed, int salt) {
  const double scale = static_cast<double>((7 % 7) + 1) / 3.0;
  const bool active = ((salt + 7) % 2) == 0;
  return Record{
      .id = salt + 7,
      .label = std::string(seed) + "-block-007",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock007(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 7) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 7;
    } else {
      total -= (7 % 5);
    }
  }
  return total;
}

std::string DescribeBlock007(const Record& record) {
  std::ostringstream stream;
  stream << "block=007"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 008 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock008 = {8, 9, 10, 11, 12, 13};

Record MakeRecord008(std::string_view seed, int salt) {
  const double scale = static_cast<double>((8 % 7) + 1) / 3.0;
  const bool active = ((salt + 8) % 2) == 0;
  return Record{
      .id = salt + 8,
      .label = std::string(seed) + "-block-008",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock008(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 8) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 8;
    } else {
      total -= (8 % 5);
    }
  }
  return total;
}

std::string DescribeBlock008(const Record& record) {
  std::ostringstream stream;
  stream << "block=008"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 009 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock009 = {9, 10, 11, 12, 13, 14};

Record MakeRecord009(std::string_view seed, int salt) {
  const double scale = static_cast<double>((9 % 7) + 1) / 3.0;
  const bool active = ((salt + 9) % 2) == 0;
  return Record{
      .id = salt + 9,
      .label = std::string(seed) + "-block-009",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock009(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 9) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 9;
    } else {
      total -= (9 % 5);
    }
  }
  return total;
}

std::string DescribeBlock009(const Record& record) {
  std::ostringstream stream;
  stream << "block=009"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 010 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock010 = {10, 11, 12, 13, 14, 15};

Record MakeRecord010(std::string_view seed, int salt) {
  const double scale = static_cast<double>((10 % 7) + 1) / 3.0;
  const bool active = ((salt + 10) % 2) == 0;
  return Record{
      .id = salt + 10,
      .label = std::string(seed) + "-block-010",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock010(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 10) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 10;
    } else {
      total -= (10 % 5);
    }
  }
  return total;
}

std::string DescribeBlock010(const Record& record) {
  std::ostringstream stream;
  stream << "block=010"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 011 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock011 = {11, 12, 13, 14, 15, 16};

Record MakeRecord011(std::string_view seed, int salt) {
  const double scale = static_cast<double>((11 % 7) + 1) / 3.0;
  const bool active = ((salt + 11) % 2) == 0;
  return Record{
      .id = salt + 11,
      .label = std::string(seed) + "-block-011",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock011(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 11) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 11;
    } else {
      total -= (11 % 5);
    }
  }
  return total;
}

std::string DescribeBlock011(const Record& record) {
  std::ostringstream stream;
  stream << "block=011"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 012 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock012 = {12, 13, 14, 15, 16, 17};

Record MakeRecord012(std::string_view seed, int salt) {
  const double scale = static_cast<double>((12 % 7) + 1) / 3.0;
  const bool active = ((salt + 12) % 2) == 0;
  return Record{
      .id = salt + 12,
      .label = std::string(seed) + "-block-012",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock012(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 12) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 12;
    } else {
      total -= (12 % 5);
    }
  }
  return total;
}

std::string DescribeBlock012(const Record& record) {
  std::ostringstream stream;
  stream << "block=012"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 013 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock013 = {13, 14, 15, 16, 17, 18};

Record MakeRecord013(std::string_view seed, int salt) {
  const double scale = static_cast<double>((13 % 7) + 1) / 3.0;
  const bool active = ((salt + 13) % 2) == 0;
  return Record{
      .id = salt + 13,
      .label = std::string(seed) + "-block-013",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock013(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 13) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 13;
    } else {
      total -= (13 % 5);
    }
  }
  return total;
}

std::string DescribeBlock013(const Record& record) {
  std::ostringstream stream;
  stream << "block=013"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 014 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock014 = {14, 15, 16, 17, 18, 19};

Record MakeRecord014(std::string_view seed, int salt) {
  const double scale = static_cast<double>((14 % 7) + 1) / 3.0;
  const bool active = ((salt + 14) % 2) == 0;
  return Record{
      .id = salt + 14,
      .label = std::string(seed) + "-block-014",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock014(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 14) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 14;
    } else {
      total -= (14 % 5);
    }
  }
  return total;
}

std::string DescribeBlock014(const Record& record) {
  std::ostringstream stream;
  stream << "block=014"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 015 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock015 = {15, 16, 17, 18, 19, 20};

Record MakeRecord015(std::string_view seed, int salt) {
  const double scale = static_cast<double>((15 % 7) + 1) / 3.0;
  const bool active = ((salt + 15) % 2) == 0;
  return Record{
      .id = salt + 15,
      .label = std::string(seed) + "-block-015",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock015(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 15) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 15;
    } else {
      total -= (15 % 5);
    }
  }
  return total;
}

std::string DescribeBlock015(const Record& record) {
  std::ostringstream stream;
  stream << "block=015"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

/*
  Multi-line comment block inserted periodically so syntax tests see
  long comment regions amid otherwise repetitive code.
*/
std::optional<Record> FindSpecialRecord015(const std::vector<Record>& records) {
  auto it = std::find_if(records.begin(), records.end(), [](const Record& record) {
    return record.active && (record.id % 5) == 0;
  });
  if (it == records.end()) {
    return std::nullopt;
  }
  return *it;
}

// block 016 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock016 = {16, 17, 18, 19, 20, 21};

Record MakeRecord016(std::string_view seed, int salt) {
  const double scale = static_cast<double>((16 % 7) + 1) / 3.0;
  const bool active = ((salt + 16) % 2) == 0;
  return Record{
      .id = salt + 16,
      .label = std::string(seed) + "-block-016",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock016(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 16) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 16;
    } else {
      total -= (16 % 5);
    }
  }
  return total;
}

std::string DescribeBlock016(const Record& record) {
  std::ostringstream stream;
  stream << "block=016"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 017 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock017 = {17, 18, 19, 20, 21, 22};

Record MakeRecord017(std::string_view seed, int salt) {
  const double scale = static_cast<double>((17 % 7) + 1) / 3.0;
  const bool active = ((salt + 17) % 2) == 0;
  return Record{
      .id = salt + 17,
      .label = std::string(seed) + "-block-017",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock017(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 17) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 17;
    } else {
      total -= (17 % 5);
    }
  }
  return total;
}

std::string DescribeBlock017(const Record& record) {
  std::ostringstream stream;
  stream << "block=017"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 018 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock018 = {18, 19, 20, 21, 22, 23};

Record MakeRecord018(std::string_view seed, int salt) {
  const double scale = static_cast<double>((18 % 7) + 1) / 3.0;
  const bool active = ((salt + 18) % 2) == 0;
  return Record{
      .id = salt + 18,
      .label = std::string(seed) + "-block-018",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock018(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 18) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 18;
    } else {
      total -= (18 % 5);
    }
  }
  return total;
}

std::string DescribeBlock018(const Record& record) {
  std::ostringstream stream;
  stream << "block=018"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 019 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock019 = {19, 20, 21, 22, 23, 24};

Record MakeRecord019(std::string_view seed, int salt) {
  const double scale = static_cast<double>((19 % 7) + 1) / 3.0;
  const bool active = ((salt + 19) % 2) == 0;
  return Record{
      .id = salt + 19,
      .label = std::string(seed) + "-block-019",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock019(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 19) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 19;
    } else {
      total -= (19 % 5);
    }
  }
  return total;
}

std::string DescribeBlock019(const Record& record) {
  std::ostringstream stream;
  stream << "block=019"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 020 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock020 = {20, 21, 22, 23, 24, 25};

Record MakeRecord020(std::string_view seed, int salt) {
  const double scale = static_cast<double>((20 % 7) + 1) / 3.0;
  const bool active = ((salt + 20) % 2) == 0;
  return Record{
      .id = salt + 20,
      .label = std::string(seed) + "-block-020",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock020(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 20) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 20;
    } else {
      total -= (20 % 5);
    }
  }
  return total;
}

std::string DescribeBlock020(const Record& record) {
  std::ostringstream stream;
  stream << "block=020"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 021 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock021 = {21, 22, 23, 24, 25, 26};

Record MakeRecord021(std::string_view seed, int salt) {
  const double scale = static_cast<double>((21 % 7) + 1) / 3.0;
  const bool active = ((salt + 21) % 2) == 0;
  return Record{
      .id = salt + 21,
      .label = std::string(seed) + "-block-021",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock021(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 21) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 21;
    } else {
      total -= (21 % 5);
    }
  }
  return total;
}

std::string DescribeBlock021(const Record& record) {
  std::ostringstream stream;
  stream << "block=021"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 022 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock022 = {22, 23, 24, 25, 26, 27};

Record MakeRecord022(std::string_view seed, int salt) {
  const double scale = static_cast<double>((22 % 7) + 1) / 3.0;
  const bool active = ((salt + 22) % 2) == 0;
  return Record{
      .id = salt + 22,
      .label = std::string(seed) + "-block-022",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock022(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 22) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 22;
    } else {
      total -= (22 % 5);
    }
  }
  return total;
}

std::string DescribeBlock022(const Record& record) {
  std::ostringstream stream;
  stream << "block=022"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 023 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock023 = {23, 24, 25, 26, 27, 28};

Record MakeRecord023(std::string_view seed, int salt) {
  const double scale = static_cast<double>((23 % 7) + 1) / 3.0;
  const bool active = ((salt + 23) % 2) == 0;
  return Record{
      .id = salt + 23,
      .label = std::string(seed) + "-block-023",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock023(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 23) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 23;
    } else {
      total -= (23 % 5);
    }
  }
  return total;
}

std::string DescribeBlock023(const Record& record) {
  std::ostringstream stream;
  stream << "block=023"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 024 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock024 = {24, 25, 26, 27, 28, 29};

Record MakeRecord024(std::string_view seed, int salt) {
  const double scale = static_cast<double>((24 % 7) + 1) / 3.0;
  const bool active = ((salt + 24) % 2) == 0;
  return Record{
      .id = salt + 24,
      .label = std::string(seed) + "-block-024",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock024(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 24) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 24;
    } else {
      total -= (24 % 5);
    }
  }
  return total;
}

std::string DescribeBlock024(const Record& record) {
  std::ostringstream stream;
  stream << "block=024"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 025 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock025 = {25, 26, 27, 28, 29, 30};

Record MakeRecord025(std::string_view seed, int salt) {
  const double scale = static_cast<double>((25 % 7) + 1) / 3.0;
  const bool active = ((salt + 25) % 2) == 0;
  return Record{
      .id = salt + 25,
      .label = std::string(seed) + "-block-025",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock025(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 25) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 25;
    } else {
      total -= (25 % 5);
    }
  }
  return total;
}

std::string DescribeBlock025(const Record& record) {
  std::ostringstream stream;
  stream << "block=025"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 026 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock026 = {26, 27, 28, 29, 30, 31};

Record MakeRecord026(std::string_view seed, int salt) {
  const double scale = static_cast<double>((26 % 7) + 1) / 3.0;
  const bool active = ((salt + 26) % 2) == 0;
  return Record{
      .id = salt + 26,
      .label = std::string(seed) + "-block-026",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock026(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 26) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 26;
    } else {
      total -= (26 % 5);
    }
  }
  return total;
}

std::string DescribeBlock026(const Record& record) {
  std::ostringstream stream;
  stream << "block=026"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 027 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock027 = {27, 28, 29, 30, 31, 32};

Record MakeRecord027(std::string_view seed, int salt) {
  const double scale = static_cast<double>((27 % 7) + 1) / 3.0;
  const bool active = ((salt + 27) % 2) == 0;
  return Record{
      .id = salt + 27,
      .label = std::string(seed) + "-block-027",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock027(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 27) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 27;
    } else {
      total -= (27 % 5);
    }
  }
  return total;
}

std::string DescribeBlock027(const Record& record) {
  std::ostringstream stream;
  stream << "block=027"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 028 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock028 = {28, 29, 30, 31, 32, 33};

Record MakeRecord028(std::string_view seed, int salt) {
  const double scale = static_cast<double>((28 % 7) + 1) / 3.0;
  const bool active = ((salt + 28) % 2) == 0;
  return Record{
      .id = salt + 28,
      .label = std::string(seed) + "-block-028",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock028(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 28) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 28;
    } else {
      total -= (28 % 5);
    }
  }
  return total;
}

std::string DescribeBlock028(const Record& record) {
  std::ostringstream stream;
  stream << "block=028"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 029 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock029 = {29, 30, 31, 32, 33, 34};

Record MakeRecord029(std::string_view seed, int salt) {
  const double scale = static_cast<double>((29 % 7) + 1) / 3.0;
  const bool active = ((salt + 29) % 2) == 0;
  return Record{
      .id = salt + 29,
      .label = std::string(seed) + "-block-029",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock029(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 29) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 29;
    } else {
      total -= (29 % 5);
    }
  }
  return total;
}

std::string DescribeBlock029(const Record& record) {
  std::ostringstream stream;
  stream << "block=029"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 030 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock030 = {30, 31, 32, 33, 34, 35};

Record MakeRecord030(std::string_view seed, int salt) {
  const double scale = static_cast<double>((30 % 7) + 1) / 3.0;
  const bool active = ((salt + 30) % 2) == 0;
  return Record{
      .id = salt + 30,
      .label = std::string(seed) + "-block-030",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock030(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 30) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 30;
    } else {
      total -= (30 % 5);
    }
  }
  return total;
}

std::string DescribeBlock030(const Record& record) {
  std::ostringstream stream;
  stream << "block=030"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

/*
  Multi-line comment block inserted periodically so syntax tests see
  long comment regions amid otherwise repetitive code.
*/
std::optional<Record> FindSpecialRecord030(const std::vector<Record>& records) {
  auto it = std::find_if(records.begin(), records.end(), [](const Record& record) {
    return record.active && (record.id % 10) == 0;
  });
  if (it == records.end()) {
    return std::nullopt;
  }
  return *it;
}

// block 031 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock031 = {31, 32, 33, 34, 35, 36};

Record MakeRecord031(std::string_view seed, int salt) {
  const double scale = static_cast<double>((31 % 7) + 1) / 3.0;
  const bool active = ((salt + 31) % 2) == 0;
  return Record{
      .id = salt + 31,
      .label = std::string(seed) + "-block-031",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock031(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 31) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 31;
    } else {
      total -= (31 % 5);
    }
  }
  return total;
}

std::string DescribeBlock031(const Record& record) {
  std::ostringstream stream;
  stream << "block=031"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 032 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock032 = {32, 33, 34, 35, 36, 37};

Record MakeRecord032(std::string_view seed, int salt) {
  const double scale = static_cast<double>((32 % 7) + 1) / 3.0;
  const bool active = ((salt + 32) % 2) == 0;
  return Record{
      .id = salt + 32,
      .label = std::string(seed) + "-block-032",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock032(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 32) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 32;
    } else {
      total -= (32 % 5);
    }
  }
  return total;
}

std::string DescribeBlock032(const Record& record) {
  std::ostringstream stream;
  stream << "block=032"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 033 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock033 = {33, 34, 35, 36, 37, 38};

Record MakeRecord033(std::string_view seed, int salt) {
  const double scale = static_cast<double>((33 % 7) + 1) / 3.0;
  const bool active = ((salt + 33) % 2) == 0;
  return Record{
      .id = salt + 33,
      .label = std::string(seed) + "-block-033",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock033(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 33) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 33;
    } else {
      total -= (33 % 5);
    }
  }
  return total;
}

std::string DescribeBlock033(const Record& record) {
  std::ostringstream stream;
  stream << "block=033"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 034 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock034 = {34, 35, 36, 37, 38, 39};

Record MakeRecord034(std::string_view seed, int salt) {
  const double scale = static_cast<double>((34 % 7) + 1) / 3.0;
  const bool active = ((salt + 34) % 2) == 0;
  return Record{
      .id = salt + 34,
      .label = std::string(seed) + "-block-034",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock034(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 34) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 34;
    } else {
      total -= (34 % 5);
    }
  }
  return total;
}

std::string DescribeBlock034(const Record& record) {
  std::ostringstream stream;
  stream << "block=034"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 035 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock035 = {35, 36, 37, 38, 39, 40};

Record MakeRecord035(std::string_view seed, int salt) {
  const double scale = static_cast<double>((35 % 7) + 1) / 3.0;
  const bool active = ((salt + 35) % 2) == 0;
  return Record{
      .id = salt + 35,
      .label = std::string(seed) + "-block-035",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock035(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 35) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 35;
    } else {
      total -= (35 % 5);
    }
  }
  return total;
}

std::string DescribeBlock035(const Record& record) {
  std::ostringstream stream;
  stream << "block=035"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 036 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock036 = {36, 37, 38, 39, 40, 41};

Record MakeRecord036(std::string_view seed, int salt) {
  const double scale = static_cast<double>((36 % 7) + 1) / 3.0;
  const bool active = ((salt + 36) % 2) == 0;
  return Record{
      .id = salt + 36,
      .label = std::string(seed) + "-block-036",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock036(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 36) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 36;
    } else {
      total -= (36 % 5);
    }
  }
  return total;
}

std::string DescribeBlock036(const Record& record) {
  std::ostringstream stream;
  stream << "block=036"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 037 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock037 = {37, 38, 39, 40, 41, 42};

Record MakeRecord037(std::string_view seed, int salt) {
  const double scale = static_cast<double>((37 % 7) + 1) / 3.0;
  const bool active = ((salt + 37) % 2) == 0;
  return Record{
      .id = salt + 37,
      .label = std::string(seed) + "-block-037",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock037(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 37) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 37;
    } else {
      total -= (37 % 5);
    }
  }
  return total;
}

std::string DescribeBlock037(const Record& record) {
  std::ostringstream stream;
  stream << "block=037"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 038 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock038 = {38, 39, 40, 41, 42, 43};

Record MakeRecord038(std::string_view seed, int salt) {
  const double scale = static_cast<double>((38 % 7) + 1) / 3.0;
  const bool active = ((salt + 38) % 2) == 0;
  return Record{
      .id = salt + 38,
      .label = std::string(seed) + "-block-038",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock038(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 38) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 38;
    } else {
      total -= (38 % 5);
    }
  }
  return total;
}

std::string DescribeBlock038(const Record& record) {
  std::ostringstream stream;
  stream << "block=038"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 039 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock039 = {39, 40, 41, 42, 43, 44};

Record MakeRecord039(std::string_view seed, int salt) {
  const double scale = static_cast<double>((39 % 7) + 1) / 3.0;
  const bool active = ((salt + 39) % 2) == 0;
  return Record{
      .id = salt + 39,
      .label = std::string(seed) + "-block-039",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock039(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 39) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 39;
    } else {
      total -= (39 % 5);
    }
  }
  return total;
}

std::string DescribeBlock039(const Record& record) {
  std::ostringstream stream;
  stream << "block=039"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 040 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock040 = {40, 41, 42, 43, 44, 45};

Record MakeRecord040(std::string_view seed, int salt) {
  const double scale = static_cast<double>((40 % 7) + 1) / 3.0;
  const bool active = ((salt + 40) % 2) == 0;
  return Record{
      .id = salt + 40,
      .label = std::string(seed) + "-block-040",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock040(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 40) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 40;
    } else {
      total -= (40 % 5);
    }
  }
  return total;
}

std::string DescribeBlock040(const Record& record) {
  std::ostringstream stream;
  stream << "block=040"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 041 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock041 = {41, 42, 43, 44, 45, 46};

Record MakeRecord041(std::string_view seed, int salt) {
  const double scale = static_cast<double>((41 % 7) + 1) / 3.0;
  const bool active = ((salt + 41) % 2) == 0;
  return Record{
      .id = salt + 41,
      .label = std::string(seed) + "-block-041",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock041(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 41) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 41;
    } else {
      total -= (41 % 5);
    }
  }
  return total;
}

std::string DescribeBlock041(const Record& record) {
  std::ostringstream stream;
  stream << "block=041"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 042 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock042 = {42, 43, 44, 45, 46, 47};

Record MakeRecord042(std::string_view seed, int salt) {
  const double scale = static_cast<double>((42 % 7) + 1) / 3.0;
  const bool active = ((salt + 42) % 2) == 0;
  return Record{
      .id = salt + 42,
      .label = std::string(seed) + "-block-042",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock042(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 42) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 42;
    } else {
      total -= (42 % 5);
    }
  }
  return total;
}

std::string DescribeBlock042(const Record& record) {
  std::ostringstream stream;
  stream << "block=042"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 043 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock043 = {43, 44, 45, 46, 47, 48};

Record MakeRecord043(std::string_view seed, int salt) {
  const double scale = static_cast<double>((43 % 7) + 1) / 3.0;
  const bool active = ((salt + 43) % 2) == 0;
  return Record{
      .id = salt + 43,
      .label = std::string(seed) + "-block-043",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock043(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 43) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 43;
    } else {
      total -= (43 % 5);
    }
  }
  return total;
}

std::string DescribeBlock043(const Record& record) {
  std::ostringstream stream;
  stream << "block=043"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 044 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock044 = {44, 45, 46, 47, 48, 49};

Record MakeRecord044(std::string_view seed, int salt) {
  const double scale = static_cast<double>((44 % 7) + 1) / 3.0;
  const bool active = ((salt + 44) % 2) == 0;
  return Record{
      .id = salt + 44,
      .label = std::string(seed) + "-block-044",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock044(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 44) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 44;
    } else {
      total -= (44 % 5);
    }
  }
  return total;
}

std::string DescribeBlock044(const Record& record) {
  std::ostringstream stream;
  stream << "block=044"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 045 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock045 = {45, 46, 47, 48, 49, 50};

Record MakeRecord045(std::string_view seed, int salt) {
  const double scale = static_cast<double>((45 % 7) + 1) / 3.0;
  const bool active = ((salt + 45) % 2) == 0;
  return Record{
      .id = salt + 45,
      .label = std::string(seed) + "-block-045",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock045(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 45) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 45;
    } else {
      total -= (45 % 5);
    }
  }
  return total;
}

std::string DescribeBlock045(const Record& record) {
  std::ostringstream stream;
  stream << "block=045"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

/*
  Multi-line comment block inserted periodically so syntax tests see
  long comment regions amid otherwise repetitive code.
*/
std::optional<Record> FindSpecialRecord045(const std::vector<Record>& records) {
  auto it = std::find_if(records.begin(), records.end(), [](const Record& record) {
    return record.active && (record.id % 15) == 0;
  });
  if (it == records.end()) {
    return std::nullopt;
  }
  return *it;
}

// block 046 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock046 = {46, 47, 48, 49, 50, 51};

Record MakeRecord046(std::string_view seed, int salt) {
  const double scale = static_cast<double>((46 % 7) + 1) / 3.0;
  const bool active = ((salt + 46) % 2) == 0;
  return Record{
      .id = salt + 46,
      .label = std::string(seed) + "-block-046",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock046(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 46) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 46;
    } else {
      total -= (46 % 5);
    }
  }
  return total;
}

std::string DescribeBlock046(const Record& record) {
  std::ostringstream stream;
  stream << "block=046"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 047 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock047 = {47, 48, 49, 50, 51, 52};

Record MakeRecord047(std::string_view seed, int salt) {
  const double scale = static_cast<double>((47 % 7) + 1) / 3.0;
  const bool active = ((salt + 47) % 2) == 0;
  return Record{
      .id = salt + 47,
      .label = std::string(seed) + "-block-047",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock047(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 47) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 47;
    } else {
      total -= (47 % 5);
    }
  }
  return total;
}

std::string DescribeBlock047(const Record& record) {
  std::ostringstream stream;
  stream << "block=047"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 048 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock048 = {48, 49, 50, 51, 52, 53};

Record MakeRecord048(std::string_view seed, int salt) {
  const double scale = static_cast<double>((48 % 7) + 1) / 3.0;
  const bool active = ((salt + 48) % 2) == 0;
  return Record{
      .id = salt + 48,
      .label = std::string(seed) + "-block-048",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock048(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 48) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 48;
    } else {
      total -= (48 % 5);
    }
  }
  return total;
}

std::string DescribeBlock048(const Record& record) {
  std::ostringstream stream;
  stream << "block=048"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 049 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock049 = {49, 50, 51, 52, 53, 54};

Record MakeRecord049(std::string_view seed, int salt) {
  const double scale = static_cast<double>((49 % 7) + 1) / 3.0;
  const bool active = ((salt + 49) % 2) == 0;
  return Record{
      .id = salt + 49,
      .label = std::string(seed) + "-block-049",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock049(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 49) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 49;
    } else {
      total -= (49 % 5);
    }
  }
  return total;
}

std::string DescribeBlock049(const Record& record) {
  std::ostringstream stream;
  stream << "block=049"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 050 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock050 = {50, 51, 52, 53, 54, 55};

Record MakeRecord050(std::string_view seed, int salt) {
  const double scale = static_cast<double>((50 % 7) + 1) / 3.0;
  const bool active = ((salt + 50) % 2) == 0;
  return Record{
      .id = salt + 50,
      .label = std::string(seed) + "-block-050",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock050(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 50) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 50;
    } else {
      total -= (50 % 5);
    }
  }
  return total;
}

std::string DescribeBlock050(const Record& record) {
  std::ostringstream stream;
  stream << "block=050"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 051 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock051 = {51, 52, 53, 54, 55, 56};

Record MakeRecord051(std::string_view seed, int salt) {
  const double scale = static_cast<double>((51 % 7) + 1) / 3.0;
  const bool active = ((salt + 51) % 2) == 0;
  return Record{
      .id = salt + 51,
      .label = std::string(seed) + "-block-051",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock051(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 51) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 51;
    } else {
      total -= (51 % 5);
    }
  }
  return total;
}

std::string DescribeBlock051(const Record& record) {
  std::ostringstream stream;
  stream << "block=051"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 052 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock052 = {52, 53, 54, 55, 56, 57};

Record MakeRecord052(std::string_view seed, int salt) {
  const double scale = static_cast<double>((52 % 7) + 1) / 3.0;
  const bool active = ((salt + 52) % 2) == 0;
  return Record{
      .id = salt + 52,
      .label = std::string(seed) + "-block-052",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock052(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 52) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 52;
    } else {
      total -= (52 % 5);
    }
  }
  return total;
}

std::string DescribeBlock052(const Record& record) {
  std::ostringstream stream;
  stream << "block=052"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 053 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock053 = {53, 54, 55, 56, 57, 58};

Record MakeRecord053(std::string_view seed, int salt) {
  const double scale = static_cast<double>((53 % 7) + 1) / 3.0;
  const bool active = ((salt + 53) % 2) == 0;
  return Record{
      .id = salt + 53,
      .label = std::string(seed) + "-block-053",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock053(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 53) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 53;
    } else {
      total -= (53 % 5);
    }
  }
  return total;
}

std::string DescribeBlock053(const Record& record) {
  std::ostringstream stream;
  stream << "block=053"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 054 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock054 = {54, 55, 56, 57, 58, 59};

Record MakeRecord054(std::string_view seed, int salt) {
  const double scale = static_cast<double>((54 % 7) + 1) / 3.0;
  const bool active = ((salt + 54) % 2) == 0;
  return Record{
      .id = salt + 54,
      .label = std::string(seed) + "-block-054",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock054(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 54) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 54;
    } else {
      total -= (54 % 5);
    }
  }
  return total;
}

std::string DescribeBlock054(const Record& record) {
  std::ostringstream stream;
  stream << "block=054"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 055 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock055 = {55, 56, 57, 58, 59, 60};

Record MakeRecord055(std::string_view seed, int salt) {
  const double scale = static_cast<double>((55 % 7) + 1) / 3.0;
  const bool active = ((salt + 55) % 2) == 0;
  return Record{
      .id = salt + 55,
      .label = std::string(seed) + "-block-055",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock055(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 55) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 55;
    } else {
      total -= (55 % 5);
    }
  }
  return total;
}

std::string DescribeBlock055(const Record& record) {
  std::ostringstream stream;
  stream << "block=055"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 056 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock056 = {56, 57, 58, 59, 60, 61};

Record MakeRecord056(std::string_view seed, int salt) {
  const double scale = static_cast<double>((56 % 7) + 1) / 3.0;
  const bool active = ((salt + 56) % 2) == 0;
  return Record{
      .id = salt + 56,
      .label = std::string(seed) + "-block-056",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock056(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 56) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 56;
    } else {
      total -= (56 % 5);
    }
  }
  return total;
}

std::string DescribeBlock056(const Record& record) {
  std::ostringstream stream;
  stream << "block=056"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 057 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock057 = {57, 58, 59, 60, 61, 62};

Record MakeRecord057(std::string_view seed, int salt) {
  const double scale = static_cast<double>((57 % 7) + 1) / 3.0;
  const bool active = ((salt + 57) % 2) == 0;
  return Record{
      .id = salt + 57,
      .label = std::string(seed) + "-block-057",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock057(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 57) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 57;
    } else {
      total -= (57 % 5);
    }
  }
  return total;
}

std::string DescribeBlock057(const Record& record) {
  std::ostringstream stream;
  stream << "block=057"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 058 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock058 = {58, 59, 60, 61, 62, 63};

Record MakeRecord058(std::string_view seed, int salt) {
  const double scale = static_cast<double>((58 % 7) + 1) / 3.0;
  const bool active = ((salt + 58) % 2) == 0;
  return Record{
      .id = salt + 58,
      .label = std::string(seed) + "-block-058",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock058(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 58) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 58;
    } else {
      total -= (58 % 5);
    }
  }
  return total;
}

std::string DescribeBlock058(const Record& record) {
  std::ostringstream stream;
  stream << "block=058"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 059 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock059 = {59, 60, 61, 62, 63, 64};

Record MakeRecord059(std::string_view seed, int salt) {
  const double scale = static_cast<double>((59 % 7) + 1) / 3.0;
  const bool active = ((salt + 59) % 2) == 0;
  return Record{
      .id = salt + 59,
      .label = std::string(seed) + "-block-059",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock059(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 59) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 59;
    } else {
      total -= (59 % 5);
    }
  }
  return total;
}

std::string DescribeBlock059(const Record& record) {
  std::ostringstream stream;
  stream << "block=059"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 060 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock060 = {60, 61, 62, 63, 64, 65};

Record MakeRecord060(std::string_view seed, int salt) {
  const double scale = static_cast<double>((60 % 7) + 1) / 3.0;
  const bool active = ((salt + 60) % 2) == 0;
  return Record{
      .id = salt + 60,
      .label = std::string(seed) + "-block-060",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock060(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 60) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 60;
    } else {
      total -= (60 % 5);
    }
  }
  return total;
}

std::string DescribeBlock060(const Record& record) {
  std::ostringstream stream;
  stream << "block=060"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

/*
  Multi-line comment block inserted periodically so syntax tests see
  long comment regions amid otherwise repetitive code.
*/
std::optional<Record> FindSpecialRecord060(const std::vector<Record>& records) {
  auto it = std::find_if(records.begin(), records.end(), [](const Record& record) {
    return record.active && (record.id % 20) == 0;
  });
  if (it == records.end()) {
    return std::nullopt;
  }
  return *it;
}

// block 061 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock061 = {61, 62, 63, 64, 65, 66};

Record MakeRecord061(std::string_view seed, int salt) {
  const double scale = static_cast<double>((61 % 7) + 1) / 3.0;
  const bool active = ((salt + 61) % 2) == 0;
  return Record{
      .id = salt + 61,
      .label = std::string(seed) + "-block-061",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock061(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 61) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 61;
    } else {
      total -= (61 % 5);
    }
  }
  return total;
}

std::string DescribeBlock061(const Record& record) {
  std::ostringstream stream;
  stream << "block=061"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 062 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock062 = {62, 63, 64, 65, 66, 67};

Record MakeRecord062(std::string_view seed, int salt) {
  const double scale = static_cast<double>((62 % 7) + 1) / 3.0;
  const bool active = ((salt + 62) % 2) == 0;
  return Record{
      .id = salt + 62,
      .label = std::string(seed) + "-block-062",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock062(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 62) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 62;
    } else {
      total -= (62 % 5);
    }
  }
  return total;
}

std::string DescribeBlock062(const Record& record) {
  std::ostringstream stream;
  stream << "block=062"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 063 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock063 = {63, 64, 65, 66, 67, 68};

Record MakeRecord063(std::string_view seed, int salt) {
  const double scale = static_cast<double>((63 % 7) + 1) / 3.0;
  const bool active = ((salt + 63) % 2) == 0;
  return Record{
      .id = salt + 63,
      .label = std::string(seed) + "-block-063",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock063(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 63) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 63;
    } else {
      total -= (63 % 5);
    }
  }
  return total;
}

std::string DescribeBlock063(const Record& record) {
  std::ostringstream stream;
  stream << "block=063"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 064 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock064 = {64, 65, 66, 67, 68, 69};

Record MakeRecord064(std::string_view seed, int salt) {
  const double scale = static_cast<double>((64 % 7) + 1) / 3.0;
  const bool active = ((salt + 64) % 2) == 0;
  return Record{
      .id = salt + 64,
      .label = std::string(seed) + "-block-064",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock064(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 64) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 64;
    } else {
      total -= (64 % 5);
    }
  }
  return total;
}

std::string DescribeBlock064(const Record& record) {
  std::ostringstream stream;
  stream << "block=064"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 065 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock065 = {65, 66, 67, 68, 69, 70};

Record MakeRecord065(std::string_view seed, int salt) {
  const double scale = static_cast<double>((65 % 7) + 1) / 3.0;
  const bool active = ((salt + 65) % 2) == 0;
  return Record{
      .id = salt + 65,
      .label = std::string(seed) + "-block-065",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock065(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 65) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 65;
    } else {
      total -= (65 % 5);
    }
  }
  return total;
}

std::string DescribeBlock065(const Record& record) {
  std::ostringstream stream;
  stream << "block=065"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 066 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock066 = {66, 67, 68, 69, 70, 71};

Record MakeRecord066(std::string_view seed, int salt) {
  const double scale = static_cast<double>((66 % 7) + 1) / 3.0;
  const bool active = ((salt + 66) % 2) == 0;
  return Record{
      .id = salt + 66,
      .label = std::string(seed) + "-block-066",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock066(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 66) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 66;
    } else {
      total -= (66 % 5);
    }
  }
  return total;
}

std::string DescribeBlock066(const Record& record) {
  std::ostringstream stream;
  stream << "block=066"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 067 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock067 = {67, 68, 69, 70, 71, 72};

Record MakeRecord067(std::string_view seed, int salt) {
  const double scale = static_cast<double>((67 % 7) + 1) / 3.0;
  const bool active = ((salt + 67) % 2) == 0;
  return Record{
      .id = salt + 67,
      .label = std::string(seed) + "-block-067",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock067(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 67) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 67;
    } else {
      total -= (67 % 5);
    }
  }
  return total;
}

std::string DescribeBlock067(const Record& record) {
  std::ostringstream stream;
  stream << "block=067"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 068 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock068 = {68, 69, 70, 71, 72, 73};

Record MakeRecord068(std::string_view seed, int salt) {
  const double scale = static_cast<double>((68 % 7) + 1) / 3.0;
  const bool active = ((salt + 68) % 2) == 0;
  return Record{
      .id = salt + 68,
      .label = std::string(seed) + "-block-068",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock068(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 68) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 68;
    } else {
      total -= (68 % 5);
    }
  }
  return total;
}

std::string DescribeBlock068(const Record& record) {
  std::ostringstream stream;
  stream << "block=068"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 069 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock069 = {69, 70, 71, 72, 73, 74};

Record MakeRecord069(std::string_view seed, int salt) {
  const double scale = static_cast<double>((69 % 7) + 1) / 3.0;
  const bool active = ((salt + 69) % 2) == 0;
  return Record{
      .id = salt + 69,
      .label = std::string(seed) + "-block-069",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock069(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 69) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 69;
    } else {
      total -= (69 % 5);
    }
  }
  return total;
}

std::string DescribeBlock069(const Record& record) {
  std::ostringstream stream;
  stream << "block=069"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 070 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock070 = {70, 71, 72, 73, 74, 75};

Record MakeRecord070(std::string_view seed, int salt) {
  const double scale = static_cast<double>((70 % 7) + 1) / 3.0;
  const bool active = ((salt + 70) % 2) == 0;
  return Record{
      .id = salt + 70,
      .label = std::string(seed) + "-block-070",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock070(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 70) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 70;
    } else {
      total -= (70 % 5);
    }
  }
  return total;
}

std::string DescribeBlock070(const Record& record) {
  std::ostringstream stream;
  stream << "block=070"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 071 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock071 = {71, 72, 73, 74, 75, 76};

Record MakeRecord071(std::string_view seed, int salt) {
  const double scale = static_cast<double>((71 % 7) + 1) / 3.0;
  const bool active = ((salt + 71) % 2) == 0;
  return Record{
      .id = salt + 71,
      .label = std::string(seed) + "-block-071",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock071(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 71) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 71;
    } else {
      total -= (71 % 5);
    }
  }
  return total;
}

std::string DescribeBlock071(const Record& record) {
  std::ostringstream stream;
  stream << "block=071"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 072 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock072 = {72, 73, 74, 75, 76, 77};

Record MakeRecord072(std::string_view seed, int salt) {
  const double scale = static_cast<double>((72 % 7) + 1) / 3.0;
  const bool active = ((salt + 72) % 2) == 0;
  return Record{
      .id = salt + 72,
      .label = std::string(seed) + "-block-072",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock072(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 72) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 72;
    } else {
      total -= (72 % 5);
    }
  }
  return total;
}

std::string DescribeBlock072(const Record& record) {
  std::ostringstream stream;
  stream << "block=072"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 073 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock073 = {73, 74, 75, 76, 77, 78};

Record MakeRecord073(std::string_view seed, int salt) {
  const double scale = static_cast<double>((73 % 7) + 1) / 3.0;
  const bool active = ((salt + 73) % 2) == 0;
  return Record{
      .id = salt + 73,
      .label = std::string(seed) + "-block-073",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock073(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 73) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 73;
    } else {
      total -= (73 % 5);
    }
  }
  return total;
}

std::string DescribeBlock073(const Record& record) {
  std::ostringstream stream;
  stream << "block=073"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 074 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock074 = {74, 75, 76, 77, 78, 79};

Record MakeRecord074(std::string_view seed, int salt) {
  const double scale = static_cast<double>((74 % 7) + 1) / 3.0;
  const bool active = ((salt + 74) % 2) == 0;
  return Record{
      .id = salt + 74,
      .label = std::string(seed) + "-block-074",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock074(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 74) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 74;
    } else {
      total -= (74 % 5);
    }
  }
  return total;
}

std::string DescribeBlock074(const Record& record) {
  std::ostringstream stream;
  stream << "block=074"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 075 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock075 = {75, 76, 77, 78, 79, 80};

Record MakeRecord075(std::string_view seed, int salt) {
  const double scale = static_cast<double>((75 % 7) + 1) / 3.0;
  const bool active = ((salt + 75) % 2) == 0;
  return Record{
      .id = salt + 75,
      .label = std::string(seed) + "-block-075",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock075(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 75) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 75;
    } else {
      total -= (75 % 5);
    }
  }
  return total;
}

std::string DescribeBlock075(const Record& record) {
  std::ostringstream stream;
  stream << "block=075"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

/*
  Multi-line comment block inserted periodically so syntax tests see
  long comment regions amid otherwise repetitive code.
*/
std::optional<Record> FindSpecialRecord075(const std::vector<Record>& records) {
  auto it = std::find_if(records.begin(), records.end(), [](const Record& record) {
    return record.active && (record.id % 25) == 0;
  });
  if (it == records.end()) {
    return std::nullopt;
  }
  return *it;
}

// block 076 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock076 = {76, 77, 78, 79, 80, 81};

Record MakeRecord076(std::string_view seed, int salt) {
  const double scale = static_cast<double>((76 % 7) + 1) / 3.0;
  const bool active = ((salt + 76) % 2) == 0;
  return Record{
      .id = salt + 76,
      .label = std::string(seed) + "-block-076",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock076(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 76) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 76;
    } else {
      total -= (76 % 5);
    }
  }
  return total;
}

std::string DescribeBlock076(const Record& record) {
  std::ostringstream stream;
  stream << "block=076"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 077 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock077 = {77, 78, 79, 80, 81, 82};

Record MakeRecord077(std::string_view seed, int salt) {
  const double scale = static_cast<double>((77 % 7) + 1) / 3.0;
  const bool active = ((salt + 77) % 2) == 0;
  return Record{
      .id = salt + 77,
      .label = std::string(seed) + "-block-077",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock077(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 77) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 77;
    } else {
      total -= (77 % 5);
    }
  }
  return total;
}

std::string DescribeBlock077(const Record& record) {
  std::ostringstream stream;
  stream << "block=077"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 078 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock078 = {78, 79, 80, 81, 82, 83};

Record MakeRecord078(std::string_view seed, int salt) {
  const double scale = static_cast<double>((78 % 7) + 1) / 3.0;
  const bool active = ((salt + 78) % 2) == 0;
  return Record{
      .id = salt + 78,
      .label = std::string(seed) + "-block-078",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock078(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 78) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 78;
    } else {
      total -= (78 % 5);
    }
  }
  return total;
}

std::string DescribeBlock078(const Record& record) {
  std::ostringstream stream;
  stream << "block=078"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 079 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock079 = {79, 80, 81, 82, 83, 84};

Record MakeRecord079(std::string_view seed, int salt) {
  const double scale = static_cast<double>((79 % 7) + 1) / 3.0;
  const bool active = ((salt + 79) % 2) == 0;
  return Record{
      .id = salt + 79,
      .label = std::string(seed) + "-block-079",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock079(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 79) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 79;
    } else {
      total -= (79 % 5);
    }
  }
  return total;
}

std::string DescribeBlock079(const Record& record) {
  std::ostringstream stream;
  stream << "block=079"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 080 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock080 = {80, 81, 82, 83, 84, 85};

Record MakeRecord080(std::string_view seed, int salt) {
  const double scale = static_cast<double>((80 % 7) + 1) / 3.0;
  const bool active = ((salt + 80) % 2) == 0;
  return Record{
      .id = salt + 80,
      .label = std::string(seed) + "-block-080",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock080(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 80) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 80;
    } else {
      total -= (80 % 5);
    }
  }
  return total;
}

std::string DescribeBlock080(const Record& record) {
  std::ostringstream stream;
  stream << "block=080"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 081 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock081 = {81, 82, 83, 84, 85, 86};

Record MakeRecord081(std::string_view seed, int salt) {
  const double scale = static_cast<double>((81 % 7) + 1) / 3.0;
  const bool active = ((salt + 81) % 2) == 0;
  return Record{
      .id = salt + 81,
      .label = std::string(seed) + "-block-081",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock081(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 81) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 81;
    } else {
      total -= (81 % 5);
    }
  }
  return total;
}

std::string DescribeBlock081(const Record& record) {
  std::ostringstream stream;
  stream << "block=081"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 082 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock082 = {82, 83, 84, 85, 86, 87};

Record MakeRecord082(std::string_view seed, int salt) {
  const double scale = static_cast<double>((82 % 7) + 1) / 3.0;
  const bool active = ((salt + 82) % 2) == 0;
  return Record{
      .id = salt + 82,
      .label = std::string(seed) + "-block-082",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock082(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 82) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 82;
    } else {
      total -= (82 % 5);
    }
  }
  return total;
}

std::string DescribeBlock082(const Record& record) {
  std::ostringstream stream;
  stream << "block=082"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 083 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock083 = {83, 84, 85, 86, 87, 88};

Record MakeRecord083(std::string_view seed, int salt) {
  const double scale = static_cast<double>((83 % 7) + 1) / 3.0;
  const bool active = ((salt + 83) % 2) == 0;
  return Record{
      .id = salt + 83,
      .label = std::string(seed) + "-block-083",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock083(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 83) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 83;
    } else {
      total -= (83 % 5);
    }
  }
  return total;
}

std::string DescribeBlock083(const Record& record) {
  std::ostringstream stream;
  stream << "block=083"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 084 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock084 = {84, 85, 86, 87, 88, 89};

Record MakeRecord084(std::string_view seed, int salt) {
  const double scale = static_cast<double>((84 % 7) + 1) / 3.0;
  const bool active = ((salt + 84) % 2) == 0;
  return Record{
      .id = salt + 84,
      .label = std::string(seed) + "-block-084",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock084(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 84) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 84;
    } else {
      total -= (84 % 5);
    }
  }
  return total;
}

std::string DescribeBlock084(const Record& record) {
  std::ostringstream stream;
  stream << "block=084"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 085 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock085 = {85, 86, 87, 88, 89, 90};

Record MakeRecord085(std::string_view seed, int salt) {
  const double scale = static_cast<double>((85 % 7) + 1) / 3.0;
  const bool active = ((salt + 85) % 2) == 0;
  return Record{
      .id = salt + 85,
      .label = std::string(seed) + "-block-085",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock085(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 85) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 85;
    } else {
      total -= (85 % 5);
    }
  }
  return total;
}

std::string DescribeBlock085(const Record& record) {
  std::ostringstream stream;
  stream << "block=085"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 086 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock086 = {86, 87, 88, 89, 90, 91};

Record MakeRecord086(std::string_view seed, int salt) {
  const double scale = static_cast<double>((86 % 7) + 1) / 3.0;
  const bool active = ((salt + 86) % 2) == 0;
  return Record{
      .id = salt + 86,
      .label = std::string(seed) + "-block-086",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock086(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 86) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 86;
    } else {
      total -= (86 % 5);
    }
  }
  return total;
}

std::string DescribeBlock086(const Record& record) {
  std::ostringstream stream;
  stream << "block=086"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 087 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock087 = {87, 88, 89, 90, 91, 92};

Record MakeRecord087(std::string_view seed, int salt) {
  const double scale = static_cast<double>((87 % 7) + 1) / 3.0;
  const bool active = ((salt + 87) % 2) == 0;
  return Record{
      .id = salt + 87,
      .label = std::string(seed) + "-block-087",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock087(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 87) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 87;
    } else {
      total -= (87 % 5);
    }
  }
  return total;
}

std::string DescribeBlock087(const Record& record) {
  std::ostringstream stream;
  stream << "block=087"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 088 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock088 = {88, 89, 90, 91, 92, 93};

Record MakeRecord088(std::string_view seed, int salt) {
  const double scale = static_cast<double>((88 % 7) + 1) / 3.0;
  const bool active = ((salt + 88) % 2) == 0;
  return Record{
      .id = salt + 88,
      .label = std::string(seed) + "-block-088",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock088(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 88) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 88;
    } else {
      total -= (88 % 5);
    }
  }
  return total;
}

std::string DescribeBlock088(const Record& record) {
  std::ostringstream stream;
  stream << "block=088"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 089 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock089 = {89, 90, 91, 92, 93, 94};

Record MakeRecord089(std::string_view seed, int salt) {
  const double scale = static_cast<double>((89 % 7) + 1) / 3.0;
  const bool active = ((salt + 89) % 2) == 0;
  return Record{
      .id = salt + 89,
      .label = std::string(seed) + "-block-089",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock089(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 89) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 89;
    } else {
      total -= (89 % 5);
    }
  }
  return total;
}

std::string DescribeBlock089(const Record& record) {
  std::ostringstream stream;
  stream << "block=089"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 090 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock090 = {90, 91, 92, 93, 94, 95};

Record MakeRecord090(std::string_view seed, int salt) {
  const double scale = static_cast<double>((90 % 7) + 1) / 3.0;
  const bool active = ((salt + 90) % 2) == 0;
  return Record{
      .id = salt + 90,
      .label = std::string(seed) + "-block-090",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock090(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 90) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 90;
    } else {
      total -= (90 % 5);
    }
  }
  return total;
}

std::string DescribeBlock090(const Record& record) {
  std::ostringstream stream;
  stream << "block=090"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

/*
  Multi-line comment block inserted periodically so syntax tests see
  long comment regions amid otherwise repetitive code.
*/
std::optional<Record> FindSpecialRecord090(const std::vector<Record>& records) {
  auto it = std::find_if(records.begin(), records.end(), [](const Record& record) {
    return record.active && (record.id % 30) == 0;
  });
  if (it == records.end()) {
    return std::nullopt;
  }
  return *it;
}

// block 091 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock091 = {91, 92, 93, 94, 95, 96};

Record MakeRecord091(std::string_view seed, int salt) {
  const double scale = static_cast<double>((91 % 7) + 1) / 3.0;
  const bool active = ((salt + 91) % 2) == 0;
  return Record{
      .id = salt + 91,
      .label = std::string(seed) + "-block-091",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock091(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 91) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 91;
    } else {
      total -= (91 % 5);
    }
  }
  return total;
}

std::string DescribeBlock091(const Record& record) {
  std::ostringstream stream;
  stream << "block=091"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 092 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock092 = {92, 93, 94, 95, 96, 97};

Record MakeRecord092(std::string_view seed, int salt) {
  const double scale = static_cast<double>((92 % 7) + 1) / 3.0;
  const bool active = ((salt + 92) % 2) == 0;
  return Record{
      .id = salt + 92,
      .label = std::string(seed) + "-block-092",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock092(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 92) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 92;
    } else {
      total -= (92 % 5);
    }
  }
  return total;
}

std::string DescribeBlock092(const Record& record) {
  std::ostringstream stream;
  stream << "block=092"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 093 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock093 = {93, 94, 95, 96, 97, 98};

Record MakeRecord093(std::string_view seed, int salt) {
  const double scale = static_cast<double>((93 % 7) + 1) / 3.0;
  const bool active = ((salt + 93) % 2) == 0;
  return Record{
      .id = salt + 93,
      .label = std::string(seed) + "-block-093",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock093(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 93) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 93;
    } else {
      total -= (93 % 5);
    }
  }
  return total;
}

std::string DescribeBlock093(const Record& record) {
  std::ostringstream stream;
  stream << "block=093"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 094 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock094 = {94, 95, 96, 97, 98, 99};

Record MakeRecord094(std::string_view seed, int salt) {
  const double scale = static_cast<double>((94 % 7) + 1) / 3.0;
  const bool active = ((salt + 94) % 2) == 0;
  return Record{
      .id = salt + 94,
      .label = std::string(seed) + "-block-094",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock094(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 94) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 94;
    } else {
      total -= (94 % 5);
    }
  }
  return total;
}

std::string DescribeBlock094(const Record& record) {
  std::ostringstream stream;
  stream << "block=094"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 095 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock095 = {95, 96, 97, 98, 99, 100};

Record MakeRecord095(std::string_view seed, int salt) {
  const double scale = static_cast<double>((95 % 7) + 1) / 3.0;
  const bool active = ((salt + 95) % 2) == 0;
  return Record{
      .id = salt + 95,
      .label = std::string(seed) + "-block-095",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock095(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 95) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 95;
    } else {
      total -= (95 % 5);
    }
  }
  return total;
}

std::string DescribeBlock095(const Record& record) {
  std::ostringstream stream;
  stream << "block=095"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 096 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock096 = {96, 97, 98, 99, 100, 101};

Record MakeRecord096(std::string_view seed, int salt) {
  const double scale = static_cast<double>((96 % 7) + 1) / 3.0;
  const bool active = ((salt + 96) % 2) == 0;
  return Record{
      .id = salt + 96,
      .label = std::string(seed) + "-block-096",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock096(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 96) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 96;
    } else {
      total -= (96 % 5);
    }
  }
  return total;
}

std::string DescribeBlock096(const Record& record) {
  std::ostringstream stream;
  stream << "block=096"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 097 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock097 = {97, 98, 99, 100, 101, 102};

Record MakeRecord097(std::string_view seed, int salt) {
  const double scale = static_cast<double>((97 % 7) + 1) / 3.0;
  const bool active = ((salt + 97) % 2) == 0;
  return Record{
      .id = salt + 97,
      .label = std::string(seed) + "-block-097",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock097(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 97) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 97;
    } else {
      total -= (97 % 5);
    }
  }
  return total;
}

std::string DescribeBlock097(const Record& record) {
  std::ostringstream stream;
  stream << "block=097"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 098 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock098 = {98, 99, 100, 101, 102, 103};

Record MakeRecord098(std::string_view seed, int salt) {
  const double scale = static_cast<double>((98 % 7) + 1) / 3.0;
  const bool active = ((salt + 98) % 2) == 0;
  return Record{
      .id = salt + 98,
      .label = std::string(seed) + "-block-098",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock098(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 98) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 98;
    } else {
      total -= (98 % 5);
    }
  }
  return total;
}

std::string DescribeBlock098(const Record& record) {
  std::ostringstream stream;
  stream << "block=098"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 099 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock099 = {99, 100, 101, 102, 103, 104};

Record MakeRecord099(std::string_view seed, int salt) {
  const double scale = static_cast<double>((99 % 7) + 1) / 3.0;
  const bool active = ((salt + 99) % 2) == 0;
  return Record{
      .id = salt + 99,
      .label = std::string(seed) + "-block-099",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock099(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 99) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 99;
    } else {
      total -= (99 % 5);
    }
  }
  return total;
}

std::string DescribeBlock099(const Record& record) {
  std::ostringstream stream;
  stream << "block=099"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 100 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock100 = {100, 101, 102, 103, 104, 105};

Record MakeRecord100(std::string_view seed, int salt) {
  const double scale = static_cast<double>((100 % 7) + 1) / 3.0;
  const bool active = ((salt + 100) % 2) == 0;
  return Record{
      .id = salt + 100,
      .label = std::string(seed) + "-block-100",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock100(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 100) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 100;
    } else {
      total -= (100 % 5);
    }
  }
  return total;
}

std::string DescribeBlock100(const Record& record) {
  std::ostringstream stream;
  stream << "block=100"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 101 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock101 = {101, 102, 103, 104, 105, 106};

Record MakeRecord101(std::string_view seed, int salt) {
  const double scale = static_cast<double>((101 % 7) + 1) / 3.0;
  const bool active = ((salt + 101) % 2) == 0;
  return Record{
      .id = salt + 101,
      .label = std::string(seed) + "-block-101",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock101(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 101) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 101;
    } else {
      total -= (101 % 5);
    }
  }
  return total;
}

std::string DescribeBlock101(const Record& record) {
  std::ostringstream stream;
  stream << "block=101"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 102 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock102 = {102, 103, 104, 105, 106, 107};

Record MakeRecord102(std::string_view seed, int salt) {
  const double scale = static_cast<double>((102 % 7) + 1) / 3.0;
  const bool active = ((salt + 102) % 2) == 0;
  return Record{
      .id = salt + 102,
      .label = std::string(seed) + "-block-102",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock102(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 102) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 102;
    } else {
      total -= (102 % 5);
    }
  }
  return total;
}

std::string DescribeBlock102(const Record& record) {
  std::ostringstream stream;
  stream << "block=102"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 103 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock103 = {103, 104, 105, 106, 107, 108};

Record MakeRecord103(std::string_view seed, int salt) {
  const double scale = static_cast<double>((103 % 7) + 1) / 3.0;
  const bool active = ((salt + 103) % 2) == 0;
  return Record{
      .id = salt + 103,
      .label = std::string(seed) + "-block-103",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock103(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 103) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 103;
    } else {
      total -= (103 % 5);
    }
  }
  return total;
}

std::string DescribeBlock103(const Record& record) {
  std::ostringstream stream;
  stream << "block=103"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 104 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock104 = {104, 105, 106, 107, 108, 109};

Record MakeRecord104(std::string_view seed, int salt) {
  const double scale = static_cast<double>((104 % 7) + 1) / 3.0;
  const bool active = ((salt + 104) % 2) == 0;
  return Record{
      .id = salt + 104,
      .label = std::string(seed) + "-block-104",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock104(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 104) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 104;
    } else {
      total -= (104 % 5);
    }
  }
  return total;
}

std::string DescribeBlock104(const Record& record) {
  std::ostringstream stream;
  stream << "block=104"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 105 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock105 = {105, 106, 107, 108, 109, 110};

Record MakeRecord105(std::string_view seed, int salt) {
  const double scale = static_cast<double>((105 % 7) + 1) / 3.0;
  const bool active = ((salt + 105) % 2) == 0;
  return Record{
      .id = salt + 105,
      .label = std::string(seed) + "-block-105",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock105(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 105) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 105;
    } else {
      total -= (105 % 5);
    }
  }
  return total;
}

std::string DescribeBlock105(const Record& record) {
  std::ostringstream stream;
  stream << "block=105"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

/*
  Multi-line comment block inserted periodically so syntax tests see
  long comment regions amid otherwise repetitive code.
*/
std::optional<Record> FindSpecialRecord105(const std::vector<Record>& records) {
  auto it = std::find_if(records.begin(), records.end(), [](const Record& record) {
    return record.active && (record.id % 35) == 0;
  });
  if (it == records.end()) {
    return std::nullopt;
  }
  return *it;
}

// block 106 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock106 = {106, 107, 108, 109, 110, 111};

Record MakeRecord106(std::string_view seed, int salt) {
  const double scale = static_cast<double>((106 % 7) + 1) / 3.0;
  const bool active = ((salt + 106) % 2) == 0;
  return Record{
      .id = salt + 106,
      .label = std::string(seed) + "-block-106",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock106(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 106) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 106;
    } else {
      total -= (106 % 5);
    }
  }
  return total;
}

std::string DescribeBlock106(const Record& record) {
  std::ostringstream stream;
  stream << "block=106"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 107 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock107 = {107, 108, 109, 110, 111, 112};

Record MakeRecord107(std::string_view seed, int salt) {
  const double scale = static_cast<double>((107 % 7) + 1) / 3.0;
  const bool active = ((salt + 107) % 2) == 0;
  return Record{
      .id = salt + 107,
      .label = std::string(seed) + "-block-107",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock107(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 107) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 107;
    } else {
      total -= (107 % 5);
    }
  }
  return total;
}

std::string DescribeBlock107(const Record& record) {
  std::ostringstream stream;
  stream << "block=107"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 108 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock108 = {108, 109, 110, 111, 112, 113};

Record MakeRecord108(std::string_view seed, int salt) {
  const double scale = static_cast<double>((108 % 7) + 1) / 3.0;
  const bool active = ((salt + 108) % 2) == 0;
  return Record{
      .id = salt + 108,
      .label = std::string(seed) + "-block-108",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock108(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 108) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 108;
    } else {
      total -= (108 % 5);
    }
  }
  return total;
}

std::string DescribeBlock108(const Record& record) {
  std::ostringstream stream;
  stream << "block=108"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 109 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock109 = {109, 110, 111, 112, 113, 114};

Record MakeRecord109(std::string_view seed, int salt) {
  const double scale = static_cast<double>((109 % 7) + 1) / 3.0;
  const bool active = ((salt + 109) % 2) == 0;
  return Record{
      .id = salt + 109,
      .label = std::string(seed) + "-block-109",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock109(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 109) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 109;
    } else {
      total -= (109 % 5);
    }
  }
  return total;
}

std::string DescribeBlock109(const Record& record) {
  std::ostringstream stream;
  stream << "block=109"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 110 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock110 = {110, 111, 112, 113, 114, 115};

Record MakeRecord110(std::string_view seed, int salt) {
  const double scale = static_cast<double>((110 % 7) + 1) / 3.0;
  const bool active = ((salt + 110) % 2) == 0;
  return Record{
      .id = salt + 110,
      .label = std::string(seed) + "-block-110",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock110(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 110) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 110;
    } else {
      total -= (110 % 5);
    }
  }
  return total;
}

std::string DescribeBlock110(const Record& record) {
  std::ostringstream stream;
  stream << "block=110"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 111 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock111 = {111, 112, 113, 114, 115, 116};

Record MakeRecord111(std::string_view seed, int salt) {
  const double scale = static_cast<double>((111 % 7) + 1) / 3.0;
  const bool active = ((salt + 111) % 2) == 0;
  return Record{
      .id = salt + 111,
      .label = std::string(seed) + "-block-111",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock111(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 111) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 111;
    } else {
      total -= (111 % 5);
    }
  }
  return total;
}

std::string DescribeBlock111(const Record& record) {
  std::ostringstream stream;
  stream << "block=111"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 112 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock112 = {112, 113, 114, 115, 116, 117};

Record MakeRecord112(std::string_view seed, int salt) {
  const double scale = static_cast<double>((112 % 7) + 1) / 3.0;
  const bool active = ((salt + 112) % 2) == 0;
  return Record{
      .id = salt + 112,
      .label = std::string(seed) + "-block-112",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock112(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 112) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 112;
    } else {
      total -= (112 % 5);
    }
  }
  return total;
}

std::string DescribeBlock112(const Record& record) {
  std::ostringstream stream;
  stream << "block=112"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 113 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock113 = {113, 114, 115, 116, 117, 118};

Record MakeRecord113(std::string_view seed, int salt) {
  const double scale = static_cast<double>((113 % 7) + 1) / 3.0;
  const bool active = ((salt + 113) % 2) == 0;
  return Record{
      .id = salt + 113,
      .label = std::string(seed) + "-block-113",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock113(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 113) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 113;
    } else {
      total -= (113 % 5);
    }
  }
  return total;
}

std::string DescribeBlock113(const Record& record) {
  std::ostringstream stream;
  stream << "block=113"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 114 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock114 = {114, 115, 116, 117, 118, 119};

Record MakeRecord114(std::string_view seed, int salt) {
  const double scale = static_cast<double>((114 % 7) + 1) / 3.0;
  const bool active = ((salt + 114) % 2) == 0;
  return Record{
      .id = salt + 114,
      .label = std::string(seed) + "-block-114",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock114(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 114) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 114;
    } else {
      total -= (114 % 5);
    }
  }
  return total;
}

std::string DescribeBlock114(const Record& record) {
  std::ostringstream stream;
  stream << "block=114"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 115 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock115 = {115, 116, 117, 118, 119, 120};

Record MakeRecord115(std::string_view seed, int salt) {
  const double scale = static_cast<double>((115 % 7) + 1) / 3.0;
  const bool active = ((salt + 115) % 2) == 0;
  return Record{
      .id = salt + 115,
      .label = std::string(seed) + "-block-115",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock115(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 115) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 115;
    } else {
      total -= (115 % 5);
    }
  }
  return total;
}

std::string DescribeBlock115(const Record& record) {
  std::ostringstream stream;
  stream << "block=115"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 116 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock116 = {116, 117, 118, 119, 120, 121};

Record MakeRecord116(std::string_view seed, int salt) {
  const double scale = static_cast<double>((116 % 7) + 1) / 3.0;
  const bool active = ((salt + 116) % 2) == 0;
  return Record{
      .id = salt + 116,
      .label = std::string(seed) + "-block-116",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock116(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 116) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 116;
    } else {
      total -= (116 % 5);
    }
  }
  return total;
}

std::string DescribeBlock116(const Record& record) {
  std::ostringstream stream;
  stream << "block=116"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 117 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock117 = {117, 118, 119, 120, 121, 122};

Record MakeRecord117(std::string_view seed, int salt) {
  const double scale = static_cast<double>((117 % 7) + 1) / 3.0;
  const bool active = ((salt + 117) % 2) == 0;
  return Record{
      .id = salt + 117,
      .label = std::string(seed) + "-block-117",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock117(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 117) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 117;
    } else {
      total -= (117 % 5);
    }
  }
  return total;
}

std::string DescribeBlock117(const Record& record) {
  std::ostringstream stream;
  stream << "block=117"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 118 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock118 = {118, 119, 120, 121, 122, 123};

Record MakeRecord118(std::string_view seed, int salt) {
  const double scale = static_cast<double>((118 % 7) + 1) / 3.0;
  const bool active = ((salt + 118) % 2) == 0;
  return Record{
      .id = salt + 118,
      .label = std::string(seed) + "-block-118",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock118(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 118) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 118;
    } else {
      total -= (118 % 5);
    }
  }
  return total;
}

std::string DescribeBlock118(const Record& record) {
  std::ostringstream stream;
  stream << "block=118"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 119 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock119 = {119, 120, 121, 122, 123, 124};

Record MakeRecord119(std::string_view seed, int salt) {
  const double scale = static_cast<double>((119 % 7) + 1) / 3.0;
  const bool active = ((salt + 119) % 2) == 0;
  return Record{
      .id = salt + 119,
      .label = std::string(seed) + "-block-119",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock119(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 119) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 119;
    } else {
      total -= (119 % 5);
    }
  }
  return total;
}

std::string DescribeBlock119(const Record& record) {
  std::ostringstream stream;
  stream << "block=119"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 120 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock120 = {120, 121, 122, 123, 124, 125};

Record MakeRecord120(std::string_view seed, int salt) {
  const double scale = static_cast<double>((120 % 7) + 1) / 3.0;
  const bool active = ((salt + 120) % 2) == 0;
  return Record{
      .id = salt + 120,
      .label = std::string(seed) + "-block-120",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock120(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 120) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 120;
    } else {
      total -= (120 % 5);
    }
  }
  return total;
}

std::string DescribeBlock120(const Record& record) {
  std::ostringstream stream;
  stream << "block=120"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

/*
  Multi-line comment block inserted periodically so syntax tests see
  long comment regions amid otherwise repetitive code.
*/
std::optional<Record> FindSpecialRecord120(const std::vector<Record>& records) {
  auto it = std::find_if(records.begin(), records.end(), [](const Record& record) {
    return record.active && (record.id % 40) == 0;
  });
  if (it == records.end()) {
    return std::nullopt;
  }
  return *it;
}

// block 121 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock121 = {121, 122, 123, 124, 125, 126};

Record MakeRecord121(std::string_view seed, int salt) {
  const double scale = static_cast<double>((121 % 7) + 1) / 3.0;
  const bool active = ((salt + 121) % 2) == 0;
  return Record{
      .id = salt + 121,
      .label = std::string(seed) + "-block-121",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock121(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 121) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 121;
    } else {
      total -= (121 % 5);
    }
  }
  return total;
}

std::string DescribeBlock121(const Record& record) {
  std::ostringstream stream;
  stream << "block=121"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 122 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock122 = {122, 123, 124, 125, 126, 127};

Record MakeRecord122(std::string_view seed, int salt) {
  const double scale = static_cast<double>((122 % 7) + 1) / 3.0;
  const bool active = ((salt + 122) % 2) == 0;
  return Record{
      .id = salt + 122,
      .label = std::string(seed) + "-block-122",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock122(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 122) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 122;
    } else {
      total -= (122 % 5);
    }
  }
  return total;
}

std::string DescribeBlock122(const Record& record) {
  std::ostringstream stream;
  stream << "block=122"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 123 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock123 = {123, 124, 125, 126, 127, 128};

Record MakeRecord123(std::string_view seed, int salt) {
  const double scale = static_cast<double>((123 % 7) + 1) / 3.0;
  const bool active = ((salt + 123) % 2) == 0;
  return Record{
      .id = salt + 123,
      .label = std::string(seed) + "-block-123",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock123(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 123) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 123;
    } else {
      total -= (123 % 5);
    }
  }
  return total;
}

std::string DescribeBlock123(const Record& record) {
  std::ostringstream stream;
  stream << "block=123"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 124 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock124 = {124, 125, 126, 127, 128, 129};

Record MakeRecord124(std::string_view seed, int salt) {
  const double scale = static_cast<double>((124 % 7) + 1) / 3.0;
  const bool active = ((salt + 124) % 2) == 0;
  return Record{
      .id = salt + 124,
      .label = std::string(seed) + "-block-124",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock124(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 124) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 124;
    } else {
      total -= (124 % 5);
    }
  }
  return total;
}

std::string DescribeBlock124(const Record& record) {
  std::ostringstream stream;
  stream << "block=124"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 125 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock125 = {125, 126, 127, 128, 129, 130};

Record MakeRecord125(std::string_view seed, int salt) {
  const double scale = static_cast<double>((125 % 7) + 1) / 3.0;
  const bool active = ((salt + 125) % 2) == 0;
  return Record{
      .id = salt + 125,
      .label = std::string(seed) + "-block-125",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock125(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 125) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 125;
    } else {
      total -= (125 % 5);
    }
  }
  return total;
}

std::string DescribeBlock125(const Record& record) {
  std::ostringstream stream;
  stream << "block=125"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 126 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock126 = {126, 127, 128, 129, 130, 131};

Record MakeRecord126(std::string_view seed, int salt) {
  const double scale = static_cast<double>((126 % 7) + 1) / 3.0;
  const bool active = ((salt + 126) % 2) == 0;
  return Record{
      .id = salt + 126,
      .label = std::string(seed) + "-block-126",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock126(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 126) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 126;
    } else {
      total -= (126 % 5);
    }
  }
  return total;
}

std::string DescribeBlock126(const Record& record) {
  std::ostringstream stream;
  stream << "block=126"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 127 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock127 = {127, 128, 129, 130, 131, 132};

Record MakeRecord127(std::string_view seed, int salt) {
  const double scale = static_cast<double>((127 % 7) + 1) / 3.0;
  const bool active = ((salt + 127) % 2) == 0;
  return Record{
      .id = salt + 127,
      .label = std::string(seed) + "-block-127",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock127(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 127) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 127;
    } else {
      total -= (127 % 5);
    }
  }
  return total;
}

std::string DescribeBlock127(const Record& record) {
  std::ostringstream stream;
  stream << "block=127"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 128 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock128 = {128, 129, 130, 131, 132, 133};

Record MakeRecord128(std::string_view seed, int salt) {
  const double scale = static_cast<double>((128 % 7) + 1) / 3.0;
  const bool active = ((salt + 128) % 2) == 0;
  return Record{
      .id = salt + 128,
      .label = std::string(seed) + "-block-128",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock128(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 128) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 128;
    } else {
      total -= (128 % 5);
    }
  }
  return total;
}

std::string DescribeBlock128(const Record& record) {
  std::ostringstream stream;
  stream << "block=128"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 129 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock129 = {129, 130, 131, 132, 133, 134};

Record MakeRecord129(std::string_view seed, int salt) {
  const double scale = static_cast<double>((129 % 7) + 1) / 3.0;
  const bool active = ((salt + 129) % 2) == 0;
  return Record{
      .id = salt + 129,
      .label = std::string(seed) + "-block-129",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock129(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 129) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 129;
    } else {
      total -= (129 % 5);
    }
  }
  return total;
}

std::string DescribeBlock129(const Record& record) {
  std::ostringstream stream;
  stream << "block=129"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 130 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock130 = {130, 131, 132, 133, 134, 135};

Record MakeRecord130(std::string_view seed, int salt) {
  const double scale = static_cast<double>((130 % 7) + 1) / 3.0;
  const bool active = ((salt + 130) % 2) == 0;
  return Record{
      .id = salt + 130,
      .label = std::string(seed) + "-block-130",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock130(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 130) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 130;
    } else {
      total -= (130 % 5);
    }
  }
  return total;
}

std::string DescribeBlock130(const Record& record) {
  std::ostringstream stream;
  stream << "block=130"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 131 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock131 = {131, 132, 133, 134, 135, 136};

Record MakeRecord131(std::string_view seed, int salt) {
  const double scale = static_cast<double>((131 % 7) + 1) / 3.0;
  const bool active = ((salt + 131) % 2) == 0;
  return Record{
      .id = salt + 131,
      .label = std::string(seed) + "-block-131",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock131(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 131) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 131;
    } else {
      total -= (131 % 5);
    }
  }
  return total;
}

std::string DescribeBlock131(const Record& record) {
  std::ostringstream stream;
  stream << "block=131"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 132 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock132 = {132, 133, 134, 135, 136, 137};

Record MakeRecord132(std::string_view seed, int salt) {
  const double scale = static_cast<double>((132 % 7) + 1) / 3.0;
  const bool active = ((salt + 132) % 2) == 0;
  return Record{
      .id = salt + 132,
      .label = std::string(seed) + "-block-132",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock132(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 132) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 132;
    } else {
      total -= (132 % 5);
    }
  }
  return total;
}

std::string DescribeBlock132(const Record& record) {
  std::ostringstream stream;
  stream << "block=132"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 133 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock133 = {133, 134, 135, 136, 137, 138};

Record MakeRecord133(std::string_view seed, int salt) {
  const double scale = static_cast<double>((133 % 7) + 1) / 3.0;
  const bool active = ((salt + 133) % 2) == 0;
  return Record{
      .id = salt + 133,
      .label = std::string(seed) + "-block-133",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock133(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 133) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 133;
    } else {
      total -= (133 % 5);
    }
  }
  return total;
}

std::string DescribeBlock133(const Record& record) {
  std::ostringstream stream;
  stream << "block=133"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 134 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock134 = {134, 135, 136, 137, 138, 139};

Record MakeRecord134(std::string_view seed, int salt) {
  const double scale = static_cast<double>((134 % 7) + 1) / 3.0;
  const bool active = ((salt + 134) % 2) == 0;
  return Record{
      .id = salt + 134,
      .label = std::string(seed) + "-block-134",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock134(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 134) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 134;
    } else {
      total -= (134 % 5);
    }
  }
  return total;
}

std::string DescribeBlock134(const Record& record) {
  std::ostringstream stream;
  stream << "block=134"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 135 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock135 = {135, 136, 137, 138, 139, 140};

Record MakeRecord135(std::string_view seed, int salt) {
  const double scale = static_cast<double>((135 % 7) + 1) / 3.0;
  const bool active = ((salt + 135) % 2) == 0;
  return Record{
      .id = salt + 135,
      .label = std::string(seed) + "-block-135",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock135(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 135) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 135;
    } else {
      total -= (135 % 5);
    }
  }
  return total;
}

std::string DescribeBlock135(const Record& record) {
  std::ostringstream stream;
  stream << "block=135"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

/*
  Multi-line comment block inserted periodically so syntax tests see
  long comment regions amid otherwise repetitive code.
*/
std::optional<Record> FindSpecialRecord135(const std::vector<Record>& records) {
  auto it = std::find_if(records.begin(), records.end(), [](const Record& record) {
    return record.active && (record.id % 45) == 0;
  });
  if (it == records.end()) {
    return std::nullopt;
  }
  return *it;
}

// block 136 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock136 = {136, 137, 138, 139, 140, 141};

Record MakeRecord136(std::string_view seed, int salt) {
  const double scale = static_cast<double>((136 % 7) + 1) / 3.0;
  const bool active = ((salt + 136) % 2) == 0;
  return Record{
      .id = salt + 136,
      .label = std::string(seed) + "-block-136",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock136(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 136) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 136;
    } else {
      total -= (136 % 5);
    }
  }
  return total;
}

std::string DescribeBlock136(const Record& record) {
  std::ostringstream stream;
  stream << "block=136"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 137 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock137 = {137, 138, 139, 140, 141, 142};

Record MakeRecord137(std::string_view seed, int salt) {
  const double scale = static_cast<double>((137 % 7) + 1) / 3.0;
  const bool active = ((salt + 137) % 2) == 0;
  return Record{
      .id = salt + 137,
      .label = std::string(seed) + "-block-137",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock137(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 137) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 137;
    } else {
      total -= (137 % 5);
    }
  }
  return total;
}

std::string DescribeBlock137(const Record& record) {
  std::ostringstream stream;
  stream << "block=137"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 138 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock138 = {138, 139, 140, 141, 142, 143};

Record MakeRecord138(std::string_view seed, int salt) {
  const double scale = static_cast<double>((138 % 7) + 1) / 3.0;
  const bool active = ((salt + 138) % 2) == 0;
  return Record{
      .id = salt + 138,
      .label = std::string(seed) + "-block-138",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock138(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 138) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 138;
    } else {
      total -= (138 % 5);
    }
  }
  return total;
}

std::string DescribeBlock138(const Record& record) {
  std::ostringstream stream;
  stream << "block=138"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 139 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock139 = {139, 140, 141, 142, 143, 144};

Record MakeRecord139(std::string_view seed, int salt) {
  const double scale = static_cast<double>((139 % 7) + 1) / 3.0;
  const bool active = ((salt + 139) % 2) == 0;
  return Record{
      .id = salt + 139,
      .label = std::string(seed) + "-block-139",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock139(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 139) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 139;
    } else {
      total -= (139 % 5);
    }
  }
  return total;
}

std::string DescribeBlock139(const Record& record) {
  std::ostringstream stream;
  stream << "block=139"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 140 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock140 = {140, 141, 142, 143, 144, 145};

Record MakeRecord140(std::string_view seed, int salt) {
  const double scale = static_cast<double>((140 % 7) + 1) / 3.0;
  const bool active = ((salt + 140) % 2) == 0;
  return Record{
      .id = salt + 140,
      .label = std::string(seed) + "-block-140",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock140(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 140) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 140;
    } else {
      total -= (140 % 5);
    }
  }
  return total;
}

std::string DescribeBlock140(const Record& record) {
  std::ostringstream stream;
  stream << "block=140"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 141 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock141 = {141, 142, 143, 144, 145, 146};

Record MakeRecord141(std::string_view seed, int salt) {
  const double scale = static_cast<double>((141 % 7) + 1) / 3.0;
  const bool active = ((salt + 141) % 2) == 0;
  return Record{
      .id = salt + 141,
      .label = std::string(seed) + "-block-141",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock141(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 141) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 141;
    } else {
      total -= (141 % 5);
    }
  }
  return total;
}

std::string DescribeBlock141(const Record& record) {
  std::ostringstream stream;
  stream << "block=141"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 142 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock142 = {142, 143, 144, 145, 146, 147};

Record MakeRecord142(std::string_view seed, int salt) {
  const double scale = static_cast<double>((142 % 7) + 1) / 3.0;
  const bool active = ((salt + 142) % 2) == 0;
  return Record{
      .id = salt + 142,
      .label = std::string(seed) + "-block-142",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock142(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 142) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 142;
    } else {
      total -= (142 % 5);
    }
  }
  return total;
}

std::string DescribeBlock142(const Record& record) {
  std::ostringstream stream;
  stream << "block=142"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 143 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock143 = {143, 144, 145, 146, 147, 148};

Record MakeRecord143(std::string_view seed, int salt) {
  const double scale = static_cast<double>((143 % 7) + 1) / 3.0;
  const bool active = ((salt + 143) % 2) == 0;
  return Record{
      .id = salt + 143,
      .label = std::string(seed) + "-block-143",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock143(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 143) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 143;
    } else {
      total -= (143 % 5);
    }
  }
  return total;
}

std::string DescribeBlock143(const Record& record) {
  std::ostringstream stream;
  stream << "block=143"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 144 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock144 = {144, 145, 146, 147, 148, 149};

Record MakeRecord144(std::string_view seed, int salt) {
  const double scale = static_cast<double>((144 % 7) + 1) / 3.0;
  const bool active = ((salt + 144) % 2) == 0;
  return Record{
      .id = salt + 144,
      .label = std::string(seed) + "-block-144",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock144(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 144) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 144;
    } else {
      total -= (144 % 5);
    }
  }
  return total;
}

std::string DescribeBlock144(const Record& record) {
  std::ostringstream stream;
  stream << "block=144"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 145 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock145 = {145, 146, 147, 148, 149, 150};

Record MakeRecord145(std::string_view seed, int salt) {
  const double scale = static_cast<double>((145 % 7) + 1) / 3.0;
  const bool active = ((salt + 145) % 2) == 0;
  return Record{
      .id = salt + 145,
      .label = std::string(seed) + "-block-145",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock145(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 145) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 145;
    } else {
      total -= (145 % 5);
    }
  }
  return total;
}

std::string DescribeBlock145(const Record& record) {
  std::ostringstream stream;
  stream << "block=145"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 146 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock146 = {146, 147, 148, 149, 150, 151};

Record MakeRecord146(std::string_view seed, int salt) {
  const double scale = static_cast<double>((146 % 7) + 1) / 3.0;
  const bool active = ((salt + 146) % 2) == 0;
  return Record{
      .id = salt + 146,
      .label = std::string(seed) + "-block-146",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock146(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 146) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 146;
    } else {
      total -= (146 % 5);
    }
  }
  return total;
}

std::string DescribeBlock146(const Record& record) {
  std::ostringstream stream;
  stream << "block=146"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 147 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock147 = {147, 148, 149, 150, 151, 152};

Record MakeRecord147(std::string_view seed, int salt) {
  const double scale = static_cast<double>((147 % 7) + 1) / 3.0;
  const bool active = ((salt + 147) % 2) == 0;
  return Record{
      .id = salt + 147,
      .label = std::string(seed) + "-block-147",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock147(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 147) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 147;
    } else {
      total -= (147 % 5);
    }
  }
  return total;
}

std::string DescribeBlock147(const Record& record) {
  std::ostringstream stream;
  stream << "block=147"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 148 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock148 = {148, 149, 150, 151, 152, 153};

Record MakeRecord148(std::string_view seed, int salt) {
  const double scale = static_cast<double>((148 % 7) + 1) / 3.0;
  const bool active = ((salt + 148) % 2) == 0;
  return Record{
      .id = salt + 148,
      .label = std::string(seed) + "-block-148",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock148(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 148) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 148;
    } else {
      total -= (148 % 5);
    }
  }
  return total;
}

std::string DescribeBlock148(const Record& record) {
  std::ostringstream stream;
  stream << "block=148"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 149 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock149 = {149, 150, 151, 152, 153, 154};

Record MakeRecord149(std::string_view seed, int salt) {
  const double scale = static_cast<double>((149 % 7) + 1) / 3.0;
  const bool active = ((salt + 149) % 2) == 0;
  return Record{
      .id = salt + 149,
      .label = std::string(seed) + "-block-149",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock149(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 149) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 149;
    } else {
      total -= (149 % 5);
    }
  }
  return total;
}

std::string DescribeBlock149(const Record& record) {
  std::ostringstream stream;
  stream << "block=149"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 150 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock150 = {150, 151, 152, 153, 154, 155};

Record MakeRecord150(std::string_view seed, int salt) {
  const double scale = static_cast<double>((150 % 7) + 1) / 3.0;
  const bool active = ((salt + 150) % 2) == 0;
  return Record{
      .id = salt + 150,
      .label = std::string(seed) + "-block-150",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock150(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 150) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 150;
    } else {
      total -= (150 % 5);
    }
  }
  return total;
}

std::string DescribeBlock150(const Record& record) {
  std::ostringstream stream;
  stream << "block=150"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

/*
  Multi-line comment block inserted periodically so syntax tests see
  long comment regions amid otherwise repetitive code.
*/
std::optional<Record> FindSpecialRecord150(const std::vector<Record>& records) {
  auto it = std::find_if(records.begin(), records.end(), [](const Record& record) {
    return record.active && (record.id % 50) == 0;
  });
  if (it == records.end()) {
    return std::nullopt;
  }
  return *it;
}

// block 151 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock151 = {151, 152, 153, 154, 155, 156};

Record MakeRecord151(std::string_view seed, int salt) {
  const double scale = static_cast<double>((151 % 7) + 1) / 3.0;
  const bool active = ((salt + 151) % 2) == 0;
  return Record{
      .id = salt + 151,
      .label = std::string(seed) + "-block-151",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock151(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 151) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 151;
    } else {
      total -= (151 % 5);
    }
  }
  return total;
}

std::string DescribeBlock151(const Record& record) {
  std::ostringstream stream;
  stream << "block=151"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 152 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock152 = {152, 153, 154, 155, 156, 157};

Record MakeRecord152(std::string_view seed, int salt) {
  const double scale = static_cast<double>((152 % 7) + 1) / 3.0;
  const bool active = ((salt + 152) % 2) == 0;
  return Record{
      .id = salt + 152,
      .label = std::string(seed) + "-block-152",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock152(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 152) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 152;
    } else {
      total -= (152 % 5);
    }
  }
  return total;
}

std::string DescribeBlock152(const Record& record) {
  std::ostringstream stream;
  stream << "block=152"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 153 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock153 = {153, 154, 155, 156, 157, 158};

Record MakeRecord153(std::string_view seed, int salt) {
  const double scale = static_cast<double>((153 % 7) + 1) / 3.0;
  const bool active = ((salt + 153) % 2) == 0;
  return Record{
      .id = salt + 153,
      .label = std::string(seed) + "-block-153",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock153(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 153) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 153;
    } else {
      total -= (153 % 5);
    }
  }
  return total;
}

std::string DescribeBlock153(const Record& record) {
  std::ostringstream stream;
  stream << "block=153"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 154 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock154 = {154, 155, 156, 157, 158, 159};

Record MakeRecord154(std::string_view seed, int salt) {
  const double scale = static_cast<double>((154 % 7) + 1) / 3.0;
  const bool active = ((salt + 154) % 2) == 0;
  return Record{
      .id = salt + 154,
      .label = std::string(seed) + "-block-154",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock154(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 154) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 154;
    } else {
      total -= (154 % 5);
    }
  }
  return total;
}

std::string DescribeBlock154(const Record& record) {
  std::ostringstream stream;
  stream << "block=154"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 155 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock155 = {155, 156, 157, 158, 159, 160};

Record MakeRecord155(std::string_view seed, int salt) {
  const double scale = static_cast<double>((155 % 7) + 1) / 3.0;
  const bool active = ((salt + 155) % 2) == 0;
  return Record{
      .id = salt + 155,
      .label = std::string(seed) + "-block-155",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock155(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 155) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 155;
    } else {
      total -= (155 % 5);
    }
  }
  return total;
}

std::string DescribeBlock155(const Record& record) {
  std::ostringstream stream;
  stream << "block=155"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 156 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock156 = {156, 157, 158, 159, 160, 161};

Record MakeRecord156(std::string_view seed, int salt) {
  const double scale = static_cast<double>((156 % 7) + 1) / 3.0;
  const bool active = ((salt + 156) % 2) == 0;
  return Record{
      .id = salt + 156,
      .label = std::string(seed) + "-block-156",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock156(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 156) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 156;
    } else {
      total -= (156 % 5);
    }
  }
  return total;
}

std::string DescribeBlock156(const Record& record) {
  std::ostringstream stream;
  stream << "block=156"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 157 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock157 = {157, 158, 159, 160, 161, 162};

Record MakeRecord157(std::string_view seed, int salt) {
  const double scale = static_cast<double>((157 % 7) + 1) / 3.0;
  const bool active = ((salt + 157) % 2) == 0;
  return Record{
      .id = salt + 157,
      .label = std::string(seed) + "-block-157",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock157(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 157) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 157;
    } else {
      total -= (157 % 5);
    }
  }
  return total;
}

std::string DescribeBlock157(const Record& record) {
  std::ostringstream stream;
  stream << "block=157"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 158 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock158 = {158, 159, 160, 161, 162, 163};

Record MakeRecord158(std::string_view seed, int salt) {
  const double scale = static_cast<double>((158 % 7) + 1) / 3.0;
  const bool active = ((salt + 158) % 2) == 0;
  return Record{
      .id = salt + 158,
      .label = std::string(seed) + "-block-158",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock158(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 158) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 158;
    } else {
      total -= (158 % 5);
    }
  }
  return total;
}

std::string DescribeBlock158(const Record& record) {
  std::ostringstream stream;
  stream << "block=158"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 159 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock159 = {159, 160, 161, 162, 163, 164};

Record MakeRecord159(std::string_view seed, int salt) {
  const double scale = static_cast<double>((159 % 7) + 1) / 3.0;
  const bool active = ((salt + 159) % 2) == 0;
  return Record{
      .id = salt + 159,
      .label = std::string(seed) + "-block-159",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock159(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 159) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 159;
    } else {
      total -= (159 % 5);
    }
  }
  return total;
}

std::string DescribeBlock159(const Record& record) {
  std::ostringstream stream;
  stream << "block=159"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 160 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock160 = {160, 161, 162, 163, 164, 165};

Record MakeRecord160(std::string_view seed, int salt) {
  const double scale = static_cast<double>((160 % 7) + 1) / 3.0;
  const bool active = ((salt + 160) % 2) == 0;
  return Record{
      .id = salt + 160,
      .label = std::string(seed) + "-block-160",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock160(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 160) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 160;
    } else {
      total -= (160 % 5);
    }
  }
  return total;
}

std::string DescribeBlock160(const Record& record) {
  std::ostringstream stream;
  stream << "block=160"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 161 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock161 = {161, 162, 163, 164, 165, 166};

Record MakeRecord161(std::string_view seed, int salt) {
  const double scale = static_cast<double>((161 % 7) + 1) / 3.0;
  const bool active = ((salt + 161) % 2) == 0;
  return Record{
      .id = salt + 161,
      .label = std::string(seed) + "-block-161",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock161(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 161) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 161;
    } else {
      total -= (161 % 5);
    }
  }
  return total;
}

std::string DescribeBlock161(const Record& record) {
  std::ostringstream stream;
  stream << "block=161"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 162 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock162 = {162, 163, 164, 165, 166, 167};

Record MakeRecord162(std::string_view seed, int salt) {
  const double scale = static_cast<double>((162 % 7) + 1) / 3.0;
  const bool active = ((salt + 162) % 2) == 0;
  return Record{
      .id = salt + 162,
      .label = std::string(seed) + "-block-162",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock162(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 162) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 162;
    } else {
      total -= (162 % 5);
    }
  }
  return total;
}

std::string DescribeBlock162(const Record& record) {
  std::ostringstream stream;
  stream << "block=162"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 163 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock163 = {163, 164, 165, 166, 167, 168};

Record MakeRecord163(std::string_view seed, int salt) {
  const double scale = static_cast<double>((163 % 7) + 1) / 3.0;
  const bool active = ((salt + 163) % 2) == 0;
  return Record{
      .id = salt + 163,
      .label = std::string(seed) + "-block-163",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock163(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 163) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 163;
    } else {
      total -= (163 % 5);
    }
  }
  return total;
}

std::string DescribeBlock163(const Record& record) {
  std::ostringstream stream;
  stream << "block=163"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 164 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock164 = {164, 165, 166, 167, 168, 169};

Record MakeRecord164(std::string_view seed, int salt) {
  const double scale = static_cast<double>((164 % 7) + 1) / 3.0;
  const bool active = ((salt + 164) % 2) == 0;
  return Record{
      .id = salt + 164,
      .label = std::string(seed) + "-block-164",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock164(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 164) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 164;
    } else {
      total -= (164 % 5);
    }
  }
  return total;
}

std::string DescribeBlock164(const Record& record) {
  std::ostringstream stream;
  stream << "block=164"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 165 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock165 = {165, 166, 167, 168, 169, 170};

Record MakeRecord165(std::string_view seed, int salt) {
  const double scale = static_cast<double>((165 % 7) + 1) / 3.0;
  const bool active = ((salt + 165) % 2) == 0;
  return Record{
      .id = salt + 165,
      .label = std::string(seed) + "-block-165",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock165(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 165) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 165;
    } else {
      total -= (165 % 5);
    }
  }
  return total;
}

std::string DescribeBlock165(const Record& record) {
  std::ostringstream stream;
  stream << "block=165"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

/*
  Multi-line comment block inserted periodically so syntax tests see
  long comment regions amid otherwise repetitive code.
*/
std::optional<Record> FindSpecialRecord165(const std::vector<Record>& records) {
  auto it = std::find_if(records.begin(), records.end(), [](const Record& record) {
    return record.active && (record.id % 55) == 0;
  });
  if (it == records.end()) {
    return std::nullopt;
  }
  return *it;
}

// block 166 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock166 = {166, 167, 168, 169, 170, 171};

Record MakeRecord166(std::string_view seed, int salt) {
  const double scale = static_cast<double>((166 % 7) + 1) / 3.0;
  const bool active = ((salt + 166) % 2) == 0;
  return Record{
      .id = salt + 166,
      .label = std::string(seed) + "-block-166",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock166(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 166) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 166;
    } else {
      total -= (166 % 5);
    }
  }
  return total;
}

std::string DescribeBlock166(const Record& record) {
  std::ostringstream stream;
  stream << "block=166"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 167 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock167 = {167, 168, 169, 170, 171, 172};

Record MakeRecord167(std::string_view seed, int salt) {
  const double scale = static_cast<double>((167 % 7) + 1) / 3.0;
  const bool active = ((salt + 167) % 2) == 0;
  return Record{
      .id = salt + 167,
      .label = std::string(seed) + "-block-167",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock167(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 167) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 167;
    } else {
      total -= (167 % 5);
    }
  }
  return total;
}

std::string DescribeBlock167(const Record& record) {
  std::ostringstream stream;
  stream << "block=167"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 168 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock168 = {168, 169, 170, 171, 172, 173};

Record MakeRecord168(std::string_view seed, int salt) {
  const double scale = static_cast<double>((168 % 7) + 1) / 3.0;
  const bool active = ((salt + 168) % 2) == 0;
  return Record{
      .id = salt + 168,
      .label = std::string(seed) + "-block-168",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock168(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 168) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 168;
    } else {
      total -= (168 % 5);
    }
  }
  return total;
}

std::string DescribeBlock168(const Record& record) {
  std::ostringstream stream;
  stream << "block=168"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 169 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock169 = {169, 170, 171, 172, 173, 174};

Record MakeRecord169(std::string_view seed, int salt) {
  const double scale = static_cast<double>((169 % 7) + 1) / 3.0;
  const bool active = ((salt + 169) % 2) == 0;
  return Record{
      .id = salt + 169,
      .label = std::string(seed) + "-block-169",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock169(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 169) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 169;
    } else {
      total -= (169 % 5);
    }
  }
  return total;
}

std::string DescribeBlock169(const Record& record) {
  std::ostringstream stream;
  stream << "block=169"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 170 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock170 = {170, 171, 172, 173, 174, 175};

Record MakeRecord170(std::string_view seed, int salt) {
  const double scale = static_cast<double>((170 % 7) + 1) / 3.0;
  const bool active = ((salt + 170) % 2) == 0;
  return Record{
      .id = salt + 170,
      .label = std::string(seed) + "-block-170",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock170(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 170) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 170;
    } else {
      total -= (170 % 5);
    }
  }
  return total;
}

std::string DescribeBlock170(const Record& record) {
  std::ostringstream stream;
  stream << "block=170"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 171 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock171 = {171, 172, 173, 174, 175, 176};

Record MakeRecord171(std::string_view seed, int salt) {
  const double scale = static_cast<double>((171 % 7) + 1) / 3.0;
  const bool active = ((salt + 171) % 2) == 0;
  return Record{
      .id = salt + 171,
      .label = std::string(seed) + "-block-171",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock171(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 171) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 171;
    } else {
      total -= (171 % 5);
    }
  }
  return total;
}

std::string DescribeBlock171(const Record& record) {
  std::ostringstream stream;
  stream << "block=171"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 172 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock172 = {172, 173, 174, 175, 176, 177};

Record MakeRecord172(std::string_view seed, int salt) {
  const double scale = static_cast<double>((172 % 7) + 1) / 3.0;
  const bool active = ((salt + 172) % 2) == 0;
  return Record{
      .id = salt + 172,
      .label = std::string(seed) + "-block-172",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock172(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 172) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 172;
    } else {
      total -= (172 % 5);
    }
  }
  return total;
}

std::string DescribeBlock172(const Record& record) {
  std::ostringstream stream;
  stream << "block=172"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 173 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock173 = {173, 174, 175, 176, 177, 178};

Record MakeRecord173(std::string_view seed, int salt) {
  const double scale = static_cast<double>((173 % 7) + 1) / 3.0;
  const bool active = ((salt + 173) % 2) == 0;
  return Record{
      .id = salt + 173,
      .label = std::string(seed) + "-block-173",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock173(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 173) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 173;
    } else {
      total -= (173 % 5);
    }
  }
  return total;
}

std::string DescribeBlock173(const Record& record) {
  std::ostringstream stream;
  stream << "block=173"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 174 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock174 = {174, 175, 176, 177, 178, 179};

Record MakeRecord174(std::string_view seed, int salt) {
  const double scale = static_cast<double>((174 % 7) + 1) / 3.0;
  const bool active = ((salt + 174) % 2) == 0;
  return Record{
      .id = salt + 174,
      .label = std::string(seed) + "-block-174",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock174(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 174) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 174;
    } else {
      total -= (174 % 5);
    }
  }
  return total;
}

std::string DescribeBlock174(const Record& record) {
  std::ostringstream stream;
  stream << "block=174"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 175 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock175 = {175, 176, 177, 178, 179, 180};

Record MakeRecord175(std::string_view seed, int salt) {
  const double scale = static_cast<double>((175 % 7) + 1) / 3.0;
  const bool active = ((salt + 175) % 2) == 0;
  return Record{
      .id = salt + 175,
      .label = std::string(seed) + "-block-175",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock175(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 175) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 175;
    } else {
      total -= (175 % 5);
    }
  }
  return total;
}

std::string DescribeBlock175(const Record& record) {
  std::ostringstream stream;
  stream << "block=175"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 176 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock176 = {176, 177, 178, 179, 180, 181};

Record MakeRecord176(std::string_view seed, int salt) {
  const double scale = static_cast<double>((176 % 7) + 1) / 3.0;
  const bool active = ((salt + 176) % 2) == 0;
  return Record{
      .id = salt + 176,
      .label = std::string(seed) + "-block-176",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock176(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 176) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 176;
    } else {
      total -= (176 % 5);
    }
  }
  return total;
}

std::string DescribeBlock176(const Record& record) {
  std::ostringstream stream;
  stream << "block=176"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 177 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock177 = {177, 178, 179, 180, 181, 182};

Record MakeRecord177(std::string_view seed, int salt) {
  const double scale = static_cast<double>((177 % 7) + 1) / 3.0;
  const bool active = ((salt + 177) % 2) == 0;
  return Record{
      .id = salt + 177,
      .label = std::string(seed) + "-block-177",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock177(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 177) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 177;
    } else {
      total -= (177 % 5);
    }
  }
  return total;
}

std::string DescribeBlock177(const Record& record) {
  std::ostringstream stream;
  stream << "block=177"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 178 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock178 = {178, 179, 180, 181, 182, 183};

Record MakeRecord178(std::string_view seed, int salt) {
  const double scale = static_cast<double>((178 % 7) + 1) / 3.0;
  const bool active = ((salt + 178) % 2) == 0;
  return Record{
      .id = salt + 178,
      .label = std::string(seed) + "-block-178",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock178(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 178) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 178;
    } else {
      total -= (178 % 5);
    }
  }
  return total;
}

std::string DescribeBlock178(const Record& record) {
  std::ostringstream stream;
  stream << "block=178"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 179 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock179 = {179, 180, 181, 182, 183, 184};

Record MakeRecord179(std::string_view seed, int salt) {
  const double scale = static_cast<double>((179 % 7) + 1) / 3.0;
  const bool active = ((salt + 179) % 2) == 0;
  return Record{
      .id = salt + 179,
      .label = std::string(seed) + "-block-179",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock179(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 179) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 179;
    } else {
      total -= (179 % 5);
    }
  }
  return total;
}

std::string DescribeBlock179(const Record& record) {
  std::ostringstream stream;
  stream << "block=179"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

// block 180 keeps structure repetitive on purpose for large-file validation.
constexpr std::array<int, 6> kBlock180 = {180, 181, 182, 183, 184, 185};

Record MakeRecord180(std::string_view seed, int salt) {
  const double scale = static_cast<double>((180 % 7) + 1) / 3.0;
  const bool active = ((salt + 180) % 2) == 0;
  return Record{
      .id = salt + 180,
      .label = std::string(seed) + "-block-180",
      .weight = ClampValue(scale * static_cast<double>(salt + 1), 0.0, 4096.0),
      .active = active,
  };
}

int AccumulateBlock180(const std::vector<Record>& records) {
  int total = 0;
  for (const Record& record : records) {
    if ((record.id + 180) % 3 == 0) {
      total += record.id + static_cast<int>(record.weight) + 180;
    } else {
      total -= (180 % 5);
    }
  }
  return total;
}

std::string DescribeBlock180(const Record& record) {
  std::ostringstream stream;
  stream << "block=180"
         << " id=" << record.id
         << " label=" << record.label
         << " weight=" << record.weight
         << " active=" << (record.active ? "true" : "false");
  return stream.str();
}

/*
  Multi-line comment block inserted periodically so syntax tests see
  long comment regions amid otherwise repetitive code.
*/
std::optional<Record> FindSpecialRecord180(const std::vector<Record>& records) {
  auto it = std::find_if(records.begin(), records.end(), [](const Record& record) {
    return record.active && (record.id % 60) == 0;
  });
  if (it == records.end()) {
    return std::nullopt;
  }
  return *it;
}

}  // namespace microide::fixtures

int main() {
  using microide::fixtures::Record;
  std::vector<Record> records;
  records.push_back(microide::fixtures::MakeRecord001("alpha", 1));
  records.push_back(microide::fixtures::MakeRecord090("beta", 2));
  records.push_back(microide::fixtures::MakeRecord180("gamma", 3));
  const int score = microide::fixtures::AccumulateBlock090(records);
  FIXTURE_TRACE(score);
  return score == 0 ? 1 : 0;
}
