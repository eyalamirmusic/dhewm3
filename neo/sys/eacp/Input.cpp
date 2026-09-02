/*
===========================================================================

dhewm 3 on eacp - the event queue and the input layer.

sys/events.cpp was 1928 lines of SDL, and step 5 deleted it. This is what
replaced it: the same queue, the same Sys_* surface, and the same rules for
turning a physical key into one of Doom 3's keyNum_t - with eacp's callbacks as
the producers instead of SDL_PollEvent.

The queue is here rather than in the view because that is where it already was:
sys/events.cpp:909 pushed console lines into SDL's own event queue as
SDL_USEREVENT and read them back out in Sys_GetEvent, because the engine polls
for events and the terminal reader hands them over. The keyboard and the mouse
arrive as callbacks and want the same treatment.

===========================================================================
*/

#include "Input.h"
#include "View.h"

#include <eacp/Graphics/Window/Window.h>

#include <string>

#include "sys/platform.h"
#include "idlib/containers/List.h"
#include "idlib/Heap.h"
#include "idlib/Str.h"
#include "framework/Common.h"
#include "framework/Console.h"
#include "framework/CmdSystem.h"
#include "framework/KeyInput.h"
#include "framework/Session_local.h"
#include "renderer/tr_local.h"
#include "sys/sys_public.h"
#include "sys/sys_imgui.h"

namespace KeyCode = eacp::Graphics::KeyCode;

using eacp::Graphics::Keyboard;
using eacp::Graphics::KeyEvent;
using eacp::Graphics::ModifierKeys;
using eacp::Graphics::MouseButton;
using eacp::Graphics::MouseEvent;

/*
================================================================================

    cvars

    The three the SDL layer owns that are about input rather than about SDL.
    in_grabKeyboard is deliberately not among them: it asks SDL to take the
    whole keyboard away from the OS so that Alt-Tab reaches the game, and eacp
    has no equivalent - a cvar that silently does nothing is worse than one
    that is absent, because the settings menu would still offer it.

================================================================================
*/

// Not static: idCmdSystem::ArgCompletion_String<_in_kbdNames> takes its address.
const char* _in_kbdNames[] = {"auto",
                              "english",
                              "french",
                              "german",
                              "italian",
                              "spanish",
                              "turkish",
                              "norwegian",
                              "brazilian",
                              nullptr};

static idCVar in_kbd("in_kbd",
                     _in_kbdNames[0],
                     CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_NOCHEAT,
                     "keyboard layout",
                     _in_kbdNames,
                     idCmdSystem::ArgCompletion_String<_in_kbdNames>);

static idCVar in_ignoreConsoleKey("in_ignoreConsoleKey",
                                  "0",
                                  CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_NOCHEAT | CVAR_BOOL,
                                  "Console only opens with Shift+Esc, not ` or ^ etc");

static idCVar in_nograb("in_nograb",
                        "0",
                        CVAR_SYSTEM | CVAR_NOCHEAT,
                        "prevents input grabbing");

// The settings menu's, not the input layer's: the menu names the pad's start
// button and offers the layout as an option. It stays here because this is
// where the pad will be if it is ever built (plan.md §5, gap 10).
idCVar joy_gamepadLayout(
    "joy_gamepadLayout",
    "-1",
    CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_NOCHEAT | CVAR_INTEGER,
    "Button layout of gamepad. -1: auto, 0: XBox-style, 1: Nintendo-style, "
    "2: PS4/5-style, 3: PS2/3-style",
    idCmdSystem::ArgCompletion_Integer<-1, 3>);

/*
================================================================================

    the queue

    Produced and consumed on the main thread only: eacp delivers input on the
    loop thread and idCommonLocal::Frame runs there too, so this needs no lock.
    Sys_ConsoleInput is not an exception - the terminal is read by
    Sys_GenerateEvents, on this thread, once a frame.

================================================================================
*/

namespace
{
constexpr auto noEvent = sysEvent_t {SE_NONE, 0, 0, 0, nullptr};

idList<sysEvent_t> eventQueue;

// What idUsercmdGen polls, as against what idEventLoop dispatches: two
// different consumers of the same physical events, and both have to be fed.
// The queue drives bindings and the GUIs, these drive the movement command.
struct KeyPoll
{
    int key = 0;
    bool state = false;
};

struct MousePoll
{
    int action = 0;
    int value = 0;
};

idList<KeyPoll> keyPolls;
idList<MousePoll> mousePolls;

// Which keys this layer believes are held. Only used to release them when the
// window loses focus - macOS delivers key-up to the key window alone, so
// without this a Cmd-Tab with W down leaves the player walking forever.
bool buttonStates[K_LAST_KEY] = {};

// Set by handleMouseGrab, read when a motion event arrives: in game the engine
// wants relative movement, in a menu it wants a position.
bool inRelativeMouseMode = false;

// Set from the window's activation callback.
bool inHasFocus = true;

void pushEvent(const sysEvent_t& event)
{
    eventQueue.Append(event);
}

void pushKeyEvent(int key, bool down)
{
    auto event = sysEvent_t {};

    event.evType = SE_KEY;
    event.evValue = key;
    event.evValue2 = down ? 1 : 0;

    pushEvent(event);

    if (key >= 0 && key < K_LAST_KEY)
        buttonStates[key] = down;

    keyPolls.Append(KeyPoll {key, down});
}

void pushCharEvent(int character)
{
    auto event = sysEvent_t {};

    event.evType = SE_CHAR;
    event.evValue = character;

    pushEvent(event);
}

void pushConsoleEvent(const char* line)
{
    auto length = strlen(line) + 1;
    auto* copy = (char*) Mem_Alloc((int) length);
    strcpy(copy, line);

    auto event = sysEvent_t {};

    event.evType = SE_CONSOLE;
    event.evPtrLength = (int) length;
    event.evPtr = copy;

    pushEvent(event);
}

/*
================================================================================

    scancodes

    Doom 3 can bind a key by physical position rather than by the character it
    prints - that is what the K_SC_* half of keyNum_t is for, and "SC_A" is what
    such a bind looks like in a config. eacp's KeyCode *is* that position (macOS
    virtual key values, which the Windows backend translates into), so this
    table is a straight renaming of the one sys/events.cpp keeps against
    SDL_Scancode.

    The entries eacp has no constant for keep their names and map to
    KeyCode::Unknown: a bind on one still parses and still round-trips through
    the config, it simply never fires - which is what SDL says too about a
    keyboard that has no such key.

================================================================================
*/

struct ScancodeName
{
    std::uint16_t keyCode = KeyCode::Unknown;
    const char* name = nullptr;
};

// scancodeNames[keynum - K_FIRST_SCANCODE] belongs to keynum. Must be kept in
// sync with the K_SC_* section of keyNum_t in framework/KeyInput.h.
constexpr ScancodeName scancodeNames[] = {

#define D3_SC_MAPPING(KC, X) {KeyCode::KC, "SC_" #X}
#define D3_SC_UNMAPPED(X) {KeyCode::Unknown, "SC_" #X}

    D3_SC_MAPPING(A, A),
    D3_SC_MAPPING(B, B),
    D3_SC_MAPPING(C, C),
    D3_SC_MAPPING(D, D),
    D3_SC_MAPPING(E, E),
    D3_SC_MAPPING(F, F),
    D3_SC_MAPPING(G, G),
    D3_SC_MAPPING(H, H),
    D3_SC_MAPPING(I, I),
    D3_SC_MAPPING(J, J),
    D3_SC_MAPPING(K, K),
    D3_SC_MAPPING(L, L),
    D3_SC_MAPPING(M, M),
    D3_SC_MAPPING(N, N),
    D3_SC_MAPPING(O, O),
    D3_SC_MAPPING(P, P),
    D3_SC_MAPPING(Q, Q),
    D3_SC_MAPPING(R, R),
    D3_SC_MAPPING(S, S),
    D3_SC_MAPPING(T, T),
    D3_SC_MAPPING(U, U),
    D3_SC_MAPPING(V, V),
    D3_SC_MAPPING(W, W),
    D3_SC_MAPPING(X, X),
    D3_SC_MAPPING(Y, Y),
    D3_SC_MAPPING(Z, Z),

    // The digit row, return, escape, backspace, tab and space are left out on
    // purpose: they are already answered as keys, above the scancode fallback.
    D3_SC_MAPPING(Minus, MINUS),
    D3_SC_MAPPING(Equals, EQUALS),
    D3_SC_MAPPING(LeftBracket, LEFTBRACKET),
    D3_SC_MAPPING(RightBracket, RIGHTBRACKET),
    D3_SC_MAPPING(Backslash, BACKSLASH),
    D3_SC_UNMAPPED(NONUSHASH),
    D3_SC_MAPPING(Semicolon, SEMICOLON),
    D3_SC_MAPPING(Quote, APOSTROPHE),
    D3_SC_MAPPING(Grave, GRAVE),
    D3_SC_MAPPING(Comma, COMMA),
    D3_SC_MAPPING(Period, PERIOD),
    D3_SC_MAPPING(Slash, SLASH),

    // The keypad is left out for the same reason as the digit row.
    D3_SC_UNMAPPED(NONUSBACKSLASH),
    D3_SC_UNMAPPED(INTERNATIONAL1), // used on Asian keyboards, see the USB doc
    D3_SC_UNMAPPED(INTERNATIONAL2),
    D3_SC_UNMAPPED(INTERNATIONAL3), // Yen
    D3_SC_UNMAPPED(INTERNATIONAL4),
    D3_SC_UNMAPPED(INTERNATIONAL5),
    D3_SC_UNMAPPED(INTERNATIONAL6),
    D3_SC_UNMAPPED(INTERNATIONAL7),
    D3_SC_UNMAPPED(INTERNATIONAL8),
    D3_SC_UNMAPPED(INTERNATIONAL9),
    D3_SC_UNMAPPED(THOUSANDSSEPARATOR),
    D3_SC_UNMAPPED(DECIMALSEPARATOR),
    D3_SC_UNMAPPED(CURRENCYUNIT),
    D3_SC_UNMAPPED(CURRENCYSUBUNIT)

#undef D3_SC_UNMAPPED
#undef D3_SC_MAPPING
};

static_assert(std::size(scancodeNames) == K_NUM_SCANCODES,
              "scancodeNames needs one entry per K_SC_* keynum, in that order");

int scancodeKeynumFor(std::uint16_t keyCode)
{
    if (keyCode == KeyCode::Unknown)
        return 0;

    for (int index = 0; index < K_NUM_SCANCODES; ++index)
        if (scancodeNames[index].keyCode == keyCode)
            return index + K_FIRST_SCANCODE;

    return 0;
}

/*
================================================================================

    the key table

    Doom 3's keyNum_t is mostly ASCII: a letter key *is* its lowercase
    character, which is why the stock config binds "w" and "a" rather than
    anything positional. So the identity of a printable key is the character it
    prints under the current layout, and that is what sys/events.cpp asks SDL
    for (SDL_GetKeyFromScancode, by way of the event's keycode).

    Reproduced here rather than replaced by a fixed US-positional table, and the
    reason is worth writing down, because the PureDOOM port did the opposite and
    had a good reason for it.

    Its rule was "never resolve a key by the character the *event* carries", and
    the reason was key up, not layout: the Windows backend fills in
    KeyEvent::characters on key down alone, so a down and its up would resolve
    differently and the engine would never clear the key - the player runs into
    a wall forever. That argument does not reach this table, because nothing
    here reads the event. The positional KeyCode is translated through the
    layout by Keyboard::keyCodeToCharacter, which is a pure function of the
    code: the same answer on the down and on the up, whatever the event carried.

    What that buys is that this build behaves like the one it is being measured
    against, on every layout rather than only on US - a German keyboard binds
    what dhewm3 says it binds - and that the K_SC_* half of keyNum_t, Doom 3's
    own answer to positional binding, keeps meaning what it means everywhere
    else.

    *Windows will need one fix before it can honour this.* eacp's
    Keyboard::keyCodeToCharacter calls ToUnicode with the live GetKeyboardState
    there, so it folds in whatever modifiers are held at the moment it is asked,
    where the macOS implementation translates with no modifiers at all. Holding
    Shift would turn ';' into ':' between a key's down and its up - exactly the
    asymmetry PureDOOM was avoiding, reached by a different road. The two
    backends disagree and one of them is wrong, so it is an eacp bug rather than
    a constraint to design around, and it is filed as a gap for the Windows host
    pass (plan.md §5, gap 16).

================================================================================
*/

// The character a physical key prints with no modifiers, as a Doom 3 keynum, or
// 0 for a key that prints nothing this layout can name.
int keynumByLayout(std::uint16_t keyCode)
{
    const auto characters = Keyboard::keyCodeToCharacter(keyCode);

    // Anything longer is a dead key or a ligature, and anything outside
    // printable ASCII is a character Doom 3 cannot bind by name. Both fall
    // through to K_SC_*, which is what SDL does with them too.
    if (characters.size() != 1)
        return 0;

    auto character = (unsigned char) characters.front();

    if (character < ' ' || character > '~')
        return 0;

    // SDL_GetKeyFromScancode reports letters lowercased and Doom 3's keynums
    // are the lowercase values. UCKeyTranslate agrees on macOS; the Windows
    // backend does not always (see above), so this is not redundant.
    if (character >= 'A' && character <= 'Z')
        character += 'a' - 'A';

    return character;
}

int keynumFor(std::uint16_t keyCode)
{
    // Keys whose meaning is their position and never their character. Doom 3
    // names every one of these in a config ("ENTER", "PGDN", "KP_HOME"), so a
    // layout cannot be given a say in them.
    switch (keyCode)
    {
        case KeyCode::Tab: return K_TAB;
        case KeyCode::Return: return K_ENTER;
        case KeyCode::Escape: return K_ESCAPE;
        case KeyCode::Space: return K_SPACE;

        // eacp names the key by what it does rather than by what the platform
        // calls it: Delete is backspace, ForwardDelete is the other one.
        case KeyCode::Delete: return K_BACKSPACE;
        case KeyCode::ForwardDelete: return K_DEL;
        case KeyCode::CapsLock: return K_CAPSLOCK;

        case KeyCode::UpArrow: return K_UPARROW;
        case KeyCode::DownArrow: return K_DOWNARROW;
        case KeyCode::LeftArrow: return K_LEFTARROW;
        case KeyCode::RightArrow: return K_RIGHTARROW;

        case KeyCode::PageUp: return K_PGUP;
        case KeyCode::PageDown: return K_PGDN;
        case KeyCode::Home: return K_HOME;
        case KeyCode::End: return K_END;

        case KeyCode::F1: return K_F1;
        case KeyCode::F2: return K_F2;
        case KeyCode::F3: return K_F3;
        case KeyCode::F4: return K_F4;
        case KeyCode::F5: return K_F5;
        case KeyCode::F6: return K_F6;
        case KeyCode::F7: return K_F7;
        case KeyCode::F8: return K_F8;
        case KeyCode::F9: return K_F9;
        case KeyCode::F10: return K_F10;
        case KeyCode::F11: return K_F11;
        case KeyCode::F12: return K_F12;

        // The keypad follows the labels on the keys rather than the digits,
        // which is what SDL does too: SDLK_KP_7 is K_KP_HOME.
        case KeyCode::Keypad0: return K_KP_INS;
        case KeyCode::Keypad1: return K_KP_END;
        case KeyCode::Keypad2: return K_KP_DOWNARROW;
        case KeyCode::Keypad3: return K_KP_PGDN;
        case KeyCode::Keypad4: return K_KP_LEFTARROW;
        case KeyCode::Keypad5: return K_KP_5;
        case KeyCode::Keypad6: return K_KP_RIGHTARROW;
        case KeyCode::Keypad7: return K_KP_HOME;
        case KeyCode::Keypad8: return K_KP_UPARROW;
        case KeyCode::Keypad9: return K_KP_PGUP;
        case KeyCode::KeypadDecimal: return K_KP_DEL;
        case KeyCode::KeypadEnter: return K_KP_ENTER;
        case KeyCode::KeypadPlus: return K_KP_PLUS;
        case KeyCode::KeypadMinus: return K_KP_MINUS;
        case KeyCode::KeypadMultiply: return K_KP_STAR;
        case KeyCode::KeypadDivide: return K_KP_SLASH;
        case KeyCode::KeypadClear: return K_KP_NUMLOCK;
        case KeyCode::KeypadEquals: return K_KP_EQUALS;

        // The digit row, positionally. sys/events.cpp does the same and says
        // why: an AZERTY keyboard has no unshifted digits in that row at all,
        // so a layout-driven answer would leave the weapon keys unbindable.
        case KeyCode::Num0: return '0';
        case KeyCode::Num1: return '1';
        case KeyCode::Num2: return '2';
        case KeyCode::Num3: return '3';
        case KeyCode::Num4: return '4';
        case KeyCode::Num5: return '5';
        case KeyCode::Num6: return '6';
        case KeyCode::Num7: return '7';
        case KeyCode::Num8: return '8';
        case KeyCode::Num9: return '9';

        default: break;
    }

    // The key between Esc, Tab and 1, whatever it prints. Positional for the
    // same reason as the digits: it is the console key on every layout, and on
    // most of them it is not a backtick.
    if (keyCode == KeyCode::Grave && !in_ignoreConsoleKey.GetBool())
        return K_CONSOLE;

    if (auto key = keynumByLayout(keyCode))
        return key;

    // A key that prints nothing this layout can name. Doom 3 can still bind it
    // by position.
    return scancodeKeynumFor(keyCode);
}

/*
================================================================================

    the console key

    Sys_GetConsoleKey answers the *character* the key between Esc, Tab and 1
    prints, which the console uses to know not to type itself into the input
    line. keynumFor above already turns that key into K_CONSOLE by position;
    this is the other half, and it needs the layout.

================================================================================
*/

struct ConsoleKeyMapping
{
    const char* langName;
    unsigned char key;
    unsigned char keyShifted;
};

ConsoleKeyMapping consoleKeyMappings[] = {
    {"auto", 0, 0}, // special case: filled in from the current layout
    {"english", '`', '~'},
    {"french", '<', '>'},
    {"german", '^', 176}, // °
    {"italian", '\\', '|'},
    {"spanish", 186, 170}, // º ª
    {"turkish", '"', 233}, // é
    {"norwegian", 124, 167}, // | §
    {"brazilian", '\'', '"'},
};

int consoleKeyMappingIdx = 0;

// Whether the layout has been looked at yet.
//
// Needed because Sys_InitInput, which is where this would otherwise be settled,
// is called from inside R_InitOpenGL (renderer/RenderSystem_init.cpp:822) -
// "input and sound systems need to be tied to the new window". With
// com_skipRenderer 1 that function is never reached, so until the renderer
// lands nothing calls it and the console key would answer 0 for the whole run.
//
// Lazily rather than from a second init path in the view, because the
// dependency is worth removing rather than working around: what the console key
// prints is a property of the keyboard layout and has nothing to do with
// whether a window has a GL context.
bool consoleKeyMappingReady = false;

// What the console key prints under the current layout, as a "High ASCII" char,
// or 0 if it prints nothing that can be one.
unsigned char layoutConsoleKeyChar()
{
    const auto characters = Keyboard::keyCodeToCharacter(KeyCode::Grave);

    if (characters.empty())
        return 0;

    char iso[8];

    if (D3_UTF8toISO8859_1(characters.c_str(), iso, sizeof(iso)) == nullptr)
        return 0;

    // One character, or it is not something the console can compare against.
    if (iso[0] == '\0' || iso[1] != '\0')
        return 0;

    return (unsigned char) iso[0];
}

void initConsoleKeyMapping()
{
    const auto count = (int) std::size(consoleKeyMappings);

    auto lang = idStr(in_kbd.GetString());

    consoleKeyMappingIdx = 0;
    consoleKeyMappings[0].key = 0;
    consoleKeyMappingReady = true;

    if (lang.Length() != 0 && lang.Icmp("auto") != 0)
    {
        for (int i = 1; i < count; ++i)
        {
            if (lang.Icmp(consoleKeyMappings[i].langName) != 0)
                continue;

            consoleKeyMappingIdx = i;

            auto keyChar = layoutConsoleKeyChar();

            if (keyChar != 0 && keyChar != consoleKeyMappings[i].key)
                common->Warning("in_kbd is set to \"%s\", but the actual character "
                                "of the 'console key' is %c (%d), not %c (%d), so "
                                "this might not work that well..\n",
                                lang.c_str(),
                                keyChar,
                                keyChar,
                                consoleKeyMappings[i].key,
                                consoleKeyMappings[i].key);
            break;
        }

        return;
    }

    auto keyChar = layoutConsoleKeyChar();

    if (keyChar == 0)
        return;

    for (int i = 1; i < count; ++i)
    {
        if (consoleKeyMappings[i].key == keyChar)
        {
            consoleKeyMappingIdx = i;
            common->Printf("Detected keyboard layout as \"%s\"\n",
                           consoleKeyMappings[i].langName);
            return;
        }
    }

    // Not one of the layouts named above. No shifted variant either: a layout
    // can say what a key prints, not what it prints with shift held.
    consoleKeyMappings[0].key = keyChar;
}
} // namespace

/*
================================================================================

    the producers - eacp's callbacks, arriving from View

================================================================================
*/

namespace dhewm3
{
namespace Input
{
namespace
{
View* activeView = nullptr;

// What is left of a movement too small to be a whole pixel. Kept rather than
// dropped because a slow, deliberate turn is made of exactly those: the
// engine's events are integers, and truncating each one on its own would make
// the mouse stop answering below some speed.
float mouseRemainderX = 0.0f;
float mouseRemainderY = 0.0f;

// The same, for the wheel. A notched wheel reports whole lines, but a trackpad
// reports fractions of a point, and Doom 3's wheel is a key: it has to be
// gathered into discrete presses before it can be one.
float wheelRemainder = 0.0f;

ModifierKeys heldModifiers;

// Nothing may reach the engine before common->Init has run: the window is up
// from before Apps::run's loop starts, the engine is not started until the
// first refresh, and the key table reads cvars that do not exist in that gap.
bool engineWantsInput()
{
    return common != nullptr && common->IsInitialized();
}
} // namespace

void setView(View* view)
{
    activeView = view;
}

View* getView()
{
    return activeView;
}

void keyEvent(const KeyEvent& event, bool down)
{
    if (!engineWantsInput())
        return;

    // Before the key itself, so the modifier's own down lands ahead of what it
    // modifies. Every event carries the modifier state that was live when the
    // platform made it, and reading it here is what makes a tap of Shift or
    // Ctrl reliable: the once-a-frame poll in View::update can miss one
    // entirely - pressed and released between two refreshes, it never differs
    // from the state the poll last saw.
    syncModifiers(event.modifiers);

    auto key = keynumFor(event.keyCode);

    if (key == 0)
    {
        if (down)
            common->Warning("unmapped key, eacp KeyCode %d (0x%x)",
                            (int) event.keyCode,
                            (int) event.keyCode);
        return;
    }

    pushKeyEvent(key, down);

    if (!down)
        return;

    // SE_CHAR is what the console and the GUI text fields read, and it answers
    // a different question from SE_KEY: which character was typed, not which
    // key was pressed. SDL answers it with a separate SDL_TEXTINPUT event; eacp
    // puts the composed text on the key event itself.

    // sys/events.cpp sends this one too, and the edit field needs it: backspace
    // composes to no text, so it would otherwise never reach CharEvent.
    if (key == K_BACKSPACE)
    {
        pushCharEvent(K_BACKSPACE);
        return;
    }

    // Command is macOS's shortcut modifier, and a shortcut is not typing.
    // Control needs no such test: a control combination composes to a control
    // character, which the printable test below drops.
    if (event.modifiers.command || event.characters.empty())
        return;

    char iso[32];

    if (D3_UTF8toISO8859_1(event.characters.c_str(), iso, sizeof(iso)) == nullptr)
        return;

    for (const char* c = iso; *c != '\0'; ++c)
    {
        auto character = (unsigned char) *c;

        // Printable ISO-8859-1 only, which is what drops the rest of what macOS
        // puts in `characters`: Return arrives as "\r", Escape as "\x1b", and
        // the arrows and function keys as private-use characters that
        // D3_UTF8toISO8859_1 has already skipped.
        auto printable =
            character >= ' ' && character != 127 && (character < 0x80 || character >= 0xA0);

        if (printable)
            pushCharEvent(character);
    }
}

void mouseButton(const MouseEvent& event, bool down)
{
    if (!engineWantsInput())
        return;

    // Same reason as in keyEvent: a Ctrl-click has to reach the engine as Ctrl
    // and then the button, in that order.
    syncModifiers(event.modifiers);

    auto key = 0;
    auto action = 0;

    switch (event.button)
    {
        case MouseButton::Left:
            key = K_MOUSE1;
            action = M_ACTION1;
            break;
        case MouseButton::Right:
            key = K_MOUSE2;
            action = M_ACTION2;
            break;
        case MouseButton::Middle:
            key = K_MOUSE3;
            action = M_ACTION3;
            break;
        default:
            // Doom 3 binds up to eight mouse buttons and eacp reports every one
            // past the third as an undifferentiated `Other`, so there is
            // nothing to bind them to (plan.md §5, gap 15).
            return;
    }

    auto sysEvent = sysEvent_t {};

    sysEvent.evType = SE_KEY;
    sysEvent.evValue = key;
    sysEvent.evValue2 = down ? 1 : 0;

    pushEvent(sysEvent);

    buttonStates[key] = down;
    mousePolls.Append(MousePoll {action, down ? 1 : 0});
}

void mouseMotion(const MouseEvent& event)
{
    if (!engineWantsInput())
        return;

    if (!inRelativeMouseMode)
    {
        // A position, in points and top-left origin: eacp's backing views set
        // isFlipped, so this is the space glConfig.winWidth/winHeight are in,
        // which is what idUserInterface scales an absolute event by.
        auto sysEvent = sysEvent_t {};

        sysEvent.evType = SE_MOUSE_ABS;
        sysEvent.evValue = (int) event.pos.x;
        sysEvent.evValue2 = (int) event.pos.y;

        pushEvent(sysEvent);
        return;
    }

    // rawDelta, not delta: the device's own movement rather than the pointer's.
    // The system's acceleration curve exists so a cursor can cross a screen and
    // still land on a target; applied to a camera it makes an identical flick
    // of the hand turn a different amount depending how fast it was made. Doom 3
    // has its own `sensitivity` cvar for the scaling, so the curve is not wanted
    // twice.
    //
    // This does mean the mouse feels different from the SDL build, which takes
    // whatever relative motion SDL hands it - accelerated, on macOS. It is a
    // deliberate difference, and the only one in this file.
    mouseRemainderX += event.rawDelta.x;
    mouseRemainderY += event.rawDelta.y;

    auto dx = (int) mouseRemainderX;
    auto dy = (int) mouseRemainderY;

    mouseRemainderX -= (float) dx;
    mouseRemainderY -= (float) dy;

    if (dx == 0 && dy == 0)
        return;

    auto sysEvent = sysEvent_t {};

    sysEvent.evType = SE_MOUSE;
    sysEvent.evValue = dx;
    sysEvent.evValue2 = dy;

    pushEvent(sysEvent);

    mousePolls.Append(MousePoll {M_DELTAX, dx});
    mousePolls.Append(MousePoll {M_DELTAY, dy});
}

void mouseWheel(const MouseEvent& event)
{
    if (!engineWantsInput())
        return;

    // A notched wheel reports whole lines and a trackpad reports points, so one
    // press of K_MWHEELUP is not the same number in the two cases. eacp says
    // which arrived; these are the thresholds that make a detent one notch and
    // a comfortable two-finger swipe a few.
    const auto notch = event.preciseScrolling ? 10.0f : 1.0f;

    wheelRemainder += event.delta.y;

    while (wheelRemainder >= notch || wheelRemainder <= -notch)
    {
        auto up = wheelRemainder > 0.0f;

        wheelRemainder -= up ? notch : -notch;

        auto sysEvent = sysEvent_t {};

        sysEvent.evType = SE_KEY;
        sysEvent.evValue = up ? K_MWHEELUP : K_MWHEELDOWN;
        sysEvent.evValue2 = 1;

        pushEvent(sysEvent);

        // And the release, which sys/events.cpp does not send. A wheel has no
        // released position to report, but idKeyInput has no notion of a key
        // that is only ever pressed: without this the wheel stays down for the
        // rest of the process, so "is any key down" answers true forever.
        sysEvent.evValue2 = 0;
        pushEvent(sysEvent);

        mousePolls.Append(MousePoll {M_DELTAZ, up ? 1 : -1});
    }
}

void syncModifiers(const ModifierKeys& current)
{
    if (!engineWantsInput())
    {
        heldModifiers = current;
        return;
    }

    struct Change
    {
        bool held;
        bool wasHeld;
        int key;
    };

    const Change changes[] = {
        {current.shift, heldModifiers.shift, K_SHIFT},
        {current.control, heldModifiers.control, K_CTRL},
        {current.alt, heldModifiers.alt, K_ALT},
        {current.command, heldModifiers.command, K_LWIN},
    };

    for (const auto& change: changes)
        if (change.held != change.wasHeld)
            pushKeyEvent(change.key, change.held);

    // One key each for Shift, Ctrl and Alt, where the SDL build tells the left
    // and the right ones apart (K_SHIFT against K_RIGHT_SHIFT). eacp's
    // ModifierKeys does not carry the side, and the stock config binds the left
    // names, so both keys act as the left one.
    heldModifiers = current;
}

void setFocus(bool hasFocus)
{
    inHasFocus = hasFocus;

    if (hasFocus || !engineWantsInput())
        return;

    // Release everything. macOS delivers key-up to the key window alone, so a
    // key held across a Cmd-Tab is never released by the platform and the
    // player keeps walking. The modifiers go with them: they are polled from
    // the window, which stops being asked while it is not key.
    for (auto key = 0; key < K_LAST_KEY; ++key)
        if (buttonStates[key])
            pushKeyEvent(key, false);

    heldModifiers = ModifierKeys {};
    mouseRemainderX = mouseRemainderY = wheelRemainder = 0.0f;
}
} // namespace Input
} // namespace dhewm3

/*
================================================================================

    the grab

    GLimp_GrabInput is implemented here rather than in sys/eacp/GLimp.cpp, and
    it belongs here: it is the input layer's, and the only reason it sits in
    sys/glimp.cpp in the SDL build is that SDL's grab calls need the window
    handle that file owns. Here the window belongs to the view, which this file
    already tracks.

================================================================================
*/

void GLimp_GrabInput(int flags)
{
    auto* view = dhewm3::Input::getView();

    if (view == nullptr)
        return;

    auto* window = view->getWindow();

    if (window == nullptr)
        return;

    // One call answers three of the four flags together: eacp's mouse lock
    // hides the cursor, pins it in place and streams relative motion, which is
    // GRAB_HIDECURSOR | GRAB_GRABMOUSE | GRAB_RELATIVEMOUSE as one decision.
    // Relative is the bit that means it, and the engine never asks for the
    // other two without it.
    //
    // What has no answer is the reverse: an open menu wants the cursor hidden
    // while the pointer stays free, because Doom 3 draws its own. eacp can hide
    // the pointer only by locking it, so the system arrow shows through the
    // menus (plan.md §5, gap 14).
    //
    // GRAB_ENABLETEXTINPUT has nothing to do either, and needs nothing: it
    // exists because SDL will not deliver text events until it is asked to, and
    // eacp puts the characters on every key event.
    window->setMouseLocked((flags & GRAB_RELATIVEMOUSE) != 0);
}

// Transcribed from sys/events.cpp, minus the parts that were SDL's. The engine
// never asks for the mouse; it describes what it is showing, and this decides.
static void handleMouseGrab()
{
    // The defaults for when the window does *not* have focus: don't grab in any
    // way.
    auto showCursor = true;
    auto grabMouse = false;
    auto relativeMouse = false;
    auto enableTextInput = false;

    const auto imguiHasFocus = D3::ImGuiHooks::ShouldShowCursor();

    // If com_editorActive, release everything, just like when we have no focus.
    if (inHasFocus && !com_editorActive && !imguiHasFocus)
    {
        // Note: this generally handles fullscreen menus, but not the PDA, which
        // is an ugly hack in game code that does not go through
        // sessLocal.guiActive - so the PDA keeps using relative mouse events to
        // set its cursor position.
        const auto menuActive = sessLocal.GetActiveMenu() != nullptr;

        if (menuActive)
        {
            showCursor = false;
            relativeMouse = false;
            grabMouse = false;
            enableTextInput = true;
        }
        else if (console->Active())
        {
            showCursor = true;
            relativeMouse = grabMouse = false;
            enableTextInput = true;
        }
        else // in game
        {
            showCursor = false;
            grabMouse = relativeMouse = true;
        }

        inRelativeMouseMode = relativeMouse;

        // With in_nograb set, inRelativeMouseMode and relativeMouse disagree on
        // purpose: don't lock the mouse, but still feed the game relative
        // events.
        if (in_nograb.GetBool())
            grabMouse = relativeMouse = false;
    }
    else
    {
        inRelativeMouseMode = false;
        enableTextInput = imguiHasFocus;
    }

    auto flags = 0;

    if (!showCursor)
        flags |= GRAB_HIDECURSOR;
    if (grabMouse)
        flags |= GRAB_GRABMOUSE;
    if (relativeMouse)
        flags |= GRAB_RELATIVEMOUSE;
    if (enableTextInput)
        flags |= GRAB_ENABLETEXTINPUT;

    GLimp_GrabInput(flags);
}

/*
===============
Sys_GrabMouseCursor

Note: grabbing is normally handled in idCommonLocal::Frame() ->
Sys_GenerateEvents() -> handleMouseGrab(). This is for releasing the mouse
before a long operation, where common->Frame() will not be called for a while.
===============
*/
void Sys_GrabMouseCursor(bool grabIt)
{
    auto flags = grabIt ? (GRAB_GRABMOUSE | GRAB_HIDECURSOR | GRAB_RELATIVEMOUSE) : 0;

    GLimp_GrabInput(flags);
}

/*
================================================================================

    the engine's side of the queue

================================================================================
*/

sysEvent_t Sys_GetEvent()
{
    if (eventQueue.Num() == 0)
        return noEvent;

    auto event = eventQueue[0];
    eventQueue.RemoveIndex(0);

    return event;
}

void Sys_ClearEvents()
{
    // The engine frees evPtr on the events it is handed; the ones dropped here
    // were never handed over, so this is the only place that owns them.
    for (int i = 0; i < eventQueue.Num(); ++i)
        if (eventQueue[i].evPtr != nullptr)
            Mem_Free(eventQueue[i].evPtr);

    eventQueue.Clear();

    keyPolls.SetNum(0, false);
    mousePolls.SetNum(0, false);

    memset(buttonStates, 0, sizeof(buttonStates));
}

void Sys_GenerateEvents()
{
    D3P_ScopedCPUSample(Sys_GenerateEvents);

    handleMouseGrab();

    if (auto* line = Sys_ConsoleInput())
        pushConsoleEvent(line);

    // Nothing to pump: eacp's loop delivers input by callback, between frames,
    // so the queue is already filled by the time this runs.
}

/*
================================================================================

    polling - what idUsercmdGen reads

================================================================================
*/

int Sys_PollKeyboardInputEvents()
{
    return keyPolls.Num();
}

int Sys_ReturnKeyboardInputEvent(const int n, int& key, bool& state)
{
    if (n >= keyPolls.Num())
        return 0;

    key = keyPolls[n].key;
    state = keyPolls[n].state;

    return 1;
}

void Sys_EndKeyboardInputEvents()
{
    keyPolls.SetNum(0, false);
}

int Sys_PollMouseInputEvents()
{
    return mousePolls.Num();
}

int Sys_ReturnMouseInputEvent(const int n, int& action, int& value)
{
    if (n >= mousePolls.Num())
        return 0;

    action = mousePolls[n].action;
    value = mousePolls[n].value;

    return 1;
}

void Sys_EndMouseInputEvents()
{
    mousePolls.SetNum(0, false);
}

/*
================================================================================

    names, and the rest of the input surface

    No gamepad: it is a scope cut (plan.md §7) and eacp has no controller module
    to build one on (§5, gap 10). The engine calls all of this every frame
    whether or not anything is bound, so none of it may be left undefined.

================================================================================
*/

const char* Sys_GetScancodeName(int key)
{
    if (key >= K_FIRST_SCANCODE && key <= K_LAST_SCANCODE)
        return scancodeNames[key - K_FIRST_SCANCODE].name;

    return nullptr;
}

// !! The returned string is only valid until the next call. !!
static const char* localizedScancodeName(int key, bool useUtf8)
{
    if (key < K_FIRST_SCANCODE || key > K_LAST_SCANCODE)
        return nullptr;

    const auto& mapping = scancodeNames[key - K_FIRST_SCANCODE];

    if (mapping.keyCode != KeyCode::Unknown)
    {
        const auto characters = Keyboard::keyCodeToCharacter(mapping.keyCode);

        if (!characters.empty())
        {
            // A key is named by the character it prints, uppercased - "A"
            // rather than "a" - which is what SDL_GetKeyName returns and what
            // the settings menu expects to draw.
            static char utf8Name[32];
            static char isoName[32];

            idStr::Copynz(utf8Name, characters.c_str(), sizeof(utf8Name));

            if (useUtf8)
            {
                // Only the ASCII half can be uppercased byte-wise; a multi-byte
                // character is returned as it came.
                if (utf8Name[0] >= 'a' && utf8Name[0] <= 'z' && utf8Name[1] == '\0')
                    utf8Name[0] -= 'a' - 'A';

                return utf8Name;
            }

            // Doom 3's own font is ISO-8859-1 ("High ASCII") and cannot print
            // anything else, so a name that will not convert falls back to
            // "SC_*" rather than being drawn as mojibake.
            auto* converted = D3_UTF8toISO8859_1(utf8Name, isoName, sizeof(isoName));

            if (converted != nullptr && isoName[0] != '\0')
            {
                auto first = (unsigned char) isoName[0];
                auto lower = (first >= 'a' && first <= 'z')
                          || (first >= 0xE0 && first <= 0xFE && first != 0xF7);

                if (lower)
                    isoName[0] = (char) (first - 32);

                return isoName;
            }
        }
    }

    return mapping.name;
}

const char* Sys_GetLocalizedScancodeName(int key)
{
    return localizedScancodeName(key, false);
}

const char* Sys_GetLocalizedScancodeNameUTF8(int key)
{
    return localizedScancodeName(key, true);
}

// Returns the K_SC_* keynum for a scancode name like "SC_A", or -1 if there is
// none. Only worth calling for a name that starts with "SC_" (or "sc_").
int Sys_GetKeynumForScancodeName(const char* name)
{
    for (int index = 0; index < K_NUM_SCANCODES; ++index)
        if (idStr::Icmp(name, scancodeNames[index].name) == 0)
            return index + K_FIRST_SCANCODE;

    return -1;
}

unsigned char Sys_GetConsoleKey(bool shifted)
{
    if (in_ignoreConsoleKey.GetBool())
        return 0;

    if (!consoleKeyMappingReady || in_kbd.IsModified())
    {
        initConsoleKeyMapping();
        in_kbd.ClearModified();
    }

    const auto& mapping = consoleKeyMappings[consoleKeyMappingIdx];

    return shifted ? mapping.keyShifted : mapping.key;
}

unsigned char Sys_MapCharForKey(int key)
{
    return (unsigned char) (key & 0xff);
}

void Sys_InitInput()
{
    initConsoleKeyMapping();
    in_kbd.ClearModified();
}

void Sys_ShutdownInput()
{
    Sys_ClearEvents();
}

void Sys_InitScanTable()
{
}

const char* Sys_GetLocalizedJoyKeyName(int)
{
    return nullptr;
}

const char* D3_GetGamepadStartButtonName()
{
    return "Pad Start";
}

void Sys_SetRumble(int, int, int)
{
}

int Sys_PollJoystickInputEvents(int)
{
    return 0;
}

int Sys_ReturnJoystickInputEvent(const int, int&, int&)
{
    return 0;
}

void Sys_EndJoystickInputEvents()
{
}

// Read by idUsercmdGenLocal::MakeCurrent (framework/UsercmdGen.cpp:922) to know
// whether a cursor GUI is swallowing the gamepad's face buttons. There is no
// gamepad here, so it never becomes true - but it is not the gamepad's
// variable, it is the game's, and the game reads it every frame.
bool D3_IN_interactiveIngameGuiActive = false;

void Sys_SetInteractiveIngameGuiActive(bool, idUserInterface*)
{
    // The bookkeeping this does in sys/events.cpp exists only to decide the
    // value above, and only the gamepad path reads it.
}
