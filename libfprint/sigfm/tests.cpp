#include "opencv2/core.hpp"
#include "opencv2/core/types.hpp"
#include "sigfm.hpp"
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "binary.hpp"
#include "tests-embedded.hpp"

#include "img-info.hpp"
#include <opencv2/opencv.hpp>

namespace cv {
bool operator==(const cv::KeyPoint& lhs, const cv::KeyPoint& rhs)
{
    return lhs.angle == rhs.angle && lhs.class_id == rhs.class_id &&
           lhs.octave == rhs.octave && lhs.size == rhs.size &&
           lhs.response == rhs.response && lhs.pt == rhs.pt;
}

} // namespace cv

namespace {
bool comp_mats(const cv::Mat& lhs, const cv::Mat& rhs)
{
    return std::equal(lhs.datastart, lhs.dataend, rhs.datastart, rhs.dataend);
}

std::string to_str(const cv::KeyPoint& k)
{
    std::stringstream s;
    s << "angle: " << k.angle << ", class_id: " << k.class_id
      << ", octave: " << k.octave << ", size: " << k.size
      << ", reponse: " << k.response << ", ptx: " << k.pt.x
      << ", pty: " << k.pt.y;
    return s.str();
}

} // namespace

template<typename T>
void check_vec(const std::vector<T>& vs)
{
    for (auto i : vs) {
        bin::stream s;
        s << i;
        T iv;
        s >> iv;
        CHECK(i == iv);
    }
}

TEST_SUITE("binary")
{

    TEST_CASE("float can be stored and restored")
    {
        check_vec<float>({3, 2.4, 6.7});
    }

    TEST_CASE("size_t can be stored and restored")
    {
        check_vec<std::size_t>({2, 5, 803, 900});
    }
    TEST_CASE("number can be stored and restored")
    {
        check_vec<int>({5, 3, 10, 16, 24, 900});
    }
    TEST_CASE("image can be stored and restored")
    {
        cv::Mat input;
        input.create(256, 256, CV_8UC1);
        std::memcpy(input.data, embedded::capture_aes3500, 256 * 256);
        bin::stream s;
        s << input;

        cv::Mat output;
        s >> output;
        CHECK(std::equal(input.datastart, input.dataend, output.datastart,
                         output.dataend));
    }

    TEST_CASE("vector of values can be stored and restored")
    {
        std::vector inputs = {3, 5, 1, 7};
        bin::stream s;
        s << inputs;

        std::vector<int> outputs;
        s >> outputs;
        CHECK(outputs == inputs);
    }

    TEST_CASE("keypoints can be stored and restored")
    {
        cv::KeyPoint pt;
        pt.angle = 20;
        pt.octave = 3;
        pt.response = 3;
        pt.size = 40;
        pt.pt = cv::Point2f{3, 1};

        bin::stream s;
        s << pt;

        cv::KeyPoint ptout;
        s >> ptout;
        CHECK(to_str(pt) == to_str(ptout));
    }
    TEST_CASE("sfm img info can be stored and restored")
    {
        constexpr auto img_w = 256;
        constexpr auto img_h = 256;
        constexpr auto img = embedded::capture_aes3500;
        SfmImgInfo* info = sfm_extract(img, img_w, img_h);
        REQUIRE(info != nullptr);
        const auto inf1desc = info->descriptors;
        cv::Mat descout;
        bin::stream s;
        s << inf1desc;
        s >> descout;
        CHECK(comp_mats(inf1desc, descout));

        int slen;
        const auto bin_data = sfm_serialize_binary(info, &slen);
        int slen2;
        SfmImgInfo* info2 = sfm_deserialize_binary(bin_data, slen);
        REQUIRE(info2);
        const auto bin_data2 = sfm_serialize_binary(info2, &slen2);
        CHECK(slen == slen2);
        CHECK(std::equal(bin_data, bin_data + slen, bin_data2,
                         bin_data2 + slen2));

        REQUIRE(info->keypoints == info2->keypoints);
        REQUIRE(std::equal(
            info->descriptors.datastart, info->descriptors.dataend,
            info2->descriptors.datastart, info2->descriptors.dataend));
        sfm_free_info(info);
        sfm_free_info(info2);
    }
}
