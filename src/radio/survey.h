// SPDX-License-Identifier: GPL-2.0-only
// Channel survey (fpv::Radio::survey): what each 156.25 kHz of the band looks like, measured with a fixed gain
// so that every step of the sweep is on the same scale. The chip's own noise floor is not flat across the band
// at a fixed gain (on the RTL8812BU, ~8 dB higher at 5640 than at 5950 MHz), but it changes slowly: each step is
// measured against its own floor, but that floor may rise at most kSlopeDb over its neighbours' (passes up and
// down the band). A signal as wide as a whole step (a 40/80/160 MHz WiFi channel) raises that step's floor
// suddenly, so it still shows; it would not if each step were measured against its own floor alone.
//
// Every bin keeps a sliding average of about the last memory() sweeps (set_memory), so what changes in the band
// (a WiFi that turns on, a pilot that leaves) shows; the strongest burst fades 20 dB over that many sweeps.
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
  static constexpr int kMemory = 20;                    // default sweeps in the sliding average
  static constexpr double kPeakFadeDb = 20;             // the strongest burst fades this over memory() sweeps
  static constexpr double kDutyOver = 4;                // 6 dB over the floor counts as "on"
  static constexpr double kSlopeDb = 2;                 // the chip's floor changes at most this from step to step
  static constexpr double kFloorWeight = 0.25;          // sliding average of each step's floor

  void reset();
  void set_memory(int sweeps) { _memory = sweeps < 1 ? 1 : sweeps; }
  int memory() const { return _memory; }
  // Floor to judge levels by at a step (the step's own, capped by its neighbours'), 0 = not seen yet
  double reference(double centre_hz) const;
  void merge(const SurveyStep &s);
  void end_sweep() { _sweeps++; }
  int sweeps() const { return _sweeps; }
  std::vector<SurveyBin> spectrum() const;

 private:
  struct Acc {
    double mean = 0, peak = 0, duty = 0;
    int n = 0;
    long step = 0;  // the step that last saw it (kHz)
  };
  void update_refs();
  std::vector<Acc> _acc;
  std::map<long, double> _step_floor;  // by step centre (kHz)
  std::map<long, double> _step_ref;    // _step_floor capped by the neighbours (update_refs)
  int _sweeps = 0;
  int _memory = kMemory;
};

// One capture: 0.1 ms slices (16 FFTs of 256, Hann). Bins within 0.3 MHz of DC (the chip's LO leak) and
// outside +-edge_hz are left out. floor_hint: SurveyGrid::reference() (0 = use this step's own floor for duty).
SurveyStep survey_measure(const cfs *x, size_t n, double centre_hz, double fs, double edge_hz, double floor_hint);

}  // namespace fpv
