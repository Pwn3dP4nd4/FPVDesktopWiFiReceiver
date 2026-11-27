// SPDX-License-Identifier: GPL-2.0-only
// fpv::Radio, derived from kepyur's native/radio/radio.h (https://github.com/gilankpam/kepyur, GPL-2.0): see radio.cpp.
// One RTL8812BU (wifi-sdr Receiver) + a DSP thread. Modes:
//   scan   - sweep the analog FPV bands at 40 MS/s, list pilots (humps that pass a sync check)
//   watch  - tune to one pilot and decode its video (frames to the FrameSink)
//   test   - stream at a given rate and only count banks (USB/controller soak, no DSP)
// The Receiver's sink only copies banks into a queue (wifi-sdr rule); everything else runs on the DSP thread.
#pragma once
#include "survey.h"
#include <map>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "spectrum.h"
#include "video.h"

namespace wsdr {
class Receiver;
}
namespace kpx {
class DigitalScan;  // src/kp_ext/digital_scan.h
}

namespace fpv {

// kepyur native/dsp types this interface uses
using kp::cf;
using kp::DecoderStats;
using kp::Frame;
using kp::FrameSink;
using kp::FrontEnd;
using kp::Hump;
using kp::Psd;

struct Pilot {
  double mhz = 0;        // measured carrier
  std::string name;      // nearest standard channel ("R4"), "" if none
  std::string std_name;  // NTSC / PAL
  float snr_db = 0;
  float lock = 0;
  int64_t seen_ms = 0;   // steady-clock ms of the last scan that saw it
  // a digital signal, not decoded: kpx::kind_code() ("hdzero", "hdzero_narrow", "dji", "digital"); "" = analog video
  std::string digital;
  float bw_mhz = 0;      // occupied width of a digital signal
};


class Radio {
public:
  explicit Radio(FrameSink frames);
  ~Radio();
  Radio(const Radio &) = delete;
  Radio &operator=(const Radio &) = delete;

  // fd >= 0: the adapter behind this usbfs fd (Android); fd < 0: the first 8812BU on the bus (desktop).
  // Blocking (~5 s chip bring-up). Throws std::exception with a user-presentable message.
  void open(int fd);
  void close();
  bool is_open() const;
  bool superspeed() const;

  // Then watch prefer_hz if found (or, with nothing found, anyway), else the previous pilot again, else the
  // strongest found. prefer_hz 0: the pilot being watched.
  void scan(double prefer_hz = 0);
  // Step centres for scan() in Hz (40 MS/s, +-16.5 MHz each); empty (the default) = the full plan. Takes
  // effect on the next scan().
  void set_scan_plan(std::vector<double> centres_hz);
  // Also list digital signals (HDZero...) found by scan(); off by default. Takes effect on the next scan().
  void set_digital_scan(bool on);
  // Channel survey: sweep the scan plan (set_scan_plan) over and over, listing pilots (digital ones too) and
  // accumulating the spectrum (survey_spectrum) until another mode is set. Never tunes to a pilot.
  // lo_hz..hi_hz (0: the 5.8 GHz default grid): the band the spectrum covers; the first and last steps measure out
  // to its edges (up to +-fs/2 of their centre). spectrum_only: no video or digital checks, no pilots (2.4 GHz).
  void survey(double lo_hz = 0, double hi_hz = 0, bool spectrum_only = false);
  // Survey: sweeps in the sliding average (src/radio/survey.h); applies at once, data kept
  void set_survey_memory(int sweeps);
  std::vector<SurveyBin> survey_spectrum(int *sweeps = nullptr);
  void watch(double hz);
  void stream_test(int rate_sel, double seconds);
  void idle();
  // Debug: the rate watch uses (1 = 40 MS/s, the default; 2 = 20 MS/s); takes effect on the next watch().
  // Also clears a 20 MS/s fallback.
  void set_watch_rate(int rate_sel);
  // Debug: gain for the next tune: < 0 = the chip's AGC, else fixed gain in dB (CCA off, IGI pinned).
  void set_gain(double db);
  // NTSC chroma comb (DecoderOpts::comb), taken at the next watch(): fewer rainbows on fine detail and OSD
  // text, ~15 % more decode CPU. Off by default.
  void set_comb(bool on);
  // Debug: save the next `seconds` of watched IQ as PREFIX.cs16 + PREFIX.json (kp_replay reads them).
  void record(double seconds, const std::string &prefix);

  std::string status_json();   // for the UI (polled)
  std::vector<Pilot> pilots();

private:
  enum class Mode { Idle, Scan, Watch, Test };
  struct Bank {
    std::vector<uint8_t> data;
    uint64_t seq;
    uint32_t gen;
    double fs, centre_hz, shift_hz;
    bool gapped = false;  // wsdr::BankMeta::gapped: not contiguous with the bank before (survey)
  };
  void dsp_loop();
  // ---- dsp_loop(), in parts ----
  enum class Got { Bank, Nothing, Stopped };  // Nothing: no bank in 200 ms; Stopped: the receiver stopped
  Got next_bank(Bank &b);
  void start_watch();  // a new mode: set the radio up for it
  void start_scan();
  void start_test();
  void account_load(std::chrono::steady_clock::time_point t0);  // the DSP thread's load, every second
  void decode_loop();
  void on_bank(const uint8_t *b, size_t n, uint64_t seq, uint32_t gen, double fs, double centre, double shift,
               bool gapped);
  void tune(double hz, int rate_sel);
  void handle_watch(Bank &b);
  bool watch_fault(double hz);
  void measure_snr(const std::vector<cf> &iq, double fs);
  void record_bank(const Bank &b);
  void abort_record(const char *why);
  void handle_scan(Bank &b);
  // ---- handle_scan(), in parts (DSP thread) ----
  // The step's samples: after a retune, kSettleBanks banks are dropped while the chip settles; then contiguous
  // banks up to what _phase needs (a gap starts the capture over). true: the capture is complete
  bool capture(const Bank &b);
  // What the spectrum step reads of the survey, in one lock
  struct SurveyView {
    bool survey = false, spectrum_only = false;
    double floor_hint = 0, gain = 0, grid_lo = 0, lo = 0, hi = 0;
  };
  SurveyView survey_view(double req) const;
  // The step's spectrum (the first kPsdSpan): its candidates (humps), the survey's measure and the digital scan.
  // true: the step is done now; false: it goes on (a sync check of its candidates, or the same step again streaming)
  bool spectrum_step(double req);
  void survey_step(const std::vector<cf> &x, double req, const SurveyView &sv);  // measure and merge into _grid
  void skip_known_humps();  // survey: humps checked in the last few sweeps, not again
  void handle_test(Bank &b);
  void scan_step_done();
  // ---- scan_step_done(), in parts ----
  struct SurveyKnown;
  // the pilots of this step: its humps with video (sync check) and the digital scan's; recheck: what each check
  // found goes to checked (survey)
  std::vector<Pilot> step_pilots(double centre, bool recheck, std::vector<SurveyKnown> &checked);
  // (under _m) into _survey_known and _pilots; survey: an analog pilot's sideband copy is dropped
  void merge_pilots(std::vector<SurveyKnown> &checked, const std::vector<Pilot> &found);
  void end_sweep();  // (under _m) survey: the sweep is over; pilots not seen for a while are gone
  void end_scan();   // (under _m) scan: watch the pilot it found (or the one before), or go idle
  void add_cut_steps(const std::vector<float> &psd_db);
  void unpack(const Bank &b, std::vector<cf> &out);

  FrameSink _frames;
  std::unique_ptr<wsdr::Receiver> _rx;
  mutable std::mutex _m;  // mode, pilots, status fields below
  Mode _mode = Mode::Idle;
  uint64_t _mode_epoch = 0;  // bumped on every mode change; the DSP thread restarts its state
  double _watch_hz = 0, _req_hz = 0;
  int _req_rate = 1;
  int _watch_rate = 1;
  bool _fallback = false;  // 40 MS/s kept faulting during a watch: watch at 20 MS/s until open() / set_watch_rate()
  double _gain_db = -1;
  bool _comb = false;
  bool _watch_comb = false;  // _comb as of the current watch (DSP thread only)
  // recording (DSP thread writes; the request is under _m)
  double _rec_req_s = 0;
  std::string _rec_req_prefix;
  FILE *_rec = nullptr;
  std::string _rec_prefix;
  uint64_t _rec_left = 0, _rec_samples = 0;
  double _rec_fs = 0, _rec_centre = 0, _rec_shift = 0;
  // stream gaps while recording: sample index in the file after each one
  uint64_t _rec_seq = 0;
  uint32_t _rec_gen = 0;
  std::vector<uint64_t> _rec_gaps;
  std::vector<Pilot> _pilots;
  bool _superspeed = false, _open = false;
  std::string _chip;

  // bank queue
  std::mutex _qm;
  std::condition_variable _qcv;
  std::deque<Bank> _q;
  std::vector<std::vector<uint8_t>> _pool;
  size_t _qcap = 512;  // 100 ms at 40 MS/s
  std::atomic<uint64_t> _banks{0}, _dropped{0};

  std::thread _dsp;
  std::atomic<bool> _stop{false};

  // decode thread: watch mode runs demod on the DSP thread and the video decoder here (two cores)
  struct DBlock {
    std::shared_ptr<FrontEnd> fe;
    std::vector<float> d;
  };
  std::thread _decoder;
  std::mutex _dqm;
  std::condition_variable _dqcv;
  std::deque<DBlock> _dq;
  std::vector<std::vector<float>> _dpool;
  std::atomic<uint64_t> _ddropped{0};
  double _dload = 0;

  // DSP-thread state
  uint64_t _epoch_seen = ~0ull;
  std::shared_ptr<FrontEnd> _fe;
  uint32_t _fe_gen = ~0u;
  std::deque<int64_t> _watch_faults;  // ms of the stream restarts seen in this watch at 40 MS/s
  // live SNR of the watched carrier: a spectrum of kPsdSpan of IQ every kSnrEveryMs
  std::unique_ptr<Psd> _snr_psd;
  size_t _snr_have = 0, _snr_need = 0;
  int64_t _snr_t_ms = 0;
  float _snr_db = -1;  // < 0: none yet (under _m)
  double _fe_fs = 0, _fe_shift = 0;
  std::vector<cf> _iq;
  // scan
  std::vector<double> _plan;
  size_t _step = 0;
  // What the scan does with the banks of the current step
  enum class StepPhase {
    Retuned,    // just retuned: the next bank starts the step
    Settling,   // dropping _settle more banks while the chip settles
    Spectrum,   // capturing kPsdSpan for the spectrum
    SyncCheck,  // the spectrum has candidates: capturing kCapture for the video sync check
  };
  StepPhase _phase = StepPhase::Retuned;
  int _settle = 0;
  uint64_t _cap_last_seq = 0;
  std::vector<std::complex<int16_t>> _cap;
  double _cap_centre = 0;
  std::vector<Hump> _humps;  // this step's candidates (from the first kPsdSpan of the capture)  // hardware centre of the capture (humps are measured from it)
  double _resume_hz = 0;
  std::vector<double> _cut_steps;  // extra steps this scan added for signals cut by a step's edge (add_cut_steps)
  std::vector<std::pair<double, double>> _whole_spans;  // Hz: humps seen whole in this scan (they need no extra step)
  int64_t _scan_start_ms = 0;      // when this scan began: a pilot seen before it is not "still there"
  std::vector<double> _scan_plan_override;     // set_scan_plan() (under _m)
  std::unique_ptr<kpx::DigitalScan> _digital;  // digital signals in this scan, or null
  bool _digital_on = false;                    // set_digital_scan() (under _m)
  // survey (accumulators under _m)
  bool _survey = false;
  bool _survey_spectrum_only = false;   // survey(): only the spectrum (under _m)
  double _survey_lo = 0, _survey_hi = 0;  // survey(): the band (Hz; 0 = the default grid) (under _m)
  SurveyGrid _grid;
  // survey: how this step is taken (DSP thread; tune() reads it). Gapped: one bank at a time, for the spectrum;
  // Recheck: again streaming, to check its candidates; DigitalRecheck: again streaming only for the digital scan's
  // timing (no new humps)
  enum class SurveyPass { Gapped, Recheck, DigitalRecheck };
  SurveyPass _pass = SurveyPass::Gapped;
  std::map<long, double> _survey_gain;  // survey: gain per step (kHz -> dB), lowered where it clips (under _m)
  struct SurveyKnown {  // survey: a hump already checked (video or not), not checked again for a few sweeps
    double mhz;
    bool video;
    int sweep;
  };
  std::vector<SurveyKnown> _survey_known;  // (under _m)
  double survey_gain(double hz) const;     // (under _m)
  int64_t _sweep_start_ms = 0, _sweep_ms = 0;  // survey: how long a sweep takes
  // test
  uint64_t _test_banks = 0, _test_gaps = 0, _test_last_seq = 0, _test_runs = 0;
  uint32_t _test_gen = 0;
  int64_t _test_t0_ms = 0, _test_end_ms = 0;
  double _test_fs = 0;
  std::string _test_result;
  // load
  double _busy_s = 0, _load = 0;
  int64_t _load_t0_ms = 0;
  uint64_t _status_ticks = 0;
  // video stats for status
  DecoderStats _vstats;
  uint64_t _frames_at_last = 0;
  int64_t _fps_t0_ms = 0;
  float _fps = 0;
};

int64_t now_ms();

}  // namespace fpv
