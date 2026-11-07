#include "x11_embed.hpp"

#include <X11/Xlib.h>

#include <algorithm>
#include <cstdint>

namespace oscillate::plugin_ui {
namespace {

constexpr long xembed_embedded_notify = 0;
constexpr long xembed_window_activate = 1;
constexpr long xembed_focus_in = 4;

void send_message(Display* display, Window child, Atom atom, long opcode, Window parent,
                  long version) {
  XEvent event{};
  event.xclient.type = ClientMessage;
  event.xclient.display = display;
  event.xclient.window = child;
  event.xclient.message_type = atom;
  event.xclient.format = 32;
  event.xclient.data.l[0] = CurrentTime;
  event.xclient.data.l[1] = opcode;
  event.xclient.data.l[3] = static_cast<long>(parent);
  event.xclient.data.l[4] = version;
  XSendEvent(display, child, False, NoEventMask, &event);
}

// The scan's own connection: a child destroyed after XQueryTree raises
// BadWindow there, which must not reach the default handler (it exits the
// process). Errors on any other connection go to the handler that was set.
Display* scanning_display = nullptr;
XErrorHandler previous_handler = nullptr;

int scan_error_handler(Display* display, XErrorEvent* error) {
  if (display == scanning_display)
    return 0;
  return previous_handler != nullptr ? previous_handler(display, error) : 0;
}

} // namespace

void notify_xembed_child(std::uintptr_t parent_window) noexcept {
  if (parent_window == 0)
    return;
  Display* display = XOpenDisplay(nullptr);
  if (!display)
    return;
  // UI thread only, like every caller of this function.
  scanning_display = display;
  previous_handler = XSetErrorHandler(scan_error_handler);
  const auto parent = static_cast<Window>(parent_window);
  const Atom info_atom = XInternAtom(display, "_XEMBED_INFO", True);
  const Atom message_atom = XInternAtom(display, "_XEMBED", False);
  Window root = 0;
  Window owner = 0;
  Window* children = nullptr;
  unsigned int count = 0;
  if (info_atom != None && message_atom != None &&
      XQueryTree(display, parent, &root, &owner, &children, &count)) {
    for (unsigned int index = 0; index < std::min(count, 64U); ++index) {
      Atom type = None;
      int format = 0;
      unsigned long items = 0;
      unsigned long remaining = 0;
      unsigned char* data = nullptr;
      const auto result = XGetWindowProperty(display, children[index], info_atom, 0, 2, False,
                                             info_atom, &type, &format, &items, &remaining, &data);
      if (result == Success && type == info_atom && format == 32 && items >= 2 && data) {
        const auto version = static_cast<long*>(static_cast<void*>(data))[0];
        send_message(display, children[index], message_atom, xembed_embedded_notify, parent,
                     version);
        send_message(display, children[index], message_atom, xembed_window_activate, parent,
                     version);
        send_message(display, children[index], message_atom, xembed_focus_in, parent, version);
      }
      if (data)
        XFree(data);
    }
  }
  if (children)
    XFree(children);
  XSync(display, False);
  XSetErrorHandler(previous_handler);
  scanning_display = nullptr;
  previous_handler = nullptr;
  XCloseDisplay(display);
}

} // namespace oscillate::plugin_ui
