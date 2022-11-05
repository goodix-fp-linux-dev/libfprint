
#pragma once

#include <opencv2/core.hpp>
#include <vector>

struct SfmImgInfo {
    std::vector<cv::KeyPoint> keypoints;
    cv::Mat descriptors;
};