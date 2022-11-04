
#include "sigfm.hpp"
#include "fpi-minutiae.h"
#include "opencv2/core/types.hpp"
#include "opencv2/features2d.hpp"
#include "opencv2/imgcodecs.hpp"
#include <algorithm>
#include <filesystem>
#include <string>

#include <opencv2/opencv.hpp>
#include <vector>

namespace fs = std::filesystem;
struct SfmEnrollData {
    fs::path img_path_base;
};

struct SfmImgInfo {
    std::vector<cv::KeyPoint> keypoints;
    cv::Mat descriptors;
};

SfmEnrollData* sfm_begin_enroll(const char* username, int finger)
{
    const auto img_path = fs::path{"~/goodixtls-store-dev-remove-later"} /
                          "prints" / username / std::to_string(finger);
    auto* enroll_data = new SfmEnrollData{img_path};
    if (!fs::exists(img_path)) {
        fs::create_directories(img_path);
    }
    cv::KeyPoint kp;
    return enroll_data;
}

fp_minutiae keypoints_to_fp_minutae(const std::vector<cv::KeyPoint>& pts)
{
    fp_minutiae out;

    // std::transform(pts.begin(), pts.end(), [](const cv::KeyPoint& pt) { pt.
    // })
    return out;
}

SfmImgInfo* sfm_extract(SfmPix* pix, int width, int height)
{
    cv::Mat img{height, width, CV_8UC1, pix};
    const auto roi = cv::Mat::ones(cv::Size{img.size[1], img.size[0]}, CV_8UC1);
    std::vector<cv::KeyPoint> pts;
    cv::Mat descs;
    cv::SIFT::create()->detectAndCompute(img, roi, pts, descs);

    auto* info = new SfmImgInfo{pts, descs};
    //*minutae = keypoints_to_fp_minutiae(pts);
    return info;
}

void sfm_free_info(SfmImgInfo* info) { delete info; }

void sfm_add_enroll_frame(SfmEnrollData* data, unsigned char* pix, int width,
                          int height)
{
    cv::Mat img{height, width, CV_8UC1, pix};
    cv::imwrite(data->img_path_base / "img.pgm", img);
}

void sfm_end_enroll(SfmEnrollData* data) { delete data; }
