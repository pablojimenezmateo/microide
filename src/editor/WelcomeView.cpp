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
  const float footer_h = line_height + 20.0f;
  const float panels_h = std::max(line_height * 3.0f, layout.card.h - header_h - footer_h - 28.0f);
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

  // Left column flows top-down: "Start" caption, the open-folder button (a
  // dedicated primary-action slot), then a "Recent" caption, then the recent
  // rows or the empty-state. The button no longer derives from the row cursor,
  // so it can never overlap the recents list or its empty caption.
  const float start_caption_y = layout.recents_panel.y;
  const float button_y = start_caption_y + line_height + 8.0f;
  const float button_h = line_height + 12.0f;
  const float button_w = std::min(360.0f, row_w);
  layout.open_folder_rect = SDL_FRect{row_x, button_y, button_w, button_h};
  layout.hit_regions.push_back(WelcomeHitRegion{
      .rect = layout.open_folder_rect,
      .kind = WelcomeHitRegion::Kind::OpenFolder,
      .recent_index = 0,
  });

  const float recents_caption_y = button_y + button_h + 16.0f;
  const float rows_top = recents_caption_y + line_height + 10.0f;
  layout.recents_rows_top = rows_top;
  const float rows_bottom = layout.recents_panel.y + layout.recents_panel.h;

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

  return layout;
}

}  // namespace microide::editor
