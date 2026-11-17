#pragma once
#include "abt_win.h"
#include <unknwn.h>
#include <new>

extern HINSTANCE g_dll;
void dll_addref();
void dll_release();

// Debug switches for experiment E1 (HKCU\Software\abthumb\Debug: NoStream/NoItem/NoFile = 1).
struct ExposeFlags { bool stream, item, file; };
const ExposeFlags& expose_flags();

HRESULT create_thumbnail_provider(REFIID riid, void** ppv);
HRESULT create_preview_handler(REFIID riid, void** ppv);

// Draws the step-2 placeholder: label + size on a colour keyed by extension. BGRA, alpha 255.
void draw_placeholder(void* bits, int w, int h, const wchar_t* path, const wchar_t* line2);
