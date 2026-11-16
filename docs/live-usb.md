# Live USB pull — not built, and why

"The calculator asks, the PC answers" needs two things that this prototype does not have:

1. **PC side:** a WinUSB/libusb implementation of the CX II link (device discovery, NNSE framing,
   native packet layer, a service port to serve requests). The ScreenMirror project documents one
   in Python (`docs/usb-protocol.md` in that repo, GPL-3.0); there is no C++ equivalent here.
2. **Calculator side:** an Ndless program that registers a USB service and handles incoming
   packets in callbacks. The Ndless SDK exposes the underlying OS USB functions, but the working
   receiver code in ScreenMirror's `calc/` folder could not be read from the environment used to
   write this prototype, so no calculator-side USB code was written blind.

What is already transport-independent: the SVD1 and SVD2 containers with their CRC32s. A future
live link can send the same bytes (header, then level payloads in chunks) and reuse
`svd_pyramid_header_parse`, `svd_pyramid_level_decode` and `svd_pyramid_render_rgb565`. Legacy
SVD1 files still use `svd_header_parse`, `svd_payload_verify` and `svd_render_rgb565`. A future USB
source would need to stream the SVD2 header and level data to the calculator, plus a server on the PC.
