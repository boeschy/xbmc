/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <vector>

class CDVDOverlayImage;
class CDVDOverlaySpu;
class CDVDOverlaySSA;
typedef struct ass_image ASS_Image;

namespace OVERLAY
{

struct SQuad
{
  int u, v;
  unsigned char r, g, b, a;
  int x, y;
  int w, h;
};

struct SQuads
{
  int size_x{0};
  int size_y{0};
  std::vector<uint8_t> texture;
  std::vector<SQuad> quad;
};

// Transparent border of a bitmap overlay as fraction of its size
struct SContentInset
{
  float left{0};
  float top{0};
  float right{0};
  float bottom{0};
};

SContentInset MeasureContentInset(const CDVDOverlayImage& o);
void convert_rgba(const CDVDOverlayImage& o, bool mergealpha, std::vector<uint32_t>& rgba);
void convert_rgba(const CDVDOverlaySpu& o,
                  bool mergealpha,
                  int& min_x,
                  int& max_x,
                  int& min_y,
                  int& max_y,
                  std::vector<uint32_t>& rgba);
bool convert_quad(ASS_Image* images, SQuads& quads, int max_x);
// subtitleDepth: authored PGS offset in pixels at 1920 wide, valid if authoredDepth
int GetStereoscopicDepth(bool isPgs = false, int subtitleDepth = 0, bool authoredDepth = false);

} // namespace OVERLAY
