// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_TRUNCATED_SEQ_H
#define MRT_TRUNCATED_SEQ_H

#include <cmath>
#include <cstddef>

namespace MapleRuntime {

// OpenJDK utilities/numberSeq.hpp:108-133 + numberSeq.cpp:139-244.
// Fixed-capacity ring; length is 100 for ZStatMutatorAllocRate (zStat.cpp:941-943).
class TruncatedSeq {
public:
    static constexpr int kMaxLength = 100;

    explicit TruncatedSeq(int length = kMaxLength) : length_(length < kMaxLength ? length : kMaxLength)
    {
        for (int i = 0; i < kMaxLength; ++i) {
            sequence_[i] = 0.0;
        }
    }

    void add(double val)
    {
        // numberSeq.cpp:150-169 — overwrite oldest, keep window sums.
        const double oldVal = sequence_[next_];
        sum_ -= oldVal;
        sumOfSquares_ -= oldVal * oldVal;
        sum_ += val;
        sumOfSquares_ += val * val;
        sequence_[next_] = val;
        next_ = (next_ + 1) % length_;
        if (num_ < length_) {
            ++num_;
        }
    }

    int num() const { return num_; }
    double sum() const { return sum_; }

    double avg() const
    {
        // numberSeq.cpp:55-59
        if (num_ == 0) {
            return 0.0;
        }
        return sum_ / static_cast<double>(num_);
    }

    double variance() const
    {
        // numberSeq.cpp:62-76
        if (num_ <= 1) {
            return 0.0;
        }
        const double xBar = avg();
        double result = sumOfSquares_ / static_cast<double>(num_) - xBar * xBar;
        if (result < 0.0) {
            result = 0.0;
        }
        return result;
    }

    double sd() const { return std::sqrt(variance()); }

    double predict_next() const
    {
        // numberSeq.cpp:206-244 — ordinary least-squares y = b0 + b1*x, x = 0..n-1.
        if (num_ == 0) {
            return 0.0;
        }
        if (num_ == 1) {
            return sequence_[0];
        }
        const double num = static_cast<double>(num_);
        double xSquaredSum = 0.0;
        double xSum = 0.0;
        double ySum = 0.0;
        double xySum = 0.0;
        const int first = (next_ + length_ - num_) % length_;
        for (int i = 0; i < num_; ++i) {
            const double x = static_cast<double>(i);
            const double y = sequence_[(first + i) % length_];
            xSquaredSum += x * x;
            xSum += x;
            ySum += y;
            xySum += x * y;
        }
        const double xAvg = xSum / num;
        const double yAvg = ySum / num;
        const double sxx = xSquaredSum - xSum * xSum / num;
        const double sxy = xySum - xSum * ySum / num;
        const double b1 = (sxx == 0.0) ? 0.0 : sxy / sxx;
        const double b0 = yAvg - b1 * xAvg;
        return b0 + b1 * num;
    }

    void reset()
    {
        num_ = 0;
        next_ = 0;
        sum_ = 0.0;
        sumOfSquares_ = 0.0;
        for (int i = 0; i < kMaxLength; ++i) {
            sequence_[i] = 0.0;
        }
    }

private:
    double sequence_[kMaxLength];
    int length_;
    int next_ = 0;
    int num_ = 0;
    double sum_ = 0.0;
    double sumOfSquares_ = 0.0;
};

} // namespace MapleRuntime
#endif // MRT_TRUNCATED_SEQ_H
