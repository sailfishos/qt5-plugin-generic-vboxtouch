/****************************************************************************
**
** Copyright (C) 2026 Herman van Hazendonk <github.com@herrie.org>
**
** GNU Lesser General Public License Usage
** This file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
****************************************************************************/

// Tests for the touch screen handler, on the offscreen platform.
//
// The input devices are FIFOs. ioctl() is defined here, so the plugin's
// calls - compiled into this binary - reach it: for the FIFO paths it
// answers like an evdev tablet, an evdev mouse or vboxguest would; every
// other fd goes to the kernel.

#include "vboxtouch.h"
#include "evdevmousehandler.h"

#include <QtTest>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QScreen>
#include <QTemporaryDir>
#include <QKeyEvent>
#include <QTouchEvent>
#include <QWheelEvent>
#include <QWindow>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QPointingDevice>
#else
#include <QTouchDevice>
#endif

#include <qpa/qwindowsysteminterface.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <map>
#include <mutex>
#include <string>

namespace {

enum class Kind { AbsTablet, Integration, RelMouse, VboxGuest };

std::mutex g_lock;
std::map<std::string, Kind> g_devices;
int g_guestX = 0;
int g_guestY = 0;
long g_clock = 0; // microseconds: every event written is later than the one before
int g_tabletX = 0; // where the kernel last saw the fake tablet
int g_tabletY = 0;
int g_integrationX = 0; // and the fake mouse integration device
int g_integrationY = 0;

const int TabletMax = 32767; // VirtualBox's USB tablet
const int IntegrationMax = 65535; // and its mouse integration device

bool kindOf(int fd, Kind *kind)
{
    char link[64];
    char path[4096];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    const ssize_t n = readlink(link, path, sizeof(path) - 1);
    if (n <= 0)
        return false;
    path[n] = '\0';
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_devices.find(path);
    if (it == g_devices.end())
        return false;
    *kind = it->second;
    return true;
}

// Mirrors the request the plugin sends (vboxtouch.cpp).
struct MouseStatus {
    uint32_t size, version, type;
    int32_t rc;
    uint32_t reserved1, reserved2, features;
    int32_t x, y;
};

int fakeIoctl(Kind kind, unsigned long request, void *arg)
{
    const unsigned type = _IOC_TYPE(request);
    const unsigned nr = _IOC_NR(request);

    if (kind == Kind::VboxGuest) {
        if (type != 'V') {
            errno = ENOTTY;
            return -1;
        }
        if (nr == 3 && _IOC_SIZE(request) == sizeof(MouseStatus)) {
            MouseStatus *status = static_cast<MouseStatus *>(arg);
            std::lock_guard<std::mutex> lock(g_lock);
            status->features = 2; // VBOXMOUSE_IS_ABSOLUTE
            status->x = g_guestX;
            status->y = g_guestY;
            status->rc = 0;
        }
        return 0; // set pointer shape (3), set mouse status (15)
    }

    if (type != 'E') {
        errno = ENOTTY;
        return -1;
    }
    if (request == EVIOCGRAB)
        return 0;
    if (nr == _IOC_NR(EVIOCGBIT(EV_ABS, 0))) {
        memset(arg, 0, _IOC_SIZE(request));
        if (kind == Kind::AbsTablet || kind == Kind::Integration) {
            unsigned char *bits = static_cast<unsigned char *>(arg);
            bits[ABS_X / 8] |= 1 << (ABS_X % 8);
            bits[ABS_Y / 8] |= 1 << (ABS_Y % 8);
        }
        return 0;
    }
    if (kind == Kind::Integration && _IOC_NR(request) == _IOC_NR(EVIOCGNAME(0))) {
        static const char name[] = "VirtualBox mouse integration";
        const size_t length = qMin(sizeof(name), size_t(_IOC_SIZE(request)));
        memcpy(arg, name, length);
        return int(length);
    }
    if ((kind == Kind::AbsTablet || kind == Kind::Integration)
        && (request == EVIOCGABS(ABS_X) || request == EVIOCGABS(ABS_Y))) {
        input_absinfo *info = static_cast<input_absinfo *>(arg);
        memset(info, 0, sizeof(*info));
        info->maximum = kind == Kind::Integration ? IntegrationMax : TabletMax;
        std::lock_guard<std::mutex> lock(g_lock);
        if (kind == Kind::Integration)
            info->value = request == EVIOCGABS(ABS_X) ? g_integrationX : g_integrationY;
        else
            info->value = request == EVIOCGABS(ABS_X) ? g_tabletX : g_tabletY;
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

} // namespace

extern "C" int ioctl(int fd, unsigned long request, ...) noexcept
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    Kind kind;
    if (kindOf(fd, &kind))
        return fakeIoctl(kind, request, arg);
    return static_cast<int>(syscall(SYS_ioctl, fd, request, arg));
}

namespace {

struct Touch {
    QEvent::Type type;
    QPointF pos;
    QList<int> ids; // of all the points
};

struct Wheel {
    QPoint angleDelta;
    QPointF pos;
};

class TouchWindow : public QWindow
{
public:
    QList<Touch> touches;
    QList<Wheel> wheels;

protected:
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::TouchBegin || e->type() == QEvent::TouchUpdate
            || e->type() == QEvent::TouchEnd) {
            const QTouchEvent *te = static_cast<QTouchEvent *>(e);
            QList<int> ids;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            for (const QEventPoint &p : te->points())
                ids << p.id();
            if (!te->points().isEmpty())
                touches << Touch{ e->type(), te->points().first().globalPosition(), ids };
#else
            for (const QTouchEvent::TouchPoint &p : te->touchPoints())
                ids << p.id();
            if (!te->touchPoints().isEmpty())
                touches << Touch{ e->type(), te->touchPoints().first().screenPos(), ids };
#endif
            e->accept();
            return true;
        }
        if (e->type() == QEvent::Wheel) {
            const QWheelEvent *we = static_cast<QWheelEvent *>(e);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
            wheels << Wheel{ we->angleDelta(), we->globalPosition() };
#else
            wheels << Wheel{ we->angleDelta(), we->globalPosF() };
#endif
            e->accept();
            return true;
        }
        return QWindow::event(e);
    }
};

// A FIFO standing in for a device; the write end is opened once the plugin
// holds the read end, and kept open so the plugin never sees EOF.
class FakeDevice
{
public:
    FakeDevice(const QString &path, Kind kind) : m_path(path)
    {
        QVERIFY2(mkfifo(qPrintable(path), 0600) == 0, qPrintable(path));
        std::lock_guard<std::mutex> lock(g_lock);
        g_devices[path.toStdString()] = kind;
    }
    ~FakeDevice()
    {
        if (m_fd >= 0)
            close(m_fd);
        std::lock_guard<std::mutex> lock(g_lock);
        g_devices.erase(m_path.toStdString());
    }
    QString path() const { return m_path; }

    void event(int type, int code, int value)
    {
        if (!openWriter())
            return;
        input_event ev;
        memset(&ev, 0, sizeof(ev));
        {
            // The kernel stamps each event; reading in order relies on it.
            std::lock_guard<std::mutex> lock(g_lock);
            ++g_clock;
            ev.input_event_sec = g_clock / 1000000;
            ev.input_event_usec = g_clock % 1000000;
        }
        ev.type = type;
        ev.code = code;
        ev.value = value;
        QCOMPARE(write(m_fd, &ev, sizeof(ev)), ssize_t(sizeof(ev)));
    }
    void sync() { event(EV_SYN, SYN_REPORT, 0); }
    void absMove(int x, int y) { event(EV_ABS, ABS_X, x); event(EV_ABS, ABS_Y, y); sync(); }
    void button(bool down) { event(EV_KEY, BTN_LEFT, down ? 1 : 0); sync(); }
    void wheel(int clicks) { event(EV_REL, REL_WHEEL, clicks); sync(); }

    void guestPosition(int x, int y)
    {
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_guestX = x;
            g_guestY = y;
        }
        if (!openWriter())
            return;
        const char c = 1;
        QCOMPARE(write(m_fd, &c, 1), ssize_t(1));
    }

private:
    bool openWriter()
    {
        if (m_fd < 0)
            m_fd = open(qPrintable(m_path), O_WRONLY | O_NONBLOCK);
        if (m_fd < 0)
            QTest::qFail(qPrintable("nobody has " + m_path + " open"), __FILE__, __LINE__);
        return m_fd >= 0;
    }

    QString m_path;
    int m_fd = -1;
};

} // namespace

#define COMPARE_POINT(actual, expected) \
    do { \
        const QPointF a_ = (actual); \
        const QPointF e_ = (expected); \
        if (qAbs(a_.x() - e_.x()) > 1.0 || qAbs(a_.y() - e_.y()) > 1.0) \
            QFAIL(qPrintable(QString::fromLatin1("touch at %1,%2, expected %3,%4") \
                             .arg(a_.x()).arg(a_.y()).arg(e_.x()).arg(e_.y()))); \
    } while (0)

class tst_VboxTouch : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void evdevTabletPressMoveRelease();
    void evdevFirstPressIsWhereThePointerIs();
    void evdevRelativeMouseOnlyAddsButtons();
    void evdevDragWithTheButtonOnAnotherDevice();
    void evdevPressQueuedBeforeItsPosition();
    void evdevFirstPressWithoutAMoveSinceStart();
    void evdevFirstPressTrustsTheIntegrationDevice();
    void evdevFirstMoveFarFromTheCorner();
    void evdevReleaseQueuedBeforeTheLastMove();
    void evdevDragQueuedAllAtOnce();
    void evdevMovesLeftAfterAPressStillCount();
    void readerHandlesWhatItReadPastTheLimit();
    void evdevWheelAtThePointer();
    void evdevWheelBeforeAnyPositionIsDropped();
    void evdevPinchHasTwoFingers();
    void devicesWithoutModeFallBackToEvdev();
    void evdevSkipsDevicesThatDoNotOpen();
    void evdevDevicesFromEnvironment();
    void evdevWithoutDevicesDoesNothing();
    void focusOnAWindowThatIsNotQuick();
    void vboxGuestPositionWithEvdevButtons();
    void vboxGuestWheelAtThePointer();
    void vboxGuestThatDoesNotOpen();

private:
    QString path(const char *name) const { return m_dir->path() + QLatin1Char('/') + QLatin1String(name); }

    // Runs the event loop until n touches arrived, or for the whole timeout.
    bool waitForTouches(int n, int timeoutMs = 2000)
    {
        QElapsedTimer timer;
        timer.start();
        while (m_window->touches.size() < n && timer.elapsed() < timeoutMs)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        return m_window->touches.size() >= n;
    }
    void settle(int ms = 200) { waitForTouches(1 << 30, ms); }
    int touchCount() const { return int(m_window->touches.size()); }
    const Touch &touch(int i) const { return m_window->touches.at(i); }

    // Where a device position (0..range) lands on the screen.
    static QPointF expected(int x, int y, int range)
    {
        const QRect s = QGuiApplication::primaryScreen()->geometry();
        const qreal nx = qreal(qRound(qreal(x) * 65535 / range)) / 65535;
        const qreal ny = qreal(qRound(qreal(y) * 65535 / range)) / 65535;
        return QPointF(nx * (s.width() - 1) + s.x(), ny * (s.height() - 1) + s.y());
    }

    QScopedPointer<QTemporaryDir> m_dir;
    QScopedPointer<TouchWindow> m_window;
};

void tst_VboxTouch::init()
{
    m_dir.reset(new QTemporaryDir(QStringLiteral(TEST_TMPDIR "/tst-XXXXXX")));
    QVERIFY(m_dir->isValid());
    qunsetenv("VIRTUALBOX_TOUCH_EVDEV_MOUSE");
    qunsetenv("VIRTUALBOX_TOUCH_GUEST_DEVICE");
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_tabletX = g_tabletY = 0;
        g_integrationX = g_integrationY = 0;
    }

    m_window.reset(new TouchWindow);
    // Frameless: offscreen windows otherwise get a frame, and touches on the
    // screen's edges would land in it.
    m_window->setFlags(Qt::FramelessWindowHint);
    m_window->setGeometry(QGuiApplication::primaryScreen()->geometry());
    m_window->show();
    m_window->requestActivate();
    settle();
    m_window->touches.clear();
}

void tst_VboxTouch::cleanup()
{
    // A test that stopped half way leaves a touch open in Qt, and the next
    // test's press would arrive as an update of it.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QList<const QInputDevice *> devices = QInputDevice::devices();
    for (const QInputDevice *device : devices) {
        if (device->type() == QInputDevice::DeviceType::TouchScreen)
            QWindowSystemInterface::handleTouchCancelEvent(
                nullptr, static_cast<const QPointingDevice *>(device));
    }
#else
    const QList<const QTouchDevice *> devices = QTouchDevice::devices();
    for (const QTouchDevice *device : devices) {
        if (device->type() == QTouchDevice::TouchScreen)
            QWindowSystemInterface::handleTouchCancelEvent(
                nullptr, const_cast<QTouchDevice *>(device));
    }
#endif
    settle(50);
    m_window.reset();
    m_dir.reset();
}

void tst_VboxTouch::evdevTabletPressMoveRelease()
{
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + tablet.path());

    tablet.absMove(16384, 16384);
    settle();
    QVERIFY2(m_window->touches.isEmpty(), "moving with the button up is not a touch");

    tablet.button(true);
    QVERIFY(waitForTouches(1));
    QCOMPARE(touch(0).type, QEvent::TouchBegin);
    COMPARE_POINT(touch(0).pos, expected(16384, 16384, TabletMax));

    tablet.absMove(8192, 24576);
    QVERIFY(waitForTouches(2));
    QCOMPARE(touch(1).type, QEvent::TouchUpdate);
    COMPARE_POINT(touch(1).pos, expected(8192, 24576, TabletMax));

    tablet.button(false);
    QVERIFY(waitForTouches(3));
    QCOMPARE(touch(2).type, QEvent::TouchEnd);
    COMPARE_POINT(touch(2).pos, expected(8192, 24576, TabletMax));
}

void tst_VboxTouch::evdevFirstPressIsWhereThePointerIs()
{
    // The very first press after start must not land at 0,0.
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + tablet.path());

    tablet.event(EV_ABS, ABS_X, 30000);
    tablet.event(EV_ABS, ABS_Y, 20000);
    tablet.event(EV_KEY, BTN_LEFT, 1);
    tablet.sync();
    QVERIFY(waitForTouches(1));
    QCOMPARE(touch(0).type, QEvent::TouchBegin);
    COMPARE_POINT(touch(0).pos, expected(30000, 20000, TabletMax));
}

void tst_VboxTouch::evdevRelativeMouseOnlyAddsButtons()
{
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    FakeDevice mouse(path("mouse"), Kind::RelMouse);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + tablet.path() + ":evdev=" + mouse.path());

    tablet.absMove(8192, 8192);
    settle();
    mouse.event(EV_REL, REL_X, 300);
    mouse.event(EV_REL, REL_Y, 300);
    mouse.sync();
    mouse.button(true);
    QVERIFY(waitForTouches(1));
    QCOMPARE(touch(0).type, QEvent::TouchBegin);
    COMPARE_POINT(touch(0).pos, expected(8192, 8192, TabletMax));

    mouse.event(EV_REL, REL_X, 50);
    mouse.sync();
    settle();
    QCOMPARE(touchCount(), 1); // relative motion does not move the touch

    mouse.button(false);
    QVERIFY(waitForTouches(2));
    QCOMPARE(touch(1).type, QEvent::TouchEnd);
    COMPARE_POINT(touch(1).pos, expected(8192, 8192, TabletMax));
}

void tst_VboxTouch::evdevDragWithTheButtonOnAnotherDevice()
{
    // With the guest additions running, VirtualBox reports the position on
    // its mouse integration device and the buttons on the PS/2 mouse. A move
    // on the one must not end the touch the other holds.
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    FakeDevice mouse(path("mouse"), Kind::RelMouse);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + tablet.path() + ":evdev=" + mouse.path());

    tablet.absMove(24576, 16384);
    settle();
    mouse.button(true);
    QVERIFY(waitForTouches(1));
    QCOMPARE(touch(0).type, QEvent::TouchBegin);

    tablet.absMove(16384, 16384);
    QVERIFY(waitForTouches(2));
    QCOMPARE(touch(1).type, QEvent::TouchUpdate);
    COMPARE_POINT(touch(1).pos, expected(16384, 16384, TabletMax));

    tablet.absMove(8192, 16384);
    QVERIFY(waitForTouches(3));
    QCOMPARE(touch(2).type, QEvent::TouchUpdate);
    COMPARE_POINT(touch(2).pos, expected(8192, 16384, TabletMax));

    mouse.button(false);
    QVERIFY(waitForTouches(4));
    QCOMPARE(touch(3).type, QEvent::TouchEnd);
    COMPARE_POINT(touch(3).pos, expected(8192, 16384, TabletMax));
}

void tst_VboxTouch::evdevPressQueuedBeforeItsPosition()
{
    // The click and the position it was made at arrive on two devices, and
    // the click's is often read first. The mouse is opened first here, so
    // that its notifier fires first when both have input.
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    FakeDevice mouse(path("mouse"), Kind::RelMouse);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + mouse.path() + ":evdev=" + tablet.path());

    tablet.absMove(8192, 8192);
    settle();
    tablet.absMove(24576, 16384);
    mouse.button(true);
    QVERIFY(waitForTouches(1));
    QCOMPARE(touch(0).type, QEvent::TouchBegin);
    COMPARE_POINT(touch(0).pos, expected(24576, 16384, TabletMax));

    mouse.button(false);
    QVERIFY(waitForTouches(2));
    QCOMPARE(touch(1).type, QEvent::TouchEnd);
    COMPARE_POINT(touch(1).pos, expected(24576, 16384, TabletMax));
}

void tst_VboxTouch::evdevFirstPressWithoutAMoveSinceStart()
{
    // The compositor (re)started under a pointer that has not moved since:
    // only the kernel knows where it is.
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_tabletX = 8192;
        g_tabletY = 24576;
    }
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    FakeDevice mouse(path("mouse"), Kind::RelMouse);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + tablet.path() + ":evdev=" + mouse.path());

    mouse.button(true);
    QVERIFY(waitForTouches(1));
    QCOMPARE(touch(0).type, QEvent::TouchBegin);
    COMPARE_POINT(touch(0).pos, expected(8192, 24576, TabletMax));
    mouse.button(false);
    QVERIFY(waitForTouches(2));
}

void tst_VboxTouch::evdevFirstPressTrustsTheIntegrationDevice()
{
    // The USB tablet still has the position it had before the guest
    // additions took over; it comes first in the specification, as on LuneOS.
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_tabletX = 30000;
        g_tabletY = 15000;
        g_integrationX = 8192;
        g_integrationY = 24576;
    }
    FakeDevice mouse(path("mouse"), Kind::RelMouse);
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    FakeDevice integration(path("integration"), Kind::Integration);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + mouse.path() + ":evdev=" + tablet.path()
                                         + ":evdev=" + integration.path());

    mouse.button(true);
    QVERIFY(waitForTouches(1));
    COMPARE_POINT(touch(0).pos, expected(8192, 24576, IntegrationMax));
    mouse.button(false);
    QVERIFY(waitForTouches(2));
}

void tst_VboxTouch::evdevFirstMoveFarFromTheCorner()
{
    // The mouse integration device spans 0..65535; measured from where the
    // device starts out, the top left corner, the bottom right of the
    // screen is further than an int holds squared.
    FakeDevice mouse(path("mouse"), Kind::RelMouse);
    FakeDevice integration(path("integration"), Kind::Integration);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + mouse.path() + ":evdev=" + integration.path());

    integration.absMove(40960, 40960);
    settle();
    mouse.button(true);
    QVERIFY(waitForTouches(1));
    COMPARE_POINT(touch(0).pos, expected(40960, 40960, IntegrationMax));
    integration.absMove(40960, 36864);
    QVERIFY(waitForTouches(2));
    COMPARE_POINT(touch(1).pos, expected(40960, 36864, IntegrationMax));
    mouse.button(false);
    QVERIFY(waitForTouches(3));
}

void tst_VboxTouch::evdevReleaseQueuedBeforeTheLastMove()
{
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    FakeDevice mouse(path("mouse"), Kind::RelMouse);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + mouse.path() + ":evdev=" + tablet.path());

    tablet.absMove(8192, 8192);
    settle();
    mouse.button(true);
    QVERIFY(waitForTouches(1));

    // The drag's last stretch and the release, read in the wrong order.
    tablet.absMove(24576, 16384);
    mouse.button(false);
    QVERIFY(waitForTouches(3));
    QCOMPARE(touch(1).type, QEvent::TouchUpdate);
    COMPARE_POINT(touch(1).pos, expected(24576, 16384, TabletMax));
    QCOMPARE(touch(2).type, QEvent::TouchEnd);
    COMPARE_POINT(touch(2).pos, expected(24576, 16384, TabletMax));
}

void tst_VboxTouch::evdevDragQueuedAllAtOnce()
{
    // A whole drag (a scripted one, or a fast flick) queued before any of it
    // is read: the moves made after the press must still be moves of the touch.
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    FakeDevice mouse(path("mouse"), Kind::RelMouse);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + mouse.path() + ":evdev=" + tablet.path());

    tablet.absMove(8192, 8192);
    settle();
    mouse.button(true);
    tablet.absMove(16384, 8192);
    tablet.absMove(24576, 8192);
    mouse.button(false);
    QVERIFY(waitForTouches(3));
    settle();
    QCOMPARE(touch(0).type, QEvent::TouchBegin);
    COMPARE_POINT(touch(0).pos, expected(8192, 8192, TabletMax));
    QCOMPARE(touch(1).type, QEvent::TouchUpdate);
    COMPARE_POINT(touch(1).pos, expected(24576, 8192, TabletMax)); // the two moves are one read
    QCOMPARE(touch(2).type, QEvent::TouchEnd);
    COMPARE_POINT(touch(2).pos, expected(24576, 8192, TabletMax));
    QCOMPARE(touchCount(), 3);
}

void tst_VboxTouch::evdevMovesLeftAfterAPressStillCount()
{
    // Taking in what came before the press reads what came after it too;
    // that must be handled even if nothing more arrives on its device.
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    FakeDevice mouse(path("mouse"), Kind::RelMouse);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + mouse.path() + ":evdev=" + tablet.path());

    tablet.absMove(8192, 8192);
    settle();
    mouse.button(true);
    tablet.absMove(16384, 16384);
    QVERIFY(waitForTouches(2));
    QCOMPARE(touch(0).type, QEvent::TouchBegin);
    COMPARE_POINT(touch(0).pos, expected(8192, 8192, TabletMax));
    QCOMPARE(touch(1).type, QEvent::TouchUpdate);
    COMPARE_POINT(touch(1).pos, expected(16384, 16384, TabletMax));

    mouse.button(false);
    QVERIFY(waitForTouches(3));
}

void tst_VboxTouch::readerHandlesWhatItReadPastTheLimit()
{
    // Reading up to a time reads what came after it from the device as
    // well, and the device's notifier will not fire for that again.
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    QScopedPointer<EvdevMouseHandler> reader(EvdevMouseHandler::create(tablet.path(), QStringLiteral("abs")));
    QVERIFY(reader);
    QSignalSpy moves(reader.data(), &EvdevMouseHandler::handleMouseEvent);

    tablet.absMove(8192, 8192);
    qint64 limit;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        limit = g_clock;
    }
    tablet.absMove(16384, 16384);

    reader->readMouseDataUntil(limit);
    QCOMPARE(moves.size(), 1);
    QCOMPARE(moves.at(0).at(0).toInt(), qRound(8192.0 * EvdevMouseHandler::AbsRange / TabletMax));
    QTRY_COMPARE(moves.size(), 2);
    QCOMPARE(moves.at(1).at(0).toInt(), qRound(16384.0 * EvdevMouseHandler::AbsRange / TabletMax));
}

void tst_VboxTouch::evdevWheelAtThePointer()
{
    // The wheel is on the PS/2 mouse; the pointer on the integration device.
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    FakeDevice mouse(path("mouse"), Kind::RelMouse);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + tablet.path() + ":evdev=" + mouse.path());

    tablet.absMove(16384, 24576);
    settle();
    mouse.wheel(-1);
    QElapsedTimer timer;
    timer.start();
    while (m_window->wheels.isEmpty() && timer.elapsed() < 2000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QCOMPARE(m_window->wheels.size(), 1);
    QCOMPARE(m_window->wheels.first().angleDelta, QPoint(0, -120));
    COMPARE_POINT(m_window->wheels.first().pos, expected(16384, 24576, TabletMax));
    QVERIFY(m_window->touches.isEmpty());
}

void tst_VboxTouch::evdevWheelBeforeAnyPositionIsDropped()
{
    // Without a position the wheel would scroll whatever is in the corner.
    FakeDevice mouse(path("mouse"), Kind::RelMouse);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + mouse.path());

    mouse.wheel(1);
    settle();
    QVERIFY(m_window->wheels.isEmpty());
}

void tst_VboxTouch::evdevPinchHasTwoFingers()
{
    // Holding Ctrl makes a press a pinch of two fingers around the pointer.
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    FakeDevice mouse(path("mouse"), Kind::RelMouse);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + tablet.path() + ":evdev=" + mouse.path());

    tablet.absMove(16384, 16384);
    settle();
    QKeyEvent ctrlDown(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
    QCoreApplication::sendEvent(m_window.data(), &ctrlDown);

    mouse.button(true);
    QVERIFY(waitForTouches(1));
    QCOMPARE(touch(0).type, QEvent::TouchBegin);
    QCOMPARE(touch(0).ids.size(), 2);
    QVERIFY(touch(0).ids.at(0) != touch(0).ids.at(1));

    mouse.button(false);
    QVERIFY(waitForTouches(2));
    QCOMPARE(touch(1).type, QEvent::TouchEnd);

    QKeyEvent ctrlUp(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
    QCoreApplication::sendEvent(m_window.data(), &ctrlUp);
}

void tst_VboxTouch::devicesWithoutModeFallBackToEvdev()
{
    // LuneOS passes its devices without a mode, and its kernel's guest
    // additions have no vboxguest interface the plugin can use.
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    FakeDevice mouse(path("mouse"), Kind::RelMouse);
    qputenv("VIRTUALBOX_TOUCH_GUEST_DEVICE", path("missing").toLocal8Bit());
    VirtualboxTouchScreenHandler handler("evdev=" + tablet.path() + ":evdev=" + mouse.path());

    tablet.absMove(8192, 24576);
    settle();
    mouse.button(true);
    QVERIFY(waitForTouches(1));
    QCOMPARE(touch(0).type, QEvent::TouchBegin);
    COMPARE_POINT(touch(0).pos, expected(8192, 24576, TabletMax));
    mouse.button(false);
    QVERIFY(waitForTouches(2));
    QCOMPARE(touch(1).type, QEvent::TouchEnd);
}

void tst_VboxTouch::evdevSkipsDevicesThatDoNotOpen()
{
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + path("missing") + ":evdev=" + tablet.path());

    tablet.absMove(100, 200);
    tablet.button(true);
    QVERIFY(waitForTouches(1));
    COMPARE_POINT(touch(0).pos, expected(100, 200, TabletMax));
}

void tst_VboxTouch::evdevDevicesFromEnvironment()
{
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    qputenv("VIRTUALBOX_TOUCH_EVDEV_MOUSE", (path("missing") + ":" + tablet.path()).toLocal8Bit());
    VirtualboxTouchScreenHandler handler("mode=evdev");

    tablet.absMove(32767, 0);
    tablet.button(true);
    QVERIFY(waitForTouches(1));
    COMPARE_POINT(touch(0).pos, expected(32767, 0, TabletMax));
}

void tst_VboxTouch::evdevWithoutDevicesDoesNothing()
{
    {
        VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + path("missing"));
        settle();
    }
    QVERIFY(m_window->touches.isEmpty());
}

void tst_VboxTouch::focusOnAWindowThatIsNotQuick()
{
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + tablet.path());

    // Focus moves to a plain QWindow: the zoom indicator must stay out of it.
    QWindow other;
    other.setGeometry(0, 0, 10, 10);
    other.show();
    other.requestActivate();
    settle();
    m_window->requestActivate();
    settle();
}

void tst_VboxTouch::vboxGuestPositionWithEvdevButtons()
{
    FakeDevice guest(path("vboxguest"), Kind::VboxGuest);
    FakeDevice mouse(path("mouse"), Kind::RelMouse);
    VirtualboxTouchScreenHandler handler("vboxguest=" + guest.path() + ":evdev=" + mouse.path());

    // The guest position is 0..65535; the screen is Qt's own geometry when
    // /dev/fb0 cannot be read, which is the case for a normal user.
    guest.guestPosition(16384, 49152);
    settle();
    QVERIFY(m_window->touches.isEmpty());

    mouse.button(true);
    QVERIFY(waitForTouches(1));
    QCOMPARE(touch(0).type, QEvent::TouchBegin);
    COMPARE_POINT(touch(0).pos, expected(16384, 49152, 65535));

    guest.guestPosition(32768, 32768);
    QVERIFY(waitForTouches(2));
    QCOMPARE(touch(1).type, QEvent::TouchUpdate);
    COMPARE_POINT(touch(1).pos, expected(32768, 32768, 65535));

    mouse.button(false);
    QVERIFY(waitForTouches(3));
    QCOMPARE(touch(2).type, QEvent::TouchEnd);
}

void tst_VboxTouch::vboxGuestWheelAtThePointer()
{
    FakeDevice guest(path("vboxguest"), Kind::VboxGuest);
    FakeDevice mouse(path("mouse"), Kind::RelMouse);
    VirtualboxTouchScreenHandler handler("vboxguest=" + guest.path() + ":evdev=" + mouse.path());

    guest.guestPosition(49152, 16384);
    settle();
    mouse.wheel(2);
    QElapsedTimer timer;
    timer.start();
    while (m_window->wheels.isEmpty() && timer.elapsed() < 2000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QCOMPARE(m_window->wheels.size(), 1);
    QCOMPARE(m_window->wheels.first().angleDelta, QPoint(0, 240));
    COMPARE_POINT(m_window->wheels.first().pos, expected(49152, 16384, 65535));
}

void tst_VboxTouch::vboxGuestThatDoesNotOpen()
{
    // Used to delete an uninitialised evdev handler pointer on destruction.
    { VirtualboxTouchScreenHandler handler("vboxguest=" + path("missing")); }
    { VirtualboxTouchScreenHandler handler("mode=bogus:vboxguest=" + path("missing")); }
}

int main(int argc, char **argv)
{
    // Touches go to a window, so a platform is needed; offscreen works
    // without a display and gives every run the same screen.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    tst_VboxTouch test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_vboxtouch.moc"
