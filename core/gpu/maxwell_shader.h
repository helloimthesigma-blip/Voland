/**
 * Maxwell (SM 5.x) shader programs (§13): decode once, interpret per
 * invocation. This is the reference execution path for guest shaders -
 * the software rasterizer (gpu/raster3d) runs vertex and pixel programs
 * through it - and its decoded form is what a later WGSL translator
 * consumes.
 *
 * Program layout (NVIDIA "Shader Program Header" + GM10x ISA): a 0x50-
 * byte SPH, then 64-bit instruction words in bundles of four - one
 * scheduling-control word followed by three instructions. Branch targets
 * are byte offsets relative to the following instruction.
 *
 * Execution model: one invocation at a time, scalar. Warp-level
 * behaviour collapses accordingly: SSY/SYNC, PBK/BRK and PCNT/CONT are
 * single-entry reconvergence stacks; VOTE and SHFL see one active lane;
 * implicit texture LOD is the base level, so
 * implicit-LOD sampling reads the base level.
 *
 * Encodings were reconstructed from NVIDIA's open-gpu-doc and from the
 * behaviour of Mesa's (MIT) GM107 emitter; the code is Voland's own.
 */
#ifndef SWITCH_GPU_MAXWELL_SHADER_H
#define SWITCH_GPU_MAXWELL_SHADER_H

#include <stdbool.h>
#include <stdint.h>

#define SM_SPH_BYTES 0x50u
#define SM_SPH_WORDS 20u
#define SM_MAX_WORDS 4096u      /* instruction words (incl. scheduling words) per program */
#define SM_REGISTERS 256u       /* R0-R254, RZ = 255 */
#define SM_RZ 255u
#define SM_PREDICATES 8u        /* P0-P6, PT = 7 */
#define SM_PT 7u
#define SM_ATTRIBUTE_WORDS 0x100u /* attribute space 0x000-0x3FC, by word */
#define SM_CBUF_SLOTS 18u
#define SM_STACK_DEPTH 16u
#define SM_LOCAL_BYTES 0x400u   /* per-invocation local memory (LDL/STL) */
#define SM_MAX_STEPS 1000000u   /* runaway guard per invocation */

/* Attribute addresses (bytes). */
#define SM_ATTR_POSITION 0x70u  /* x, y, z, w */
#define SM_ATTR_GENERIC 0x80u   /* + 16 * n, n < 32 */
#define SM_ATTR_GENERIC_COUNT 32u
#define SM_ATTR_INSTANCE_ID 0x2F8u
#define SM_ATTR_VERTEX_ID 0x2FCu
#define SM_ATTR_FRONT_FACING 0x3FCu

typedef enum Sm_Stage {
  SM_STAGE_VERTEX = 1,
  SM_STAGE_TESS_CONTROL = 2,
  SM_STAGE_TESS_EVAL = 3,
  SM_STAGE_GEOMETRY = 4,
  SM_STAGE_PIXEL = 5,
} Sm_Stage;

/* Pixel-shader input interpolation (SPH PS ImapGenericVector, 2 bits per
 * component). */
typedef enum Sm_Interp {
  SM_INTERP_UNUSED = 0,
  SM_INTERP_CONSTANT = 1,
  SM_INTERP_PERSPECTIVE = 2,
  SM_INTERP_SCREEN_LINEAR = 3,
} Sm_Interp;

typedef enum Sm_Op {
  SM_OP_INVALID = 0,
  SM_OP_SCHED, /* scheduling-control word */
  SM_OP_NOP,
  /* float */
  SM_OP_FADD, SM_OP_FADD32I, SM_OP_FMUL, SM_OP_FMUL32I, SM_OP_FFMA, SM_OP_FFMA32I,
  SM_OP_FMNMX, SM_OP_FSET, SM_OP_FSETP, SM_OP_FCMP, SM_OP_MUFU, SM_OP_RRO, SM_OP_FSWZADD,
  SM_OP_F2F, SM_OP_F2I, SM_OP_I2F, SM_OP_I2I,
  /* half precision (two f16 per register) */
  SM_OP_HADD2, SM_OP_HMUL2, SM_OP_HFMA2, SM_OP_HSET2, SM_OP_HSETP2,
  /* integer */
  SM_OP_IADD, SM_OP_IADD32I, SM_OP_IADD3, SM_OP_ISCADD, SM_OP_ISCADD32I, SM_OP_IMNMX,
  SM_OP_ISET, SM_OP_ISETP, SM_OP_ICMP, SM_OP_IMUL, SM_OP_IMUL32I, SM_OP_IMAD, SM_OP_XMAD,
  SM_OP_LOP, SM_OP_LOP32I, SM_OP_LOP3, SM_OP_SHL, SM_OP_SHR, SM_OP_BFE, SM_OP_BFI,
  SM_OP_POPC, SM_OP_FLO, SM_OP_PRMT, SM_OP_SEL, SM_OP_MOV, SM_OP_MOV32I,
  SM_OP_PSETP, SM_OP_P2R, SM_OP_R2P, SM_OP_CSETP,
  /* system / misc */
  SM_OP_S2R, SM_OP_CS2R, SM_OP_VOTE, SM_OP_SHFL, SM_OP_BARRIER, /* DEPBAR, BAR, MEMBAR */
  /* memory */
  SM_OP_ALD, SM_OP_AST, SM_OP_IPA, SM_OP_LDC, SM_OP_LD, SM_OP_ST, SM_OP_LDG, SM_OP_STG,
  SM_OP_LDL, SM_OP_STL, SM_OP_OUT,
  /* texture */
  SM_OP_TEX, SM_OP_TEX_B, SM_OP_TEXS, SM_OP_TLD, SM_OP_TLDS, SM_OP_TLD4, SM_OP_TLD4S, SM_OP_TXQ, SM_OP_TMML,
  SM_OP_TXD,
  /* control */
  SM_OP_BRA, SM_OP_SSY, SM_OP_SYNC, SM_OP_PBK, SM_OP_BRK, SM_OP_PCNT, SM_OP_CONT,
  SM_OP_CAL, SM_OP_RET, SM_OP_EXIT, SM_OP_KIL,
  SM_OP_COUNT
} Sm_Op;

/* Second-source form for the R/C/I opcode families. */
typedef enum Sm_Form {
  SM_FORM_REG = 0,  /* Rb (bits 20-27) */
  SM_FORM_CBUF,     /* c[bits 34-38][bits 20-33 * 4] */
  SM_FORM_IMM,      /* 20-bit immediate, sign at bit 56 */
  SM_FORM_IMM32,    /* bits 20-51 */
  SM_FORM_REG_CBUF, /* 3-source: b = Rc slot (bits 39-46), c = cbuf */
} Sm_Form;

typedef struct Sm_Insn {
  uint64_t raw;
  uint16_t op;      /* Sm_Op */
  uint8_t form;     /* Sm_Form */
  uint8_t pred;     /* bits 0-2 predicate, bit 3 negate; 7 = always */
  uint8_t cbuf;     /* constant buffer slot for SM_FORM_CBUF / REG_CBUF */
  uint16_t next;    /* word to run after this one falls through (scheduling words and no-ops skipped) */
  uint32_t imm;     /* immediate value, or constant-buffer byte offset */
  int32_t target;   /* branch / stack target word index, -1 none */
} Sm_Insn;

typedef struct Sm_Header {
  uint32_t words[SM_SPH_WORDS];
  Sm_Stage stage;
  bool kills_pixels;
  bool mrt_enable;
  /* VTG: generic inputs read / outputs written (bit per component,
   * vector*4 + comp). */
  uint32_t input_generic[4];      /* 128 bits */
  uint32_t output_generic[4];     /* 128 bits */
  /* PS: interpolation per generic input component (vector*4 + comp). */
  uint8_t input_interp[SM_ATTR_GENERIC_COUNT * 4u];
  uint8_t position_interp[4];     /* PS position x/y/z/w inputs */
  uint32_t omap_target;           /* PS: 4 bits per render target */
  bool omap_depth;
  bool omap_sample_mask;
  uint32_t local_memory_bytes;
} Sm_Header;

typedef struct Sm_Program {
  uint64_t address;               /* GPU VA of the SPH */
  uint32_t hash;                  /* of SPH + code as read */
  uint32_t byte_size;             /* SPH + code bytes considered */
  uint32_t word_count;
  Sm_Header header;
  uint32_t cbuf_used;             /* bit per slot */
  uint32_t cbuf_extent[SM_CBUF_SLOTS]; /* highest byte touched + 4 (direct); 0x10000 if indirect */
  uint32_t unknown_ops;           /* words that decoded as SM_OP_INVALID (diagnostics) */
  /* Pixel programs: which fragment inputs vary per pixel beyond the
   * generic varyings (lets the rasterizer shade flat triangles once). */
  bool reads_fragcoord_xy;        /* IPA a[0x70]/a[0x74], or an indexed IPA */
  bool reads_fragcoord_z;         /* IPA a[0x78] */
  /* Uses lanes' neighbours (FSWZADD, SHFL): pixel programs then run in
   * 2x2 quads, lanes 4q..4q+3 = top-left, top-right, bottom-left,
   * bottom-right, with helper lanes for uncovered quad members. */
  bool uses_quads;
  Sm_Insn insns[SM_MAX_WORDS];
} Sm_Program;

/* Texture requests the interpreter hands to its environment. */
typedef enum Sm_Tex_Kind {
  SM_TEX_SAMPLE = 0, /* filtered, float coordinates */
  SM_TEX_FETCH,      /* texel fetch, integer coordinates */
  SM_TEX_GATHER,
  SM_TEX_QUERY_DIMS,
  SM_TEX_QUERY_LOD,
} Sm_Tex_Kind;

/* Lanes of one SIMT invocation (Sm_Thread below): a bit per lane. */
#define SM_LANES 32u
typedef uint32_t Sm_Mask;

typedef struct Sm_Tex_Request {
  Sm_Tex_Kind kind;
  uint32_t handle;      /* bits 0-19 texture header index, 20-31 sampler index */
  uint8_t dims;         /* 1, 2, 3 */
  bool array;
  bool cube;
  bool shadow;
  bool has_lod;         /* explicit LOD (LL / LZ / TLD.LL) */
  bool has_bias;
  bool has_offset;
  uint8_t gather_component;
  float coords[3];      /* SAMPLE / GATHER */
  int32_t icoords[3];   /* FETCH */
  float layer;          /* array index (float for SAMPLE, rounded) */
  float lod;            /* LOD, or bias */
  float dref;
  int32_t offset[3];
  int32_t ilod;         /* FETCH / QUERY */
  uint32_t pc;          /* the texture instruction's word index */
} Sm_Tex_Request;

/* Everything an invocation reads from outside its registers. */
typedef struct Sm_Env {
  const uint8_t *cbuf[SM_CBUF_SLOTS];
  uint32_t cbuf_size[SM_CBUF_SLOTS];
  uint32_t texture_cbuf_slot; /* SET_BINDLESS_TEXTURE: bound texture handles live here */
  void *user;
  /* Writes four results (floats, or raw integers for integer formats). */
  void (*texture)(void *user, const Sm_Tex_Request *request, uint32_t out[4]);
  /* Optional, preferred when set: every lane of one texture instruction at
   * once - requests[l] -> out[l] for each lane set in `lanes` (out is
   * pre-filled with 0, 0, 0, 1). Must give the per-lane results of
   * `texture`; it exists to resolve a handle once per instruction. */
  void (*texture_batch)(void *user, const Sm_Tex_Request *requests, Sm_Mask lanes, uint32_t (*out)[4]);
  bool (*global_read)(void *user, uint64_t gpu_va, void *out, uint32_t size);
  bool (*global_write)(void *user, uint64_t gpu_va, const void *src, uint32_t size);
} Sm_Env;

/* Invocations run SIMT-style in up to SM_LANES lanes: each instruction
 * is decoded once and applied to every active lane. Register files are
 * lane-minor (r[register][lane]); predicates and condition codes are lane
 * masks. Lanes whose branches disagree split into separate warps that
 * each keep their own reconvergence stack, so every lane observes exactly
 * the scalar semantics. */
#define SM_ALL_LANES ((Sm_Mask)0xFFFFFFFFu)

typedef struct Sm_Thread {
  uint32_t lanes;                        /* lanes in use, 1..SM_LANES */
  uint32_t r[SM_REGISTERS][SM_LANES];    /* r[SM_RZ] stays zero */
  Sm_Mask p[SM_PREDICATES];              /* p[SM_PT] = every lane */
  Sm_Mask cc_carry;
  Sm_Mask cc_zero;
  Sm_Mask cc_sign;
  Sm_Mask cc_overflow;
  uint32_t attr_in[SM_ATTRIBUTE_WORDS][SM_LANES];  /* VTG inputs; PS interpolated values */
  uint32_t attr_out[SM_ATTRIBUTE_WORDS][SM_LANES]; /* VTG outputs */
  uint32_t vertex_id[SM_LANES];
  uint32_t instance_id[SM_LANES];
  Sm_Mask front_facing;
  Sm_Mask killed;
  bool faulted;                          /* runaway / bad branch (any lane) */
  uint32_t discard[SM_LANES];            /* writes to RZ land here */
  uint8_t local[SM_LANES][SM_LOCAL_BYTES];
} Sm_Thread;

/* Parses the 0x50-byte header. */
void sm_header_parse(const uint32_t words[SM_SPH_WORDS], Sm_Header *out);

/* Decodes `size` bytes (SPH + code) at `bytes` into `out`. */
void sm_program_decode(const uint8_t *bytes, uint32_t size, uint64_t address, Sm_Program *out);

/* Bytes of a program worth reading: stops after the self-branch the
 * compiler appends past the last EXIT, or at `size`. Returns SPH + code. */
uint32_t sm_program_extent(const uint8_t *bytes, uint32_t size);

/* FNV-1a over a byte range (program cache validation). */
uint32_t sm_hash(const uint8_t *bytes, uint32_t size);

/* Clears registers, predicates and flags for `lanes` lanes. Attribute
 * arrays are left to the caller. */
void sm_thread_reset(Sm_Thread *thread, uint32_t lanes);

/* Predicates and flags only: registers keep stale values (programs never
 * read a register they did not write; RZ is not stored). For per-pixel
 * use. */
void sm_thread_reset_light(Sm_Thread *thread, uint32_t lanes);

/* Runs `thread->lanes` invocations from the first instruction until every
 * lane has reached EXIT (or KIL). Returns false when the program faulted
 * (runaway, bad stack). */
bool sm_run(const Sm_Program *program, const Sm_Env *env, Sm_Thread *thread);

/* Short mnemonic for diagnostics. */
const char *sm_op_name(Sm_Op op);

#endif /* SWITCH_GPU_MAXWELL_SHADER_H */
