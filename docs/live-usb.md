# Live USB pull — not built, and why

"The calculator asks, the PC answers" needs two things that this prototype does not have:

1. **PC side:** a WinUSB/libusb implementation of the CX II link (device discovery, NNSE framing,
   native packet layer, a service port to serve requests). The ScreenMirror project documents one
   in Python (`docs/usb-protocol.md` in that repo, GPL-3.0); there is no C++ equivalent here.
2. **Calculator side:** an Ndless program that registers a USB service and handles incoming
   packets in callbacks. The Ndless SDK exposes the underlying OS USB functions, but the working
   receiver code in ScreenMirror's `calc/` folder could not be read from the environment used to
   write this prototype, so no calculator-side USB code was written blind.

What is already transport-independent: the SVD1 container with its CRC32s. A future live link
can send the same bytes (header, then payload in chunks) and reuse `svd_header_parse`,
`svd_payload_verify` and `svd_render_rgb565` unchanged. Only a new "source" in
`calculator/main.c` (replacing `load()`) and a server on the PC would be needed.
