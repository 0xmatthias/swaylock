# swaylock that acts like slock

swaylock is a screen locking utility for Wayland compositors. It is compatible
with any Wayland compositor which implements the following Wayland protocols:

- wlr-layer-shell
- wlr-input-inhibitor
- xdg-output
- xdg-shell

See the man page, `swaylock(1)`, for instructions on using swaylock.

### Compiling from Source

Install dependencies:

* meson \*
* wayland
* wayland-protocols \*
* libxkbcommon
* cairo
* gdk-pixbuf2 \*\*
* pam (optional)
* [scdoc](https://git.sr.ht/~sircmpwn/scdoc) (optional: man pages) \*
* git \*

_\*Compile-time dep_

_\*\*optional: required for background images other than PNG_

Run these commands:

    meson build
    ninja -C build
    sudo ninja -C build install

On systems without PAM, you need to suid the swaylock binary:

    sudo chmod a+s /usr/local/bin/swaylock

Swaylock will drop root permissions shortly after startup.
