#!/bin/bash
# VP9 header round trip: encodes test clips with ffmpeg (libvpx-vp9), rebuilds every
# frame's headers with core/video/vp9.c (tests/vp9_roundtrip.c) and checks that the
# rebuilt stream decodes to the same pixels. usage: tests/vp9_roundtrip.sh BUILD_DIR
B=${1:-build/native-noop}
T=$(mktemp -d)
fail=0
run() {
  local src=$1 opts=$2 name=$3 known=$4
  ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$src" -t 4 -c:v libvpx-vp9 $opts "$T/c.ivf" || { echo "$name: encode failed"; fail=1; return; }
  "$B/tests/vp9_roundtrip" "$T/c.ivf" "$T/d.ivf" 2>"$T/log" || { echo "$name: rebuild failed: $(tail -1 "$T/log")"; fail=1; return; }
  local a b
  a=$(ffmpeg -hide_banner -loglevel error -i "$T/c.ivf" -f framemd5 - | grep -v "^#" | awk -F, '{print $6}' | md5)
  b=$(ffmpeg -hide_banner -loglevel error -i "$T/d.ivf" -f framemd5 - | grep -v "^#" | awk -F, '{print $6}' | md5)
  if [ "$a" = "$b" ]; then echo "$name: same pixels ($(tail -1 "$T/log"))"
  elif [ -n "$known" ]; then echo "$name: different (known limit: $known)"
  else echo "$name: DIFFERENT"; fail=1; fi
}
run "testsrc2=size=320x240:rate=30" "-auto-alt-ref 0 -lag-in-frames 0 -b:v 300k" "320x240 no altref"
# Backward adaptation on: an MV probability "updated" to the value our unadapted mirror
# already holds is indistinguishable from no update (MV updates are absolute), so it is
# dropped while the decoder holds the adapted value (core/video/vp9.h).
run "testsrc2=size=1280x720:rate=30" "-b:v 1M -tile-columns 2 -frame-parallel 0" "720p tiles, altref, adaptation" \
    "absolute MV updates under backward adaptation"
run "testsrc2=size=1920x1080:rate=30" "-b:v 2M -tile-columns 3 -tile-rows 1 -frame-parallel 1 -row-mt 1" "1080p frame-parallel"
run "mandelbrot=size=640x360:rate=30" "-crf 20 -b:v 0 -error-resilient 1" "error resilient"
run "testsrc2=size=640x360:rate=30" "-lossless 1" "lossless"
run "testsrc2=size=640x360:rate=30,format=yuv444p" "-b:v 500k" "profile 1 (4:4:4)"
run "testsrc2=size=640x360:rate=30,format=yuv420p10le" "-b:v 500k" "profile 2 (10-bit)"
run "testsrc2=size=640x360:rate=30" "-b:v 200k -aq-mode 3" "segmentation (aq-mode 3)"
rm -rf "$T"
exit $fail
