# Third-Party Notices

`libmimhid` itself is licensed under the MIT License (see [`LICENSE`](LICENSE)).

This document records the third-party software used in or alongside this
repository, separated by distribution artifact, and reproduces the full
copyright notices, license conditions, and disclaimers required when
redistributing source or prebuilt firmware binaries.

---

## 1. Host Library Archives (`libmimhid-<version>-<platform>-<arch>-*`)

The host library (`libmimhid.so`, `libmimhid.dylib`, `mimhid.dll`,
`libmimhid.a`, `mimhid_static.lib`) is built entirely from first-party MIT
source code (`src/`, `include/`, `proto/`).

- **Third-party library dependencies**: None. Serial I/O and port enumeration
  use native OS interfaces (`termios` / `sysfs` on Linux, `termios` / `IOKit`
  on macOS, Win32 overlapped file I/O and `SetupAPI` on Windows).
- **C/C++ runtime**: On Linux, `libstdc++` and `libgcc` are linked statically
  with `-static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL` under the
  [GCC Runtime Library Exception, version 3.1](https://www.gnu.org/licenses/gcc-exception-3.1.html),
  which permits propagating the resulting binary under `libmimhid`'s MIT
  License because the compilation process uses unmodified GCC.

---

## 2. Source Tree Third-Party Files

### 2.1 `firmware/pico_sdk_import.cmake` (Raspberry Pi Pico SDK CMake locator)

- **Origin**: `external/pico_sdk_import.cmake` from [`raspberrypi/pico-sdk`](https://github.com/raspberrypi/pico-sdk)
- **License**: `BSD-3-Clause`
- **Copyright**: `Copyright 2020 (c) 2020 Raspberry Pi (Trading) Ltd.`

```text
Copyright 2020 (c) 2020 Raspberry Pi (Trading) Ltd.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
   disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
   disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## 3. Prebuilt Firmware Images (`mimhid_firmware-<board>-<version>.{uf2,hex,bin}`)

Prebuilt microcontroller firmware images published in GitHub Releases link
`libmimhid`'s MIT firmware core (`firmware/common/`, `firmware/rp/`,
`firmware/ports/`) against the target microcontroller's vendor SDK, USB device
stack, and bare-metal C runtime. Because raw microcontroller flash images
(`.uf2`, `.hex`, `.bin`) cannot carry embedded text documentation, this file
(and the standalone `mimhid_firmware-THIRD_PARTY_NOTICES-<version>.txt` asset
published alongside the firmware binaries in every GitHub Release) serves as the
accompanying documentation required by the licenses below.

| Firmware binary | Linked third-party components |
| :--- | :--- |
| `mimhid_firmware-rp2040-<version>.uf2` | TinyUSB 0.18.0 (MIT), Raspberry Pi Pico SDK 2.2.0 (BSD-3-Clause), Arm CMSIS Core (Apache-2.0), Newlib C runtime |
| `mimhid_firmware-rp2350-<version>.uf2` | TinyUSB 0.18.0 (MIT), Raspberry Pi Pico SDK 2.2.0 (BSD-3-Clause), Arm CMSIS Core (Apache-2.0), Newlib C runtime |
| `mimhid_firmware-nrf52840-<version>.{hex,bin}` | TinyUSB 0.18.0 (MIT), Nordic `nrfx` (BSD-3-Clause), Arm CMSIS 5 (Apache-2.0), Newlib C runtime |
| `mimhid_firmware-stm32f4-<version>.{hex,bin}` | TinyUSB 0.18.0 (MIT), STMicroelectronics `cmsis_device_f4` (Apache-2.0 / BSD-3-Clause) & `stm32f4xx_hal_driver` (BSD-3-Clause), Arm CMSIS 5 (Apache-2.0), Newlib C runtime |
| `mimhid_firmware-esp32s3-<version>.bin` | TinyUSB 0.18.0 (MIT), Espressif ESP-IDF v5.4.2 (Apache-2.0), Amazon FreeRTOS Kernel (MIT), Newlib C runtime |

---

### 3.1 TinyUSB (used in all firmware binaries)

- **Repository**: <https://github.com/hathach/tinyusb>
- **Pinned version**: `0.18.0` (commit `86ad6e56c1700e85f1c5678607a762cfe3aa2f47`)
- **License**: `MIT`
- **Copyright**:
  - `Copyright (c) 2018-2024 Ha Thach (tinyusb.org)`
  - `Copyright (c) 2020 Raspberry Pi (Trading) Ltd.` (`src/portable/raspberrypi/rp2040/`)
  - `Copyright (c) 2019 William D. Jones`, `Copyright (c) 2020 Jan Duempelmann`, `Copyright (c) 2020 Reinhard Panhuber` (`src/portable/synopsys/dwc2/`)

```text
The MIT License (MIT)

Copyright (c) 2018, hathach (tinyusb.org)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

---

### 3.2 Raspberry Pi Pico SDK (used in `rp2040` and `rp2350` firmware binaries)

- **Repository**: <https://github.com/raspberrypi/pico-sdk>
- **Pinned version**: `2.2.0` (commit `a1438dff1d38bd9c65dbd693f0e5db4b9ae91779`)
- **License**: `BSD-3-Clause`
- **Copyright**: `Copyright 2020 (c) 2020 Raspberry Pi (Trading) Ltd.`

```text
Copyright 2020 (c) 2020 Raspberry Pi (Trading) Ltd.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
   disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
   disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

### 3.3 Nordic Semiconductor `nrfx` (used in `nrf52840` firmware binaries)

- **Repository**: <https://github.com/NordicSemiconductor/nrfx>
- **Pinned commit**: `7c47cc0a56ce44658e6da2458e86cd8783ccc4a2`
- **License**: `BSD-3-Clause`
- **Copyright**: `Copyright (c) 2017 - 2024, Nordic Semiconductor ASA`

```text
Copyright (c) 2017 - 2024, Nordic Semiconductor ASA
All rights reserved.

SPDX-License-Identifier: BSD-3-Clause

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from this
   software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

---

### 3.4 STMicroelectronics `stm32f4xx_hal_driver` and `cmsis_device_f4` (used in `stm32f4` firmware binaries)

- **Repositories**:
  - <https://github.com/STMicroelectronics/stm32f4xx_hal_driver> (commit `04e99fbdabd00ab8f370f377c66b0a4570365b58`)
  - <https://github.com/STMicroelectronics/cmsis_device_f4> (commit `2615e866fa48fe1ff1af9e31c348813f2b19e7ec`)
- **Licenses**: `BSD-3-Clause` (`stm32f4xx_hal_driver` and `cmsis_device_f4` source headers) and `Apache-2.0` (`cmsis_device_f4/License.md`)
- **Copyright**: `Copyright (c) 2016-2019 STMicroelectronics. All rights reserved.`

```text
Copyright (c) 2016-2019 STMicroelectronics. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:
  1. Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.
  3. Neither the name of STMicroelectronics nor the names of its contributors
     may be used to endorse or promote products derived from this software
     without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

### 3.5 Espressif Systems ESP-IDF and Amazon FreeRTOS Kernel (used in `esp32s3` firmware binary)

- **Repositories**:
  - <https://github.com/espressif/esp-idf> (tag `v5.4.2`)
  - <https://github.com/FreeRTOS/FreeRTOS-Kernel> (bundled in ESP-IDF v5.4.2)
- **Licenses**: `Apache-2.0` (ESP-IDF) and `MIT` (FreeRTOS Kernel)
- **Copyright**:
  - `SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD`
  - `Copyright (C) 2021 Amazon.com, Inc. or its affiliates. All Rights Reserved.`

FreeRTOS Kernel MIT License:

```text
MIT License

Copyright (C) 2021 Amazon.com, Inc. or its affiliates. All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

---

### 3.6 Arm CMSIS Core (`CMSIS_5` and Pico SDK CMSIS stub, used in ARM Cortex-M firmware binaries)

- **Repository**: <https://github.com/ARM-software/CMSIS_5> (commit `2b7495b8535bdcb306dac29b9ded4cfb679d7e5c`) and Raspberry Pi Pico SDK CMSIS headers
- **License**: `Apache-2.0`
- **Copyright**: `Copyright (c) 2009-2024 Arm Limited. All rights reserved.`

---

### 3.7 Apache License, Version 2.0 (covering ESP-IDF, Arm CMSIS, and ST `cmsis_device_f4`)

```text
                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

   TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

   1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (ii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

   2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

   3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

   4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

   5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

   6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

   7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

   8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

   9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

   END OF TERMS AND CONDITIONS
```

---

### 3.8 Bare-Metal C Runtime (Newlib / libgcc in `arm-none-eabi-gcc` and `xtensa-esp32s3-elf-gcc`)

The prebuilt firmware binaries link small freestanding routines (`memcpy`,
`memset`, integer division helpers) from the GCC `libgcc` runtime (covered by
the [GCC Runtime Library Exception 3.1](https://www.gnu.org/licenses/gcc-exception-3.1.html))
and from Red Hat / Cygnus Newlib (`libc_nano.a` / `libnosys.a`), which is
distributed under permissive BSD-style and MIT-style notices:

```text
Copyright (c) 1994-2009 Red Hat, Inc. All rights reserved.
Copyright (c) 1981-2000 The Regents of the University of California. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:
1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the University nor the names of its contributors may
   be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
