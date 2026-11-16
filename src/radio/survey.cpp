// SPDX-License-Identifier: GPL-2.0-only
#include "survey.h"

#include <algorithm>
#include <cmath>

#include "spectrum.h"  // kepyur: fft_inplace

namespace fpv {

void SurveyGrid::reset() {
  _acc.assign(kBins, Acc{});
  _step_floor.clear();
  _step_ref.clear();
  _sweeps = 0;
}

double SurveyGrid::reference(double centre_hz) const {
  const long key = std::lround(centre_hz / 1e3);
  const auto it = _step_ref.find(key);
  if (it != _step_ref.end()) return it->second;
  // a step not seen yet: the nearest one's, plus the slope it may have over that distance (30 MHz a step)
  double best = 0;
  long dist = 0;
  for (const auto &kv : _step_ref) {
    const long d = std::labs(kv.first - key);
    if (best == 0 || d < dist) {
      best = kv.second;
      dist = d;
    }
  }
  if (best <= 0) return 0;
  return best * std::pow(10.0, kSlopeDb / 10 * std::ceil(static_cast<double>(dist) / 30e3));
}

void SurveyGrid::update_refs() {
  const double k = std::pow(10.0, kSlopeDb / 10);
  _step_ref = _step_floor;
  double prev = 0;
  for (auto &kv : _step_ref) {  // up the band
    if (prev > 0) kv.second = std::min(kv.second, prev * k);
    prev = kv.second;
  }
  prev = 0;
  for (auto it = _step_ref.rbegin(); it != _step_ref.rend(); ++it) {  // and down
    if (prev > 0) it->second = std::min(it->second, prev * k);
    prev = it->second;
  }
}

void SurveyGrid::merge(const SurveyStep &s) {
  if (_acc.size() != static_cast<size_t>(kBins)) _acc.assign(kBins, Acc{});
  const long key = std::lround(s.centre_hz / 1e3);
  if (s.floor <= 0) return;
  auto f = _step_floor.find(key);
  if (f == _step_floor.end()) _step_floor[key] = s.floor;
  else f->second += kFloorWeight * (s.floor - f->second);
  update_refs();
  const double decay = std::pow(10.0, -kPeakDecayDb / 10);
  for (const auto &b : s.bins) {
    if (b.index < 0 || b.index >= kBins) continue;
    auto &a = _acc[static_cast<size_t>(b.index)];
    // plain average for the first kMemory updates, then exponential with the same weight
    a.n = std::min(a.n + 1, kMemory);
    const double w = 1.0 / a.n;
    a.mean += w * (b.mean - a.mean);
    a.duty += w * (b.duty - a.duty);
    a.peak = std::max(b.peak, a.peak * decay);
    a.step = key;
  }
}

std::vector<SurveyBin> SurveyGrid::spectrum() const {
  std::vector<SurveyBin> out;
  for (size_t i = 0; i < _acc.size(); i++) {
    const auto &a = _acc[i];
    if (a.n == 0) continue;
    const auto r = _step_ref.find(a.step);
    if (r == _step_ref.end() || r->second <= 0) continue;
    const double f = r->second;
    SurveyBin b;
    b.mhz = (kLo + (static_cast<double>(i) + 0.5) * kBin) / 1e6;
    b.mean_db = static_cast<float>(10 * std::log10(std::max(a.mean, 1e-30) / f));
    b.peak_db = static_cast<float>(10 * std::log10(std::max(a.peak, 1e-30) / f));
    b.duty = static_cast<float>(a.duty);
    out.push_back(b);
  }
  return out;
}

SurveyStep survey_measure(const cfs *x, size_t n, double centre_hz, double fs, double edge_hz, double floor_hint) {
  constexpr int kN = 256, kFrames = 16, kSlice = kN * kFrames;
  SurveyStep out;
  out.centre_hz = centre_hz;
  const size_t slices = n / kSlice;
  if (slices < 8) return out;
  std::vector<float> win(kN);
  for (int i = 0; i < kN; i++) win[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * i / kN));
  std::vector<cfs> buf(kN);
  std::vector<std::vector<float>> p(slices, std::vector<float>(kN, 0.f));
  for (size_t s = 0; s < slices; s++)
    for (int fr = 0; fr < kFrames; fr++) {
      const cfs *src = x + s * kSlice + static_cast<size_t>(fr) * kN;
      for (int i = 0; i < kN; i++) buf[i] = src[i] * win[i];
      kp::fft_inplace(buf.data(), kN);
      for (int k = 0; k < kN; k++) p[s][k] += std::norm(buf[k]) / kFrames;
    }
  const auto freq = [&](int k) { return (k < kN / 2 ? k : k - kN) * fs / kN; };
  std::vector<int> use;
  for (int k = 0; k < kN; k++)
    if (std::fabs(freq(k)) < edge_hz && std::fabs(freq(k)) > 0.3e6) use.push_back(k);
  if (use.empty()) return out;
  std::vector<double> mean(kN, 0);
  for (int k : use) {
    for (size_t s = 0; s < slices; s++) mean[k] += p[s][k];
    mean[k] /= static_cast<double>(slices);
  }
  std::vector<double> m;
  for (int k : use) m.push_back(mean[k]);
  std::nth_element(m.begin(), m.begin() + m.size() / 10, m.end());
  out.floor = std::max(m[m.size() / 10], 1e-30);
  const double thr = SurveyGrid::kDutyOver * (floor_hint > 0 ? std::min(floor_hint, out.floor) : out.floor);
  for (int k : use) {
    float pk = 0;
    int on = 0;
    for (size_t s = 0; s < slices; s++) {
      pk = std::max(pk, p[s][k]);
      on += p[s][k] > thr;
    }
    const long i = static_cast<long>(std::floor((centre_hz + freq(k) - SurveyGrid::kLo) / SurveyGrid::kBin));
    out.bins.push_back({i, mean[k], pk, static_cast<float>(on) / static_cast<float>(slices)});
  }
  return out;
}

}  // namespace fpv
