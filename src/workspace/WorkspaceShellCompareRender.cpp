#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr float kMergeToolbarHeight = 54.0f;
constexpr float kMergeToolbarButtonHeight = 22.0f;
constexpr float kMergeToolbarButtonGap = 8.0f;

SDL_Color BlendColor(SDL_Color base, SDL_Color tint, float amount) {
  const float clamped_amount = std::clamp(amount, 0.0f, 1.0f);
  const auto blend = [&](Uint8 base_component, Uint8 tint_component) {
    return static_cast<Uint8>(std::lround(static_cast<float>(base_component) * (1.0f - clamped_amount) +
                                          static_cast<float>(tint_component) * clamped_amount));
  };
  return SDL_Color{
      blend(base.r, tint.r),
      blend(base.g, tint.g),
      blend(base.b, tint.b),
      0xff,
  };
}

std::size_t Utf8SequenceLength(std::string_view text, std::size_t offset) {
  if (offset >= text.size()) {
    return 0;
  }

  const unsigned char lead = static_cast<unsigned char>(text[offset]);
  if (lead <= 0x7F) {
    return 1;
  }

  auto continuation = [&](std::size_t count) {
    if (offset + count >= text.size()) {
      return false;
    }
    for (std::size_t i = 1; i <= count; ++i) {
      const unsigned char byte = static_cast<unsigned char>(text[offset + i]);
      if ((byte & 0xC0) != 0x80) {
        return false;
      }
    }
    return true;
  };

  if (lead >= 0xC2 && lead <= 0xDF && continuation(1)) {
    return 2;
  }
  if (lead == 0xE0 && continuation(2)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0xA0 && second <= 0xBF) {
      return 3;
    }
  }
  if (((lead >= 0xE1 && lead <= 0xEC) || (lead >= 0xEE && lead <= 0xEF)) && continuation(2)) {
    return 3;
  }
  if (lead == 0xED && continuation(2)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x80 && second <= 0x9F) {
      return 3;
    }
  }
  if (lead == 0xF0 && continuation(3)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x90 && second <= 0xBF) {
      return 4;
    }
  }
  if (lead >= 0xF1 && lead <= 0xF3 && continuation(3)) {
    return 4;
  }
  if (lead == 0xF4 && continuation(3)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x80 && second <= 0x8F) {
      return 4;
    }
  }

  return 1;
}

SDL_Color CompareTokenColor(const render::Theme& theme,
                            editor::SyntaxTokenKind kind,
                            SDL_Color fallback,
                            bool selected) {
  switch (kind) {
    case editor::SyntaxTokenKind::Keyword:
      return theme.syntax_keyword;
    case editor::SyntaxTokenKind::Type:
      return theme.syntax_type;
    case editor::SyntaxTokenKind::String:
      return theme.syntax_string;
    case editor::SyntaxTokenKind::Comment:
      return theme.syntax_comment;
    case editor::SyntaxTokenKind::Number:
      return theme.syntax_number;
    case editor::SyntaxTokenKind::Constant:
      return theme.syntax_constant;
    case editor::SyntaxTokenKind::Preprocessor:
      return theme.syntax_preprocessor;
    case editor::SyntaxTokenKind::Operator:
      return theme.syntax_operator;
    case editor::SyntaxTokenKind::Plain:
    default:
      return selected ? theme.text_primary : fallback;
  }
}

}  // namespace

void WorkspaceShell::RenderCompareSurface(SDL_Renderer* renderer, const SDL_FRect& rect) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (renderer == nullptr || compare_tab == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }

  static const std::vector<editor::SyntaxTokenKind> kEmptyTokens;

  DrawFilledRect(renderer, rect, theme_.editor_background);

  const float line_height = text_renderer_.LineHeight();
  const float gutter_width = std::max(
      28.0f,
      text_renderer_.MeasureWidth(std::to_string(compare_tab->model.rows.size() + 1)) + 12.0f);
  const float divider_width = 18.0f;
  const float content_width = std::max(40.0f, rect.w - gutter_width * 2.0f - divider_width - 16.0f);
  const float left_width = std::floor(content_width * 0.5f);
  const float right_width = content_width - left_width;
  const float left_x = rect.x + 8.0f;
  const float center_x = left_x + gutter_width + left_width;
  const float right_x = center_x + divider_width + gutter_width;
  const float header_y = rect.y + 6.0f;
  const float rows_y = rect.y + line_height + 12.0f;
  const int visible_rows = CompareVisibleRows(rect);
  ClampCompareScrollRow(*compare_tab, visible_rows);

  DrawFilledRect(renderer, MakeRect(rect.x, rows_y - 6.0f, rect.w, 1.0f), theme_.border);
  DrawFilledRect(renderer, MakeRect(center_x - 6.0f, rect.y, 1.0f, rect.h), theme_.border);
  DrawFilledRect(renderer, MakeRect(right_x - 6.0f, rect.y, 1.0f, rect.h), theme_.border);

  text_renderer_.DrawString(renderer, left_x + gutter_width, header_y,
                            theme_.text_secondary,
                            TruncateLabel(compare_tab->left_label, left_width - 8.0f));
  text_renderer_.DrawString(renderer, right_x + gutter_width, header_y,
                            theme_.text_secondary,
                            TruncateLabel(compare_tab->right_label, right_width - 8.0f));

  for (int row = 0; row < visible_rows; ++row) {
    const int model_index = compare_tab->scroll_row + row;
    if (model_index >= static_cast<int>(compare_tab->model.rows.size())) {
      break;
    }

    const auto& compare_row = compare_tab->model.rows[static_cast<std::size_t>(model_index)];
    const float y = rows_y + static_cast<float>(row) * line_height;
    const bool selected = static_cast<std::size_t>(model_index) == compare_tab->selected_row;
    if (selected) {
      DrawFilledRect(renderer, MakeRect(rect.x + 1.0f, y - 1.0f, rect.w - 2.0f, line_height),
                     theme_.row_highlight);
    }

    const SDL_Color row_background = selected ? theme_.row_highlight : theme_.editor_background;
    const auto draw_text = [&](float x, float width, SDL_Color color, const std::string& text) {
      const std::string display_text = TruncateLabel(text, width);
      if (display_text.empty()) {
        return;
      }
      text_renderer_.DrawStringOn(renderer, x, y, color, row_background, display_text);
    };
    const auto draw_syntax_text = [&](float x,
                                      float width,
                                      SDL_Color plain_color,
                                      const std::string& text,
                                      const std::vector<editor::SyntaxTokenKind>& full_tokens,
                                      const std::vector<compare::CompareTextSpan>& changed_spans,
                                      SDL_Color changed_background) {
      if (text.empty()) {
        return;
      }

      const std::string display_text = TruncateLabel(text, width);
      if (display_text.empty()) {
        return;
      }

      const std::size_t visible_text_bytes =
          display_text != text && EndsWith(display_text, "...") && display_text.size() >= 3
              ? display_text.size() - 3
              : display_text.size();
      const auto token_kind_at = [&](std::size_t byte_offset) {
        if (byte_offset < visible_text_bytes && byte_offset < full_tokens.size()) {
          return full_tokens[byte_offset];
        }
        return editor::SyntaxTokenKind::Plain;
      };
      std::size_t changed_span_index = 0;
      const auto byte_is_changed = [&](std::size_t byte_offset) {
        if (byte_offset >= visible_text_bytes) {
          return false;
        }
        while (changed_span_index < changed_spans.size() &&
               changed_spans[changed_span_index].end <= byte_offset) {
          ++changed_span_index;
        }
        return changed_span_index < changed_spans.size() &&
               byte_offset >= changed_spans[changed_span_index].start &&
               byte_offset < changed_spans[changed_span_index].end;
      };

      float segment_x = x;
      for (std::size_t segment_start = 0; segment_start < display_text.size();) {
        const editor::SyntaxTokenKind kind = token_kind_at(segment_start);
        const bool changed = byte_is_changed(segment_start);
        std::size_t segment_end = segment_start;
        while (segment_end < display_text.size()) {
          const std::size_t next = segment_end + Utf8SequenceLength(display_text, segment_end);
          if (next >= display_text.size()) {
            segment_end = display_text.size();
            break;
          }
          if (token_kind_at(next) != kind || byte_is_changed(next) != changed) {
            segment_end = next;
            break;
          }
          segment_end = next;
        }

        const std::string_view segment_text(display_text.data() + segment_start,
                                            segment_end - segment_start);
        text_renderer_.DrawStringOn(
            renderer, segment_x, y,
            CompareTokenColor(theme_, kind, plain_color, selected),
            changed ? changed_background : row_background, segment_text);
        segment_x += text_renderer_.MeasureWidth(segment_text);
        segment_start = segment_end;
      }
    };

    if (compare_row.left_line > 0) {
      draw_text(left_x, gutter_width - 4.0f,
                selected ? theme_.current_line_number : theme_.line_number,
                std::to_string(compare_row.left_line));
    }
    if (compare_row.right_line > 0) {
      draw_text(right_x, gutter_width - 4.0f,
                selected ? theme_.current_line_number : theme_.line_number,
                std::to_string(compare_row.right_line));
    }

    SDL_Color left_color = theme_.text_secondary;
    SDL_Color right_color = theme_.text_secondary;
    SDL_Color marker_color = selected ? theme_.text_secondary : theme_.text_muted;
    char marker = ' ';
    switch (compare_row.kind) {
      case compare::CompareRowKind::Added:
        right_color = theme_.diff_added;
        marker_color = theme_.diff_added;
        marker = '+';
        break;
      case compare::CompareRowKind::Deleted:
        left_color = theme_.diff_deleted;
        marker_color = theme_.diff_deleted;
        marker = '-';
        break;
      case compare::CompareRowKind::Modified:
        left_color = theme_.diff_modified;
        right_color = theme_.diff_modified;
        marker_color = theme_.diff_modified;
        marker = '~';
        break;
      case compare::CompareRowKind::Unchanged:
      default:
        break;
    }

    const SDL_Color left_changed_background = BlendColor(
        row_background,
        compare_row.kind == compare::CompareRowKind::Deleted ? theme_.diff_deleted
                                                             : theme_.diff_modified,
        selected ? 0.42f : 0.28f);
    const SDL_Color right_changed_background = BlendColor(
        row_background,
        compare_row.kind == compare::CompareRowKind::Added ? theme_.diff_added
                                                           : theme_.diff_modified,
        selected ? 0.42f : 0.28f);

    if (compare_row.left_line > 0) {
      const std::vector<editor::SyntaxTokenKind>* cached_tokens =
          static_cast<std::size_t>(model_index) < compare_tab->left_tokens_by_row.size()
              ? &compare_tab->left_tokens_by_row[static_cast<std::size_t>(model_index)]
              : &kEmptyTokens;
      draw_syntax_text(left_x + gutter_width, left_width - 8.0f, left_color, compare_row.left_text,
                       *cached_tokens, compare_row.left_changed_spans, left_changed_background);
    }
    if (compare_row.right_line > 0) {
      const std::vector<editor::SyntaxTokenKind>* cached_tokens =
          static_cast<std::size_t>(model_index) < compare_tab->right_tokens_by_row.size()
              ? &compare_tab->right_tokens_by_row[static_cast<std::size_t>(model_index)]
              : &kEmptyTokens;
      draw_syntax_text(right_x + gutter_width, right_width - 8.0f, right_color,
                       compare_row.right_text, *cached_tokens, compare_row.right_changed_spans,
                       right_changed_background);
    }
    draw_text(center_x + 4.0f, divider_width - 6.0f, marker_color, std::string(1, marker));
  }
}

void WorkspaceShell::RenderMergeSurface(SDL_Renderer* renderer, const SDL_FRect& rect) {
  MergeTabState* merge_tab = ActiveMergeTab();
  if (renderer == nullptr || merge_tab == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }

  static const std::vector<editor::SyntaxTokenKind> kEmptyTokens;

  DrawFilledRect(renderer, rect, theme_.editor_background);

  const float line_height = text_renderer_.LineHeight();
  const std::size_t max_line_count =
      std::max({merge_tab->model.incoming_lines.size(), merge_tab->result_viewport.lines().size(),
                merge_tab->model.current_lines.size(), std::size_t{1}});
  const float gutter_width =
      std::max(28.0f, text_renderer_.MeasureWidth(std::to_string(max_line_count + 1)) + 12.0f);
  const float divider_width = 16.0f;
  const float content_width =
      std::max(60.0f, rect.w - gutter_width * 3.0f - divider_width * 2.0f - 16.0f);
  const float pane_width = std::floor(content_width / 3.0f);
  const float left_width = pane_width;
  const float center_width = pane_width;
  const float right_width = content_width - left_width - center_width;
  const float left_x = rect.x + 8.0f;
  const float center_x = left_x + gutter_width + left_width + divider_width;
  const float right_x = center_x + gutter_width + center_width + divider_width;
  const float button_y = rect.y + 6.0f;
  const float secondary_button_y = button_y + kMergeToolbarButtonHeight + 6.0f;
  const float header_y = rect.y + kMergeToolbarHeight + 4.0f;
  const float rows_y = rect.y + kMergeToolbarHeight + line_height + 12.0f;
  const int visible_rows = MergeVisibleRows(rect);
  ClampMergeScrollRow(*merge_tab, visible_rows);
  const std::size_t selected_hunk =
      merge_tab->model.hunks.empty()
          ? 0
          : std::min(merge_tab->selected_hunk, merge_tab->model.hunks.size() - 1);

  const auto draw_button = [&](const SDL_FRect& button_rect,
                               std::string_view label,
                               bool selected,
                               bool primary = false) {
    const SDL_Color background =
        selected ? theme_.chrome_active : primary ? theme_.surface_raised : theme_.surface_background;
    DrawFilledRect(renderer, button_rect, background);
    DrawRect(renderer, button_rect, selected ? theme_.accent : theme_.border);
    text_renderer_.DrawStringOn(
        renderer, button_rect.x + 10.0f, button_rect.y + 4.0f,
        selected ? theme_.text_primary : theme_.text_secondary, background,
        TruncateLabel(label, button_rect.w - 18.0f));
  };
  const auto make_button_rect = [&](float x, float y, std::string_view label) {
    const float width =
        std::clamp(text_renderer_.MeasureWidth(label) + 18.0f, 64.0f, 160.0f);
    return MakeRect(x, y, width, kMergeToolbarButtonHeight);
  };

  float button_x = rect.x + 8.0f;
  const SDL_FRect incoming_all_rect = make_button_rect(button_x, button_y, "All Incoming");
  button_x += incoming_all_rect.w + kMergeToolbarButtonGap;
  const SDL_FRect auto_rect = make_button_rect(button_x, button_y, "All Auto");
  button_x += auto_rect.w + kMergeToolbarButtonGap;
  const SDL_FRect current_all_rect = make_button_rect(button_x, button_y, "All Current");
  button_x += current_all_rect.w + kMergeToolbarButtonGap;
  const SDL_FRect base_all_rect = make_button_rect(button_x, button_y, "All Base");
  const SDL_FRect save_rect = make_button_rect(rect.x + rect.w - 92.0f, button_y, "Save");
  draw_button(incoming_all_rect, "All Incoming", false, true);
  draw_button(auto_rect, "All Auto", false, true);
  draw_button(current_all_rect, "All Current", false, true);
  draw_button(base_all_rect, "All Base", false, true);
  draw_button(save_rect, "Save", false, true);

  if (!merge_tab->model.hunks.empty()) {
    button_x = rect.x + 8.0f;
    const compare::MergeChoice active_choice = merge_tab->model.hunks[selected_hunk].choice;
    const SDL_FRect incoming_rect = make_button_rect(button_x, secondary_button_y, "Incoming");
    button_x += incoming_rect.w + kMergeToolbarButtonGap;
    const SDL_FRect base_rect = make_button_rect(button_x, secondary_button_y, "Base");
    button_x += base_rect.w + kMergeToolbarButtonGap;
    const SDL_FRect current_rect = make_button_rect(button_x, secondary_button_y, "Current");
    button_x += current_rect.w + kMergeToolbarButtonGap;
    const SDL_FRect both_rect = make_button_rect(button_x, secondary_button_y, "Both");
    button_x += both_rect.w + kMergeToolbarButtonGap;
    const SDL_FRect open_rect = make_button_rect(button_x, secondary_button_y, "Open Result");

    draw_button(incoming_rect, "Incoming", active_choice == compare::MergeChoice::Incoming);
    draw_button(base_rect, "Base", active_choice == compare::MergeChoice::Base);
    draw_button(current_rect, "Current", active_choice == compare::MergeChoice::Current);
    draw_button(both_rect, "Both", active_choice == compare::MergeChoice::Both);
    draw_button(open_rect, "Open Result", false);
  }

  DrawFilledRect(renderer, MakeRect(rect.x, rows_y - 6.0f, rect.w, 1.0f), theme_.border);
  DrawFilledRect(renderer, MakeRect(center_x - 6.0f, rect.y, 1.0f, rect.h), theme_.border);
  DrawFilledRect(renderer, MakeRect(right_x - 6.0f, rect.y, 1.0f, rect.h), theme_.border);

  text_renderer_.DrawString(renderer, left_x + gutter_width, header_y, theme_.text_secondary,
                            TruncateLabel(merge_tab->incoming_label, left_width - 8.0f));
  text_renderer_.DrawString(renderer, center_x + gutter_width, header_y, theme_.text_secondary,
                            TruncateLabel(merge_tab->result_label, center_width - 8.0f));
  text_renderer_.DrawString(renderer, right_x + gutter_width, header_y, theme_.text_secondary,
                            TruncateLabel(merge_tab->current_label, right_width - 8.0f));

  const auto draw_text = [&](float x, float y, float width, SDL_Color color, SDL_Color background,
                             const std::string& text) {
    const std::string display_text = TruncateLabel(text, width);
    if (display_text.empty()) {
      return;
    }
    text_renderer_.DrawStringOn(renderer, x, y, color, background, display_text);
  };
  const auto draw_syntax_text = [&](float x,
                                    float y,
                                    float width,
                                    SDL_Color plain_color,
                                    SDL_Color background,
                                    const std::string& text,
                                    const std::vector<editor::SyntaxTokenKind>& full_tokens) {
    if (text.empty()) {
      return;
    }

    const std::string display_text = TruncateLabel(text, width);
    if (display_text.empty()) {
      return;
    }

    const std::size_t visible_text_bytes =
        display_text != text && EndsWith(display_text, "...") && display_text.size() >= 3
            ? display_text.size() - 3
            : display_text.size();
    const auto token_kind_at = [&](std::size_t byte_offset) {
      if (byte_offset < visible_text_bytes && byte_offset < full_tokens.size()) {
        return full_tokens[byte_offset];
      }
      return editor::SyntaxTokenKind::Plain;
    };

    float segment_x = x;
    for (std::size_t segment_start = 0; segment_start < display_text.size();) {
      const editor::SyntaxTokenKind kind = token_kind_at(segment_start);
      std::size_t segment_end = segment_start;
      while (segment_end < display_text.size()) {
        const std::size_t next = segment_end + Utf8SequenceLength(display_text, segment_end);
        if (next >= display_text.size()) {
          segment_end = display_text.size();
          break;
        }
        if (token_kind_at(next) != kind) {
          segment_end = next;
          break;
        }
        segment_end = next;
      }

      const std::string_view segment_text(display_text.data() + segment_start,
                                          segment_end - segment_start);
      text_renderer_.DrawStringOn(
          renderer, segment_x, y, CompareTokenColor(theme_, kind, plain_color, false), background,
          segment_text);
      segment_x += text_renderer_.MeasureWidth(segment_text);
      segment_start = segment_end;
    }
  };

  for (int row = 0; row < visible_rows; ++row) {
    const int model_index = merge_tab->scroll_row + row;
    if (model_index >= static_cast<int>(merge_tab->display_model.rows.size())) {
      break;
    }

    const auto& merge_row = merge_tab->display_model.rows[static_cast<std::size_t>(model_index)];
    const float y = rows_y + static_cast<float>(row) * line_height;
    const bool selected = merge_row.hunk >= 0 &&
                          static_cast<std::size_t>(merge_row.hunk) == merge_tab->selected_hunk;
    const SDL_Color row_background = selected ? theme_.row_highlight : theme_.editor_background;
    if (selected) {
      DrawFilledRect(renderer, MakeRect(rect.x + 1.0f, y - 1.0f, rect.w - 2.0f, line_height),
                     row_background);
    }

    const SDL_Color incoming_background =
        merge_row.incoming_changed
            ? BlendColor(row_background, merge_row.conflict ? theme_.diff_deleted : theme_.diff_added,
                         selected ? 0.42f : 0.24f)
            : row_background;
    const SDL_Color result_background =
        merge_row.result_changed
            ? BlendColor(row_background,
                         merge_row.conflict ? theme_.diff_modified : theme_.diff_added,
                         selected ? 0.42f : 0.24f)
            : row_background;
    const SDL_Color current_background =
        merge_row.current_changed
            ? BlendColor(row_background, theme_.diff_modified, selected ? 0.42f : 0.24f)
            : row_background;
    const SDL_Color incoming_color =
        merge_row.incoming_changed ? theme_.diff_added : theme_.text_secondary;
    const SDL_Color current_color =
        merge_row.current_changed ? theme_.diff_modified : theme_.text_secondary;
    SDL_Color result_color = theme_.text_secondary;
    if (merge_row.hunk >= 0 &&
        static_cast<std::size_t>(merge_row.hunk) < merge_tab->model.hunks.size()) {
      switch (merge_tab->model.hunks[static_cast<std::size_t>(merge_row.hunk)].choice) {
        case compare::MergeChoice::Incoming:
          result_color = theme_.diff_added;
          break;
        case compare::MergeChoice::Current:
          result_color = theme_.diff_modified;
          break;
        case compare::MergeChoice::Both:
          result_color = theme_.accent;
          break;
        case compare::MergeChoice::Base:
        case compare::MergeChoice::Auto:
        default:
          result_color = merge_row.result_changed ? theme_.diff_added : theme_.text_secondary;
          break;
      }
    }

    if (merge_row.incoming_line > 0) {
      draw_text(left_x, y, gutter_width - 4.0f,
                selected ? theme_.current_line_number : theme_.line_number, row_background,
                std::to_string(merge_row.incoming_line));
    }
    if (merge_row.result_line > 0) {
      draw_text(center_x, y, gutter_width - 4.0f,
                selected ? theme_.current_line_number : theme_.line_number, row_background,
                std::to_string(merge_row.result_line));
    }
    if (merge_row.current_line > 0) {
      draw_text(right_x, y, gutter_width - 4.0f,
                selected ? theme_.current_line_number : theme_.line_number, row_background,
                std::to_string(merge_row.current_line));
    }

    if (merge_row.incoming_line > 0) {
      const std::vector<editor::SyntaxTokenKind>* tokens =
          static_cast<std::size_t>(merge_row.incoming_line - 1) < merge_tab->incoming_tokens.size()
              ? &merge_tab->incoming_tokens[static_cast<std::size_t>(merge_row.incoming_line - 1)]
              : &kEmptyTokens;
      draw_syntax_text(left_x + gutter_width, y, left_width - 8.0f, incoming_color,
                       incoming_background, merge_row.incoming_text, *tokens);
    }
    if (merge_row.result_line > 0) {
      const std::vector<editor::SyntaxTokenKind>* tokens =
          static_cast<std::size_t>(merge_row.result_line - 1) < merge_tab->result_tokens.size()
              ? &merge_tab->result_tokens[static_cast<std::size_t>(merge_row.result_line - 1)]
              : &kEmptyTokens;
      draw_syntax_text(center_x + gutter_width, y, center_width - 8.0f, result_color,
                       result_background, merge_row.result_text, *tokens);
    }
    if (merge_row.current_line > 0) {
      const std::vector<editor::SyntaxTokenKind>* tokens =
          static_cast<std::size_t>(merge_row.current_line - 1) < merge_tab->current_tokens.size()
              ? &merge_tab->current_tokens[static_cast<std::size_t>(merge_row.current_line - 1)]
              : &kEmptyTokens;
      draw_syntax_text(right_x + gutter_width, y, right_width - 8.0f, current_color,
                       current_background, merge_row.current_text, *tokens);
    }
  }
}

}  // namespace microide::workspace
