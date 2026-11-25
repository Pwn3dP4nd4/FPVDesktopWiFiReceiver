// SPDX-License-Identifier: GPL-2.0-only
// Channel survey (fpv::Radio::survey): what each 156.25 kHz of the band looks like, measured with a fixed gain
// so that every step of the sweep is on the same scale. The chip's own noise floor is not flat across the band
// at a fixed gain (on the RTL8812BU, ~8 dB higher at 5640 than at 5950 MHz), but it changes slowly: each step is
// measured against its own floor, but that floor may rise at most kSlopeDbPerMhz over its neighbours' (passes up
// and down the band). A signal as wide as a whole step (a 40/80/160 MHz WiFi channel) raises that step's floor
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
  unsigned seq = 0;    // SurveyGrid::merges() when this bin was last measured (which part of the band is fresh)
};

// One step of the sweep, measured outside any lock (survey_measure), merged into the grid later
struct SurveyStep {
  double centre_hz = 0;
  double floor = 0;  // 10th percentile of the step's bins (linear, FFT units)
  float clip = 0;    // fraction of samples at the ADC's full scale (a strong signal close by: the gain is too high)
  float peak_abs = 0;  // largest |I| or |Q|, as a fraction of full scale
  struct Bin {
    long index;      // in the grid
    double mean;     // linear, FFT units
    double peak;
    float duty;
  };
  std::vector<Bin> bins;
};

// Values measured at a gain g dB below the survey's: power x 10^(g/10), so every step is on the same scale
void survey_scale(SurveyStep &s, double db);

class SurveyGrid {
 public:
  static constexpr double kLo = 5300e6, kBin = 40e6 / 256;
  static constexpr int kBins = 4480;                    // 5300..6000 MHz
  static constexpr int kMemory = 20;                    // default sweeps in the sliding average
  static constexpr double kPeakFadeDb = 20;             // the strongest burst fades this over memory() sweeps
  static constexpr double kDutyOver = 4;                // 6 dB over the floor counts as "on"
  static constexpr double kSlopeDbPerMhz = 0.065;       // the chip's floor changes at most this (measured ~0.026)
  static constexpr double kFloorWeight = 0.25;          // sliding average of each step's floor

  void reset();
  void set_memory(int sweeps) { _memory = sweeps < 1 ? 1 : sweeps; }
  int memory() const { return _memory; }
  // Floor to judge levels by at a step (the step's own, capped by its neighbours'), 0 = not seen yet
  double reference(double centre_hz) const;
  void merge(const SurveyStep &s);
  void end_sweep() { _sweeps++; }
  int sweeps() const { return _sweeps; }
  // Steps merged so far (grows by one each merge): a bin's SurveyBin::seq says how recently it was measured
  unsigned merges() const { return _merges; }
  std::vector<SurveyBin> spectrum() const;

 private:
  struct Acc {
    double mean = 0, peak = 0, duty = 0;
    int n = 0;
    long step = 0;  // the step that last saw it (kHz)
    unsigned seq = 0;  // _merges when it was last updated
  };
  void update_refs();
  std::vector<Acc> _acc;
  std::map<long, double> _step_floor;  // by step centre (kHz)
  std::map<long, double> _step_ref;    // _step_floor capped by the neighbours (update_refs)
  int _sweeps = 0;
  unsigned _merges = 0;
  int _memory = kMemory;
};

// One capture: 0.1 ms slices (16 FFTs of 256, Hann). Only bins in [-reach_hz, +reach_hz) of the centre go into
// the step: with steps 2 x reach apart, every frequency is measured once a sweep (overlapping steps would count
// some twice and mix in the filter's edges); bins within 0.3 MHz of DC (the chip's LO leak) are left out.
// floor_hint: SurveyGrid::reference(), in this capture's units (0 = use this step's own floor for duty).
// full_scale: the ADC's (12 bits: 2048), for clip and peak_abs.
SurveyStep survey_measure(const cfs *x, size_t n, double centre_hz, double fs, double reach_hz, double floor_hint,
                          float full_scale = 2048);

}  // namespace fpv
