#pragma once

#include <array>
#include <algorithm>
#include <cstddef>

// Fixed-size rolling history buffer (ring buffer) for doubles.
// - Push() overwrites the oldest sample when full.
// - GetOldestToNewest(i) returns samples in time order.

template <size_t N>
struct RingBufferD
{
	static_assert(N > 1);

	void Clear() noexcept { head_ = 0; count_ = 0; }

	void Push(double v) noexcept
	{
		data_[head_] = v;
		head_ = (head_ + 1) % N;
		if(count_ < N) count_++;
	}

	size_t Count() const noexcept { return count_; }
	constexpr size_t Capacity() const noexcept { return N; }

	double GetOldestToNewest(size_t i) const noexcept
	{
		// i in [0, count_-1]
		if(count_ == 0) return 0.0;
		const auto oldest = (head_ + N - count_) % N;
		return data_[(oldest + i) % N];
	}

	void MinMax(double& outMin, double& outMax) const noexcept
	{
		if(count_ == 0) { outMin = 0.0; outMax = 0.0; return; }
		auto mn = GetOldestToNewest(0);
		auto mx = mn;
		for(size_t i = 1; i < count_; i++)
		{
			auto v = GetOldestToNewest(i);
			if(v < mn) mn = v;
			if(v > mx) mx = v;
		}
		outMin = mn;
		outMax = mx;
	}

private:
	std::array<double, N> data_{};
	size_t head_ = 0;
	size_t count_ = 0;
};
