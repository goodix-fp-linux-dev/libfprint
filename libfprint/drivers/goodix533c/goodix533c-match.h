/*
 * Goodix 27c6:533c native driver for libfprint — SIGFM template format and matching
 *
 * Copyright (C) 2026 libfprint contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include "goodix533c-private.h"

/* Opaque handle for extracted SIGFM features (struct SigfmImgInfo). Only
 * this module talks to the SIGFM/OpenCV implementation directly. */
typedef struct SigfmImgInfo GoodixMatchInfo;

typedef enum {
  GOODIX533C_SIGFM_TEMPLATE_OK,
  GOODIX533C_SIGFM_TEMPLATE_INCOMPATIBLE,
  GOODIX533C_SIGFM_TEMPLATE_INVALID,
} Goodix533cSigfmTemplateStatus;

/* Extract SIGFM features (CLAHE + SIFT) from a processed 8-bit sensor frame
 * of GOODIX533C_SENSOR_WIDTH x GOODIX533C_SENSOR_HEIGHT pixels. Free the
 * result with goodix533c_match_free_info(). Returns NULL on failure (never
 * throws across the C ABI -- see sigfm.cpp). */
GoodixMatchInfo *goodix533c_match_extract (const guint8 *image);

int  goodix533c_match_keypoints_count (GoodixMatchInfo *info);

void goodix533c_match_free_info (GoodixMatchInfo *info);

/* Serialize extracted features into a driver-owned template (magic + version
 * header + serialized features). Returns NULL on serialization failure. */
GBytes *goodix533c_match_serialize_template (GoodixMatchInfo *info);

/* Score @probe_info against one serialized enrolled sample. On
 * GOODIX533C_SIGFM_TEMPLATE_OK, *score holds the SIGFM match score. */
Goodix533cSigfmTemplateStatus goodix533c_match_serialized_feature (GoodixMatchInfo *probe_info,
                                                                   const guint8    *feature,
                                                                   gsize            feature_len,
                                                                   int             *score);
