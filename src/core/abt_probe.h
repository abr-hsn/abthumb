/* Probing: identify a file and list its embedded previews without decoding anything heavy.
   All offsets are relative to the start of the file data passed in. */
#pragma once
#include "abt_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum abt_preview_kind {
    ABT_PV_NONE = 0,
    ABT_PV_JPEG = 1,   /* JFIF/JPEG bytes (XMP thumbnail after base64, PSD 1036, PS EPS) */
    ABT_PV_TIFF = 2,   /* TIFF bytes (DOS EPS preview section) */
    ABT_PV_WMF = 3,    /* Windows metafile bytes (DOS EPS preview section) */
    ABT_PV_EPSI = 4,   /* %%BeginPreview hex lines; width/height/bits set */
    ABT_PV_JPEG_BGR = 5,/* PSD resource 1033: JPEG with R and B swapped (Photoshop 4) */
    ABT_PV_PNG = 6
} abt_preview_kind;

enum {
    ABT_PVF_BASE64 = 1u << 0,  /* data is XMP base64 text (may contain &#xA; entities, whitespace) */
    ABT_PVF_ASPECT_MISMATCH = 1u << 1, /* preview does not match the first page/artboard (fallback only) */
    ABT_PVF_HEX_PSRES = 1u << 8 /* Photoshop EPS: hex text of image resources; use abt_eps_psres_jpeg */
};

typedef struct abt_preview {
    uint32_t kind;     /* abt_preview_kind */
    uint32_t flags;    /* ABT_PVF_* */
    uint32_t width;    /* pixel size if known from metadata, else 0 */
    uint32_t height;
    uint32_t bits;     /* EPSI bits per pixel */
    uint32_t source;   /* abt_preview_source, for logging */
    uint64_t offset;   /* encoded data location in the file */
    uint64_t length;
} abt_preview;

typedef enum abt_preview_source {
    ABT_PVS_XMP = 1, ABT_PVS_EPS_TIFF, ABT_PVS_EPS_WMF, ABT_PVS_EPSI, ABT_PVS_PSD_1036, ABT_PVS_PSD_1033,
    ABT_PVS_PS_EPS_1036, ABT_PVS_PDF_THUMB
} abt_preview_source;

#define ABT_MAX_PREVIEWS 8

typedef struct abt_probe {
    abt_file_info info;
    int n_previews;
    abt_preview previews[ABT_MAX_PREVIEWS];
    /* PostScript (EPS / AI 1-8): where the PostScript program is, and its bounding box. */
    uint64_t ps_offset, ps_length;
    float bbox[4];        /* llx lly urx ury in points; all 0 if unknown */
    /* PSD / PSB section offsets (see abt_psd.h) */
    uint64_t psd_layers_offset, psd_layers_length;
    uint64_t psd_image_offset;   /* image data section (compression word) */
    uint64_t psd_icc_offset, psd_icc_length;
    uint64_t psd_colormode_offset, psd_colormode_length;
    uint32_t psd_channels;
    /* AI-specific */
    uint32_t ai_version;  /* major Illustrator version if known (e.g. 30 for 2026), 0 unknown */
} abt_probe;

/* Identify the family from the first bytes (needs at least 1 KB if available). */
abt_family abt_detect(const uint8_t* data, size_t size);

/* Full probe: family, info, previews. Never decodes image data. */
abt_status abt_probe_file(const uint8_t* data, size_t size, abt_probe* out);

/* Family probes (called by abt_probe_file; exposed for tests). */
abt_status abt_probe_pdf(const uint8_t* data, size_t size, abt_probe* out); /* PDF and AI 9+ */
abt_status abt_probe_eps(const uint8_t* data, size_t size, abt_probe* out); /* EPS and AI 1-8 */
abt_status abt_probe_psd(const uint8_t* data, size_t size, abt_probe* out); /* PSD and PSB */

/* Adds a preview to the probe if there is room. */
void abt_probe_add_preview(abt_probe* p, const abt_preview* pv);

/* Returns the index of the best preview for a request whose long edge is `edge`:
   the smallest preview with long edge >= edge, else the largest one. -1 if none.
   Previews with unknown size are treated as 0 x 0 (used only if nothing else). */
int abt_probe_best_preview(const abt_probe* p, int edge);

/* XMP: scan [data, data+size) for xmpGImg thumbnails and add each as a preview
   (kind JPEG, flag BASE64). Returns number added. */
int abt_xmp_find_thumbnails(const uint8_t* file, size_t file_size, size_t scan_off, size_t scan_len, abt_probe* p);
/* Reads XMP creator tool string (xmp:CreatorTool) into out (UTF-8), returns 1 if found. */
int abt_xmp_creator_tool(const uint8_t* data, size_t size, char* out, size_t cap);
/* Reads a numeric XMP property (element or attribute form), 0 if absent. */
float abt_xmp_float(const uint8_t* data, size_t size, const char* name);

/* base64 decode tolerant of whitespace and XML character references (&#xA; &#10;).
   Returns decoded byte count; output buffer must hold at least len*3/4+3 bytes. */
size_t abt_base64_decode(const uint8_t* in, size_t len, uint8_t* out);

/* Photoshop EPS: decodes the hex resource block of a preview with ABT_PVF_HEX_PSRES and returns a
   malloc'd copy of the resource-1036 JFIF bytes (caller frees), 0 on failure. */
size_t abt_eps_psres_jpeg(const uint8_t* file, size_t n, const abt_preview* pv, uint8_t** jpeg);

/* EPSI preview decode into a newly allocated BGRA image (opaque). */
#include "abt_image.h"
abt_status abt_epsi_decode(const uint8_t* data, size_t size, const abt_preview* pv, abt_image* out);

#ifdef __cplusplus
}
#endif
