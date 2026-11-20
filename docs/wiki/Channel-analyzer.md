# Channel analyzer

***English** · [Español](Analizador-de-canales)*

The analyzer (*Tools → Channel analyzer*) sweeps the 5.8 GHz band with the adapter and recommends which channels
to use for several analog and HDZero pilots at once, so that they do not step on each other or on whatever is
already on the air.

> This page explains **what the recommendation is based on**. How to use the window is in the
> [README](https://github.com/Pwn3dP4nd4/FPVDesktopWiFiReceiver#channel-analyzer).

## What it measures

With a fixed gain, for every 156 kHz of the band it keeps three things, averaged over the last sweeps
(**Remember**: 5, 10, 20 or 40):

- **Average level** over the noise floor: whatever is always there (a VTX, a busy router).
- **Strongest recent burst**, which fades over time: it gives away what comes and goes.
- **Share of time on**: how much of the time something intermittent hits that slice of the band.

## How it chooses

Each candidate channel gets a score, and the lowest total wins:

1. **Average interference** within its width (analog 20 MHz, HDZero 27 MHz or 17 in narrow mode).
2. **Bursts**: the share of time something intermittent hits it (WiFi and the like). The average alone hides them.
3. **WiFi networks seen**, even while they are quiet (see below).
4. A channel where **a pilot is already flying** is ruled out. A pilot on a pinned channel is yours already (see
   below); any other of your own VTXs can be ticked in *On the air* so it does not count.

Between the chosen channels it requires a minimum spacing, penalises going under the ideal one, avoids 3rd-order
intermodulation between analog pilots and, on a clean band, spreads them as far apart as possible.

## WiFi networks

5 GHz WiFi is the most common interference at 5.8 GHz, and it has a catch: **a router with no traffic is almost
invisible**. It only sends a *beacon* (a short packet) about ten times a second, so it barely touches the channel
and its average is close to the noise. As soon as someone uses that network (a video, a download), the channel
becomes unusable. That is why the analyzer does not only look at how busy each channel is now, but also at
**which networks are there**:

- **How it recognises them**: a channel of the WiFi grid (5000 + 5·n MHz) that lights up in bursts across nearly
  all of its width (~17 MHz), clearly above the noise peaks. A narrow burst, a pilot (which transmits all the
  time) or the spill of a strong signal onto its sides do not count.
- **It remembers them for 5 minutes**: each step of the sweep measures a few milliseconds, so a quiet router is
  only caught now and then. Once seen, a network keeps counting for up to 5 minutes, even if it does not show in
  every sweep. Starting a new analysis forgets them all.
- **How much they weigh**: depending on how strong the network is, up to 6 dB on the channels it covers.
- **Their width**: a router on 40 or 80 MHz sends its beacons only on its primary channel; the rest of its block
  lights up only with traffic (checked with the adapter). While no traffic has been seen on it, its whole block
  counts half. Once traffic is seen, what it uses is known and what it does not use stops counting.
- **WiFi rules** (Europe / USA and others): in Europe channels 144 and 169–177 do not exist, so a router on
  132–140 is at most 40 MHz wide and one on 165 stands alone. By default it follows the computer's regional
  settings.

In the graph they show as **WiFi 149** (or **WiFi 100–112** for an 80 MHz network), with their block dashed while
their width is unknown.

## Pinned channels and race tracking

**Pinning channels.** Once you have given channels to some pilots, pin them: tick the **Pin** box on their row of
the recommendation table, or right-click a channel mark on the graph to pin any other one (also one that is not
recommended). Pinned channels are always part of the recommendation, even if busy, noisy or outside the chosen
bands, and the search only picks the missing ones, keeping spacing and intermodulation with them: with 4 pilots
and 2 pinned, it looks for 2. On the graph they have a stronger band with marked edges and a pin; in the summary,
*Pinned* is listed apart from *Suggested for the rest*. They are saved and kept between analyses and sessions. **Unpin all** clears them.

A pilot on the air on a pinned channel (same system, within 5 MHz) is always yours: it does not take its own channel
or count as interference, and in the *On the air* list it shows ticked and greyed out, *(pinned)*. The tick in that
list is for your other pilots, the ones not pinned yet: their channel is not taken and the advice may give them
another one, or put someone else next to them, as if they were going to move too. If one of them is already on a
recommended channel, its row says *your pilot … is already here*; if not, the summary says it must change channel.
If it is going to stay where it is, pin its channel instead. The tick lasts one analysis.

**Race tracking** (*Race* tab). With pinned channels and the analysis running, tick **Race tracking** when the race
starts: the analyzer keeps sweeping and, sweep by sweep, records for each pinned channel:

- the **SNR** of its pilot (median and the worst moments, 10th percentile);
- **dropouts**: sweeps in which its pilot is missing, between the first and the last time it was seen (take-off
  and landing do not count);
- **intruders**: the share of sweeps in which someone else's signal (matching no pinned channel) overlaps it;
- the **interference** on its channel, with its own pilot's signal removed (noise, bursts, remembered WiFi);
- an estimated video **quality** (the worst SNR minus what the channel and the dropouts take away). The worst one
  is highlighted.

For the first 10 sweeps it is learning. After that, if one of the pilots with the worst
video would clearly do better (at least 3 dB) on another free channel compatible with the other pinned ones, it
says so ("Move the pilot on R3 to R2: about +X dB", with the cause: someone else transmitting on top of it, or
interference or WiFi on its channel). **Apply** changes that pinned channel (tell the pilot to switch) and restarts
only its tracking; the others keep their history. Changing the pinned channels by hand restarts the tracking.

What it cannot see:

- **Interference right under the carrier of a pilot that is transmitting**: it is removed together with its
  signal. It shows in the sweeps where that pilot is missing, in the remembered WiFi networks and in whatever
  sticks out beyond its width.
- **Low SNR or dropouts on a clean channel are not fixed by changing channel**: that is the antenna, the VTX power
  or the distance. In that case it does not suggest a move and says so.

## Ideas taken from Ryan Quellet's video

Part of this comes from the video
[*FPV Range Secrets Revealed – Analog, HDZero, Divimath*](https://youtu.be/Yqq9Sd2aWXg) (Ryan Quellet,
DiviMath/HDZero), which gives advice on choosing channels and getting range. Which ideas were used, and how:

| Idea from the video | Time | In the analyzer |
|---|---|---|
| Look at the spectrum with a WiFi analyzer and pick the channel by what is there | [14:55](https://youtu.be/Yqq9Sd2aWXg?t=895) | That is what the analyzer does, but measuring the FPV band with the adapter itself instead of a phone |
| Avoid the channels that overlap common 5 GHz WiFi (R3–R6) | [17:17](https://youtu.be/Yqq9Sd2aWXg?t=1037) | Not as a fixed rule: the networks actually present are measured and penalised even while quiet |
| A router on 40/80 MHz takes several channels | [14:55](https://youtu.be/Yqq9Sd2aWXg?t=895) | Its block is taken into account while its width is unknown |
| DFS channels (R1, R2, Low Race) are "safe" because the router has to move away | [15:56](https://youtu.be/Yqq9Sd2aWXg?t=956) | **Not used**: DFS makes a router move away from a radar, not from a VTX. A network on a DFS channel counts the same |
| HDZero narrow mode (17 MHz instead of 27) for more range | [11:08](https://youtu.be/Yqq9Sd2aWXg?t=668) | Already there: the *HDZero narrow mode* checkbox |
| VTX output power is not the same on every channel (higher in the middle) | [17:37](https://youtu.be/Yqq9Sd2aWXg?t=1057) | **Not used**: there are no reliable measurements for each VTX |
| A powerful ELRS/Crossfire transmitter next to the receiver overloads it | [11:48](https://youtu.be/Yqq9Sd2aWXg?t=708) | **Not detected yet**: it raises the noise of the whole band evenly, and the analyzer measures over the noise |
| Low Race only with authorisation | [18:19](https://youtu.be/Yqq9Sd2aWXg?t=1099) | The L band is unticked by default |

## Checked with the adapter

With an RTL8812BU on USB 3:

- Noise peaks are not negligible (half of the band reaches ~6 dB and there are whole stretches at 7–9 dB), so
  the threshold to recognise a network follows the noise at each moment.
- A 20 MHz network with traffic is recognised on the first sweep, with its typical shape (16.6 MHz with a gap in
  the middle).
- A router on 80 MHz streaming a video lights up its whole block (80 MHz), not just its primary channel.
- A strong signal spills onto its sides (in the case measured, from 24 dB down to 13 dB over the next 25 MHz);
  that is not taken for another network.

## Limitations

- **What the analyzer sees is where the adapter is.** If the goggles or the receiver are somewhere else, what
  they get may differ.
- **A router that has never been caught does not count.** Let the analysis run for at least a minute before
  trusting the recommendation.
- **The analyzer sweeps 5300 to 6000 MHz.** Networks on 5 GHz channels 36–64 are mostly outside it, but they do
  not reach any FPV channel either.
- **It does not detect overload from a powerful transmitter nearby.** If you fly with the radio at high power,
  keep it away from the adapter and the goggles.

## Regulations

In the EU, 5.8 GHz FPV video goes in the short-range band **5725–5875 MHz at 25 mW EIRP**. R1, R2, R8 and the
L band are outside it. Check what is allowed where you fly: the analyzer recommends among the bands you tick, it
does not check whether they are legal.
