PRoot
=====

*chroot, mount --bind, and binfmt_misc without privilege/setup for Linux*

Build status
============

**Please take the PRoot Usage Survey for 2023!**

.. image:: https://img.shields.io/badge/survey-2023-green?style=flat-square
   :target: https://www.surveymonkey.com/r/7GVXS7W
   
--

.. image:: https://img.shields.io/gitlab/pipeline/proot/proot.svg?label=gitlab-ci&style=flat-square
   :target: https://gitlab.com/proot/proot/pipelines

.. image:: https://img.shields.io/badge/scan--build-latest-yellow.svg?style=flat-square
   :target: https://proot.gitlab.io/proot/reports/scan-build

.. image:: https://img.shields.io/badge/lcov-latest-6688D4.svg?style=flat-square
   :target: https://proot.gitlab.io/proot/reports/lcov

.. image:: https://img.shields.io/cii/summary/2444.svg?label=cii-best-practices&style=flat-square
   :target: https://bestpractices.coreinfrastructure.org/projects/2444

.. image:: https://img.shields.io/badge/DOI-10.5281%2Fzenodo.5371409-blue?style=flat-square
   :target: https://doi.org/10.5281/zenodo.5371409

Compiling
=========

The following commands can be used to compile PRoot and CARE::

    make -C src loader.elf loader-m32.elf build.h # first build the config and loader
    make -C src proot care # then compile PRoot and CARE
    make -C test # run test suite

|asciicast|

.. |asciicast| image:: https://asciinema.org/a/315367.svg
   :target: https://asciinema.org/a/315367

Dependencies
============

- `libarchive <https://libarchive.org>`_
- `libtalloc <https://talloc.samba.org>`_
- `uthash <https://troydhanson.github.io/uthash>`_ (only required for building CARE)

Manuals
=======

- `PRoot <https://github.com/proot-me/proot/blob/master/doc/proot/manual.rst#proot>`_

- `CARE <https://github.com/proot-me/proot/blob/master/doc/care/manual.rst#care>`_

Support
=======

- `Mailing List <mailto:proot_me@googlegroups.com>`_
- `Forum <https://groups.google.com/forum/?fromgroups#!forum/proot_me>`_
- `Chat <https://gitter.im/proot-me/devs>`_

License
=======

SPDX-License-Identifier: `GPL-2.0-or-later <COPYING>`_

ASDroid Android launch diagnostics patch
========================================

The ASDroid fork patches ``src/tracee/event.c`` so the forked tracee never
returns into PRoot's main control flow after ``PTRACE_TRACEME``, ``SIGSTOP``
or ``execvp`` fails. Those paths now emit the system error and exit with a
specific child status (126/127), instead of potentially surfacing as an opaque
PRoot exit status 255. This is a diagnostic/control-flow fix; it does not
bypass Android SELinux or ptrace restrictions. Rebuild the native PRoot binary
and loader and package both into the APK before testing.
