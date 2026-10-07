# Notices

jaunt's indirect display driver is under the MIT license (LICENSE). It draws on the following.

## Microsoft's IddSampleDriver (Windows-driver-samples, video/IndirectDisplay)

The adapter, monitor and swap chain handling in `src/driver.cpp`, `src/driver.h`, `jaunt-idd.inf`
and `jaunt-idd.vcxproj` follow this sample's structure and its use of the IddCx API:
https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay

    MIT License

    Copyright (c) Microsoft Corporation. All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

## Virtual Display Driver (VirtualDrivers/Virtual-Display-Driver)

Its design guided this driver: monitors without an EDID whose modes the driver reports, and a
named pipe to control the driver from user space. No code from it is copied here. This driver
differs where VDD left a weakness open: its pipe is open to SYSTEM and one named account only
(VDD's is open to Everyone), it takes any mode per request instead of a list fixed in an XML file,
and a watchdog removes each connection's monitors when the connection ends or goes silent.
https://github.com/VirtualDrivers/Virtual-Display-Driver (MIT License; its copyright notice is in
its LICENSE file, and goes here with any of its code that is ever reused).
