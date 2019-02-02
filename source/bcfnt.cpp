/*------------------------------------------------------------------------------
 * Copyright (c) 2019
 *     Michael Theall (mtheall)
 *     piepie62
 *
 * This file is part of tex3ds.
 *
 * tex3ds is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * tex3ds is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with tex3ds.  If not, see <http://www.gnu.org/licenses/>.
 *----------------------------------------------------------------------------*/
/** @file bcfnt.cpp
 *  @brief BCFNT definitions
 */

#include "bcfnt.h"
#include "ft_error.h"
#include "magick_compat.h"
#include "quantum.h"
#include "swizzle.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>

namespace
{
void appendSheet(std::vector<std::uint8_t> &data, Magick::Image &sheet)
{
  swizzle(sheet, false);

  const unsigned w = sheet.columns();
  const unsigned h = sheet.rows();

  data.reserve(data.size() + w*h/2);

  Magick::Pixels cache(sheet);
  PixelPacket p = cache.get(0, 0, w, h);
  for(unsigned i = 0; i < w*h; i += 2)
  {
    data.emplace_back((quantum_to_bits<4>(quantumAlpha(p[i+1])) << 4)
                    | (quantum_to_bits<4>(quantumAlpha(p[i+0])) << 0));
  }
}

// todo: find runs of 1-entry cmaps and consolidate into SCAN types
void coalesceCMAP(std::vector<bcfnt::CMAP> &cmaps)
{
}

std::vector<std::uint8_t>& operator<<(std::vector<std::uint8_t> &o, const char *str)
{
  const std::size_t len = std::strlen(str);

  o.reserve(o.size() + len);
  o.insert(o.end(), str, str+len);

  return o;
}

std::vector<std::uint8_t>& operator<<(std::vector<std::uint8_t> &o, std::uint8_t v)
{
  o.emplace_back(v);

  return o;
}

std::vector<std::uint8_t>& operator<<(std::vector<std::uint8_t> &o, std::uint16_t v)
{
  o.reserve(o.size() + 2);
  o.emplace_back((v >> 0) & 0xFF);
  o.emplace_back((v >> 8) & 0xFF);

  return o;
}

std::vector<std::uint8_t>& operator<<(std::vector<std::uint8_t> &o, std::uint32_t v)
{
  o.reserve(o.size() + 4);
  o.emplace_back((v >>  0) & 0xFF);
  o.emplace_back((v >>  8) & 0xFF);
  o.emplace_back((v >> 16) & 0xFF);
  o.emplace_back((v >> 24) & 0xFF);

  return o;
}
}

namespace bcfnt
{
struct CharMap
{
  const FT_ULong      code      = 0; ///< Code point.
  const FT_UInt       faceIndex = 0; ///< FreeType face index.
  const std::uint16_t cfntIndex = 0; ///< CFNT glyph index.
};

BCFNT::BCFNT(FT_Face face)
{
  lineFeed = face->size->metrics.height >> 6;
  height = (face->bbox.yMax - face->bbox.yMin) >> 6;
  width = (face->bbox.xMax - face->bbox.xMin) >> 6;
  maxWidth = face->size->metrics.max_advance >> 6;
  ascent = face->size->metrics.ascender >> 6;

  std::map<FT_ULong, CharMap> faceMap;

  std::uint16_t cfntIndex = 0;
  FT_UInt faceIndex;
  FT_ULong code = FT_Get_First_Char(face, &faceIndex);
  while(code != 0)
  {
    // only supports 16-bit code points; also 0xFFFF is explicitly a non-character
    if(code >= std::numeric_limits<std::uint16_t>::max())
      continue;

    assert(cfntIndex != std::numeric_limits<std::uint16_t>::max());

    faceMap.emplace(code, CharMap{code, faceIndex, cfntIndex++});
    code = FT_Get_Next_Char(face, code, &faceIndex);
  }

  if(faceMap.empty())
    return;

  // try to provide a replacement character
  if(faceMap.count(0xFFFD))
    altIndex = faceMap[0xFFFD].cfntIndex;
  else if(faceMap.count('?'))
    altIndex = faceMap['?'].cfntIndex;
  else if(faceMap.count(' '))
    altIndex = faceMap[' '].cfntIndex;
  else
    altIndex = 0;

  // collect character mappings
  for(const auto &pair: faceMap)
  {
    const FT_ULong &code    = pair.first;
    const CharMap  &charMap = pair.second;

    if(code == 0 || cmaps.empty() || cmaps.back().codeEnd != code-1)
    {
      cmaps.emplace_back();
      auto &cmap = cmaps.back();
      cmap.codeBegin = cmap.codeEnd = code;
      cmap.data = std::make_unique<CMAPDirect>(charMap.cfntIndex);
      cmap.mappingMethod = cmap.data->type();
    }
    else
      cmaps.back().codeEnd = code;
  }

  // convert from 26.6 fixed-point format
  const int baseline = face->size->metrics.ascender >> 6;

  // extract cwdh and sheet data
  std::unique_ptr<Magick::Image> sheet;
  for(const auto &cmap: cmaps)
  {
    for(std::uint16_t code = cmap.codeBegin; code <= cmap.codeEnd; ++code)
    {
      // load glyph and render
      FT_Error error = FT_Load_Glyph(face, faceMap[code].faceIndex, FT_LOAD_RENDER);
      if(error)
      {
        std::fprintf(stderr, "FT_Load_Glyph: %s\n", ft_error(error));
        continue;
      }

      // convert from 26.6 fixed-point format
      const std::int8_t  left       = face->glyph->metrics.horiBearingX >> 6;;
      const std::uint8_t glyphWidth = face->glyph->metrics.width >> 6;
      const std::uint8_t charWidth  = face->glyph->metrics.horiAdvance >> 6;

      // add char width info to cwdh
      widths.emplace_back(CharWidthInfo{left, glyphWidth, charWidth});

      if(faceMap[code].cfntIndex % 170 == 0)
      {
        if(sheet)
        {
          appendSheet(sheetData, *sheet);
          ++numSheets;
        }

        sheet = std::make_unique<Magick::Image>(Magick::Geometry(256, 512), transparent());
      }

      assert(sheet);

      const unsigned sheetIndex = faceMap[code].cfntIndex % 170;
      const unsigned sheetX = (sheetIndex % 10) * 24;
      const unsigned sheetY = (sheetIndex / 10) * 30;

      Magick::Pixels cache(*sheet);
      assert(sheetX + 24 < sheet->columns());
      assert(sheetY + 30 < sheet->rows());
      PixelPacket p = cache.get(sheetX, sheetY, 24, 30);
      for(unsigned y = 0; y < face->glyph->bitmap.rows; ++y)
      {
        for(unsigned x = 0; x < face->glyph->bitmap.width; ++x)
        {
          const int px = x + face->glyph->bitmap_left;
          const int py = y + (baseline - face->glyph->bitmap_top);

          if(px < 0 || px >= 24 || py < 0 || py >= 30)
            continue;

          const std::uint8_t v = face->glyph->bitmap.buffer[y * face->glyph->bitmap.width + x];

          Magick::Color c;
          quantumRed(c,   bits_to_quantum<8>(0));
          quantumGreen(c, bits_to_quantum<8>(0));
          quantumBlue(c,  bits_to_quantum<8>(0));
          quantumAlpha(c, bits_to_quantum<8>(v));

          p[py*24 + px] = c;
        }
      }
      cache.sync();
    }
  }

  if(sheet)
  {
    appendSheet(sheetData, *sheet);
    ++numSheets;
  }

  coalesceCMAP(cmaps);
}

bool BCFNT::serialize(const std::string &path)
{
  std::vector<std::uint8_t> output;

  std::uint32_t fileSize = 0;
  fileSize += 0x14; // CFNT header

  const std::uint32_t finfOffset = fileSize;
  fileSize += 0x20; // FINF header

  const std::uint32_t tglpOffset = fileSize;
  fileSize += 0x20; // TGLP header

  // CWDH headers + data
  const std::uint32_t cwdhOffset = fileSize;
  fileSize += 0x10;              // CWDH header
  fileSize += 3 * widths.size(); // CWDH data

  // CMAP headers + data
  std::uint32_t cmapOffset = fileSize;
  for(const auto &cmap: cmaps)
  {
    fileSize += 0x14; // CMAP header

    switch(cmap.mappingMethod)
    {
    case CMAPData::CMAP_TYPE_DIRECT:
      fileSize += 0x2;
      break;

    case CMAPData::CMAP_TYPE_TABLE:
    case CMAPData::CMAP_TYPE_SCAN:
    default:
      abort();
    }
  }

  const std::uint32_t sheetOffset = fileSize;
  fileSize += sheetData.size();

  // FINF, TGLP, CWDH, CMAPs
  std::uint32_t numBlocks = 3 + cmaps.size();

  // CFNT header
  output << "CFNT"                                 // magic
         << static_cast<std::uint16_t>(0xFEFF)     // byte-order-mark
         << static_cast<std::uint16_t>(0x14)       // header size
         << static_cast<std::uint32_t>(0x3)        // version
         << static_cast<std::uint32_t>(fileSize)   // file size
         << static_cast<std::uint32_t>(numBlocks); // number of blocks

  // FINF header
  assert(output.size() == finfOffset);
  output << "FINF"                                   // magic
         << static_cast<std::uint32_t>(0x20)         // section size
         << static_cast<std::uint8_t>(0x1)           // font type
         << static_cast<std::uint8_t>(lineFeed)      // line feed
         << static_cast<std::uint16_t>(altIndex)     // alternate char index
         << static_cast<std::uint8_t>(0x0)           // default width (left)
         << static_cast<std::uint8_t>(0x0)           // default width (glyph width)
         << static_cast<std::uint8_t>(0x0)           // default width (char width)
         << static_cast<std::uint8_t>(0x1)           // encoding
         << static_cast<std::uint32_t>(tglpOffset+8) // TGLP offset
         << static_cast<std::uint32_t>(cwdhOffset+8) // CWDH offset
         << static_cast<std::uint32_t>(cmapOffset+8) // CMAP offset
         << static_cast<std::uint8_t>(height)        // font height
         << static_cast<std::uint8_t>(width)         // font width
         << static_cast<std::uint8_t>(ascent)        // font ascent
         << static_cast<std::uint8_t>(0x0);          // padding

  // TGLP header
  assert(output.size() == tglpOffset);
  output << "TGLP"                                       // magic
         << static_cast<std::uint32_t>(0x20)             // section size
         << static_cast<std::uint8_t>(24)                // cell width
         << static_cast<std::uint8_t>(30)                // cell height
         << static_cast<std::uint8_t>(ascent)            // cell baseline
         << static_cast<std::uint8_t>(maxWidth)          // max character width
         << static_cast<std::uint32_t>(sheetData.size()) // sheet data size
         << static_cast<std::uint16_t>(numSheets)        // number of sheets
         << static_cast<std::uint16_t>(0xB)              // 4-bit alpha format
         << static_cast<std::uint16_t>(10)               // num columns
         << static_cast<std::uint16_t>(10)               // num rows
         << static_cast<std::uint16_t>(256)              // sheet width
         << static_cast<std::uint16_t>(512)              // sheet height
         << static_cast<std::uint32_t>(sheetOffset);     // sheet data offset

  // CWDH header + data
  assert(output.size() == cwdhOffset);

  output << "CWDH" // magic
         << static_cast<std::uint32_t>(0x10 + 3*widths.size()) // section size
         << static_cast<std::uint16_t>(0)                      // start index
         << static_cast<std::uint16_t>(widths.size())          // end index
         << static_cast<std::uint32_t>(0);                     // next CWDH offset

  for(const auto &info: widths)
  {
    output << static_cast<std::uint8_t>(info.left)
           << static_cast<std::uint8_t>(info.glyphWidth)
           << static_cast<std::uint8_t>(info.charWidth);
  }

  for(const auto &cmap: cmaps)
  {
    assert(output.size() == cmapOffset);

    // todo: this only handles DIRECT
    const std::uint32_t size = 0x14 + 0x2;

    output << "CMAP"                                         // magic
           << static_cast<std::uint32_t>(size)               // section size
           << static_cast<std::uint16_t>(cmap.codeBegin)     // code begin
           << static_cast<std::uint16_t>(cmap.codeEnd)       // code end
           << static_cast<std::uint16_t>(cmap.mappingMethod) // mapping method
           << static_cast<std::uint16_t>(0x0);               // padding

    // next CMAP offset
    if(&cmap == &cmaps.back())
      output << static_cast<std::uint32_t>(0); 
    else
      output << static_cast<std::uint32_t>(cmapOffset + size + 8);

    switch(cmap.mappingMethod)
    {
    case CMAPData::CMAP_TYPE_DIRECT:
    {
      const auto &direct = dynamic_cast<const CMAPDirect&>(*cmap.data);
      output << static_cast<std::uint16_t>(direct.offset);
      break;
    }

    case CMAPData::CMAP_TYPE_TABLE:
    case CMAPData::CMAP_TYPE_SCAN:
    default:
      abort();
    }
 
    cmapOffset += size;
  }

  assert(output.size() == sheetOffset);
  output.reserve(output.size() + sheetData.size());
  output.insert(output.end(), sheetData.begin(), sheetData.end());

  assert(output.size() == fileSize);

  FILE *fp = std::fopen(path.c_str(), "wb");
  if(!fp)
    return false;

  std::size_t offset = 0;
  while(offset < output.size())
  {
    std::size_t rc = std::fwrite(&output[offset], 1, output.size() - offset, fp);
    if(rc != output.size() - offset)
    {
      if(rc == 0 || std::ferror(fp))
      {
        if(std::ferror(fp))
          std::fprintf(stderr, "fwrite: %s\n", std::strerror(errno));
        else
          std::fprintf(stderr, "fwrite: Unknown write failure\n");

        std::fclose(fp);
        return false;
      }
    }

    offset += rc;
  }

  if(std::fclose(fp) != 0)
  {
     std::fprintf(stderr, "fclose: %s\n", std::strerror(errno));
     return false;
  }

  return true;
}
}
