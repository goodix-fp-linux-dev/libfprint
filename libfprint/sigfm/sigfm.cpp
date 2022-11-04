
#include "sigfm.hpp"
#include "opencv2/core/persistence.hpp"
#include "opencv2/core/types.hpp"
#include "opencv2/features2d.hpp"
#include "opencv2/imgcodecs.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include <opencv2/opencv.hpp>
#include <vector>

namespace fs = std::filesystem;

namespace {
constexpr auto distance_match = 0.75;
constexpr auto length_match = 0.05;
constexpr auto angle_match = 0.05;
constexpr auto min_match = 5;
struct match {
    cv::Point2i p1;
    cv::Point2i p2;
    match(cv::Point2i ip1, cv::Point2i ip2)
    {
        this->p1 = ip1;
        this->p2 = ip2;
    }
    match()
    {
        this->p1 = cv::Point2i(0, 0);
        this->p2 = cv::Point2i(0, 0);
    }
    bool operator==(const match& right) const
    {
        return std::tie(this->p1, this->p2) == std::tie(right.p1, right.p2);
    }
    bool operator<(const match& right) const
    {
        return (this->p1.y < right.p1.y) ||
               ((this->p1.y < right.p1.y) && this->p1.x < right.p1.x);
    }
};
inline std::ostream& operator<<(std::ostream& os, const match& arg)
{
    os << "Point 1: (" << arg.p1.x << ", " << arg.p1.y << ")" << '\n'
       << "Point 2: (" << arg.p2.x << ", " << arg.p2.y << ")" << '\n';
    return os;
}
inline std::string to_string(match const& arg)
{
    std::ostringstream ss;
    ss << arg;
    return std::move(ss).str(); // enable efficiencies in c++17
}
struct angle {
    double cos;
    double sin;
    match corr_matches[2];
    angle(double cos, double sin, match m1, match m2)
    {
        this->cos = cos;
        this->sin = sin;
        this->corr_matches[0] = m1;
        this->corr_matches[1] = m2;
    }
};
} // namespace

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

SfmImgInfo* sfm_copy_info(SfmImgInfo* info) { return new SfmImgInfo{*info}; }

int sfm_keypoints_count(SfmImgInfo* info) { return info->keypoints.size(); }
unsigned char* sfm_serialize_binary(SfmImgInfo* info, int* outlen)
{
    const auto store_path = fs::temp_directory_path() / "sfm_tmp_store";
    cv::FileStorage store{store_path.string(),
                          cv::FileStorage::Mode::FORMAT_JSON |
                              cv::FileStorage::Mode::WRITE};
    const auto& desc = info->descriptors;
    store.write("d", desc);
    cv::write(store, "k", info->keypoints);

    store.release();
    std::ifstream inf{store_path};
    std::stringstream ss;
    ss << inf.rdbuf();
    const auto outs = ss.str();
    char* out = static_cast<char*>(calloc(outs.size() + 1, 1));
    strncpy(out, outs.c_str(), outs.size());

    inf.close();
    fs::remove(store_path);
    *outlen = outs.size();
    return reinterpret_cast<unsigned char*>(out);
}

SfmImgInfo* sfm_deserialize_binary(unsigned char* bytes, int len)
{
    const auto store_path = fs::temp_directory_path() / "sfm_tmp_store_write";
    std::ofstream tmpsf{store_path};
    tmpsf.write(reinterpret_cast<char*>(bytes), len);
    tmpsf.close();

    cv::FileStorage store{store_path.string(),
                          cv::FileStorage::Mode::FORMAT_JSON};

    auto info = std::make_unique<SfmImgInfo>();
    cv::read(store["d"], info->descriptors);
    cv::read(store["k"], info->keypoints);
    store.release();

    fs::remove(store_path);
    return info.release();
}

SfmImgInfo* sfm_extract(SfmPix* pix, int width, int height)
{
    cv::Mat img{height, width, CV_8UC1, pix};
    const auto roi = cv::Mat::ones(cv::Size{img.size[1], img.size[0]}, CV_8UC1);
    std::vector<cv::KeyPoint> pts;
    cv::Mat descs;
    cv::SIFT::create()->detectAndCompute(img, roi, pts, descs);
    //cv::imwrite("./finger-extract.png", img);

    auto* info = new SfmImgInfo{pts, descs};
    //*minutae = keypoints_to_fp_minutiae(pts);
    return info;
}

int sfm_match_score(SfmImgInfo* frame, SfmImgInfo* enrolled)
{
    std::vector<std::vector<cv::DMatch>> points;
    auto bfm = cv::BFMatcher::create();
    bfm->knnMatch(frame->descriptors, enrolled->descriptors, points, 2);
    std::set<match> matches_unique;
    int nb_matched = 0;
    for (const auto& pts : points) {
        const cv::DMatch& match_1 = pts.at(0);
        if (match_1.distance < distance_match * pts.at(1).distance) {
            matches_unique.emplace(
                match{frame->keypoints.at(match_1.queryIdx).pt,
                      enrolled->keypoints.at(match_1.trainIdx).pt});
            nb_matched++;
        }
    }
    if (nb_matched < min_match) {
        return 0;
    }
    std::vector<match> matches{matches_unique.begin(), matches_unique.end()};

    std::vector<angle> angles;
    for (int j = 0; j < matches.size(); j++) {
        match match_1 = matches[j];
        for (int k = j + 1; k < matches.size(); k++) {
            match match_2 = matches[k];

            int vec_1[2] = {match_1.p1.x - match_2.p1.x,
                            match_1.p1.y - match_2.p1.y};
            int vec_2[2] = {match_1.p2.x - match_2.p2.x,
                            match_1.p2.y - match_2.p2.y};

            double length_1 = sqrt(pow(vec_1[0], 2) + pow(vec_1[1], 2));
            double length_2 = sqrt(pow(vec_2[0], 2) + pow(vec_2[1], 2));

            if (1 - std::min(length_1, length_2) /
                        std::max(length_1, length_2) <=
                length_match) {

                double product = length_1 * length_2;
                angles.emplace_back(angle(
                    M_PI / 2 +
                        asin((vec_1[0] * vec_2[0] + vec_1[1] * vec_2[1]) /
                             product),
                    acos((vec_1[0] * vec_2[1] - vec_1[1] * vec_2[0]) / product),
                    match_1, match_2));
            }
        }
    }

    if (angles.size() < min_match) {
        return 0;
    }

    int count = 0;
    for (int j = 0; j < angles.size(); j++) {
        angle angle_1 = angles[j];
        for (int k = j + 1; k < angles.size(); k++) {
            angle angle_2 = angles[k];

            if (1 - std::min(angle_1.sin, angle_2.sin) /
                            std::max(angle_1.sin, angle_2.sin) <=
                    angle_match &&
                1 - std::min(angle_1.cos, angle_2.cos) /
                            std::max(angle_1.cos, angle_2.cos) <=
                    angle_match) {

                count += 1;
            }
        }
    }
    return count;
}

void sfm_free_info(SfmImgInfo* info) { delete info; }

void sfm_add_enroll_frame(SfmEnrollData* data, unsigned char* pix, int width,
                          int height)
{
    cv::Mat img{height, width, CV_8UC1, pix};
    cv::imwrite(data->img_path_base / "img.pgm", img);
}

void sfm_end_enroll(SfmEnrollData* data) { delete data; }
