# TODO: raw HCGE viewport for SF3000/SF3500

The SF3000/SF3500 stock display driver accepts the source frame and a
fit/fill flag, but does not expose an arbitrary destination viewport or crop.
Because the chipset is extremely slow, do not implement this by reshaping or
scaling a frame in software.

Investigate the raw HCGE/display-controller API for a hardware destination
rectangle (including the 270-degree rotation path), then restore custom 4:3,
16:9, 3:2, 5:4, 8:7, and 16:10 modes on SF3000/SF3500 once the viewport is
programmed directly.
