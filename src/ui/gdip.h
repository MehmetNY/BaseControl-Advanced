// GDI+ include wrapper. mingw's <gdiplus.h> calls unqualified min()/max(), which NOMINMAX removes.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ole2.h>
#include <propidl.h>

#include <algorithm>
using std::max;
using std::min;

#include <gdiplus.h>
