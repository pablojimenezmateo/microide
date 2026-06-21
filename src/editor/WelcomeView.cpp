#include "editor/WelcomeView.h"

#include <algorithm>

namespace microide::editor {

WelcomeLayout ComputeWelcomeLayout(const SDL_FRect& rect, const WelcomeViewModel& model,
                                   float line_height) {
  WelcomeLayout layout;

  const float row_step = line_height + 8.0f;
  const float card_width = std::min(rect.w - 48.0f, 1100.0f);
  const float card_height = std::min(rect.h - 48.0f, 600.0f);
  const float card_x = rect.x + std::max(24.0f, (rect.w - card_width) * 0.5f);
  const float card_y = rect.y + std::max(24.0f, rect.h * 0.06f);
  layout.card = SDL_FRect{card_x, card_y, std::max(360.0f, card_width), std::max(220.0f, card_height)};

  const float inset = 20.0f;
  const float header_h = line_height * 2.0f + 22.0f;
  layout.header = SDL_FRect{layout.card.x, layout.card.y, layout.card.w, header_h};

  const float panels_y = layout.card.y + header_h + 6.0f;
  const float footer_h = line_height + 20.0f;
  const float panels_h = std::max(line_height * 3.0f, layout.card.h - header_h - footer_h - 12.0f);
  const float gap = 24.0f;
  const float usable_w = layout.card.w - inset * 2.0f - gap;
  const float recents_w = usable_w * 0.56f;
  const float shortcuts_w = usable_w - recents_w;

  layout.recents_panel = SDL_FRect{layout.card.x + inset, panels_y, recents_w, panels_h};
  layout.shortcuts_panel =
      SDL_FRect{layout.recents_panel.x + recents_w + gap, panels_y, shortcuts_w, panels_h};

  // Recent-project rows fill the recents panel below its heading; the open-folder
  // affordance sits one row below the last recent (or right under the heading when
  // there are none).
  const float rows_top = layout.recents_panel.y + line_height + 14.0f;
  const float row_x = layout.recents_panel.x;
  const float row_w = layout.recents_panel.w;
  const float row_h = row_step - 2.0f;
  const float rows_bottom = layout.recents_panel.y + layout.recents_panel.h - row_step;

  float row_y = rows_top;
  for (std::size_t i = 0; i < model.recent_projects.size(); ++i) {
    if (row_y + row_h > rows_bottom) {
      break;
    }
    layout.hit_regions.push_back(WelcomeHitRegion{
        .rect = SDL_FRect{row_x, row_y, row_w, row_h},
        .kind = WelcomeHitRegion::Kind::RecentProject,
        .recent_index = i,
    });
    row_y += row_step;
  }

  // Open-folder affordance: a row below the recents (kept inside the panel).
  const float open_y = std::min(row_y + 6.0f, layout.recents_panel.y + layout.recents_panel.h - row_h);
  layout.open_folder_rect = SDL_FRect{row_x, open_y, row_w, row_h};
  layout.hit_regions.push_back(WelcomeHitRegion{
      .rect = layout.open_folder_rect,
      .kind = WelcomeHitRegion::Kind::OpenFolder,
      .recent_index = 0,
  });

  return layout;
}

}  // namespace microide::editor
