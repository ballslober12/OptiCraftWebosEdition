# Controls — OptiCraft webOS Edition

Input reaches the game through three paths that are all live at the same time:
the Magic Remote (SDL key events), a USB mouse or the remote pointer (SDL mouse
events), and gamepads (`SDL_GameController`). Menus use the on-screen pointer;
in the world the mouse is grabbed and the pointer moves the camera.

## Gamepad

Works with any controller SDL recognises (Xbox / PlayStation / most Bluetooth
and USB pads). Pads SDL has no mapping for are usually still readable as
`SDL_Joystick`, but the button names below assume a game-controller mapping.

| Gamepad | In the world | In menus / inventory / chat |
|---|---|---|
| Left stick | walk (analog, `platformGamepadSnapshot`) | move the pointer |
| Right stick | look around | move the pointer |
| A | jump | left click |
| B | Q (drop item) | Escape |
| X | inventory (E) | E (closes the inventory) |
| Y | inventory (E) | E |
| RT | break block (left click, hold to mine) | left click |
| LT | place / use (right click) | right click |
| LB / RB | previous / next hotbar slot | scroll |
| Start | Escape (pause) | Escape |
| Back | player list (Tab) | Tab |
| L3 (left stick click) | sneak (Shift) | – |
| R3 (right stick click) | sprint (Ctrl) | – |
| D-pad up | debug screen (F3) | one slot up; keyboard up: move up |
| D-pad down | – | one slot down; text screen: show / hide the keyboard |
| D-pad left / right | left: open chat (T) / right: debug (F3) | one slot left / right |
| D-pad down (text field focused) | – | open the on-screen keyboard |

Stick **deadzone**, **look speed** and **menu pointer speed** are in
`webos.cfg` (`deadzone`, `look_speed`, `menu_speed`), with
`pad_invert_y`, `pad_swap_sticks` and `hat_invert_y` for taste.

## Magic Remote

### In the world

| Remote button | Action |
|---|---|
| Arrow up / down | walk forward / back |
| Arrow left / right | turn the camera |
| OK | attack / break block (left click) |
| Back | Escape (pause menu) |
| Red | use / place block (right click) |
| Green | jump |
| Yellow | inventory |
| Blue | sneak |
| CH+ / CH- | hotbar next / previous |
| 1–9, 0 | hotbar slots / typed digits in text fields |

### In menus and inventory

| Remote button | Action |
|---|---|
| Pointer / arrow keys | move the on-screen pointer (arrow keys accelerate while held) |
| OK | left click |
| Red | right click |
| Back | Escape |
| 0–9 | typed as digits when the pointer is over a text field |
| 2 | closes the inventory (E) |

### While the on-screen keyboard is open

| Button | Action |
|---|---|
| Arrow keys | move between keys |
| OK | press the highlighted key |
| Back | Escape |

## On-screen keyboard

The webOS userland gives native apps no virtual-keyboard API, so the game draws
its own QWERTY grid over the current screen. It is only offered on screens that
need text (chat, multiplayer address, world name / seed, renaming a world, sign
editing, player name) and never appears while you are just playing.

| Button | Action |
|---|---|
| D-pad | move the highlight |
| A | type the highlighted key |
| X | backspace |
| Y | space |
| LB | shift (one character) |
| Start | Enter (sends chat / confirms) |
| B | close the keyboard |

A USB keyboard works as usual as soon as one is plugged in; its text goes
straight into the focused field.

## Pointer and cursor

The pointer is drawn in menus and GUI screens (inventory, chat, …) and hidden
while playing in the world. It follows the Magic Remote, a USB mouse, the left
stick, the right stick and the d-pad.
