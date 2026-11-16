// SPDX-License-Identifier: GPL-2.0-only
// Channel survey (fpv::Radio::survey): what each 156.25 kHz of the band looks like, measured with a fixed gain
// so that every step of the sweep is on the same scale. Levels are given in dB over the noise floor of the
// quietest step: a signal as wide as a whole step (a 40/80/160 MHz WiFi channel) still shows, which it would
// not if each step were measured against its own floor.
//
// Every bin keeps a sliding average (about the last kMemory sweeps), so what changes in the band (a WiFi that
// turns on, a pilot that leaves) shows within a minute; the strongest burst fades kPeakDecayDb per sweep.
#pragma once
#include <complex>
#include <map>
#include <vector>

namespace fpv {

using cfs = std::complex<float>;

struct SurveyBin {
  double mhz = 0;      // centre of the bin
  float mean_db = 0;   // average power, dB over the floor
  float peak_db = 0;   // strongest 0.1 ms slice recently seen, dB over the floor (fades)
  float duty = 0;      // fraction of 0.1 ms slices more than 6 dB over the floor
};

// One step of the sweep, measured outside any lock (survey_measure), merged into the grid later
struct SurveyStep {
  double centre_hz = 0;
  double floor = 0;  // 10th percentile of the step's bins (linear, FFT units)
  struct Bin {
    long index;      // in the grid
    double mean;     // linear, FFT units
    double peak;
    float duty;
  };
  std::vector<Bin> bins;
};

class SurveyGrid {
 public:
  static constexpr double kLo = 5300e6, kBin = 40e6 / 256;
  static constexpr int kBins = 4480;                    // 5300..6000 MHz
  static constexpr int kMemory = 20;                    // sweeps in the sliding average (~40 s)
  static constexpr double kPeakDecayDb = 1;             // per sweep
  static constexpr double kDutyOver = 4;                // 6 dB over the floor counts as "on"

  void reset();
  // Floor to judge duty by: the quietest step seen so far, 0 = none yet
  double floor() const;
  void merge(const SurveyStep &s);
  void end_sweep() { _sweeps++; }
  int sweeps() const { return _sweeps; }
  std::vector<SurveyBin> spectrum() const;

 private:
  struct Acc {
    double mean = 0, peak = 0, duty = 0;
    int n = 0;
  };
  std::vector<Acc> _acc;
  std::map<long, double> _step_floor;  // by step centre (kHz)
  int _sweeps = 0;
};

// One capture: 0.1 ms slices (16 FFTs of 256, Hann). Bins within 0.3 MHz of DC (the chip's LO leak) and
// outside +-edge_hz are left out. floor_hint: SurveyGrid::floor() (0 = use this step's own floor for duty).
SurveyStep survey_measure(const cfs *x, size_t n, double centre_hz, double fs, double edge_hz, double floor_hint);

}  // namespace fpv
