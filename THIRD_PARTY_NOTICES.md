# Third-Party Notices

XOA incorporates the following third-party libraries. Their respective
licenses and copyright notices are reproduced or referenced below.

---

## JUCE Framework

- **Website**: https://juce.com
- **License**: AGPLv3 or commercial JUCE License
- **Copyright**: Raw Material Software Limited

The JUCE Framework modules are dual-licensed under the AGPLv3
(https://www.gnu.org/licenses/agpl-3.0.en.html) and the commercial JUCE 8
End User Licence Agreement (https://juce.com/legal/juce-8-licence/).

XOA uses JUCE under the AGPLv3, which is compatible with GPLv3.

JUCE bundles additional dependencies with their own licenses. See
`ThirdParty/JUCE/LICENSE.md` for the full list.

---

## Steinberg ASIO SDK

- **Website**: https://www.steinberg.net
- **License**: Steinberg proprietary or GPLv3 (dual license)
- **Copyright**: (c) 2025 Steinberg Media Technologies GmbH

Used under the GPLv3 path. Since JUCE 8.0.11 the ASIO SDK ships bundled
with JUCE; full license text in
`ThirdParty/JUCE/modules/juce_audio_devices/native/asio/LICENSE.txt`.

THE SDK IS PROVIDED BY STEINBERG MEDIA TECHNOLOGIES GMBH "AS IS" AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL STEINBERG MEDIA TECHNOLOGIES GMBH BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

---

## HIDAPI

- **Website**: https://github.com/libusb/hidapi
- **License**: GPLv3, BSD, or original HIDAPI license (at user's discretion)
- **Copyright**: (c) Alan Ott, Signal 11 Software / libusb/hidapi Team

Used under the GPLv3 path, consumed as a git submodule at
`ThirdParty/hidapi`. Full license texts in `ThirdParty/hidapi/LICENSE*.txt`.

---

## ROLI Blocks Basics

- **Website**: https://github.com/WeAreROLI
- **License**: ISC
- **Copyright**: (c) 2020 ROLI Ltd.

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND ROLI LTD DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS. IN NO EVENT SHALL ROLI LTD BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.

---

## juce_simpleweb

- **Website**: https://github.com/benkuper/juce_simpleweb
- **License**: GPLv3
- **Author**: Ben Kuperberg

JUCE module providing HTTP and WebSocket server functionality using
SimpleWeb and standalone ASIO. Used by spatcore's MCP transport (and by the
OSCQuery WebSocket transport once ported). Built with
`SIMPLEWEB_SECURE_SUPPORTED=0` (no TLS code paths, no OpenSSL runtime
dependency).

Includes standalone ASIO (https://github.com/chriskohlhoff/asio) under the
Boost Software License 1.0.

---

## spatcore

- **Repository**: https://github.com/pob31/spatcore
- **License**: follows the consumer projects' licensing (GPLv3 here)
- **Copyright**: (c) Pierre-Olivier Boulant

The shared real-time spatial-audio core extracted from WFS-DIY, consumed as
a git submodule at `spatcore/`.
