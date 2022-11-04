
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

#ifdef __cplusplus
}
#endif
