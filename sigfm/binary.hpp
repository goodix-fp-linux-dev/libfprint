
#pragma once

#include "opencv2/core/mat.hpp"
#include <array>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace bin {
using byte = unsigned char;

class stream;

template<typename T>
struct serializer : public std::false_type {
    void serialize(const T& m, stream& out);
};

template<typename T>
struct deserializer : public std::false_type {
    T deserialize(stream& in);
};
class stream {
public:
    stream() = default;

    stream(const byte* begin, const byte* end) : view_{begin}, view_size_{static_cast<std::size_t>(end - begin)}
    {
    }

    template<
        typename Iter,
        std::enable_if_t<std::is_same_v<typename std::iterator_traits<
                                            std::decay_t<Iter>>::value_type,
                                        byte>,
                         bool> = true>
    stream(Iter begin, Iter end) : store_{begin, end}
    {
    }

    template<typename T, std::enable_if_t<serializer<T>::value, bool> = true>
    constexpr stream& operator<<(T v)
    {
        serializer<T>::serialize(v, *this);
        return *this;
    }

    template<typename T, std::enable_if_t<deserializer<T>::value, bool> = true>
    constexpr stream& operator>>(T& v)
    {
        v = deserializer<T>::deserialize(*this);
        return *this;
    }
    template<typename T, std::enable_if_t<std::is_trivial_v<T>, bool> = true>
    constexpr stream& operator<<(T v)
    {
        using seg_store = std::array<byte, sizeof(T)>;
        alignas(T) seg_store s = {};
        std::memcpy(s.data(), &v, sizeof(T));
        stream::write(s.begin(), s.end());
        return *this;
    }

    template<typename T, std::enable_if_t<std::is_trivial_v<T>, bool> = true>
    constexpr stream& operator>>(T& v)
    {
        using seg_store = std::array<byte, sizeof(T)>;
        alignas(T) seg_store s = {};
        if (size() < s.size()) {
            throw std::runtime_error{"tried to extract from too small stream"};
        }
        stream::read(s.begin(), s.end());
        memcpy(&v, s.data(), sizeof(T));
        return *this;
    }
    template<
        typename Iter,
        std::enable_if_t<std::is_same_v<typename std::iterator_traits<
                                            std::decay_t<Iter>>::value_type,
                                        byte>,
                         bool> = true>
    constexpr stream& write(Iter&& begin, Iter&& end)
    {
        compact();
        std::copy(std::forward<Iter>(begin), std::forward<Iter>(end),
                  std::back_inserter(store_));
        return *this;
    }

    template<typename T, std::enable_if_t<serializer<T>::value, bool> = true>
    stream& serialize(const T& m, stream& out)
    {
        serializer<T>::serialize(m, out);
        return out;
    }

    template<
        typename Iter,
        std::enable_if_t<std::is_same_v<typename std::iterator_traits<
                                            std::decay_t<Iter>>::value_type,
                                        byte>,
                         bool> = true>
    constexpr stream& read(Iter&& begin, Iter&& end)
    {
        const auto dist = std::distance(begin, end);
        return stream::read(begin, dist);
    }

    template<
        typename Iter,
        std::enable_if_t<std::is_same_v<typename std::iterator_traits<
                                            std::decay_t<Iter>>::value_type,
                                        byte>,
                         bool> = true>
    constexpr stream& read(Iter&& begin, std::size_t dist)
    {
        if (size() < dist) {
            throw std::runtime_error{"tried to read past end of stream"};
        }

        if (view_ != nullptr) {
            std::copy(view_ + pos_, view_ + pos_ + dist, begin);
        } else {
            std::copy(store_.begin() + pos_, store_.begin() + pos_ + dist, begin);
        }
        pos_ += dist;
        return *this;
    }
    byte* copy_buffer() const
    {
        const auto remaining = size();
        byte* raw = static_cast<byte*>(malloc(remaining));
        if (view_ != nullptr) {
            std::copy(view_ + pos_, view_ + view_size_, raw);
        } else {
            std::copy(store_.begin() + pos_, store_.end(), raw);
        }
        return raw;
    }
    std::size_t size() const
    {
        return view_ != nullptr ? view_size_ - pos_ : store_.size() - pos_;
    }

private:
    void compact()
    {
        if (view_ != nullptr) {
            store_.assign(view_ + pos_, view_ + view_size_);
            view_ = nullptr;
            view_size_ = 0;
            pos_ = 0;
            return;
        }

        if (pos_ == 0) {
            return;
        } else {
            store_.erase(store_.begin(), store_.begin() + pos_);
        }
        pos_ = 0;
    }

    std::vector<byte> store_;
    const byte* view_ = nullptr;
    std::size_t view_size_ = 0;
    std::size_t pos_ = 0;
};

template<>
struct serializer<cv::Mat> : public std::true_type {
    static void serialize(const cv::Mat& m, stream& out)
    {
        out << m.type() << m.rows << m.cols;
        out.write(m.datastart, m.dataend);
    }
};

template<>
struct deserializer<cv::Mat> : public std::true_type {
    static cv::Mat deserialize(stream& in)
    {
        int rows, cols, type;
        in >> type >> rows >> cols;
        cv::Mat m;
        m.create(rows, cols, type);
        in.read(m.data, std::distance(m.datastart, m.dataend));
        return m;
    }
};

template<typename T>
struct deserializer<cv::Point_<T>> : public std::true_type {
    static cv::Point2f deserialize(stream& in)
    {
        cv::Point_<T> p;
        in >> p.x >> p.y;
        return p;
    }
};
template<typename T>
struct serializer<cv::Point_<T>> : public std::true_type {
    static void serialize(const cv::Point_<T>& pt, stream& out)
    {
        out << pt.x << pt.y;
    }
};

template<>
struct serializer<cv::KeyPoint> : public std::true_type {
    static void serialize(const cv::KeyPoint& pt, stream& out)
    {
        out << pt.class_id << pt.angle << pt.octave << pt.response << pt.size
            << pt.pt;
    }
};

template<>
struct deserializer<cv::KeyPoint> : public std::true_type {
    static cv::KeyPoint deserialize(stream& in)
    {
        cv::KeyPoint pt;
        in >> pt.class_id >> pt.angle >> pt.octave >> pt.response >> pt.size >>
            pt.pt;
        return pt;
    }
};

template<typename T>
struct serializer<std::vector<T>> : public std::true_type {
    static void serialize(const std::vector<T>& vs, stream& out)
    {
        out << static_cast<std::size_t>(vs.size());
        std::for_each(vs.begin(), vs.end(),
                      [&out](const auto& el) { out << el; });
    }
};

template<typename T>
struct deserializer<std::vector<T>> : public std::true_type {
    static std::vector<T> deserialize(stream& in)
    {
        std::size_t size;
        in >> size;
        std::vector<T> vs;
        vs.reserve(size);
        for (std::size_t n = 0; n != size; ++n) {
            T v;
            in >> v;
            vs.emplace_back(std::move(v));
        }
        return vs;
    }
};
} // namespace bin
