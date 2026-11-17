#include "abt_core.h"

const char* abt_status_str(abt_status s) {
    switch (s) {
    case ABT_OK: return "ok";
    case ABT_E_NOTFOUND: return "not found";
    case ABT_E_FORMAT: return "bad format";
    case ABT_E_TRUNCATED: return "truncated";
    case ABT_E_UNSUPPORTED: return "unsupported";
    case ABT_E_NOMEM: return "out of memory";
    case ABT_E_IO: return "io error";
    case ABT_E_CANCELLED: return "cancelled";
    case ABT_E_LIMIT: return "limit exceeded";
    }
    return "?";
}

const char* abt_family_str(abt_family f) {
    switch (f) {
    case ABT_FAM_AI_PDF: return "AI";
    case ABT_FAM_AI_PS: return "AI (PostScript)";
    case ABT_FAM_EPS: return "EPS";
    case ABT_FAM_PSD: return "PSD";
    case ABT_FAM_PSB: return "PSB";
    case ABT_FAM_PDF: return "PDF";
    default: return "unknown";
    }
}

const char* abt_color_mode_str(abt_color_mode m) {
    switch (m) {
    case ABT_CM_BITMAP: return "Bitmap";
    case ABT_CM_GRAY: return "Grayscale";
    case ABT_CM_INDEXED: return "Indexed";
    case ABT_CM_RGB: return "RGB";
    case ABT_CM_CMYK: return "CMYK";
    case ABT_CM_MULTICHANNEL: return "Multichannel";
    case ABT_CM_DUOTONE: return "Duotone";
    case ABT_CM_LAB: return "Lab";
    default: return "";
    }
}
