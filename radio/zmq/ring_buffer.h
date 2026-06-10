/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once
#include <vector>
#include <mutex>
#include <memory>
#include "common/platform_types.h"

// A basic cirular sample buffer class
class ring_buffer {
 private:
  std::unique_ptr<cf_t[]> buffer_;
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t size_ = 0;
  size_t max_size_;

 public:
  ring_buffer(size_t max_size = 614400);
  size_t push_samples(const cf_t *samples, size_t nsamps);
  size_t push_zeros(size_t num_zeros);
  size_t pop_samples(cf_t *samples, size_t num_samples);
  void reset();
  void clear_samples();
  size_t size() const;
};

// A thread-safe wrapper around ring_buffer that counts overflows
class overflow_buffer {
  ring_buffer buffer_;
  size_t zeros_to_send_;
  std::mutex mutex_;

 public:
  overflow_buffer(size_t max_size = 614400) : buffer_(max_size), zeros_to_send_(0)
  {
  }
  size_t push_samples(const cf_t *samples, size_t nsamps);
  size_t push_zeros(size_t num_zeros);
  size_t pop_samples(cf_t *samples, size_t num_samples);
  void reset();
  void clear_samples();
  size_t size();
};
