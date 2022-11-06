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
typedef unsigned char SfmPix;
/**
 * @brief Contains information used by the sigfm algorithm for matching
 * @details Get one from sfm_extract() and make sure to clean it up with sfm_free_info()
 * @struct SfmImgInfo
 */
typedef struct SfmImgInfo SfmImgInfo;

/**
 * @brief Extracts information from an image for later use sfm_match_score
 *
 * @param pix Pixels of the image must be width * height in length
 * @param width Width of the image
 * @param height Height of the image
 * @return SfmImgInfo* Info that can be used with the API
 */
SfmImgInfo* sfm_extract(const SfmPix* pix, int width, int height);

/**
 * @brief Destroy an SfmImgInfo
 * @warning Call this instead of free() or you will get UB!
 * @param info SfmImgInfo to destroy
 */
void sfm_free_info(SfmImgInfo* info);

/**
 * @brief Score how closely a frame matches another
 *
 * @param frame Print to be checked
 * @param enrolled Canonical print to verify against
 * @return int Score of how closely they match, values <0 indicate error, 0 means always reject
 */
int sfm_match_score(SfmImgInfo* frame, SfmImgInfo* enrolled);

/**
 * @brief Serialize an image info for storage
 *
 * @param info SfmImgInfo to store
 * @param outlen output: Length of the returned byte array
 * @return unsigned* char byte array for storage, should be free'd by the callee
 */
unsigned char* sfm_serialize_binary(SfmImgInfo* info, int* outlen);
/**
 * @brief Deserialize an SfmImgInfo from storage
 *
 * @param bytes Byte array to deserialize from
 * @param len Length of the byte array
 * @return SfmImgInfo* Deserialized info, or NULL if deserialization failed
 */
SfmImgInfo* sfm_deserialize_binary(const unsigned char* bytes, int len);

/**
 * @brief Keypoints for an image. Low keypoints generally means the image is
 * low quality for matching
 *
 * @param info
 * @return int
 */

int sfm_keypoints_count(SfmImgInfo* info);

/**
 * @brief Copy an SfmImgInfo
 *
 * @param info Source of copy
 * @return SfmImgInfo* Newly allocated and copied version of info
 */
SfmImgInfo* sfm_copy_info(SfmImgInfo* info);

#ifdef __cplusplus
}
#endif
