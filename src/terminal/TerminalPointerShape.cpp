#include "terminal/TerminalPointerShape.h"

#include <utility>

namespace microide::terminal {

std::optional<TerminalPointerShape> TerminalPointerShapeFromName(std::string_view name) {
  using S = TerminalPointerShape;
  static constexpr std::pair<std::string_view, TerminalPointerShape> kNames[] = {
      // CSS cursor keywords.
      {"default", S::Default},         {"auto", S::Default},          {"text", S::Text},
      {"vertical-text", S::Text},      {"pointer", S::Pointer},       {"help", S::Default},
      {"context-menu", S::Default},    {"wait", S::Wait},             {"progress", S::Progress},
      {"crosshair", S::Crosshair},     {"cell", S::Crosshair},        {"move", S::Move},
      {"all-scroll", S::Move},         {"grab", S::Move},             {"grabbing", S::Move},
      {"not-allowed", S::NotAllowed},  {"no-drop", S::NotAllowed},    {"ew-resize", S::EwResize},
      {"col-resize", S::EwResize},     {"ns-resize", S::NsResize},    {"row-resize", S::NsResize},
      {"nwse-resize", S::NwseResize},  {"nesw-resize", S::NeswResize}, {"n-resize", S::NResize},
      {"e-resize", S::EResize},        {"s-resize", S::SResize},      {"w-resize", S::WResize},
      {"ne-resize", S::NeResize},      {"se-resize", S::SeResize},    {"sw-resize", S::SwResize},
      {"nw-resize", S::NwResize},      {"alias", S::Default},         {"copy", S::Default},
      {"zoom-in", S::Default},         {"zoom-out", S::Default},
      // X cursor-font names (xterm's pointerShape / OSC 22).
      {"left_ptr", S::Default},        {"arrow", S::Default},         {"top_left_arrow", S::Default},
      {"xterm", S::Text},              {"ibeam", S::Text},            {"hand1", S::Pointer},
      {"hand2", S::Pointer},           {"pointing_hand", S::Pointer}, {"watch", S::Wait},
      {"left_ptr_watch", S::Progress}, {"cross", S::Crosshair},       {"tcross", S::Crosshair},
      {"fleur", S::Move},              {"X_cursor", S::NotAllowed},   {"pirate", S::NotAllowed},
      {"sb_h_double_arrow", S::EwResize}, {"sb_v_double_arrow", S::NsResize},
      {"top_side", S::NResize},        {"bottom_side", S::SResize},   {"left_side", S::WResize},
      {"right_side", S::EResize},      {"top_left_corner", S::NwResize},
      {"top_right_corner", S::NeResize}, {"bottom_left_corner", S::SwResize},
      {"bottom_right_corner", S::SeResize},
  };
  for (const auto& [known, shape] : kNames) {
    if (known == name) {
      return shape;
    }
  }
  return std::nullopt;
}

std::optional<TerminalPointerShape> TerminalPointerShapeFromOsc22(std::string_view payload) {
  if (!payload.empty() && payload.front() == '?') {
    return std::nullopt;  // a query: not answered, nothing changes
  }
  if (!payload.empty() && (payload.front() == '>' || payload.front() == '=')) {
    payload.remove_prefix(1);
  }
  if (payload.empty() || payload.front() == '<') {
    return TerminalPointerShape::None;  // reset (or a pop: no stack here)
  }
  while (!payload.empty()) {
    const std::size_t comma = payload.find(',');
    if (const std::optional<TerminalPointerShape> shape = TerminalPointerShapeFromName(payload.substr(0, comma))) {
      return shape;
    }
    payload = comma == std::string_view::npos ? std::string_view() : payload.substr(comma + 1);
  }
  return std::nullopt;  // nothing known: leave it as it was
}

}  // namespace microide::terminal
