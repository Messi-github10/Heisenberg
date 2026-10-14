//
// Created by NiceFold on 2026/7/21.
//


#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <ostream>
#include <string>

extern "C" {
#include <libavutil/rational.h>
}

namespace heisenberg {

class Rational {
public:
    Rational()
        : num_(0)
        , den_(1) {}

    explicit Rational(int numerator)
        : num_(numerator)
        , den_(1) {}

    Rational(int numerator, int denominator)
        : num_(numerator)
        , den_(denominator) {
        fixSigns();
        reduce();
    }

    Rational(const Rational &rhs) = default;

    Rational(const AVRational &r)
        : num_(r.num)
        , den_(r.den) {
        fixSigns();
    }

    static Rational fromDouble(double value, bool *ok = nullptr) {
        if (ok) *ok = false;
        // av_d2q uses checked continued fractions with the correct initial
        // convergents. Never cast a non-finite or out-of-range double to int.
        if (!std::isfinite(value) ||
            std::abs(value) > std::numeric_limits<int>::max()) {
            return NaN;
        }
        const AVRational fraction = av_d2q(value, std::numeric_limits<int>::max());
        if (fraction.den <= 0) return NaN;
        const Rational result(fraction);
        const double error = std::abs(result.toDouble() - value);
        if (ok) *ok = error <= 1e-6 ||
            (value != 0.0 && error / std::abs(value) <= 1e-6);
        return result;
    }

    static Rational fromString(const std::string &str, bool *ok = nullptr) {
        if (ok) *ok = true;

        auto slashPos = str.find('/');
        if (slashPos != std::string::npos) {
            try {
                int num = std::stoi(str.substr(0, slashPos));
                int den = std::stoi(str.substr(slashPos + 1));
                if (den == 0) {
                    if (ok) *ok = false;
                    return NaN;
                }
                return Rational(num, den);
            } catch (...) {
                if (ok) *ok = false;
                return NaN;
            }
        }

        try {
            size_t pos = 0;
            double d = std::stod(str, &pos);
            if (pos != str.size()) {
                if (ok) *ok = false;
                return NaN;
            }
            return fromDouble(d, ok);
        } catch (...) {
            if (ok) *ok = false;
            return NaN;
        }
    }

    static const Rational NaN;

    Rational &operator=(const Rational &rhs) = default;

    Rational &operator+=(const Rational &rhs) {
        num_ = num_ * rhs.den_ + rhs.num_ * den_;
        den_ = den_ * rhs.den_;
        fixSigns();
        reduce();
        return *this;
    }

    Rational &operator-=(const Rational &rhs) {
        num_ = num_ * rhs.den_ - rhs.num_ * den_;
        den_ = den_ * rhs.den_;
        fixSigns();
        reduce();
        return *this;
    }

    Rational &operator*=(const Rational &rhs) {
        num_ *= rhs.num_;
        den_ *= rhs.den_;
        fixSigns();
        reduce();
        return *this;
    }

    Rational &operator/=(const Rational &rhs) {
        num_ *= rhs.den_;
        den_ *= rhs.num_;
        fixSigns();
        reduce();
        return *this;
    }

    Rational operator+(const Rational &rhs) const {
        Rational r(*this);
        r += rhs;
        return r;
    }

    Rational operator-(const Rational &rhs) const {
        Rational r(*this);
        r -= rhs;
        return r;
    }

    Rational operator*(const Rational &rhs) const {
        Rational r(*this);
        r *= rhs;
        return r;
    }

    Rational operator/(const Rational &rhs) const {
        Rational r(*this);
        r /= rhs;
        return r;
    }

    bool operator==(const Rational &rhs) const {
        if (isNaN() || rhs.isNaN()) return false;
        // Compare cross-multiplied to avoid reducing
        return (int64_t)num_ * rhs.den_ == (int64_t)rhs.num_ * den_;
    }

    bool operator!=(const Rational &rhs) const { 
        return !(*this == rhs);
    }

    bool operator<(const Rational &rhs) const {
        if (isNaN() || rhs.isNaN()) return false;
        return (int64_t)num_ * rhs.den_ < (int64_t)rhs.num_ * den_;
    }

    bool operator>(const Rational &rhs) const {
        if (isNaN() || rhs.isNaN()) return false;
        return (int64_t)num_ * rhs.den_ > (int64_t)rhs.num_ * den_;
    }

    bool operator<=(const Rational &rhs) const { return !(*this > rhs); }
    bool operator>=(const Rational &rhs) const { return !(*this < rhs); }

    const Rational &operator+() const { return *this; }

    Rational operator-() const
    {
        return Rational(-num_, den_);
    }

    bool operator!() const { return !num_; }

    double toDouble() const
    {
        if (isNaN()) return std::numeric_limits<double>::quiet_NaN();
        if (den_ == 0) return num_ >= 0
            ? std::numeric_limits<double>::infinity()
            : -std::numeric_limits<double>::infinity();
        return static_cast<double>(num_) / static_cast<double>(den_);
    }

    AVRational toAVRational() const
    {
        AVRational r;
        r.num = num_;
        r.den = den_;
        return r;
    }

    Rational flipped() const
    {
        return Rational(den_, num_);
    }

    void flip()
    {
        std::swap(num_, den_);
        fixSigns();
    }

    bool isNull() const { return num_ == 0; }

    bool isNaN() const { return den_ == 0; }

    int numerator() const { return num_; }
    int denominator() const { return den_; }

    std::string toString() const
    {
        if (isNaN()) return "NaN";
        if (den_ == 1) return std::to_string(num_);
        return std::to_string(num_) + '/' + std::to_string(den_);
    }

    friend std::ostream &operator<<(std::ostream &out, const Rational &value)
    {
        out << value.toString();
        return out;
    }

private:
    void fixSigns()
    {
        if (den_ < 0) {
            num_ = -num_;
            den_ = -den_;
        }
    }

    void reduce()
    {
        // A zero denominator is the invalid sentinel, including 0/0.
        if (den_ == 0) return;
        if (num_ == 0) {
            den_ = 1;
            return;
        }
        const int64_t g = std::gcd(static_cast<int64_t>(num_),
                                   static_cast<int64_t>(den_));
        num_ = static_cast<int>(num_ / g);
        den_ = static_cast<int>(den_ / g);
    }

    int num_;
    int den_;
};

inline const Rational Rational::NaN(0, 0);

} // namespace heisenberg
