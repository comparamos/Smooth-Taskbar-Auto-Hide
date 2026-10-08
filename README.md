# Smooth Taskbar Auto-Hide

Replaces the taskbar's instant auto-hide behavior with a smooth slide animation. The taskbar window itself moves—it does not fade—so it remains visually identical to the native Windows taskbar.

> **Requirement:** Enable Windows' **“Automatically hide the taskbar”** setting. When this setting is disabled, the mod does nothing and normal taskbar behavior is restored.

## How it works

The mod intercepts Explorer's decisions to show or hide the taskbar (`SetWindowPos` on `Shell_TrayWnd` and `Shell_SecondaryTrayWnd`) and turns them into a smoothly eased animation.

It also adds bottom-edge cursor hover detection and an optional trigger to show the taskbar when a window is minimized. These triggers are suppressed while a fullscreen app is in the foreground.

## Animation direction

**Automatic** moves the taskbar toward the edge where it is docked. For example, a taskbar docked at the bottom slides downward when it hides. Forcing a different direction makes the taskbar sweep across the screen toward that side, which is rarely desirable.
