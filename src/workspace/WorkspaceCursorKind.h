#pragma once

#include <cstddef>
#include <string_view>

#include "terminal/TerminalPointerShape.h"

namespace microide::workspace {

// The mouse pointer the window shows (WorkspaceShell::CursorKindForPosition), one
// per desktop cursor-theme shape it uses. Its own header so the shell's member
// list names it in one line, and a terminal program's OSC 22 shape maps onto it.
enum class CursorKind {
  Default,
  Text,
  Pointer,
  EwResize,
  NsResize,
  NResize,
  EResize,
  SResize,
  WResize,
  NeResize,
  SeResize,
  SwResize,
  NwResize,
  Wait,
  Progress,
  Crosshair,
  Move,
  NotAllowed,
  NwseResize,
  NeswResize,
};
inline constexpr std::size_t kCursorKindCount = static_cast<std::size_t>(CursorKind::NeswResize) + 1;

// Its CSS name: what the control channel's `ui` query reports.
constexpr std::string_view CursorKindName(CursorKind kind) {
  switch (kind) {
    case CursorKind::Default: return "default";
    case CursorKind::Text: return "text";
    case CursorKind::Pointer: return "pointer";
    case CursorKind::EwResize: return "ew-resize";
    case CursorKind::NsResize: return "ns-resize";
    case CursorKind::NResize: return "n-resize";
    case CursorKind::EResize: return "e-resize";
    case CursorKind::SResize: return "s-resize";
    case CursorKind::WResize: return "w-resize";
    case CursorKind::NeResize: return "ne-resize";
    case CursorKind::SeResize: return "se-resize";
    case CursorKind::SwResize: return "sw-resize";
    case CursorKind::NwResize: return "nw-resize";
    case CursorKind::Wait: return "wait";
    case CursorKind::Progress: return "progress";
    case CursorKind::Crosshair: return "crosshair";
    case CursorKind::Move: return "move";
    case CursorKind::NotAllowed: return "not-allowed";
    case CursorKind::NwseResize: return "nwse-resize";
    case CursorKind::NeswResize: return "nesw-resize";
  }
  return "default";
}

// A terminal program's pointer (TerminalSession::PointerShapeAt) as the window's.
constexpr CursorKind CursorKindFor(terminal::TerminalPointerShape shape) {
  using S = terminal::TerminalPointerShape;
  switch (shape) {
    case S::None:
    case S::Default: return CursorKind::Default;
    case S::Text: return CursorKind::Text;
    case S::Pointer: return CursorKind::Pointer;
    case S::Wait: return CursorKind::Wait;
    case S::Progress: return CursorKind::Progress;
    case S::Crosshair: return CursorKind::Crosshair;
    case S::Move: return CursorKind::Move;
    case S::NotAllowed: return CursorKind::NotAllowed;
    case S::EwResize: return CursorKind::EwResize;
    case S::NsResize: return CursorKind::NsResize;
    case S::NwseResize: return CursorKind::NwseResize;
    case S::NeswResize: return CursorKind::NeswResize;
    case S::NResize: return CursorKind::NResize;
    case S::EResize: return CursorKind::EResize;
    case S::SResize: return CursorKind::SResize;
    case S::WResize: return CursorKind::WResize;
    case S::NeResize: return CursorKind::NeResize;
    case S::SeResize: return CursorKind::SeResize;
    case S::SwResize: return CursorKind::SwResize;
    case S::NwResize: return CursorKind::NwResize;
  }
  return CursorKind::Default;
}

}  // namespace microide::workspace
