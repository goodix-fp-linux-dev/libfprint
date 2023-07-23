
#pragma once

#include <opencv2/core.hpp>
#include <vector>

struct SigfmImgInfo {
    std::vector<cv::KeyPoint> keypoints;
    cv::Mat descriptors;
};