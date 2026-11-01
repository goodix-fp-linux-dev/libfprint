
#pragma once

#include <opencv2/core.hpp>
#include <vector>

struct SigfmImgInfo {
    // SIFT keypoints are kept in row order with their corresponding descriptor rows.
    std::vector<cv::KeyPoint> keypoints;
    cv::Mat descriptors;
};
