#pragma once
#define IDI_APP 101
#define IDR_SOUND_VI 201
#define IDR_SOUND_EN 202
#define IDR_EMOJI_JSON 203
#define IDR_FONT_REGULAR 210
#define IDR_FONT_MEDIUM 211
#define IDR_FONT_SEMIBOLD 212
#define IDR_FONT_BOLD 213
#ifndef FRIKEY_VERSION
#define FRIKEY_VERSION "0.2.0"
#endif
#ifndef VIKEY_VERSION
#define VIKEY_VERSION "0.1.0"
#endif
#define VIKEY_W2(x) L##x
#define VIKEY_W(x) VIKEY_W2(x)
#define VIKEY_VERSION_W VIKEY_W(VIKEY_VERSION)
