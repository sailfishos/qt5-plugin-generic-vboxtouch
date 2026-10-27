# Unit tests for the plugin. Not part of the package build (the spec builds
# vboxtouch/ only); run them with: qmake && make check
TEMPLATE = app
TARGET = tst_vboxtouch
CONFIG += testcase c++11
CONFIG -= app_bundle

QT += testlib core-private gui-private quick

# The plugin's sources are compiled in, so that the ioctl() the test defines
# is the one they call.
PLUGIN_DIR = $$PWD/../vboxtouch
INCLUDEPATH += $$PLUGIN_DIR

SOURCES = \
    tst_vboxtouch.cpp \
    $$PLUGIN_DIR/evdevmousehandler.cpp \
    $$PLUGIN_DIR/setshape.cpp \
    $$PLUGIN_DIR/vboxtouch.cpp \
    $$PLUGIN_DIR/zoomindicator.cpp

HEADERS = \
    $$PLUGIN_DIR/evdevmousehandler.h \
    $$PLUGIN_DIR/vboxtouch.h \
    $$PLUGIN_DIR/zoomindicator.h

# The fake devices are FIFOs, made in the build directory.
DEFINES += TEST_TMPDIR=\\\"$$OUT_PWD\\\"

# Qt takes touch and wheel positions in native pixels: run everything again
# with the screen scaled, where logical and native pixels differ.
hidpicheck.commands = QT_SCALE_FACTOR=2 ./$(QMAKE_TARGET)
hidpicheck.depends = $(QMAKE_TARGET)
QMAKE_EXTRA_TARGETS += hidpicheck
check.depends += hidpicheck
