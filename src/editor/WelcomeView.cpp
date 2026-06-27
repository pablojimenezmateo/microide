#include "editor/WelcomeView.h"

#include <algorithm>

namespace microide::editor {

WelcomeLayout ComputeWelcomeLayout(const SDL_FRect& rect, const WelcomeViewModel& model,
                                   float line_height) {
  WelcomeLayout layout;

  const float row_step = line_height + 8.0f;

  // Bold hero card: fill the editor surface with a tight margin so the card and
  // the editor frame read as one panel instead of a small box floating inside a
  // larger one. Width is capped only to avoid an absurdly stretched card on very
  // wide monitors, in which case it re-centers.
  const float margin = 16.0f;
  const float avail_w = rect.w - margin * 2.0f;
  const float card_w = std::max(360.0f, std::min(avail_w, 1400.0f));
  const float card_h = std::max(220.0f, rect.h - margin * 2.0f);
  const float card_x = rect.x + std::max(margin, (rect.w - card_w) * 0.5f);
  const float card_y = rect.y + margin;
  layout.card = SDL_FRect{card_x, card_y, card_w, card_h};

  const float inset = 24.0f;
  const float header_h = line_height * 2.0f + 24.0f;
  layout.header = SDL_FRect{layout.card.x, layout.card.y, layout.card.w, header_h};

  const float panels_y = layout.card.y + header_h + 16.0f;
  const float panels_h = std::max(line_height * 3.0f, layout.card.h - header_h - 28.0f);
  const float gap = 32.0f;
  const float usable_w = layout.card.w - inset * 2.0f - gap;
  const float recents_w = usable_w * 0.54f;
  const float shortcuts_w = usable_w - recents_w;

  layout.recents_panel = SDL_FRect{layout.card.x + inset, panels_y, recents_w, panels_h};
  layout.shortcuts_panel =
      SDL_FRect{layout.recents_panel.x + recents_w + gap, panels_y, shortcuts_w, panels_h};

  const float row_x = layout.recents_panel.x;
  const float row_w = layout.recents_panel.w;
  const float row_h = row_step - 2.0f;
  const float rows_bottom = layout.recents_panel.y + layout.recents_panel.h;
  const float button_h = line_height + 12.0f;
  const float button_w = std::min(360.0f, row_w);

  // A primary-action button slot at `button_y`; returns the Y below it. Each button is a
  // dedicated slot (not derived from the recent-row cursor) so it can never overlap the
  // list or its empty-state caption. The renderer and hit-test share these rects.
  const auto push_button = [&](float button_y, WelcomeHitRegion::Kind kind) {
    layout.hit_regions.push_back(WelcomeHitRegion{
        .rect = SDL_FRect{row_x, button_y, button_w, button_h},
        .kind = kind,
        .recent_index = 0,
    });
  };

  // Lay out the recent-row list (or its empty-state slot) starting at `rows_top`, emitting
  // a hit region of `kind` per entry that fits. Records `recents_rows_top` for the renderer.
  const auto push_recent_rows = [&](float rows_top, WelcomeHitRegion::Kind kind,
                                    std::size_t count) {
    layout.recents_rows_top = rows_top;
    float row_y = rows_top;
    for (std::size_t i = 0; i < count; ++i) {
      if (row_y + row_h > rows_bottom) {
        break;
      }
      layout.hit_regions.push_back(WelcomeHitRegion{
          .rect = SDL_FRect{row_x, row_y, row_w, row_h},
          .kind = kind,
          .recent_index = i,
      });
      row_y += row_step;
    }
  };

  if (model.kind == WelcomeKind::ProjectHome) {
    // Left column flows top-down: an "Actions" caption, three primary-action buttons,
    // a "Recent files" caption, then the recent-file rows (or an empty-state).
    float button_y = layout.recents_panel.y + line_height + 8.0f;
    push_button(button_y, WelcomeHitRegion::Kind::NewFile);
    button_y += button_h + 8.0f;
    push_button(button_y, WelcomeHitRegion::Kind::OpenFile);
    button_y += button_h + 8.0f;
    push_button(button_y, WelcomeHitRegion::Kind::FindInProject);
    button_y += button_h;

    const float recents_caption_y = button_y + 16.0f;
    push_recent_rows(recents_caption_y + line_height + 10.0f, WelcomeHitRegion::Kind::RecentFile,
                     model.recent_files.size());
    return layout;
  }

  // NoProject: "Start" caption, the open-folder button, a "Recent" caption, recent rows.
  const float button_y = layout.recents_panel.y + line_height + 8.0f;
  layout.open_folder_rect = SDL_FRect{row_x, button_y, button_w, button_h};
  push_button(button_y, WelcomeHitRegion::Kind::OpenFolder);

  const float recents_caption_y = button_y + button_h + 16.0f;
  push_recent_rows(recents_caption_y + line_height + 10.0f, WelcomeHitRegion::Kind::RecentProject,
                   model.recent_projects.size());
  return layout;
}

}  // namespace microide::editor
