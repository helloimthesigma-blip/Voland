/**
 * Vertex-program captures for the differential test of the WGSL vertex
 * translation (platform/web/tools/vertex-vectors.mjs): a program's WGSL,
 * the constant buffers (and traced global buffers) it reads, and per
 * vertex its inputs and the interpreter's position and generic outputs,
 * as one JSON object. voland-cli writes them to files (VOLAND_VS_CAPTURE);
 * the web core logs one program's (VOLAND_VS_CAPTURE_PROGRAM).
 */
#ifndef SWITCH_GPU_VS_CAPTURE_H
#define SWITCH_GPU_VS_CAPTURE_H

#include <stddef.h>

#include "gpu/maxwell_shader.h"

/* Receives the JSON text in pieces, in order. */
typedef void (*Vs_Capture_Emit)(void *user, const char *text, size_t length);

/* The capture of `program` shaded with `env` by `thread` (its lanes the
 * vertices), through `emit`. Programs the test cannot run (they sample
 * textures, or read untraced global memory) come out with a "skip" reason. */
void vs_capture_write(const Sm_Program *program, const Sm_Env *env, const Sm_Thread *thread, Vs_Capture_Emit emit, void *user);

#endif /* SWITCH_GPU_VS_CAPTURE_H */
