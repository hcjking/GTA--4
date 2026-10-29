/**
 * @file        ui/immediate_drawer_web.h
 * @brief       [web port] Immediate drawer for the browser page
 *
 * The web build has no presenter, so overlays (the PS5 hub, dialogs) draw in
 * detached mode: ReXApp drives ImGuiDrawer::Draw itself and this drawer
 * records each frame's triangles into a mailbox the page reads
 * (rex_web_ui_frame) and draws with WebGPU over the game.
 *
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstdint>
#include <memory>

#include <rex/ui/immediate_drawer.h>

namespace rex::ui {

// What the page reads. `sequence` changes last, after the rest is in place.
struct WebUiTexture {
  uint32_t id;
  uint32_t width;
  uint32_t height;
  uint32_t filter_linear;
  uint64_t pixels;  // RGBA8, width * 4 bytes per row; kept while the texture lives
};

struct WebUiDraw {
  uint32_t texture;      // WebUiTexture::id, 0 = colour only
  uint32_t lines;        // 1: line list, else triangles
  uint32_t index_offset; // into WebUiFrame::indices
  uint32_t count;        // indices
  int32_t base_vertex;   // into WebUiFrame::vertices
  uint32_t scissor;      // 1: clip to the rectangle below
  float scissor_rect[4]; // left, top, right, bottom in coordinate space
};

struct WebUiFrame {
  uint32_t sequence;
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t draw_count;
  uint64_t vertices;  // ImmediateVertex (x, y, u, v, colour 0xAABBGGRR) x vertex_count
  uint64_t indices;   // uint16_t x index_count
  uint64_t draws;     // WebUiDraw x draw_count
  float width;        // coordinate space
  float height;
  uint32_t texture_count;
  uint32_t pad;
  uint64_t textures;  // WebUiTexture x texture_count
};

class WebImmediateDrawer final : public ImmediateDrawer {
 public:
  WebImmediateDrawer() = default;

  std::unique_ptr<ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                  ImmediateTextureFilter filter, bool is_repeated,
                                                  const uint8_t* data) override;
  void Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
             float coordinate_space_height) override;
  void BeginDrawBatch(const ImmediateDrawBatch& batch) override;
  void Draw(const ImmediateDraw& draw) override;
  void EndDrawBatch() override;
  void End() override;
};

// A presenter-less draw context for detached overlay drawing (its
// constructor is protected in UIDrawContext).
class WebUIDrawContext final : public UIDrawContext {
 public:
  WebUIDrawContext(uint32_t width, uint32_t height) : UIDrawContext(width, height) {}
};

}  // namespace rex::ui
