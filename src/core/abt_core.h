/* abthumb core: shared portable types. C11, no OS headers. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum abt_status {
    ABT_OK = 0,
    ABT_E_NOTFOUND = -1,    /* thing we looked for is not in the file */
    ABT_E_FORMAT = -2,      /* not this format / malformed */
    ABT_E_TRUNCATED = -3,   /* structure runs past end of data */
    ABT_E_UNSUPPORTED = -4, /* valid but we cannot handle this variant */
    ABT_E_NOMEM = -5,
    ABT_E_IO = -6,
    ABT_E_CANCELLED = -7,
    ABT_E_LIMIT = -8        /* exceeds a safety limit (dimensions, sizes) */
} abt_status;

typedef enum abt_family {
    ABT_FAM_UNKNOWN = 0,
    ABT_FAM_AI_PDF = 1,  /* Illustrator 9+ (PDF-based) */
    ABT_FAM_AI_PS = 2,   /* Illustrator 1-8 (PostScript-based) */
    ABT_FAM_EPS = 3,
    ABT_FAM_PSD = 4,
    ABT_FAM_PSB = 5,
    ABT_FAM_PDF = 6
} abt_family;

typedef enum abt_color_mode {
    ABT_CM_UNKNOWN = 0,
    ABT_CM_BITMAP, ABT_CM_GRAY, ABT_CM_INDEXED, ABT_CM_RGB, ABT_CM_CMYK,
    ABT_CM_MULTICHANNEL, ABT_CM_DUOTONE, ABT_CM_LAB
} abt_color_mode;

/* abt_file_info.flags */
enum {
    ABT_INFO_HAS_PDF_CONTENT   = 1u << 0, /* AI: renderable PDF content present */
    ABT_INFO_NO_PDF_CONTENT    = 1u << 1, /* AI: saved without PDF content (placeholder page) */
    ABT_INFO_MERGED_DATA       = 1u << 2, /* PSD: real merged composite present */
    ABT_INFO_NO_MERGED_DATA    = 1u << 3, /* PSD: Maximize Compatibility was off */
    ABT_INFO_ENCRYPTED         = 1u << 4,
    ABT_INFO_HAS_ALPHA         = 1u << 5,
    ABT_INFO_ICC_PROFILE       = 1u << 6,
    ABT_INFO_LAYERS            = 1u << 7,
    ABT_INFO_CANVAS_BEYOND_PAGE1 = 1u << 8, /* AI: several artboards or art outside artboard 1, so the
                                              whole-canvas thumbnail differs from artboard 1 (D-029) */
    ABT_INFO_PAGE_EMPTY = 1u << 9          /* the rendered page / artboard has no visible artwork (D-031) */
};

/* Fixed-layout description of a file, shared by worker, agent cache and DLL.
   Keep it POD and append-only (it crosses process boundaries and is stored in the cache). */
typedef struct abt_file_info {
    uint32_t family;      /* abt_family */
    uint32_t flags;       /* ABT_INFO_* */
    uint32_t page_count;  /* pages / artboards; 1 for raster */
    uint32_t color_mode;  /* abt_color_mode */
    uint32_t bit_depth;   /* bits per channel */
    uint32_t px_width;    /* raster size (PSD/PSB) */
    uint32_t px_height;
    float pt_width;       /* vector page size in points (AI/EPS/PDF), first page */
    float pt_height;
    uint32_t preview_w;   /* best embedded preview, 0 if none */
    uint32_t preview_h;
    char creator[48];     /* UTF-8, e.g. "Adobe Illustrator 30.0" */
    char version[16];     /* UTF-8, e.g. "PDF 1.6", "PSD v1", "AI 8" */
} abt_file_info;

const char* abt_status_str(abt_status s);
const char* abt_family_str(abt_family f);
const char* abt_color_mode_str(abt_color_mode m);

#ifdef __cplusplus
}
#endif
