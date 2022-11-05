// SIGFM algorithm for libfprint

// Copyright (C) 2022 Matthieu CHARETTE <matthieu.charette@gmail.com>
// Copyright (c) 2022 Natasha England-Elbro <ashenglandelbro@protonmail.com>

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
//

#pragma once

#ifdef __cplusplus
extern "C" {
#endif
struct SfmEnrollData;
typedef struct SfmEnrollData SfmEnrollData;
typedef unsigned char SfmPix;
typedef struct SfmImgInfo SfmImgInfo;

SfmEnrollData* sfm_begin_enroll(const char* username, int finger);
void sfm_add_enroll_frame(SfmEnrollData* data, unsigned char* pix, int width,
                          int height);
SfmImgInfo* sfm_extract(SfmPix* pix, int width, int height);

void sfm_end_enroll(SfmEnrollData* data);
void sfm_free_info(SfmImgInfo* info);
int sfm_match_score(SfmImgInfo* frame, SfmImgInfo* enrolled);
unsigned char* sfm_serialize_binary(SfmImgInfo* info, int* outlen);
SfmImgInfo* sfm_deserialize_binary(unsigned char* bytes, int len);
int sfm_keypoints_count(SfmImgInfo* info);
SfmImgInfo* sfm_copy_info(SfmImgInfo* info);

#ifdef __cplusplus
}
#endif
