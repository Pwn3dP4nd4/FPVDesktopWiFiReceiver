// SPDX-License-Identifier: GPL-2.0-only
// fpv::Radio: one RTL8812BU (wifi-sdr Receiver) + a DSP thread for FPVDesktopWiFiReceiver.
// Derived from kepyur's native/radio/radio.cpp (https://github.com/gilankpam/kepyur, GPL-2.0), with this app's
// additions as plain code: scan plans (set_scan_plan), the NTSC chroma comb (set_comb), digital signals in the
// scan (set_digital_scan, src/kp_ext/digital_scan.cpp) and stream gaps in IQ recordings. The video DSP stays
// kepyur's own native/dsp, used unmodified.
#include "radio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "log.h"
#include "receiver.h"
#include "spectrum.h"
#include "stream88xx.h"
#include "digital_scan.h"  // src/kp_ext

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace fpv {

using namespace kp;  // kepyur native/dsp: FrontEnd, Psd, find_humps, check_video, nearest_channel...

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

namespace {
constexpr double kFs = 40e6;
constexpr double kEdge = 16.5e6;     // usable +- span at 40 MS/s (BW40 DFIR is flat to ~17 MHz)
constexpr double kStep = 20e6;       // a hump up to ~13 MHz wide is fully inside some step's span
constexpr double kCapture = 0.09;    // s of IQ per scan step (60 ms acquisition + ~300 lines)
constexpr double kPsdSpan = 0.012;   // s of it for the spectrum
constexpr int kSettleBanks = 40;     // ~8 ms after a retune (chip AGC)
// A watch at 40 MS/s that restarts this often falls back to 20 MS/s (same picture: the BW20 RX filter is
// +-10 MHz either way). A link that cannot carry 40 MS/s restarts every 0.2-0.3 s (Chrome's WebUSB: ~14k
// transfers/s against the 24.4k needed); a healthy one runs minutes without one.
constexpr int64_t kSnrEveryMs = 500;  // live SNR while watching (~2 % of a core at 20 MS/s)
constexpr double kSnrSpan = 8e6;      // +- span the BW20 RX filter passes flat (-3 dB at ~9.2 MHz)
constexpr int kFallbackFaults = 3;
// survey: fixed gain, so every step is on the same scale (src/radio/survey.h). High enough for a far WiFi to
// stand over the chip's noise, low enough that a VTX a few metres away does not clip.
constexpr double kSurveyGainDb = 30;
// survey: a step whose samples clip (more than this fraction at full scale) is measured again with less gain
constexpr float kSurveyClipMax = 1e-4f;
constexpr double kSurveyGainStepDown = 10, kSurveyGainStepUp = 5, kSurveyGainMin = 0;
// ... and gets its gain back when its loudest sample is under this fraction of full scale (18 dB of headroom)
constexpr float kSurveyRaiseUnder = 0.12f;
// survey: each step measures [-reach, +reach) of its centre; src/scan_plan.cpp puts steps 2 x reach apart
constexpr double kSurveyReach = 10e6;
// survey of a band (survey(lo, hi)): the first and last steps measure out to the band's edges, at most this far from
// their centre (the 40 MHz RX filter passes +-20 MHz; the 2.4 GHz synthesizer at 40 MHz width only tunes 2422..2462)
constexpr double kSurveyEdgeReach = 20e6;
// survey: a pilot not seen for this many sweeps has left
constexpr int kSurveyLostSweeps = 3;
// survey: an analog pilot this close to a stronger one is its sideband copy (see scan_step_done)
constexpr double kSurveySidebandMhz = 15;
// survey: a hump already checked for video is not checked again (streaming, ~100 ms) for this many sweeps
constexpr int kSurveyRecheckSweeps = 5;
constexpr int64_t kFallbackWindowMs = 5000;
// scan: a signal cut by a step's edge gets a step of its own (see add_cut_steps), centred so that this much on
// each side of where it was seen fits (find_humps takes humps up to 24 MHz wide)
constexpr double kCutReach = 12e6;
constexpr float kCutMinSnrDb = 10;  // weaker cut signals are left alone (a wide WiFi's skirts, noise)
constexpr size_t kMaxCutSteps = 8;  // per scan: a band full of WiFi does not double the scan time

// Raise this thread's priority (the USB stream thread must never miss a chip deadline; the DSP thread
// must keep up). Best effort: logs what the OS allowed.
void boost_thread(const char *what) {
#ifdef _WIN32
  const int tid = static_cast<int>(GetCurrentThreadId());
  const int rc = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) ? 0 : -1;  // ~ nice -10
#else
  const int tid = static_cast<int>(gettid());
  const int rc = setpriority(PRIO_PROCESS, tid, -10);
#endif
  KP_LOGI("radio: %s thread %d nice -10: %s", what, tid, rc == 0 ? "ok" : "refused");
}

std::vector<double> scan_plan() {
  std::vector<double> p;
  // the common analog bands first (A, B, E, F, R: 5645..5945), then the low band (L: 5362..5621)
  for (double c = 5652e6; c <= 5940e6; c += kStep) p.push_back(c);
  for (double c = 5370e6; c <= 5630e6; c += kStep) p.push_back(c);
  return p;
}
}  // namespace

Radio::Radio(FrameSink frames) : _frames(std::move(frames)) {}

Radio::~Radio() { close(); }

bool Radio::is_open() const {
  std::lock_guard<std::mutex> l(_m);
  return _open;
}

bool Radio::superspeed() const {
  std::lock_guard<std::mutex> l(_m);
  return _superspeed;
}

void Radio::open(int fd) {
  close();
  auto log = std::make_shared<Logger>();
  auto rx = std::make_unique<wsdr::Receiver>(log);
  wsdr::RxSettings s;
  s.freq = 5769e6;
  s.rate_sel = 1;  // 40 MS/s
  s.wide = true;
  s.agc = true;
  KP_LOGI("radio: opening adapter (%s)", fd >= 0 ? "Android fd" : "bus scan");
  rx->open(wsdr::kPid8812bu, s, -1, {}, fd);
  const auto caps = rx->caps();
  KP_LOGI("radio: %s, USB %s, fastest continuous rate %.1f MS/s", caps.chip.c_str(), caps.superspeed ? "3" : "2",
          wsdr::Receiver::rate_of(caps.min_rate_sel) / 1e6);
  if (!caps.superspeed) {
    // 40 MS/s does not stream on USB 2; open at a rate it can, so the stream still comes up for diagnostics
    s.rate_sel = caps.min_rate_sel;
    rx->set(s);
  }
  {
    std::lock_guard<std::mutex> l(_m);
    _superspeed = caps.superspeed;
    _chip = caps.chip;
    _open = true;
    _mode = Mode::Idle;
    _mode_epoch++;
    _req_hz = s.freq;
    _req_rate = s.rate_sel;
    _fallback = false;
  }
  _banks = 0;
  _dropped = 0;
  _stop = false;
  _rx = std::move(rx);
  _ddropped = 0;
  _dsp = std::thread([this] { dsp_loop(); });
  _decoder = std::thread([this] { decode_loop(); });
  _rx->start([this](const uint8_t *b, size_t n, const wsdr::BankMeta &m) {
    on_bank(b, n, m.seq, m.gen, m.fs, m.centre_hz, m.shift_hz, m.gapped);
  });
}

void Radio::close() {
  _stop = true;
  _qcv.notify_all();
  if (_rx) _rx->stop();
  if (_dsp.joinable()) _dsp.join();
  _dqcv.notify_all();
  if (_decoder.joinable()) _decoder.join();
  abort_record("closed");  // the DSP thread is gone: its recording state is ours now
  {
    std::lock_guard<std::mutex> l(_dqm);
    _dq.clear();
  }
  _fe.reset();
  if (_rx) {
    _rx->close();
    _rx.reset();
    KP_LOGI("radio: closed");
  }
  std::lock_guard<std::mutex> l(_m);
  _open = false;
  _mode = Mode::Idle;
  std::lock_guard<std::mutex> q(_qm);
  _q.clear();
}

// Receiver stream thread: copy only.
void Radio::on_bank(const uint8_t *b, size_t n, uint64_t seq, uint32_t gen, double fs, double centre, double shift,
                    bool gapped) {
  thread_local bool boosted = false;
  if (!boosted) {
    boosted = true;
    boost_thread("usb stream");
  }
  std::vector<uint8_t> v;
  {
    std::lock_guard<std::mutex> l(_qm);
    if (!_pool.empty()) {
      v = std::move(_pool.back());
      _pool.pop_back();
    }
  }
  v.assign(b, b + n);
  {
    std::lock_guard<std::mutex> l(_qm);
    if (_q.size() >= _qcap) {
      _pool.push_back(std::move(_q.front().data));
      _q.pop_front();
      _dropped++;
    }
    _q.push_back(Bank{std::move(v), seq, gen, fs, centre, shift, gapped});
  }
  _banks++;
  _qcv.notify_one();
}

void Radio::tune(double hz, int rate_sel) {
  wsdr::RxSettings s = _rx->settings();
  s.freq = hz;
  s.rate_sel = rate_sel;
  s.wide = true;
  {
    std::lock_guard<std::mutex> l(_m);
    // survey: fixed gain for the spectrum; the candidate check (_pass) with the usual gain (AGC): a VTX
    // close by clips at the survey's fixed gain and its video never locks (measured: lock 0.08 vs 0.96)
    const bool gapped = _survey && _pass == SurveyPass::Gapped;
    const double g = gapped ? survey_gain(hz) : _gain_db;
    // survey: one bank at a time (cmake/kepyur_patches.cmake, set "gapped"): it needs no contiguous samples
    // and the chip then never misses a deadline while it retunes nonstop
    s.allow_gapped = gapped;
    s.agc = g < 0;
    if (g >= 0) s.gain_db = g;
  }
  _rx->set(s);
  std::lock_guard<std::mutex> l(_m);
  _req_hz = hz;
  _req_rate = rate_sel;
}

void Radio::scan(double prefer_hz) {
  std::lock_guard<std::mutex> l(_m);
  _survey = false;
  _resume_hz = prefer_hz > 0 ? prefer_hz : _mode == Mode::Watch ? _watch_hz : 0;
  _mode = Mode::Scan;
  _mode_epoch++;
}

void Radio::survey(double lo_hz, double hi_hz, bool spectrum_only) {
  std::lock_guard<std::mutex> l(_m);
  _survey = true;
  _survey_spectrum_only = spectrum_only;
  _survey_lo = lo_hz;
  _survey_hi = hi_hz;
  _resume_hz = 0;
  {
    const int m = _grid.memory();
    // a band outside the default grid (5300..6000 MHz) gets a grid of its own, with kSurveyEdgeReach to spare
    if (hi_hz > lo_hz && (lo_hz < SurveyGrid::kLo || hi_hz > SurveyGrid::kLo + SurveyGrid::kBins * SurveyGrid::kBin)) {
      const double glo = std::floor((lo_hz - kSurveyEdgeReach) / 1e6) * 1e6;
      _grid.reset(glo, static_cast<int>(std::ceil((hi_hz - glo + kSurveyEdgeReach) / SurveyGrid::kBin)));
    } else {
      _grid.reset();
    }
    _grid.set_memory(m);
  }
  _sweep_start_ms = now_ms();
  _sweep_ms = 0;
  _survey_gain.clear();
  _survey_known.clear();
  _pilots.clear();
  _mode = Mode::Scan;
  _mode_epoch++;
}

double Radio::survey_gain(double hz) const {
  const auto it = _survey_gain.find(std::lround(hz / 1e3));
  return it == _survey_gain.end() ? kSurveyGainDb : it->second;
}

void Radio::set_survey_memory(int sweeps) {
  std::lock_guard<std::mutex> l(_m);
  _grid.set_memory(sweeps);
}

std::vector<SurveyBin> Radio::survey_spectrum(int *sweeps) {
  std::lock_guard<std::mutex> l(_m);
  if (sweeps) *sweeps = _grid.sweeps();
  return _grid.spectrum();
}

void Radio::watch(double hz) {
  std::lock_guard<std::mutex> l(_m);
  _survey = false;
  _watch_hz = hz;
  _mode = Mode::Watch;
  _mode_epoch++;
}

void Radio::stream_test(int rate_sel, double seconds) {
  std::lock_guard<std::mutex> l(_m);
  _mode = Mode::Test;
  _survey = false;
  _mode_epoch++;
  _req_rate = rate_sel;
  _test_end_ms = static_cast<int64_t>(seconds * 1000);
}

void Radio::idle() {
  std::lock_guard<std::mutex> l(_m);
  _survey = false;
  _mode = Mode::Idle;
  _mode_epoch++;
}

void Radio::set_watch_rate(int rate_sel) {
  std::lock_guard<std::mutex> l(_m);
  _watch_rate = rate_sel;
  _fallback = false;
}

void Radio::set_gain(double db) {
  std::lock_guard<std::mutex> l(_m);
  _gain_db = db;
}

void Radio::set_scan_plan(std::vector<double> centres_hz) {
  std::lock_guard<std::mutex> l(_m);
  _scan_plan_override = std::move(centres_hz);
}

void Radio::set_digital_scan(bool on) {
  std::lock_guard<std::mutex> l(_m);
  _digital_on = on;
}

void Radio::set_comb(bool on) {
  std::lock_guard<std::mutex> l(_m);
  _comb = on;
}

void Radio::record(double seconds, const std::string &prefix) {
  std::lock_guard<std::mutex> l(_m);
  _rec_req_s = seconds;
  _rec_req_prefix = prefix;
}

std::vector<Pilot> Radio::pilots() {
  std::lock_guard<std::mutex> l(_m);
  return _pilots;
}

void Radio::unpack(const Bank &b, std::vector<cf> &out) {
  const size_t ns = b.data.size() / 8;
  out.resize(ns);
  const uint8_t *p = b.data.data();
  for (size_t i = 0; i < ns; i++) {
    uint64_t w;
    std::memcpy(&w, p + 8 * i, 8);
    out[i] = cf(static_cast<float>(wsdr::la_i(w)), static_cast<float>(wsdr::la_q(w)));
  }
}

void Radio::dsp_loop() {
  boost_thread("dsp");
  Bank b;
  _load_t0_ms = now_ms();
  while (!_stop) {
    {
      std::unique_lock<std::mutex> l(_qm);
      _qcv.wait_for(l, std::chrono::milliseconds(200), [&] { return !_q.empty() || _stop; });
      if (_q.empty()) {
        if (_rx && !_rx->status().running) {
          KP_LOGE("radio: receiver stopped: %s", _rx->status().error.c_str());
          break;
        }
        continue;
      }
      b = std::move(_q.front());
      _q.pop_front();
    }
    const auto t0 = std::chrono::steady_clock::now();
    Mode mode;
    uint64_t epoch;
    {
      std::lock_guard<std::mutex> l(_m);
      mode = _mode;
      epoch = _mode_epoch;
    }
    const bool fresh = epoch != _epoch_seen;
    _epoch_seen = epoch;
    if (fresh) {
      // a mode change: set the radio up for it (a new watch builds a new decoder). A recording does not carry
      // over: it would go on later, maybe on another channel, under the first one's centre_hz.
      abort_record("mode change");
      _fe.reset();
      _fe_gen = ~0u;
      if (mode == Mode::Watch) {
        double hz;
        {
          std::lock_guard<std::mutex> l(_m);
          hz = _watch_hz;
        }
        // Watch at 40 MS/s with the chip's AGC, or 20 MS/s after a fallback (watch_fault). Both on a 40 MHz
        // channel: at 20 MHz width the OFDM CCA/AGC froze the LA in 120-sample runs (wifi-sdr RxSettings::wide).
        int rate;
        {
          std::lock_guard<std::mutex> l(_m);
          rate = _fallback && _watch_rate == 1 ? 2 : _watch_rate;
          _watch_comb = _comb;  // a decoder rebuilt later in this watch (fallback, shift) keeps it
        }
        KP_LOGI("radio: watch %.3f MHz at %.0f MS/s", hz / 1e6, wsdr::Receiver::rate_of(rate) / 1e6);
        _watch_faults.clear();
        _snr_psd.reset();
        _snr_t_ms = 0;
        {
          std::lock_guard<std::mutex> l(_m);
          _snr_db = -1;
        }
        tune(hz, rate);
      } else if (mode == Mode::Scan) {
        {
          std::lock_guard<std::mutex> l(_m);
          _plan = _scan_plan_override.empty() ? scan_plan() : _scan_plan_override;
        }
        _step = 0;
        _cap.clear();
        _phase = StepPhase::Retuned;
        _pass = SurveyPass::Gapped;
        _cut_steps.clear();
        _whole_spans.clear();
        _scan_start_ms = now_ms();
        {
          std::lock_guard<std::mutex> l(_m);
          // the survey always lists digital signals (not the spectrum-only one: 2.4 GHz)
          if ((!_digital_on && !_survey) || (_survey && _survey_spectrum_only)) _digital.reset();
          else if (!_digital) _digital = std::make_unique<kpx::DigitalScan>();
        }
        if (_digital) _digital->reset();
        KP_LOGI("radio: scan, %zu steps", _plan.size());
        tune(_plan[0], 1);
      } else if (mode == Mode::Test) {
        int rate;
        {
          std::lock_guard<std::mutex> l(_m);
          rate = _req_rate;
          _test_result.clear();
        }
        _test_banks = _test_gaps = 0;
        _test_last_seq = 0;
        _test_t0_ms = 0;
        _test_fs = wsdr::Receiver::rate_of(rate);
        KP_LOGI("radio: stream test at %.1f MS/s", _test_fs / 1e6);
        tune(5769e6, rate);
      }
    }
    switch (mode) {
    case Mode::Watch: handle_watch(b); break;
    case Mode::Scan: handle_scan(b); break;
    case Mode::Test: handle_test(b); break;
    case Mode::Idle: break;
    }
    {
      std::lock_guard<std::mutex> l(_qm);
      if (_pool.size() < _qcap) _pool.push_back(std::move(b.data));
    }
    _busy_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const int64_t t = now_ms();
    if (t - _load_t0_ms >= 1000) {
      std::lock_guard<std::mutex> l(_m);
      _load = _busy_s / ((t - _load_t0_ms) / 1000.0);
      _busy_s = 0;
      _load_t0_ms = t;
      if (++_status_ticks % 5 == 0 && _mode == Mode::Watch)
        KP_LOGI("status: watch %.1f MHz, cpu demod %.0f%% decode %.0f%%, dropped usb %llu decode %llu, restarts %llu, "
                "%s %s lock %.2f fps %.0f, acq %llu lost %llu rephase %llu", _watch_hz / 1e6, _load * 100, _dload * 100,
                (unsigned long long)_dropped.load(), (unsigned long long)_ddropped.load(),
                (unsigned long long)(_rx ? _rx->status().restarts : 0), _vstats.std_name,
                _vstats.colour ? "colour" : "grey", _vstats.lock_recent, _fps,
                (unsigned long long)_vstats.acquisitions, (unsigned long long)_vstats.losses,
                (unsigned long long)_vstats.rephases);
    }
  }
}

// A stream restart during a watch. At 40 MS/s, kFallbackFaults of them within kFallbackWindowMs switch the
// watch to 20 MS/s for the rest of the session; true when it did (the bank in hand is from the old stream).
bool Radio::watch_fault(double hz) {
  if (_fe_fs != 40e6) return false;
  const int64_t t = now_ms();
  _watch_faults.push_back(t);
  while (!_watch_faults.empty() && t - _watch_faults.front() > kFallbackWindowMs) _watch_faults.pop_front();
  if (static_cast<int>(_watch_faults.size()) < kFallbackFaults) return false;
  {
    std::lock_guard<std::mutex> l(_m);
    _fallback = true;
  }
  KP_LOGW("radio: 40 MS/s restarted %zu times in %lld s: watching at 20 MS/s from now on", _watch_faults.size(),
          static_cast<long long>(kFallbackWindowMs / 1000));
  _watch_faults.clear();
  _fe.reset();
  _fe_gen = ~0u;
  abort_record("fallback to 20 MS/s");  // the file's sample_rate would no longer hold
  tune(hz, 2);
  return true;
}

void Radio::measure_snr(const std::vector<cf> &iq, double fs) {
  if (!_snr_psd) {
    if (now_ms() - _snr_t_ms < kSnrEveryMs) return;
    _snr_psd = std::make_unique<Psd>(1024);
    _snr_have = 0;
    _snr_need = static_cast<size_t>(kPsdSpan * fs);
  }
  _snr_psd->add(iq.data(), iq.size());
  _snr_have += iq.size();
  if (_snr_have < _snr_need) return;
  const float snr = carrier_snr(_snr_psd->db(), fs, std::min(kSnrSpan, 0.4 * fs));
  _snr_psd.reset();
  _snr_t_ms = now_ms();
  std::lock_guard<std::mutex> l(_m);
  _snr_db = snr;
}

void Radio::handle_watch(Bank &b) {
  double req;
  int req_rate;
  {
    std::lock_guard<std::mutex> l(_m);
    req = _req_hz;
    req_rate = _req_rate;
  }
  // ignore banks from before the retune (or the rate change of a fallback)
  if (std::fabs(b.centre_hz + b.shift_hz - req) > 1e3 || b.fs != wsdr::Receiver::rate_of(req_rate)) return;
  if (!_fe || b.fs != _fe_fs || b.shift_hz != _fe_shift) {
    DecoderOpts o;
    o.comb = _watch_comb;
    _fe = std::make_shared<FrontEnd>(b.fs, b.shift_hz, _frames, o);
    _fe_fs = b.fs;
    _fe_shift = b.shift_hz;
  } else if (b.gen != _fe_gen) {
    // same settings: keep the decoder; it re-phases onto the restarted stream within a few lines
    std::string why;
    if (_rx) why = _rx->status().last_fault;
    KP_LOGW("radio: stream restarted (gen %u, %s), decoder keeps going", b.gen, why.c_str());
    if (watch_fault(req)) return;
  }
  _fe_gen = b.gen;
  unpack(b, _iq);
  measure_snr(_iq, b.fs);
  record_bank(b);
  std::vector<float> d;
  {
    std::lock_guard<std::mutex> l(_dqm);
    if (!_dpool.empty()) {
      d = std::move(_dpool.back());
      _dpool.pop_back();
    }
  }
  _fe->demod(_iq.data(), _iq.size(), d);
  {
    std::lock_guard<std::mutex> l(_dqm);
    if (_dq.size() >= 128) {  // ~26 ms behind: drop the oldest (the decoder re-phases)
      _dpool.push_back(std::move(_dq.front().d));
      _dq.pop_front();
      _ddropped++;
    }
    _dq.push_back(DBlock{_fe, std::move(d)});
  }
  _dqcv.notify_one();
}

// Debug recording: the watched IQ as int16 (LA value << 4, as wsdr_probe writes .cs16), whole banks.
void Radio::record_bank(const Bank &b) {
  if (!_rec) {
    double secs;
    std::string prefix;
    {
      std::lock_guard<std::mutex> l(_m);
      secs = _rec_req_s;
      prefix = _rec_req_prefix;
      _rec_req_s = 0;
    }
    if (secs <= 0) return;
    _rec = std::fopen((prefix + ".cs16").c_str(), "wb");
    if (!_rec) {
      KP_LOGE("record: cannot open %s.cs16", prefix.c_str());
      return;
    }
    std::setvbuf(_rec, nullptr, _IOFBF, 1 << 20);
    _rec_prefix = prefix;
    _rec_left = static_cast<uint64_t>(secs * b.fs);
    _rec_samples = 0;
    _rec_fs = b.fs;
    _rec_centre = b.centre_hz;
    _rec_shift = b.shift_hz;
    KP_LOGI("record: %.1f s at %.0f MS/s -> %s.cs16", secs, b.fs / 1e6, prefix.c_str());
  }
  if (_rec_samples == 0) _rec_gaps.clear();
  else if (b.gen != _rec_gen || b.seq != _rec_seq + 1) _rec_gaps.push_back(_rec_samples);
  _rec_seq = b.seq;
  _rec_gen = b.gen;
  const size_t ns = std::min<uint64_t>(_iq.size(), _rec_left);
  std::vector<int16_t> buf(2 * ns);
  for (size_t i = 0; i < ns; i++) {
    buf[2 * i] = static_cast<int16_t>(static_cast<int>(_iq[i].real()) * 16);
    buf[2 * i + 1] = static_cast<int16_t>(static_cast<int>(_iq[i].imag()) * 16);
  }
  if (std::fwrite(buf.data(), 4, ns, _rec) != ns) {
    abort_record("write failed (disk full?)");
    return;
  }
  _rec_left -= ns;
  _rec_samples += ns;
  if (_rec_left == 0) {
    std::fclose(_rec);
    _rec = nullptr;
    if (FILE *j = std::fopen((_rec_prefix + ".json").c_str(), "w")) {
      std::string gap_at;
      for (size_t g = 0; g < _rec_gaps.size() && g < 1000; g++)
        gap_at += (g ? "," : "") + std::to_string(_rec_gaps[g]);
      std::fprintf(j, "{\"format\":\"cs16\",\"sample_rate\":%.0f,\"centre_hz\":%.0f,\"shift_hz\":%.0f,\"samples\":%llu,"
                   "\"gaps\":%zu,\"gap_at\":[%s],\"source\":\"kepyur watch\"}\n", _rec_fs, _rec_centre, _rec_shift,
                   (unsigned long long)_rec_samples, _rec_gaps.size(), gap_at.c_str());
      std::fclose(j);
    }
    KP_LOGI("record: done, %llu samples -> %s.cs16", (unsigned long long)_rec_samples, _rec_prefix.c_str());
  }
}

// A debug recording cut short (mode change, fallback, close, write error): the .cs16 is closed as it is and gets
// no .json, so the GUI reports it incomplete instead of taking it for a good one. A request not started yet is
// dropped too: it would start on whatever is watched next. DSP thread (or with it stopped).
void Radio::abort_record(const char *why) {
  {
    std::lock_guard<std::mutex> l(_m);
    _rec_req_s = 0;
  }
  if (!_rec) return;
  std::fclose(_rec);
  _rec = nullptr;
  KP_LOGW("record: %s.cs16 cut short (%s) after %llu samples, no .json", _rec_prefix.c_str(), why,
          (unsigned long long)_rec_samples);
}

void Radio::decode_loop() {
  boost_thread("decode");
  std::shared_ptr<FrontEnd> cur;
  double busy = 0;
  int64_t t0 = now_ms();
  while (!_stop) {
    DBlock blk;
    {
      std::unique_lock<std::mutex> l(_dqm);
      _dqcv.wait_for(l, std::chrono::milliseconds(200), [&] { return !_dq.empty() || _stop; });
      if (_dq.empty()) continue;
      blk = std::move(_dq.front());
      _dq.pop_front();
    }
    const auto ta = std::chrono::steady_clock::now();
    if (blk.fe != cur) {
      cur = blk.fe;
      _frames_at_last = 0;
      _fps_t0_ms = now_ms();
    }
    cur->decode(blk.d.data(), blk.d.size());
    busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - ta).count();
    {
      std::lock_guard<std::mutex> l(_dqm);
      if (_dpool.size() < 256) _dpool.push_back(std::move(blk.d));
    }
    const int64_t t = now_ms();
    if (t - _fps_t0_ms >= 1000) {
      const auto s = cur->stats();
      std::lock_guard<std::mutex> l(_m);
      _vstats = s;
      _fps = static_cast<float>((s.frames - _frames_at_last) * 1000.0 / (t - _fps_t0_ms));
      _frames_at_last = s.frames;
      _fps_t0_ms = t;
    }
    if (t - t0 >= 1000) {
      std::lock_guard<std::mutex> l(_m);
      _dload = busy / ((t - t0) / 1000.0);
      busy = 0;
      t0 = t;
    }
  }
}

void Radio::handle_scan(Bank &b) {
  if (_step >= _plan.size()) return;
  const double req = _plan[_step];
  if (std::fabs(b.centre_hz + b.shift_hz - req) > 1e3 || b.fs != kFs) return;
  if (!capture(b)) return;
  if (_phase == StepPhase::Spectrum && !spectrum_step(req)) return;
  scan_step_done();
}

bool Radio::capture(const Bank &b) {
  // survey: the step taken again streaming, on the same frequency: banks still gapped are from before the change
  if (_pass != SurveyPass::Gapped && b.gapped) return false;
  if (_phase == StepPhase::Retuned) {
    _phase = StepPhase::Settling;
    _settle = kSettleBanks;
    _cap.clear();
    _humps.clear();
  }
  if (_phase == StepPhase::Settling) {
    if (_settle > 0) {
      _settle--;
      _cap_last_seq = b.seq;
      return false;
    }
    _phase = StepPhase::Spectrum;  // first just enough for the spectrum
  }
  if (!_cap.empty() && b.seq != _cap_last_seq + 1) _cap.clear();  // a gap: start the capture over
  _cap_last_seq = b.seq;
  _cap_centre = b.centre_hz;
  const size_t need = static_cast<size_t>((_phase == StepPhase::Spectrum ? kPsdSpan : kCapture) * kFs);
  const size_t ns = b.data.size() / 8;
  const uint8_t *p = b.data.data();
  for (size_t i = 0; i < ns && _cap.size() < need; i++) {
    uint64_t w;
    std::memcpy(&w, p + 8 * i, 8);
    _cap.emplace_back(static_cast<int16_t>(wsdr::la_i(w)), static_cast<int16_t>(wsdr::la_q(w)));
  }
  return _cap.size() >= need;
}

Radio::SurveyView Radio::survey_view(double req) const {
  std::lock_guard<std::mutex> l(_m);
  SurveyView v;
  v.survey = _survey;
  v.spectrum_only = _survey && _survey_spectrum_only;
  v.floor_hint = _grid.reference(_cap_centre);
  v.gain = survey_gain(req);
  v.grid_lo = _grid.lo_hz();
  v.lo = _survey_lo;
  v.hi = _survey_hi;
  return v;
}

bool Radio::spectrum_step(double req) {
  // spectrum first: a step with no candidate is done now; otherwise keep capturing for the sync check
  std::vector<cf> x(_cap.size());
  for (size_t i = 0; i < _cap.size(); i++) x[i] = cf(_cap[i].real(), _cap[i].imag());
  Psd psd(1024);
  psd.add(x.data(), x.size());
  const std::vector<float> psd_db = psd.db();
  _humps = find_humps(psd_db, kFs, kEdge);
  const SurveyView sv = survey_view(req);
  // the survey streams gapped (one bank of ~0.2 ms, then a gap): fine for the spectrum, but the digital scan's
  // timing (band_duty, tdd_frame) needs contiguous samples. A step with a block that may be digital is taken
  // again streaming, as for the video sync check, and that capture replaces this one. Measured with a real DJI
  // in R2: gapped, its 2.5 ms frame read as 1.28 ms (every burst and gap half as long) and it was never listed.
  const bool gapped = sv.survey && _pass == SurveyPass::Gapped;
  // spectrum only (2.4 GHz): no video sync or digital checks, just the spectrum of every step
  if (sv.spectrum_only) _humps.clear();
  if (_digital && !sv.spectrum_only) _digital->analyse(psd_db, x.data(), x.size(), kFs, kEdge);
  const bool digital_timing = gapped && !sv.spectrum_only && _digital && _digital->wants_timing();
  if (digital_timing) KP_LOGI("survey: %.1f MHz taken again streaming, for the digital scan", req / 1e6);
  if (_pass == SurveyPass::DigitalRecheck) _humps.clear();  // already checked: no second sync check
  if (!sv.survey) {  // the survey keeps its fixed steps: they tile its spectrum
    for (const auto &h : _humps)  // whole here: an edge of it cut in another step needs no step of its own
      _whole_spans.emplace_back(_cap_centre + h.offset_hz - h.width_hz / 2, _cap_centre + h.offset_hz + h.width_hz / 2);
    add_cut_steps(psd_db);
  }
  if (gapped) {  // (a step taken again to check a candidate is already counted)
    survey_step(x, req, sv);
    skip_known_humps();
  }
  if (gapped && (!_humps.empty() || digital_timing)) {
    // the video sync check needs contiguous samples, and the survey streams gapped: the same step again,
    // streaming (the humps are found again on its spectrum)
    _pass = _humps.empty() ? SurveyPass::DigitalRecheck : SurveyPass::Recheck;
    tune(req, 1);
    _phase = StepPhase::Retuned;
    return false;
  }
  if (!_humps.empty()) {
    _phase = StepPhase::SyncCheck;
    return false;
  }
  return true;
}

void Radio::survey_step(const std::vector<cf> &x, double req, const SurveyView &sv) {
  // measured gain dB under the survey's: back to its scale (the hint comes in that scale)
  const double corr = kSurveyGainDb - sv.gain;
  // [-reach, +reach) of the centre; with a band (survey(lo, hi)), the end steps out to its edges
  double from = -kSurveyReach, to = kSurveyReach;
  if (sv.hi > sv.lo) {
    if (_step == 0) from = std::min(from, std::max(-kSurveyEdgeReach, sv.lo - _cap_centre));
    if (_step + 1 == _plan.size()) to = std::max(to, std::min(kSurveyEdgeReach, sv.hi - _cap_centre));
  }
  SurveyStep st = survey_measure(x.data(), x.size(), _cap_centre, kFs, from, to, sv.grid_lo,
                                 sv.floor_hint > 0 ? sv.floor_hint / std::pow(10.0, corr / 10) : 0);
  survey_scale(st, corr);
  const bool clipped = st.clip > kSurveyClipMax;
  std::lock_guard<std::mutex> l(_m);
  const long key = std::lround(req / 1e3);
  if (clipped && sv.gain > kSurveyGainMin) {
    // a strong signal close by clips the ADC: its spurious products would read as interference. Not
    // counted; next sweep with less gain
    _survey_gain[key] = std::max(kSurveyGainMin, sv.gain - kSurveyGainStepDown);
    KP_LOGI("survey: %.1f MHz clips (%.3f%% of samples): gain %.0f -> %.0f dB", req / 1e6, st.clip * 100.0,
            sv.gain, _survey_gain[key]);
  } else {
    if (_survey) _grid.merge(st);
    if (!clipped && st.peak_abs < kSurveyRaiseUnder && sv.gain < kSurveyGainDb)
      _survey_gain[key] = std::min(kSurveyGainDb, sv.gain + kSurveyGainStepUp);
  }
}

void Radio::skip_known_humps() {
  if (_humps.empty()) return;
  // humps checked in the last few sweeps are not checked again: a pilot with video stays listed, a hump
  // without (a spur of the chip) is left alone
  std::lock_guard<std::mutex> l(_m);
  const int sw = _grid.sweeps();
  for (const auto &h : _humps) {
    const double mhz = (_cap_centre + h.offset_hz) / 1e6;
    const auto k = std::find_if(_survey_known.begin(), _survey_known.end(), [&](const SurveyKnown &s) {
      return std::fabs(s.mhz - mhz) < 3 && sw - s.sweep < kSurveyRecheckSweeps;
    });
    if (k == _survey_known.end()) return;
  }
  for (const auto &h : _humps) {
    const double mhz = (_cap_centre + h.offset_hz) / 1e6;
    for (auto &p : _pilots)
      if (p.digital.empty() && std::fabs(p.mhz - mhz) < 4) p.seen_ms = now_ms();
  }
  _humps.clear();
}

// find_humps lists only humps whole inside the step's +-kEdge. With steps kStep apart that is sure only up to
// 2 x kEdge - kStep = 13 MHz wide, and a VTX close by is 14-17 MHz wide: measured, F4 at 5800 MHz (5792-5809)
// was cut by the edge of both the 5790 and the 5810 MHz steps in 3 scans of 4 and never listed. A signal cut by
// one edge of this step gets a step of its own right after this one, centred kCutReach inside the end of it that
// is seen here. Not one cut by both edges: wider than the step, not a VTX (a WiFi block).
void Radio::add_cut_steps(const std::vector<float> &psd_db) {
  for (const auto &r : kpx::find_wide_regions(psd_db, kFs, kEdge)) {
    if (r.clip_lo == r.clip_hi || r.snr_db < kCutMinSnrDb) continue;
    const double f0 = _cap_centre + r.f0_hz, f1 = _cap_centre + r.f1_hz;  // the part seen in this step
    // the other half of a signal that already has its step (seen cut at the other edge of the step before), or
    // the edge of one already seen whole in another step (measured: R3 whole in the 5730 MHz step, its top cut
    // by the 5750 one's low edge)
    bool done = false;
    for (double c : _cut_steps) done = done || (f0 > c - kEdge && f1 < c + kEdge);
    for (const auto &[w0, w1] : _whole_spans) done = done || std::min(f1, w1) - std::max(f0, w0) > 0.5 * (f1 - f0);
    if (done || _cut_steps.size() >= kMaxCutSteps) continue;
    const double c = r.clip_hi ? f0 + kCutReach : f1 - kCutReach;
    _cut_steps.push_back(c);
    {
      std::lock_guard<std::mutex> l(_m);  // status_json() reads its size
      _plan.insert(_plan.begin() + static_cast<std::ptrdiff_t>(_step) + 1, c);
    }
    KP_LOGI("scan: %.1f MHz step, signal %.1f..%.1f MHz cut by the %s edge (%.1f dB): extra step at %.1f MHz",
            _cap_centre / 1e6, f0 / 1e6, f1 / 1e6, r.clip_hi ? "high" : "low", r.snr_db, c / 1e6);
  }
}

void Radio::scan_step_done() {
  const double centre = _cap_centre;  // the samples are centred on the hardware centre, not the request
  // start the next retune now; the chip settles while this step is analysed
  const bool recheck = _pass != SurveyPass::Gapped;
  _pass = SurveyPass::Gapped;  // survey: back to gapped
  _step++;
  if (_step < _plan.size()) {
    tune(_plan[_step], 1);
    _phase = StepPhase::Retuned;
  }
  std::vector<cf> x(_humps.empty() ? 0 : _cap.size());
  for (size_t i = 0; i < x.size(); i++) x[i] = cf(_cap[i].real(), _cap[i].imag());
  const auto humps = _humps;
  std::vector<Pilot> found;
  std::vector<SurveyKnown> checked;
  for (const auto &h : humps) {
    const auto vc = check_video(x.data(), x.size(), kFs, h.offset_hz);
    const double mhz = (centre + h.offset_hz) / 1e6;
    if (recheck) checked.push_back({mhz, vc.video, 0});
    KP_LOGI("scan: %.1f MHz hump %+.2f MHz (%.1f MHz wide, %.1f dB) -> %s %s lock %.2f", centre / 1e6,
            h.offset_hz / 1e6, h.width_hz / 1e6, h.snr_db, vc.video ? "VIDEO" : "no video", vc.std_name, vc.lock);
    if (!vc.video) continue;
    Pilot pl;
    pl.mhz = mhz;
    const auto *ch = nearest_channel(mhz);
    pl.name = ch ? ch->name : "";
    pl.std_name = vc.std_name;
    pl.snr_db = h.snr_db;
    pl.lock = vc.lock;
    pl.seen_ms = now_ms();
    found.push_back(pl);
  }
  if (_digital)
    for (const auto &d : _digital->step_done(centre)) {
      const double mhz = d.centre_hz / 1e6;
      // analog video already found there (its sync check passed): that is what it is
      if (std::any_of(found.begin(), found.end(), [&](const Pilot &p) { return std::fabs(p.mhz - mhz) < 4; }))
        continue;
      Pilot pl;
      pl.mhz = mhz;
      const auto *ch = nearest_channel(mhz, 3.0);
      pl.name = ch ? ch->name : "";
      pl.snr_db = d.snr_db;
      pl.seen_ms = now_ms();
      pl.digital = kpx::kind_code(d.kind);
      pl.bw_mhz = static_cast<float>(d.width_hz / 1e6);
      found.push_back(pl);
    }
  bool done = _step >= _plan.size();
  double next_watch = 0;
  bool restart = false;
  {
    std::lock_guard<std::mutex> l(_m);
    for (auto &c : checked) {  // survey: remember what this check found
      c.sweep = _grid.sweeps();
      _survey_known.erase(std::remove_if(_survey_known.begin(), _survey_known.end(),
                                         [&](const SurveyKnown &k) { return std::fabs(k.mhz - c.mhz) < 3; }),
                          _survey_known.end());
      _survey_known.push_back(c);
    }
    for (const auto &f : found) {
      auto it = std::find_if(_pilots.begin(), _pilots.end(), [&](const Pilot &p) { return std::fabs(p.mhz - f.mhz) < 4; });
      if (it == _pilots.end()) _pilots.push_back(f);
      else if (f.snr_db >= it->snr_db - 3) *it = f;  // prefer the newer sighting unless much weaker
      else it->seen_ms = f.seen_ms;
    }
    std::sort(_pilots.begin(), _pilots.end(), [](const Pilot &a, const Pilot &b) { return a.snr_db > b.snr_db; });
    if (_survey) {
      // survey: an analog VTX has a copy of its signal ~12 MHz away, a few dB weaker, whose video can lock too
      // (measured: R3 at 5731.4 MHz 35 dB and 5743.4 MHz 32 dB, lock 0.75). An analog pilot that close to a
      // stronger one is taken as that copy, not as another pilot (two real ones that close would be unflyable)
      std::vector<Pilot> kept;
      for (const auto &p : _pilots) {  // strongest first
        const bool copy = p.digital.empty() && std::any_of(kept.begin(), kept.end(), [&](const Pilot &k) {
          return k.digital.empty() && std::fabs(k.mhz - p.mhz) < kSurveySidebandMhz && k.snr_db >= p.snr_db + 2;
        });
        if (!copy) kept.push_back(p);
      }
      _pilots.swap(kept);
    }
    if (done && _survey) {
      // survey: sweep again; pilots not seen for kSurveyLostSweeps sweeps are gone
      _grid.end_sweep();
      restart = true;
      const int64_t t = now_ms();
      _sweep_ms = t - _sweep_start_ms;
      _sweep_start_ms = t;
      const int64_t keep = t - kSurveyLostSweeps * std::max<int64_t>(_sweep_ms, 1000);
      _pilots.erase(std::remove_if(_pilots.begin(), _pilots.end(), [&](const Pilot &p) { return p.seen_ms < keep; }),
                    _pilots.end());
    } else if (done) {
      // after a scan: the pilot watched before, if still there, else the strongest seen in this scan. Only pilots
      // seen in this scan: _pilots keeps older ones, and a VTX moved to another channel left one behind that the
      // scan went back to (measured: F4 seen at 00:36, the VTX moved to R3, the 00:40 scan found R3 and watched F4)
      for (const auto &p : _pilots)
        if (_resume_hz > 0 && p.seen_ms >= _scan_start_ms && std::fabs(p.mhz * 1e6 - _resume_hz) < 4e6)
          next_watch = p.mhz * 1e6;
      if (next_watch == 0)
        for (const auto &p : _pilots)
          if (p.seen_ms >= _scan_start_ms && p.digital.empty()) {  // a digital one shows no picture
            next_watch = p.mhz * 1e6;
            break;
          }
      if (next_watch == 0 && _resume_hz > 0) next_watch = _resume_hz;
      KP_LOGI("scan: done, %zu pilot(s)%s", _pilots.size(), next_watch > 0 ? ", watching" : "");
      if (next_watch > 0) {
        _watch_hz = next_watch;
        _mode = Mode::Watch;
      } else {
        _mode = Mode::Idle;
      }
      _mode_epoch++;
    }
  }
  _cap.clear();
  if (restart) {
    _step = 0;
    _phase = StepPhase::Retuned;
    if (_digital) _digital->reset();
    tune(_plan[0], 1);
  }
}

void Radio::handle_test(Bank &b) {
  if (std::fabs(b.fs - _test_fs) > 1) return;
  const int64_t t = now_ms();
  if (_test_t0_ms == 0) {
    _test_t0_ms = t;
    _test_last_seq = b.seq;
    _test_gen = b.gen;
    _test_runs = 1;
    KP_LOGI("test: first bank at %.1f MS/s (gen %u)", b.fs / 1e6, b.gen);
  } else if (b.gen != _test_gen) {
    _test_gen = b.gen;  // a stream restart (seq starts over): counted as a run, not as a gap
    _test_runs++;
    KP_LOGW("test: stream restarted (gen %u): %s", b.gen, _rx->status().last_fault.c_str());
  } else if (b.seq != _test_last_seq + 1) {
    _test_gaps++;
  }
  _test_last_seq = b.seq;
  _test_banks++;
  const double el = (t - _test_t0_ms) / 1000.0;
  const double expect = el * b.fs / wsdr::Stream8822b::kBankSamples;
  std::ostringstream o;
  o << "{\"rate_msps\":" << b.fs / 1e6 << ",\"seconds\":" << el << ",\"banks\":" << _test_banks
    << ",\"expected\":" << static_cast<uint64_t>(expect) << ",\"seq_gaps\":" << _test_gaps
    << ",\"stream_runs\":" << _test_runs
    << ",\"restarts\":" << _rx->status().restarts << ",\"queue_dropped\":" << _dropped.load()
    << ",\"done\":" << (el * 1000 >= _test_end_ms ? "true" : "false") << "}";
  std::lock_guard<std::mutex> l(_m);
  _test_result = o.str();
  if (el * 1000 >= _test_end_ms) {
    KP_LOGI("test: result %s", _test_result.c_str());
    _mode = Mode::Idle;
    _mode_epoch++;
  }
}

static std::string jstr(const std::string &s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (static_cast<unsigned char>(c) < 0x20) continue;
    o += c;
  }
  return o + "\"";
}

std::string Radio::status_json() {
  std::ostringstream o;
  wsdr::RxStatus rs;
  if (_rx) rs = _rx->status();
  std::lock_guard<std::mutex> l(_m);
  static const char *modes[] = {"idle", "scan", "watch", "test"};
  o << "{\"open\":" << (_open ? "true" : "false") << ",\"usb3\":" << (_superspeed ? "true" : "false")
    << ",\"chip\":" << jstr(_chip) << ",\"rx_state\":" << jstr(wsdr::rx_state_name(rs.state))
    << ",\"rx_error\":" << jstr(rs.error) << ",\"last_fault\":" << jstr(rs.last_fault)
    << ",\"mode\":\"" << modes[static_cast<int>(_mode)] << "\",\"freq_hz\":" << _req_hz
    << ",\"watch_hz\":" << _watch_hz << ",\"fs\":" << rs.fs << ",\"banks\":" << _banks.load()
    << ",\"dropped\":" << _dropped.load() << ",\"decode_dropped\":" << _ddropped.load()
    << ",\"restarts\":" << rs.restarts << ",\"dsp_load\":" << _load << ",\"decode_load\":" << _dload
    << ",\"watch_msps\":" << (_mode == Mode::Watch ? wsdr::Receiver::rate_of(_req_rate) / 1e6 : 0)
    << ",\"fallback\":" << (_fallback ? "true" : "false");
  if (_mode == Mode::Watch && _snr_db >= 0) o << ",\"snr_db\":" << _snr_db;
  if (_mode == Mode::Scan) o << ",\"scan_step\":" << _step << ",\"scan_steps\":" << _plan.size();
  if (_survey) o << ",\"survey\":true,\"sweeps\":" << _grid.sweeps();
  o << ",\"video\":{\"locked\":" << (!_vstats.acquiring && _vstats.lock_recent > 0.5f ? "true" : "false")
    << ",\"lock\":" << _vstats.lock_recent << ",\"std\":" << jstr(_vstats.std_name) << ",\"fps\":" << _fps
    << ",\"acquisitions\":" << _vstats.acquisitions << ",\"losses\":" << _vstats.losses
    << ",\"rephases\":" << _vstats.rephases << ",\"colour\":" << (_vstats.colour ? "true" : "false") << "}";
  o << ",\"test\":" << (_test_result.empty() ? "null" : _test_result);
  o << ",\"pilots\":[";
  const int64_t t = now_ms();
  for (size_t i = 0; i < _pilots.size(); i++) {
    const auto &p = _pilots[i];
    o << (i ? "," : "") << "{\"mhz\":" << p.mhz << ",\"name\":" << jstr(p.name) << ",\"std\":" << jstr(p.std_name)
      << ",\"snr\":" << p.snr_db << ",\"age_s\":" << (t - p.seen_ms) / 1000;
    if (!p.digital.empty()) o << ",\"digital\":" << jstr(p.digital) << ",\"bw\":" << p.bw_mhz;
    o << "}";
  }
  o << "]}";
  return o.str();
}

}  // namespace fpv
