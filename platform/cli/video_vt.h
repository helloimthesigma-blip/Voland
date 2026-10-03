/**
 * voland-cli's native video decoder (VideoToolbox on macOS). See video_vt.c.
 */
#ifndef VOLAND_CLI_VIDEO_VT_H
#define VOLAND_CLI_VIDEO_VT_H

#include "video/video_stream.h"

/* The backend to hand emulator_set_video_backend; it fills `video`'s slots. */
const Video_Backend *video_vt_backend(Video_Stream *video);

/* We are a fork()ed child (a snapshot job): decode nothing from here on
 * (VideoToolbox cannot start after fork); access units are still dumped. */
void video_vt_forked(void);

/* Prints decode totals (end of run). */
void video_vt_report(void);

#endif /* VOLAND_CLI_VIDEO_VT_H */
