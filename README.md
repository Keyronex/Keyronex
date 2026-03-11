![Keyronex Logo](docs/keyronex.svg)

Keyronex is a hobby operating system. It makes no pretences to be anything novel
or exciting, and doesn't do anything likely to be interesting to anyone. Not a
great deal is implemented yet, and of what is implemented, a lot needs much more
work to bring into decent shape.

The long-term goal is a fairly competent, moderately featureful operating system
by something like the standards of the early 1990s. The system takes technical
influence mainly from the Unix tradition (principally Solaris, NetBSD, and
Mach/NeXTSTEP/OS X) and the VMS tradition (OpenVMS and NT). It is a portable
system with complete ports to amd64 and m68k, with aarch64 and riscv planned for
the future.


Licence
-------

Code original to Keyronex is licenced under the Mozilla Public Licence v2.0
(MPLv2).
Other components are under their own licences, all of which are MPL compatible;
these are mostly the BSD or similar licences.
Most third-party components are in the `vendor` folder and their licences are
displayed there or in their source files.

Building
--------

Keyronex building is tested on CI under Ubuntu 24.04 LTS, so that is probably
the easiest platform to build it on.
To build Keyronex and all of the userspace you will need the following
dependencies:

```
autopoint
gettext
git
gperf
help2man
libgmp-dev
libmpc-dev
libmpfr-dev
libtool
m4
meson (>= 0.57.0)
pkg-config
python3
python3-mako
python3-pip
texinfo
yacc
xbstrap
xorriso
```

These packages are gotten with `apt install` on Ubuntu, except for `xbstrap`,
which is gotten with `pip install xbstrap`.
