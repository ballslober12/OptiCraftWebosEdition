// NetcastOsk.h -- on-screen keyboard for the NetCast gamepad.
#pragma once
#include <string>

class FontRenderer;

namespace netcast
{
// Called every frame the current GUI screen is drawn. fieldText == nullptr when
// no text field is focused (the keyboard then closes by itself).
void oskRender(FontRenderer *font, int screenW, int screenH, const std::string *fieldText);

// Used by the pad pump.
bool oskActive();                 // open and a text field is focused
bool oskCanOpen();                // a text field is focused
void oskOpen();
void oskClose();
void oskAutoOpenNextField();      // open as soon as the next text field gets focus (chat)
// dx/dy: navigation steps; type/back/space/shift/enter/close: button edges.
void oskInput(int dx, int dy, bool type, bool back, bool space, bool shift, bool enter, bool close);
}
