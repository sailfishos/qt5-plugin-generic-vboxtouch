/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtGui module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "evdevmousehandler.h"

#include <QSocketNotifier>
#include <QStringList>
#include <QPoint>
#include <QLoggingCategory>

#include <qplatformdefs.h>
#include <private/qcore_unix_p.h> // overrides QT_OPEN

#include <errno.h>
#include <string.h>
#include <limits>

#ifdef Q_OS_FREEBSD
#include <dev/evdev/input.h>
#else
#include <linux/kd.h>
#include <linux/input.h>
#endif

#define TEST_BIT(array, bit)    (array[bit/8] & (1<<(bit%8)))

Q_LOGGING_CATEGORY(qLcEvdevMouse, "qt.qpa.input")

EvdevMouseHandler *EvdevMouseHandler::create(const QString &device, const QString &specification)
{
    qCDebug(qLcEvdevMouse) << "create mouse handler for" << device << specification;

    bool compression = true;
    int jitterLimit = 0;
    int grab = 0;
    bool abs = false;

    const QStringList args = specification.split(QLatin1Char(':'));
    for (const QString &arg : args) {
        if (arg == QLatin1String("nocompress"))
            compression = false;
        else if (arg.startsWith(QLatin1String("dejitter=")))
            jitterLimit = arg.mid(9).toInt();
        else if (arg.startsWith(QLatin1String("grab=")))
            grab = arg.mid(5).toInt();
        else if (arg == QLatin1String("abs"))
            abs = true;
    }

    int fd;
    fd = qt_safe_open(device.toLocal8Bit().constData(), O_RDONLY | O_NDELAY, 0);
    if (fd >= 0) {
        ::ioctl(fd, EVIOCGRAB, grab);
        return new EvdevMouseHandler(device, fd, abs, compression, jitterLimit);
    } else {
        qErrnoWarning(errno, "Cannot open mouse input device %s", qPrintable(device));
        return 0;
    }
}

EvdevMouseHandler::EvdevMouseHandler(const QString &device, int fd, bool abs, bool compression, int jitterLimit)
    : m_device(device), m_fd(fd), m_notify(0), m_x(0), m_y(0), m_prevx(0), m_prevy(0),
      m_abs(abs), m_compression(compression), m_buttons(Qt::NoButton), m_button(Qt::NoButton),
      m_eventType(QEvent::MouseMove), m_jitterLimitSquared(jitterLimit * jitterLimit), m_prevInvalid(true),
      m_hardwareMinX(0), m_hardwareMinY(0), m_hardwareWidth(0), m_hardwareHeight(0),
      m_frameTime(0)
{
    setObjectName(QLatin1String("Evdev Mouse Handler"));

    char kernelName[256];
    const int length = ioctl(m_fd, EVIOCGNAME(sizeof(kernelName)), kernelName);
    if (length > 0)
        m_name = QString::fromLocal8Bit(kernelName, int(qstrnlen(kernelName, size_t(length))));

    // Some touch screens present as mice with absolute coordinates.
    // These can not be differentiated from touchpads, so supplying abs to QT_QPA_EVDEV_MOUSE_PARAMETERS
    // will force qevdevmousehandler to treat the coordinates as absolute, scaled to the hardware maximums.
    // Turning this on will not affect mice as these do not report in absolute coordinates
    // but will make touchpads act like touch screens
    if (m_abs)
        m_abs = getHardwareMaximum();

    // socket notifier for events on the mouse device
    m_notify = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notify, &QSocketNotifier::activated,
            this, &EvdevMouseHandler::readMouseData);
}

EvdevMouseHandler::~EvdevMouseHandler()
{
    if (m_fd >= 0)
        qt_safe_close(m_fd);
}

// Ask touch screen hardware for information on coordinate maximums
// If any ioctls fail, revert to non abs mode
bool EvdevMouseHandler::getHardwareMaximum()
{
    unsigned char absFeatures[(ABS_MAX / 8) + 1];
    memset(absFeatures, '\0', sizeof (absFeatures));

    // test if ABS_X, ABS_Y are available
    if (ioctl(m_fd, EVIOCGBIT(EV_ABS, sizeof (absFeatures)), absFeatures) == -1)
        return false;

    if ((!TEST_BIT(absFeatures, ABS_X)) || (!TEST_BIT(absFeatures, ABS_Y)))
        return false;

    // ask hardware for minimum and maximum values
    struct input_absinfo absInfo;
    if (ioctl(m_fd, EVIOCGABS(ABS_X), &absInfo) == -1)
        return false;

    m_hardwareMinX = absInfo.minimum;
    m_hardwareWidth = absInfo.maximum - absInfo.minimum;

    if (ioctl(m_fd, EVIOCGABS(ABS_Y), &absInfo) == -1)
        return false;

    m_hardwareMinY = absInfo.minimum;
    m_hardwareHeight = absInfo.maximum - absInfo.minimum;

    if (m_hardwareWidth <= 0 || m_hardwareHeight <= 0)
        return false;

    qCInfo(qLcEvdevMouse) << "Absolute pointing device" << m_device
                          << "x" << m_hardwareMinX << "+" << m_hardwareWidth
                          << "y" << m_hardwareMinY << "+" << m_hardwareHeight;

    return true;
}

int EvdevMouseHandler::scaled(int value, int min, int range) const
{
    return qBound(0, qRound(static_cast<qreal>(value - min) * AbsRange / range), int(AbsRange));
}

bool EvdevMouseHandler::currentPosition(int *x, int *y) const
{
    if (!m_abs || m_fd < 0)
        return false;

    struct input_absinfo absX, absY;
    if (ioctl(m_fd, EVIOCGABS(ABS_X), &absX) == -1 || ioctl(m_fd, EVIOCGABS(ABS_Y), &absY) == -1)
        return false;

    *x = scaled(absX.value, m_hardwareMinX, m_hardwareWidth);
    *y = scaled(absY.value, m_hardwareMinY, m_hardwareHeight);
    return true;
}

void EvdevMouseHandler::sendMouseEvent()
{
    int x;
    int y;

    if (!m_abs) {
        x = m_x - m_prevx;
        y = m_y - m_prevy;

        // There is no previous position to measure a movement from yet.
        // Only a relative device has this problem: zeroing an absolute
        // position put the first press after every compositor start at the
        // top left corner of the screen, wherever it really was.
        if (m_prevInvalid) {
            x = y = 0;
            m_prevInvalid = false;
        }
    }
    else {
        // Scaled to the device range rather than to the screen: whoever turns
        // it into a screen position does so with the screen as it is then. The
        // plugin is loaded while the compositor is still coming up, and a scale
        // taken from the screen it saw then was not always right for the one it
        // ended up with (every touch at twice its position, on some boots).
        x = scaled(m_x, m_hardwareMinX, m_hardwareWidth);
        y = scaled(m_y, m_hardwareMinY, m_hardwareHeight);
    }

    emit handleMouseEvent(x, y, m_abs, m_buttons, m_button, m_eventType);

    m_prevx = m_x;
    m_prevy = m_y;
}

void EvdevMouseHandler::readMouseData()
{
    readAvailable();
    processQueue(std::numeric_limits<qint64>::max());
}

void EvdevMouseHandler::readMouseDataUntil(qint64 limit)
{
    readAvailable();
    processQueue(limit);
    // What came later is handled once the event that asked for this is.
    if (!m_queue.isEmpty()) {
        QMetaObject::invokeMethod(this, [this] {
            processQueue(std::numeric_limits<qint64>::max());
        }, Qt::QueuedConnection);
    }
}

void EvdevMouseHandler::readAvailable()
{
    if (m_fd < 0)
        return;

    struct ::input_event buffer[32];
    int n = 0;
    forever {
        int result = QT_READ(m_fd, reinterpret_cast<char *>(buffer) + n, qint64(sizeof(buffer)) - n);

        if (result == 0) {
            qWarning("evdevmouse: Got EOF from the input device");
            return;
        } else if (result < 0) {
            // Nothing (more) to read: this is also called to take in what a
            // device has queued, which may be nothing at all.
            if (errno == EAGAIN)
                break;
            if (errno != EINTR) {
                qErrnoWarning(errno, "evdevmouse: Could not read from input device");
                // If the device got disconnected, stop reading, otherwise we get flooded
                // by the above error over and over again.
                if (errno == ENODEV) {
                    delete m_notify;
                    m_notify = nullptr;
                    qt_safe_close(m_fd);
                    m_fd = -1;
                }
                return;
            }
        } else {
            n += result;
            if (n % sizeof(buffer[0]) == 0)
                break;
        }
    }

    m_queue.append(reinterpret_cast<const char *>(buffer), n - n % int(sizeof(buffer[0])));
}

void EvdevMouseHandler::processQueue(qint64 limit)
{
    bool posChanged = false, btnChanged = false;
    bool pendingMouseEvent = false;
    const int n = int(m_queue.size() / sizeof(struct ::input_event));
    int i = 0;
    bool frameStart = true;
    for (; i < n; ++i) {
        // Copied out: the queue's bytes need not be aligned for input_event.
        struct ::input_event event;
        memcpy(&event, m_queue.constData() + i * sizeof(event), sizeof(event));
        const struct ::input_event *data = &event;
        const qint64 time = qint64(data->input_event_sec) * 1000000 + qint64(data->input_event_usec);
        if (frameStart && time > limit)
            break;
        frameStart = data->type == EV_SYN && data->code == SYN_REPORT;
        m_frameTime = time;
        if (data->type == EV_ABS) {
            // Touchpads: store the absolute position for now, will calculate a relative one later.
            if (data->code == ABS_X && m_x != data->value) {
                m_x = data->value;
                posChanged = true;
            } else if (data->code == ABS_Y && m_y != data->value) {
                m_y = data->value;
                posChanged = true;
            }
        } else if (data->type == EV_REL) {
            QPoint delta;
            if (data->code == REL_X) {
                m_x += data->value;
                posChanged = true;
            } else if (data->code == REL_Y) {
                m_y += data->value;
                posChanged = true;
            } else if (data->code == REL_WHEEL) { // vertical scroll
                // data->value: positive == up, negative == down
                delta.setY(120 * data->value);
                emit handleWheelEvent(delta);
            } else if (data->code == REL_HWHEEL) { // horizontal scroll
                // data->value: positive == right, negative == left
                delta.setX(-120 * data->value);
                emit handleWheelEvent(delta);
            }
        } else if (data->type == EV_KEY && data->code == BTN_TOUCH) {
            // We care about touchpads only, not touchscreens -> don't map to button press.
            // Need to invalidate prevx/y however to get proper relative pos.
            m_prevInvalid = true;
        } else if (data->type == EV_KEY && data->code >= BTN_LEFT && data->code <= BTN_JOYSTICK) {
            Qt::MouseButton button = Qt::NoButton;
            // BTN_LEFT == 0x110 in kernel's input.h
            // The range of possible mouse buttons ends just before BTN_JOYSTICK, value 0x120.
            switch (data->code) {
            case 0x110: button = Qt::LeftButton; break;    // BTN_LEFT
            case 0x111: button = Qt::RightButton; break;
            case 0x112: button = Qt::MiddleButton; break;
            case 0x113: button = Qt::ExtraButton1; break;  // AKA Qt::BackButton
            case 0x114: button = Qt::ExtraButton2; break;  // AKA Qt::ForwardButton
            case 0x115: button = Qt::ExtraButton3; break;  // AKA Qt::TaskButton
            case 0x116: button = Qt::ExtraButton4; break;
            case 0x117: button = Qt::ExtraButton5; break;
            case 0x118: button = Qt::ExtraButton6; break;
            case 0x119: button = Qt::ExtraButton7; break;
            case 0x11a: button = Qt::ExtraButton8; break;
            case 0x11b: button = Qt::ExtraButton9; break;
            case 0x11c: button = Qt::ExtraButton10; break;
            case 0x11d: button = Qt::ExtraButton11; break;
            case 0x11e: button = Qt::ExtraButton12; break;
            case 0x11f: button = Qt::ExtraButton13; break;
            }
            if (data->value)
                m_buttons |= button;
            else
                m_buttons &= ~Qt::MouseButtons(button);
            m_button = button;
            m_eventType = data->value == 0 ? QEvent::MouseButtonRelease : QEvent::MouseButtonPress;
            btnChanged = true;
        } else if (data->type == EV_SYN && data->code == SYN_REPORT) {
            if (btnChanged) {
                btnChanged = posChanged = false;
                sendMouseEvent();
                pendingMouseEvent = false;
            } else if (posChanged) {
                m_eventType = QEvent::MouseMove;
                posChanged = false;
                if (m_compression) {
                    pendingMouseEvent = true;
                } else {
                    sendMouseEvent();
                }
            }
        } else if (data->type == EV_MSC && data->code == MSC_SCAN) {
            // kernel encountered an unmapped key - just ignore it
            continue;
        }
    }
    m_queue.remove(0, qsizetype(i) * qsizetype(sizeof(struct ::input_event)));
    if (m_compression && pendingMouseEvent) {
        // In 64 bits: across the 0..65535 of VirtualBox's mouse integration
        // device the square overflows an int, went negative and dropped every
        // move until the pointer came back near where the last one was sent.
        const qint64 dx = qint64(m_x) - m_prevx, dy = qint64(m_y) - m_prevy;
        if (dx * dx + dy * dy > m_jitterLimitSquared)
            sendMouseEvent();
    }
}

