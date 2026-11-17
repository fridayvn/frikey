// FriKey's settings window (FriSettings.cpp): pages Gõ tiếng Việt, Dịch, Giới thiệu.
#pragma once
#include <windows.h>
namespace vikey { namespace ui {
enum Page { PageTyping = 0, PageTranslate = 1, PageAbout = 2 };
void showSettings(HINSTANCE inst, int page);
void languageChanged(int lang);   // the typing language changed (tray, hotkey): the window and the emoji picker follow
void refreshSettings();           // translate mode switched from the tray or its hotkey
}}
