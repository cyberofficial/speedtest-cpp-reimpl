# Speedtest Windows client, C++ reimplementation

An independent C++20 reimplementation of the Speedtest by Ookla Windows desktop
client: the measurement engine and the Direct2D user interface.

This project is **not affiliated with, endorsed by, or connected to Ookla**. No
Ookla code, binaries, assets or trademarks are included in this repository. See
[Provenance](#provenance).

## What it is

* A portable single EXE. No installer, no runtime prerequisites, static CRT (`/MT`).
* Win32 window with a Direct2D and DirectWrite user interface.
* WinHTTP for HTTP transport, WS2_32 for raw TCP, BCrypt for hashing.
* Measures latency, jitter, download and upload against Ookla-compatible
  endpoints, discovering servers from the public server list.
* Writes results to `history.tsv` beside the executable, never to `%LOCALAPPDATA%`.

## Why this exists

This was built out of affection for the original, not out of spite toward it.

The Speedtest desktop client is genuinely good software, and for a lot of people it
is how they first found out what their connection was actually doing. It is also a
Unity application tied to an installer and a runtime, built for a narrow band of
Windows versions, and it is awkward or impossible to get running on plenty of
hardware people still own: older machines, stripped down or embedded Windows
images, locked down systems where nothing may be installed, and platforms the
original never targeted.

So the purpose here is a simple one. Keep the functionality usable on hardware and
architectures the original does not support, and leave behind source that is small,
self contained and easy to retarget. One portable executable with nothing to
install, and a codebase a person can read in an afternoon and rebuild for something
else.

That is the entire motivation. There is no interest in passing this off as Ookla's
work, in suggesting any connection with them, or in taking anything away from them.
The protocol work exists so this program can talk to the service, not so it can
replace or impersonate it. It is a rebuild made by someone who liked the tool and
wanted it to keep working.

Honest scope note, so that the intent is not mistaken for a claim: this particular
build targets 64 bit Windows 10 and 11, which is what the original targets as well.
It does not run on ARM64 or on 32 bit Windows today. Getting it there means
rebuilding the source, which is the reason the source is published, and nothing
here claims that support already exists.

## Repository layout

```
CMakeLists.txt          top-level build (defines the console target)
src/main.cpp            command line, self-test modes, JSON output
src/engine.cpp/.h       measurement engine, stage machine, progress callbacks
src/http_client.cpp/.h  WinHTTP wrapper, extra request headers
src/json.cpp/.h         minimal JSON reader/writer
src/loopback_server.*   loopback HTTP server used by the offline self-test
src/share.cpp/.h        result share upload (see "Share" below)
src/ui/CMakeLists.txt   defines the GUI target
src/ui/ui_main.cpp      entry point and message loop
src/ui/ui_app.cpp/.h    window procedure, engine driving, history persistence
src/ui/ui_draw.cpp/.h   all Direct2D drawing
src/ui/ui_model.h       UI state model
src/ui/ui_share.cpp/.h  share button, clipboard, toast
src/ui/ui_theme.h       colours, fonts, layout constants
```

## Build

Requirements:

* Visual Studio 2022 (or Build Tools) with the C++ workload and a Windows 10 SDK
* CMake 3.20 or newer

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

Both targets build by default:

| Output | Subsystem | Purpose |
|---|---|---|
| `build\bin\speedtest.exe` | console | headless measurement, self-tests, JSON output |
| `build\bin\speedtest-ui.exe` | GUI | the windowed application |

`src/ui` can also be configured on its own:

```bat
cmake -S src/ui -B src/ui/.build -G "Visual Studio 17 2022" -A x64
```

## Run

Copy `speedtest-ui.exe` anywhere and double-click it. It is self-contained, and
its `history.tsv` is created next to the executable. If that folder is not
writable the app still runs, shows results on screen, and displays
`History is not writable next to the executable; results will not be saved.`

`history.tsv` is tab separated with no header row:

```
when                 ping_ms   download_mbps   upload_mbps   demo
10.13.26|03:09 PM    27.931    110.773         10.938        0
```

`demo` is `1` only for the visibly flagged demo mode, otherwise `0`. The file is
capped at 50 rows and lines with fewer than 4 fields are ignored.

### Headless modes

These flags belong to `speedtest.exe`, the console binary. **The GUI binary is a
Windows GUI subsystem executable: it ignores command line flags and opens its
window instead.** Check the subsystem before scripting either one.

```
speedtest 0.2.0 (C++20 reimplementation, headless)
usage: speedtest.exe --selftest [--offline [--force-json-fallback]] [--duration SEC]
                     [--server ID] [--threads N] [--upload-threads N] [--config-url URL]
                     [--quiet]
       speedtest.exe --http-check
       speedtest.exe --share-test

  --selftest          run one full measurement, print JSON on stdout
  --offline           run against the built-in loopback HTTP server
  --force-json-fallback  make the XML config lookup fail so the JSON fallback runs
  --duration SEC      download and upload stage length (default 10, offline 3)
  --server ID         server id / host / url / name / sponsor substring
  --threads N         parallel download connections (default 24, server-capped)
  --upload-threads N  parallel upload connections (default 2)
  --config-url URL    override the config endpoint (default api/windows/config.php)
  --http-check        User-Agent regression check against the config endpoint
  --share-test        upload a SYNTHETIC result to the share endpoint and report
  --quiet             do not print progress lines to stderr

exit codes: 0 ok, 2 usage, 3 measurement failed, 4 offline self-check failed,
            5 http-check failed, 6 share upload failed
```

`--selftest --offline` needs no network at all: it drives the full stage machine
against a loopback HTTP server built into the binary.

## Share

The results screen has a `SHARE` control. Pressing it uploads the result to
Ookla's reporting endpoint and copies a link of the form
`https://www.speedtest.net/my-result/d/<guid>` to the clipboard, then shows
`Sharing link copied to clipboard.` This matches the original client, which also
copies the link rather than opening it.

* The upload runs **only** when the button is pressed. Nothing is sent at
  startup, after a measurement, or on a timer.
* It is the only request the application makes to a host other than the
  measurement server and `www.speedtest.net`.
* If the upload fails, the success toast is replaced within about a second by
  `Could not share result.` with the underlying error. The clipboard still holds
  the link, which is the original client's behaviour.
* `--share-test` exercises the same code path headlessly with a synthetic result.

## Verification status

Independently verified on Windows 10 x64 against
`sha256 C943D5D0B2CD85EF9CD9EF43921AFFF6796C65BC6A02EDB92DCC3F156ACA4B06`
(470,016 bytes, subsystem 2, GUI):

| Check | Result |
|---|---|
| Runs as a lone EXE in a clean folder, no install | PASS |
| Cold start, window titled `Speedtest` | PASS, 388 to 566 ms |
| Writes no data to `%LOCALAPPDATA%` | PASS |
| History written beside the executable, not the working directory | PASS |
| x64, OS-shipped imports only, no `VCRUNTIME140`/`MSVCP140`/`api-ms-win-crt` | PASS |
| No installer traces anywhere (no ARP entry, no install dir, no MSI) | PASS |
| Share button produces a real, publicly viewable result link | PASS |
| Clipboard receives the link, read back from a separate process | PASS |
| Share failure shows a failure instead of a success toast | PASS |
| No upload traffic without a button press | PASS |
| Live throughput is a running average that converges on the recorded value | PASS |
| Header value, needle and dial centre never disagree | PASS |

### Not verified

* A genuinely clean Windows 10 or 11 machine. All evidence is from one host.
* SmartScreen behaviour for an unsigned downloaded EXE.
* A read-only or network-share executable folder. The warning message exists but
  was never exercised.
* Behaviour behind a proxy or captive portal.
* Throughput parity with the original client. Our runs land in the 53 to 85 Mbps
  range against a stored original result of 135.49 Mbps on the same server. The
  gap is only partly explained.
* The ping delta against the original's recorded 23 ms.
* Mid-phase on-screen latency value, and needle angle at pixel level.

## Provenance

Built by reverse engineering a shipped 2019-era Windows MSI using Ghidra.
The measurement protocol, the UI geometry and the result share protocol were
recovered from the original application's managed assembly and its native engine
library. The share request format was validated against the live endpoint, and
the reimplementation sends a full result payload so the server stores it
properly rather than dead-lettering it.

This repository deliberately excludes the original MSI, the extracted
application, decompiled output, and any of the original's artwork or fonts.

## Third-party code

None. The build uses only the Windows SDK and the C++ standard library.

## Legal

### License

Released under the **Cyber Official Permissive License 1.0 (COPL-1.0)**. The
operative text is in [`LICENSE`](LICENSE); that file is the only version that
counts, and this summary is a convenience rather than a substitute for it.

In plain terms: anyone may use, copy, modify, adapt, publish and redistribute this
software, on any device, for any non-commercial purpose, and may build on it
however they like. Two conditions apply. The copyright notice and the license must
travel with the software, and no trademark rights are granted.

**Selling is not permitted.** You may not sell, resell, sublicense, rent, lease or
otherwise charge anyone for this software or for a derivative work, and you may not
bundle it into a product or service offered for a fee. Commercial use requires
prior written permission from the copyright holder. Asking costs nothing.

### What the license does, and what it cannot do

Stated plainly, because overstating this would be worse than saying nothing.

**What it does.** Between the copyright holder and a user who is actually bound by
it, the license disclaims all warranties, limits liability, requires the user to
indemnify the holder for claims arising from the user's own use, and places the
risk of use on the user. The warranty disclaimer uses the standard formalities: it
is conspicuous and it expressly names merchantability, fitness, title and
non-infringement, which is what makes that kind of clause work.

**What it cannot do.** It governs the relationship between the copyright holder and
a user. It does not bind anyone else, and it does not reduce exposure to a claim
brought by a third party. A contract cannot bind a nonparty, and a third party's
copyright or trademark claim is unaffected by anything written here. Anyone who
tells you a license clause solves that problem is wrong.

**Two further limits, said out loud.** Whether a repository license file binds a
given user at all turns on notice and assent, and there is no clickwrap, installer
or first-run prompt here, so that is a genuine weak point rather than a solved
problem. And a limitation of liability is enforced most reliably between commercial
parties of comparable bargaining power; it is at its most vulnerable when it is
imposed on a consumer.

### Not for production or safety-critical use

This software is an educational and experimental study. It is **not designed,
tested, certified or warranted for use in a production environment**, and it is not
designed or warranted for use where an inaccurate measurement could cause harm.

That includes production networks, commercial service delivery, capacity planning,
billing, service-level reporting, medical devices, industrial control, and any
safety-critical or life-critical system.

If you use it in such an environment, you do so entirely at your own risk and on
your own judgement, you are solely responsible for independently validating and
hardening it, and the warranty disclaimer, the limitation of liability and the
indemnity apply in full.

This is written as a statement of scope and a warning rather than a flat
prohibition, and that choice is deliberate: the warning is the form that has legal
effect, it maps onto the adequate-warning provisions at `N.J.S.A. 2A:58C-3(a)(3)`
and `N.J.S.A. 2A:58C-4`, and it is conventional drafting for software of this kind.
A prohibition would not have protected anyone and would have contradicted the grant
in section 1 of the license.

### Governing law and jurisdiction

The license is governed by the laws of the State of New Jersey, United States,
without regard to its conflict of law rules, and the state and federal courts
located in New Jersey have exclusive jurisdiction over any dispute arising out of
or relating to it. That binds the copyright holder and a user who is bound by the
license. It cannot bind a third party, who may bring a claim wherever the law
allows.

On enforceability, stated accurately rather than favourably. New Jersey enforces
warranty disclaimers and limitations of liability most reliably in the ordinary
commercial case between parties of comparable bargaining power (`Chatlos Systems
v. NCR Corp.`, 635 F.2d 1081 (3d Cir. 1980)). They are refused when the clause is
unconscionable or is an adhesion term against a consumer with unequal bargaining
power, which is what happened in `Lucier v. Williams`, 366 N.J. Super. 485 (App.
Div. 2004), where the limitation was struck down and the court held it immaterial
that the plaintiff had agreed to it. They are also refused where the clause touches
a statutory duty or purports to cover recklessness, wilful misconduct or gross
negligence (`Stelluti v. Casapenn Enterprises`, 203 N.J. 286 (2010)), and a cap on
personal-injury damages in a consumer contract is prima facie unconscionable under
`N.J.S.A. 12A:2-719(3)`.

None of that is a guarantee of any outcome.

### Trademarks and affiliation

Not affiliated with, endorsed by, sponsored by, or approved by **Ookla, LLC**, and
not a product of Ookla. This is an independent reimplementation.

Ookla, LLC holds live United States federal registrations including `OOKLA`
(Reg. 4239996), `OOKLA & Design` (Reg. 5306685), `SPEEDTEST & Design`
(Reg. 4982850) and a design mark for the Speedtest gauge (Reg. 4913264). The bare
word mark `SPEEDTEST` (Reg. 4047266) is registered to a different owner and sits on
the Supplemental Register, which is a weaker form of registration.

Those names appear in this repository only to describe, factually, what this
software interoperates with. No trademark rights are granted by the license, and
no permission to use any Ookla mark is claimed or implied. Whether a particular use
qualifies as nominative fair use is a fact-specific question; in the Third Circuit,
which covers New Jersey, the governing test is from `Century 21 Real Estate Corp.
v. Lendingtree, Inc.`, 425 F.3d 211 (3d Cir. 2005).

For context rather than reassurance: documented instances of Ookla asserting its
rights include a trademark complaint about Firefox add-ons that used the Speedtest
name and gauge logos, and a domain dispute over `speedtest.org` that Ookla **lost**,
with the panel finding reverse domain name hijacking. No litigation brought by
Ookla, no DMCA notice and no cease and desist letter were found in the sources
searched.

### Third-party services are your responsibility

The software contacts endpoints operated by Ookla and by third-party measurement
servers that this project does not run. Ookla's Terms of Use governs
`www.speedtest.net` and `napi.speedtest.net` through its definition of "Services",
and it is not conditioned on holding an account: using the service is treated as
acceptance. The `/speedtest/` paths on individual measurement servers are governed
by whatever terms that server's operator sets.

Their availability, their accuracy and their terms are outside this project's
control, and complying with them is your responsibility, not the copyright
holder's.

### What actually reduces exposure

The research behind this section is blunt on one point: **no clause that can be
added to a license reduces exposure to a third party's intellectual property
claim.** Only factual and engineering choices do.

What this project does, deliberately:

* Contains no Ookla code, binaries, assets, fonts or artwork. The reimplementation
  was written from observed behaviour and protocol facts, not from their source.
* Reimplements functional and interface behaviour rather than reproducing
  protected expression, which is the ground addressed in `Sega v. Accolade`,
  977 F.2d 1510 (9th Cir. 1992), `Sony v. Connectix`, 203 F.3d 596 (9th Cir. 2000)
  and `Google v. Oracle`, 593 U.S. 1 (2021).
* Keeps a provenance record of how the behaviour was determined.
* Claims no rights in any third-party mark, and ships no logo or gauge artwork from
  the original.

The largest remaining risk is not liability. It is trademark, because the project
is named after another company's product and its interface shows a wordmark and a
gauge. Renaming the project and dropping the wordmark and gauge would reduce that
risk far more than any wording in this file. That change has not been made, and the
exposure is real.

### Why the license is non-commercial

This is a deliberate choice by the copyright holder, not an oversight, and the
reasoning is worth stating so nobody misreads it.

**The software is free to use, and selling it is not allowed.** Anyone may use,
study, modify and share it. Nobody may sell it or charge for it, or fold it into
something they charge for, without asking first. That applies to derivative works
too, so a modified version cannot be sold either.

**Being straight about what this does and does not achieve.** A term like this
governs what a user may do. It does not reduce the copyright holder's exposure to
a claim, and it is not a liability shield. The clauses that do that work are the
warranty disclaimer, the limitation of liability and the indemnity, and even those
reach only a user who is bound by the license.

**And being straight about the cost.** A non-commercial license is not an
open-source license in the OSI sense, and some people and most companies will
decline to use software licensed this way, because their legal teams cannot accept
the uncertainty. That is a real tradeoff and it was made with eyes open: the point
of the term is that nobody profits from this work without the author's agreement,
and that is worth more here than maximum adoption.

If the copyright holder ever wants to allow commercial use, the grant in section 1
of the license is the single place to change.

### Not legal advice

Written to be accurate and readable. It is not legal advice and creates no
attorney-client relationship. For certainty about your own situation, consult a
licensed attorney in your jurisdiction.
