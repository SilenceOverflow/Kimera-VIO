/* ----------------------------------------------------------------------------
 * Copyright 2017, Massachusetts Institute of Technology,
 * Cambridge, MA 02139
 * All Rights Reserved
 * Authors: Luca Carlone, et al. (see THANKS for the full author list)
 * See LICENSE for the license information
 * -------------------------------------------------------------------------- */

/**
 * @file   testTemporalCalibration.cpp
 * @brief  Unit tests for time alignment
 * @author Nathan Hughes
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "kimera-vio/frontend/MonoVisionImuFrontend-definitions.h"
#include "kimera-vio/frontend/StereoVisionImuFrontend-definitions.h"
#include "kimera-vio/frontend/Tracker-definitions.h"
#include "kimera-vio/initial/CrossCorrTimeAligner.h"
#include "kimera-vio/initial/TimeAlignerBase.h"

#include <Eigen/Dense>
#include <vector>

namespace VIO {

using ::testing::_;
using ::testing::AllArgs;
using ::testing::Invoke;
using ::testing::Ne;
using ::testing::NotNull;

typedef std::pair<TrackingStatus, gtsam::Pose3> RansacResult;

class MockTracker : public Tracker {
 public:
  MockTracker()
      : Tracker(FrontendParams(), std::make_shared<Camera>(CameraParams())) {}

  ~MockTracker() = default;

  MOCK_METHOD(RansacResult,
              geometricOutlierRejectionMono,
              (Frame*, Frame*),
              (override));
};

class ReturnHelper {
 public:
  ReturnHelper(const std::vector<RansacResult>& values) : vec_(values) {
    vec_iter_ = vec_.begin();
  }

  RansacResult getNext(Frame* /* ref */, Frame* /* curr */) {
    if (vec_iter_ == vec_.end()) {
      return std::make_pair(TrackingStatus::INVALID, gtsam::Pose3());
    }
    RansacResult to_return = *vec_iter_;
    ++vec_iter_;
    return to_return;
  }

 private:
  std::vector<RansacResult> vec_;
  std::vector<RansacResult>::const_iterator vec_iter_;
};

FrontendOutputPacketBase::Ptr makeOutput(
    Timestamp timestamp,
    FrontendType frontend_type = FrontendType::kStereoImu) {
  Frame fake_frame(1, timestamp, CameraParams(), cv::Mat());
  if (frontend_type == FrontendType::kMonoImu) {
    return std::make_shared<MonoFrontendOutput>(
        false,
        StatusMonoMeasurementsPtr(nullptr),
        TrackingStatus::VALID,
        gtsam::Pose3(),
        gtsam::Pose3(),
        fake_frame,
        ImuFrontend::PimPtr(nullptr),
        ImuAccGyrS(6, 1),
        cv::Mat(),
        DebugTrackerInfo());
  } else {
    StereoFrame fake_stereo(
        fake_frame.id_, fake_frame.timestamp_, fake_frame, fake_frame);
    return std::make_shared<StereoFrontendOutput>(
        false,
        StatusStereoMeasurementsPtr(nullptr),
        TrackingStatus::VALID,
        gtsam::Pose3(),
        gtsam::Pose3(),
        gtsam::Pose3(),
        fake_stereo,
        ImuFrontend::PimPtr(nullptr),
        ImuAccGyrS(6, 1),
        cv::Mat(),
        DebugTrackerInfo());
  }
}

struct TestData {
  ImuParams params;
  std::vector<RansacResult> results;
  std::vector<FrontendOutputPacketBase::Ptr> outputs;
  std::vector<ImuStampS> imu_stamps;
  std::vector<ImuAccGyrS> imu_values;
  double expected_delay = 0.0;
};

void addFirstFrame(TestData& data) {
  ImuStampS times = ImuStampS::Zero(1, 1);
  ImuAccGyrS values = ImuAccGyrS::Zero(6, 1);

  data.outputs.push_back(makeOutput(0));
  data.imu_stamps.push_back(times);
  data.imu_values.push_back(values);
}

struct SignalData {
  std::vector<Timestamp> vision_times;
  std::vector<double> vision_angles;
  std::vector<Timestamp> imu_times;
  std::vector<double> imu_angles;
};

SignalData generateSignal(int num_frames,
                          int num_imu_per,
                          int num_delay,
                          double rotation_scale,
                          double imu_period_s) {
  SignalData signal;
  if (num_delay < 0) {
    for (int i = 0; i < std::abs(num_delay); ++i) {
      signal.imu_angles.push_back(0.0);
      signal.imu_times.push_back(i);
    }
  } else {
    signal.imu_angles.push_back(0.0);
    signal.imu_times.push_back(0);
  }

  double prev_angle = 0.0;
  for (size_t i = 1; i <= num_frames; ++i) {
    double angle;  // rotation angle for image
    if (i <= num_frames / 2) {
      angle = rotation_scale * i;
    } else {
      angle = rotation_scale * (num_frames - i);
    }

    signal.vision_times.push_back(i * num_imu_per);
    signal.vision_angles.push_back(angle);

    double value_diff = angle - prev_angle;
    for (size_t k = 1; k <= num_imu_per; ++k) {
      double ratio = k / static_cast<double>(num_imu_per);
      double imu_angle = (ratio * value_diff + prev_angle) / imu_period_s;

      signal.imu_times.push_back(signal.imu_times.back() + 1);
      signal.imu_angles.push_back(imu_angle);
    }

    prev_angle = angle;
  }

  if (num_delay > 0) {
    for (int i = 0; i < num_delay; ++i) {
      signal.imu_angles.push_back(0.0);
      signal.imu_times.push_back(signal.imu_times.back() + 1);
    }
  }

  return signal;
}

TestData makeTestData(size_t num_frames = 10,
                      size_t num_imu_per = 5,
                      double rotation_scale = 0.1,
                      bool imu_rate = true,
                      int num_delay = 0) {
  TestData to_return;
  // set up some important data
  to_return.params.gyro_noise_density_ = 0.0;
  to_return.params.do_imu_rate_time_alignment_ = imu_rate;
  to_return.params.time_alignment_window_size_ =
      imu_rate ? num_frames * num_imu_per : num_frames;
  to_return.params.nominal_sampling_time_s_ = 1.0e-9;

  // correlation should ideally produce this
  if (imu_rate) {
    int delay_ns = num_delay - std::copysign(1, num_delay);
    to_return.expected_delay =
        to_return.params.nominal_sampling_time_s_ * delay_ns;
  } else {
    double imu_multiplier = static_cast<double>(num_imu_per);
    int delay_periods = std::round(num_delay / imu_multiplier);
    to_return.expected_delay = to_return.params.nominal_sampling_time_s_ *
                               imu_multiplier * delay_periods;
  }

  // add the first frame used to start the process
  addFirstFrame(to_return);

  SignalData signal = generateSignal(num_frames,
                                     num_imu_per,
                                     num_delay,
                                     rotation_scale,
                                     to_return.params.nominal_sampling_time_s_);
  for (size_t i = 0; i < signal.vision_angles.size(); ++i) {
    // this is actually a different axis, but the transform doesn't matter
    gtsam::Pose3 pose(gtsam::Rot3::Rz(signal.vision_angles[i]),
                      Eigen::Vector3d::Zero());
    to_return.results.emplace_back(TrackingStatus::VALID, pose);
    to_return.outputs.emplace_back(makeOutput(signal.vision_times[i]));
  }

  Timestamp first_imu_time =
      num_delay > 0 ? signal.imu_times[num_delay] : signal.imu_times.front();

  for (size_t i = 0; i < num_frames; ++i) {
    ImuStampS times = ImuStampS::Zero(1, num_imu_per + 1);
    ImuAccGyrS values = ImuAccGyrS::Zero(6, num_imu_per + 1);

    size_t offset =
        num_delay > 0 ? num_imu_per * i + num_delay : num_imu_per * i;
    for (size_t k = 0; k <= num_imu_per; ++k) {
      times(0, k) = signal.imu_times[k + offset] - first_imu_time;
      values(3, k) = signal.imu_angles[k + offset];
    }

    to_return.imu_stamps.emplace_back(times);
    to_return.imu_values.emplace_back(values);
  }

  return to_return;
}

TEST(temporalCalibration, testBadRansacStatus) {
  MockTracker tracker;

  std::vector<RansacResult> results;
  results.emplace_back(TrackingStatus::INVALID, gtsam::Pose3());
  results.emplace_back(TrackingStatus::DISABLED, gtsam::Pose3());

  ReturnHelper helper(results);
  EXPECT_CALL(tracker, geometricOutlierRejectionMono(NotNull(), NotNull()))
      .With(AllArgs(Ne()))
      .Times(2)
      .WillRepeatedly(Invoke(&helper, &ReturnHelper::getNext));

  ImuParams params;
  CrossCorrTimeAligner aligner(params);

  FrontendOutputPacketBase::Ptr output = makeOutput(1);
  ImuStampS times(1, 0);
  ImuAccGyrS values(6, 0);

  // Set initial frame
  TimeAlignerBase::Result result =
      aligner.estimateTimeAlignment(tracker, *output, times, values);
  EXPECT_FALSE(result.valid);
  EXPECT_EQ(0.0, result.imu_time_shift);

  // Time alignment "succeeds" if RANSAC is invalid (first result)
  result = aligner.estimateTimeAlignment(tracker, *output, times, values);
  EXPECT_TRUE(result.valid);
  EXPECT_EQ(0.0, result.imu_time_shift);

  // Time alignment "succeeds" if 5pt RANSAC is disabled (second result)
  result = aligner.estimateTimeAlignment(tracker, *output, times, values);
  EXPECT_TRUE(result.valid);
  EXPECT_EQ(0.0, result.imu_time_shift);
}

TEST(temporalCalibration, testEmptyImu) {
  MockTracker tracker;

  std::vector<RansacResult> results;
  results.emplace_back(TrackingStatus::VALID, gtsam::Pose3());
  results.emplace_back(TrackingStatus::VALID, gtsam::Pose3());
  results.emplace_back(TrackingStatus::VALID, gtsam::Pose3());

  ReturnHelper helper(results);
  EXPECT_CALL(tracker, geometricOutlierRejectionMono(NotNull(), NotNull()))
      .With(AllArgs(Ne()))
      .Times(1)
      .WillRepeatedly(Invoke(&helper, &ReturnHelper::getNext));

  ImuParams params;
  CrossCorrTimeAligner aligner(params);

  FrontendOutputPacketBase::Ptr output = makeOutput(1);
  ImuStampS times(1, 0);
  ImuAccGyrS values(6, 0);

  // Set initial frame
  TimeAlignerBase::Result result =
      aligner.estimateTimeAlignment(tracker, *output, times, values);
  EXPECT_FALSE(result.valid);
  EXPECT_EQ(0.0, result.imu_time_shift);

  // Time alignment "succeeds" if the IMU isn't present between frames
  result = aligner.estimateTimeAlignment(tracker, *output, times, values);
  EXPECT_TRUE(result.valid);
  EXPECT_EQ(0.0, result.imu_time_shift);
}

TEST(temporalCalibration, testLessThanWindow) {
  MockTracker tracker;

  std::vector<RansacResult> results;
  results.emplace_back(TrackingStatus::VALID, gtsam::Pose3());
  results.emplace_back(TrackingStatus::VALID, gtsam::Pose3());
  results.emplace_back(TrackingStatus::VALID, gtsam::Pose3());

  ReturnHelper helper(results);
  EXPECT_CALL(tracker, geometricOutlierRejectionMono(NotNull(), NotNull()))
      .With(AllArgs(Ne()))
      .Times(results.size())
      .WillRepeatedly(Invoke(&helper, &ReturnHelper::getNext));

  ImuParams params;
  params.time_alignment_window_size_ = 10;
  CrossCorrTimeAligner aligner(params);

  for (size_t i = 0; i <= results.size(); ++i) {
    FrontendOutputPacketBase::Ptr output = makeOutput(i);
    ImuStampS times(1, 1);
    times << i;
    ImuAccGyrS values(6, 1);

    TimeAlignerBase::Result result =
        aligner.estimateTimeAlignment(tracker, *output, times, values);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(0.0, result.imu_time_shift);
  }
}

TEST(temporalCalibration, testLessThanWindowFrameRate) {
  MockTracker tracker;

  std::vector<RansacResult> results;
  results.emplace_back(TrackingStatus::VALID, gtsam::Pose3());
  results.emplace_back(TrackingStatus::VALID, gtsam::Pose3());
  results.emplace_back(TrackingStatus::VALID, gtsam::Pose3());

  ReturnHelper helper(results);
  EXPECT_CALL(tracker, geometricOutlierRejectionMono(NotNull(), NotNull()))
      .With(AllArgs(Ne()))
      .Times(results.size())
      .WillRepeatedly(Invoke(&helper, &ReturnHelper::getNext));

  ImuParams params;
  params.time_alignment_window_size_ = 10;
  params.do_imu_rate_time_alignment_ = false;
  CrossCorrTimeAligner aligner(params);

  for (size_t i = 0; i <= results.size(); ++i) {
    FrontendOutputPacketBase::Ptr output = makeOutput(i);
    ImuStampS times(1, 1);
    times << i;
    ImuAccGyrS values(6, 1);

    TimeAlignerBase::Result result =
        aligner.estimateTimeAlignment(tracker, *output, times, values);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(0.0, result.imu_time_shift);
  }
}

TEST(temporalCalibration, testLowVariance) {
  MockTracker tracker;

  ImuParams params;
  params.gyro_noise_density_ = 1.0;
  params.time_alignment_window_size_ = 3;
  params.do_imu_rate_time_alignment_ = false;
  CrossCorrTimeAligner aligner(params);

  std::vector<RansacResult> results;
  for (size_t i = 0; i < params.time_alignment_window_size_; ++i) {
    results.emplace_back(TrackingStatus::VALID, gtsam::Pose3());
  }

  ReturnHelper helper(results);
  EXPECT_CALL(tracker, geometricOutlierRejectionMono(NotNull(), NotNull()))
      .With(AllArgs(Ne()))
      .Times(results.size())
      .WillRepeatedly(Invoke(&helper, &ReturnHelper::getNext));

  for (size_t i = 0; i <= results.size(); ++i) {
    FrontendOutputPacketBase::Ptr output = makeOutput(i);
    ImuStampS times(1, 1);
    times << i;
    ImuAccGyrS values = ImuAccGyrS::Zero(6, 1);

    // We get false either from not having enough data or not having enough
    // variance
    TimeAlignerBase::Result result =
        aligner.estimateTimeAlignment(tracker, *output, times, values);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(0.0, result.imu_time_shift);
  }
}

TEST(temporalCalibration, testEnoughVariance) {
  MockTracker tracker;

  ImuParams params;
  params.gyro_noise_density_ = 0.0;
  params.time_alignment_window_size_ = 3;
  params.do_imu_rate_time_alignment_ = false;
  CrossCorrTimeAligner aligner(params);

  std::vector<RansacResult> results;
  for (size_t i = 0; i < params.time_alignment_window_size_; ++i) {
    results.emplace_back(TrackingStatus::VALID, gtsam::Pose3());
  }

  ReturnHelper helper(results);
  EXPECT_CALL(tracker, geometricOutlierRejectionMono(NotNull(), NotNull()))
      .With(AllArgs(Ne()))
      .Times(results.size())
      .WillRepeatedly(Invoke(&helper, &ReturnHelper::getNext));

  for (size_t i = 0; i <= results.size(); ++i) {
    FrontendOutputPacketBase::Ptr output = makeOutput(i);
    ImuStampS times(1, 1);
    times << i;
    ImuAccGyrS values = ImuAccGyrS::Zero(6, 1);

    TimeAlignerBase::Result result =
        aligner.estimateTimeAlignment(tracker, *output, times, values);
    if (i < results.size()) {
      // We get false from not having enough data
      EXPECT_FALSE(result.valid);
      EXPECT_EQ(0.0, result.imu_time_shift);
    } else {
      EXPECT_TRUE(result.valid);
      // result needs to be within the min and max possible time
      EXPECT_GE(UtilsNumerical::NsecToSec(results.size() - 1),
                result.imu_time_shift);
      EXPECT_LE(UtilsNumerical::NsecToSec(-results.size() + 1),
                result.imu_time_shift);
    }
  }
}

TEST(temporalCalibration, testWellFormedNoDelay) {
  TestData data = makeTestData(10, 1, 0.1, true);

  MockTracker tracker;
  // handle the extra IMU measurement at the start
  data.params.time_alignment_window_size_ += 1;
  CrossCorrTimeAligner aligner(data.params);

  ReturnHelper helper(data.results);
  EXPECT_CALL(tracker, geometricOutlierRejectionMono(NotNull(), NotNull()))
      .With(AllArgs(Ne()))
      .Times(data.results.size())
      .WillRepeatedly(Invoke(&helper, &ReturnHelper::getNext));

  TimeAlignerBase::Result result;
  for (size_t i = 0; i < data.results.size(); ++i) {
    result = aligner.estimateTimeAlignment(
        tracker, *(data.outputs[i]), data.imu_stamps[i], data.imu_values[i]);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(0.0, result.imu_time_shift);
  }

  result = aligner.estimateTimeAlignment(tracker,
                                         *(data.outputs.back()),
                                         data.imu_stamps.back(),
                                         data.imu_values.back());
  EXPECT_TRUE(result.valid);
  EXPECT_EQ(0.0, result.imu_time_shift);
}

TEST(temporalCalibration, testWellFormedMultiImuNoDelayImuRate) {
  TestData data = makeTestData(10, 5, 0.1, true);

  MockTracker tracker;
  CrossCorrTimeAligner aligner(data.params);

  ReturnHelper helper(data.results);
  EXPECT_CALL(tracker, geometricOutlierRejectionMono(NotNull(), NotNull()))
      .With(AllArgs(Ne()))
      .Times(data.results.size())
      .WillRepeatedly(Invoke(&helper, &ReturnHelper::getNext));

  TimeAlignerBase::Result result;
  for (size_t i = 0; i < data.results.size(); ++i) {
    result = aligner.estimateTimeAlignment(
        tracker, *(data.outputs[i]), data.imu_stamps[i], data.imu_values[i]);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(0.0, result.imu_time_shift);
  }

  result = aligner.estimateTimeAlignment(tracker,
                                         *(data.outputs.back()),
                                         data.imu_stamps.back(),
                                         data.imu_values.back());
  EXPECT_TRUE(result.valid);
  EXPECT_EQ(0.0, result.imu_time_shift);
}

TEST(temporalCalibration, testWellFormedMultiImuNoDelayFrameRate) {
  TestData data = makeTestData(10, 5, 0.1, false);

  MockTracker tracker;
  CrossCorrTimeAligner aligner(data.params);

  ReturnHelper helper(data.results);
  EXPECT_CALL(tracker, geometricOutlierRejectionMono(NotNull(), NotNull()))
      .With(AllArgs(Ne()))
      .Times(data.results.size())
      .WillRepeatedly(Invoke(&helper, &ReturnHelper::getNext));

  TimeAlignerBase::Result result;
  for (size_t i = 0; i < data.results.size(); ++i) {
    result = aligner.estimateTimeAlignment(
        tracker, *(data.outputs[i]), data.imu_stamps[i], data.imu_values[i]);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(0.0, result.imu_time_shift);
  }

  result = aligner.estimateTimeAlignment(tracker,
                                         *(data.outputs.back()),
                                         data.imu_stamps.back(),
                                         data.imu_values.back());
  EXPECT_TRUE(result.valid);
  EXPECT_EQ(0.0, result.imu_time_shift);
}

TEST(temporalCalibration, testNegDelayImuRate) {
  TestData data = makeTestData(10, 5, 0.1, true, -8);

  MockTracker tracker;
  CrossCorrTimeAligner aligner(data.params);

  ReturnHelper helper(data.results);
  EXPECT_CALL(tracker, geometricOutlierRejectionMono(NotNull(), NotNull()))
      .With(AllArgs(Ne()))
      .Times(data.results.size())
      .WillRepeatedly(Invoke(&helper, &ReturnHelper::getNext));

  TimeAlignerBase::Result result;
  for (size_t i = 0; i < data.results.size(); ++i) {
    result = aligner.estimateTimeAlignment(
        tracker, *(data.outputs[i]), data.imu_stamps[i], data.imu_values[i]);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(0.0, result.imu_time_shift);
  }

  result = aligner.estimateTimeAlignment(tracker,
                                         *(data.outputs.back()),
                                         data.imu_stamps.back(),
                                         data.imu_values.back());
  EXPECT_TRUE(result.valid);
  EXPECT_EQ(data.expected_delay, result.imu_time_shift);
}

TEST(temporalCalibration, testPosDelayImuRate) {
  TestData data = makeTestData(10, 5, 0.1, true, 7);

  MockTracker tracker;
  CrossCorrTimeAligner aligner(data.params);

  ReturnHelper helper(data.results);
  EXPECT_CALL(tracker, geometricOutlierRejectionMono(NotNull(), NotNull()))
      .With(AllArgs(Ne()))
      .Times(data.results.size())
      .WillRepeatedly(Invoke(&helper, &ReturnHelper::getNext));

  TimeAlignerBase::Result result;
  for (size_t i = 0; i < data.results.size(); ++i) {
    result = aligner.estimateTimeAlignment(
        tracker, *(data.outputs[i]), data.imu_stamps[i], data.imu_values[i]);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(0.0, result.imu_time_shift);
  }

  result = aligner.estimateTimeAlignment(tracker,
                                         *(data.outputs.back()),
                                         data.imu_stamps.back(),
                                         data.imu_values.back());
  EXPECT_TRUE(result.valid);
  EXPECT_EQ(data.expected_delay, result.imu_time_shift);
}

TEST(temporalCalibration, testNegDelayFrameRate) {
  TestData data = makeTestData(10, 5, 0.1, false, -8);

  MockTracker tracker;
  CrossCorrTimeAligner aligner(data.params);

  ReturnHelper helper(data.results);
  EXPECT_CALL(tracker, geometricOutlierRejectionMono(NotNull(), NotNull()))
      .With(AllArgs(Ne()))
      .Times(data.results.size())
      .WillRepeatedly(Invoke(&helper, &ReturnHelper::getNext));

  TimeAlignerBase::Result result;
  for (size_t i = 0; i < data.results.size(); ++i) {
    result = aligner.estimateTimeAlignment(
        tracker, *(data.outputs[i]), data.imu_stamps[i], data.imu_values[i]);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(0.0, result.imu_time_shift);
  }

  result = aligner.estimateTimeAlignment(tracker,
                                         *(data.outputs.back()),
                                         data.imu_stamps.back(),
                                         data.imu_values.back());
  EXPECT_TRUE(result.valid);
  EXPECT_EQ(data.expected_delay, result.imu_time_shift);
}

TEST(temporalCalibration, testPosDelayFrameRate) {
  TestData data = makeTestData(10, 5, 0.1, false, 7);

  MockTracker tracker;
  CrossCorrTimeAligner aligner(data.params);

  ReturnHelper helper(data.results);
  EXPECT_CALL(tracker, geometricOutlierRejectionMono(NotNull(), NotNull()))
      .With(AllArgs(Ne()))
      .Times(data.results.size())
      .WillRepeatedly(Invoke(&helper, &ReturnHelper::getNext));

  TimeAlignerBase::Result result;
  for (size_t i = 0; i < data.results.size(); ++i) {
    result = aligner.estimateTimeAlignment(
        tracker, *(data.outputs[i]), data.imu_stamps[i], data.imu_values[i]);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(0.0, result.imu_time_shift);
  }

  result = aligner.estimateTimeAlignment(tracker,
                                         *(data.outputs.back()),
                                         data.imu_stamps.back(),
                                         data.imu_values.back());
  EXPECT_TRUE(result.valid);
  EXPECT_EQ(data.expected_delay, result.imu_time_shift);
}

}  // namespace VIO
