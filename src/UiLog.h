#pragma once

class QApplication;

// Logs what the user sees and does, for every window: each message box (with the
// button chosen) and status bar message, and at debug level the menu items, toolbar
// buttons, shortcuts and buttons used.
namespace UiLog {
void install(QApplication &app);
}
