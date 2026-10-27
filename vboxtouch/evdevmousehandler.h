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

#ifndef QEVDEVMOUSEHANDLER_P_H
#define QEVDEVMOUSEHANDLER_P_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QPoint>
#include <QEvent>

class QSocketNotifier;

class EvdevMouseHandler : public QObject
{
    Q_OBJECT
public:
    // An absolute device's position is reported scaled to 0..AbsRange, the
    // range vboxguest reports positions in, whatever the device's own range.
    static const int AbsRange = 65535;

    static EvdevMouseHandler *create(const QString &device, const QString &specification);
    ~EvdevMouseHandler();

    void readMouseData();
    // Takes in the queued events no later than limit (microseconds, the
    // kernel's timestamps), leaving the rest for the next event loop pass.
    void readMouseDataUntil(qint64 limit);
    // When the event behind the last handleMouseEvent happened.
    qint64 lastEventTime() const { return m_frameTime; }
    const QString &device() const { return m_device; }
    const QString &name() const { return m_name; } // as the kernel names the device
    // Where an absolute device is now, as the kernel last heard from it,
    // scaled like handleMouseEvent's; false if the device is not absolute.
    bool currentPosition(int *x, int *y) const;

signals:
    void handleMouseEvent(int x, int y, bool abs, Qt::MouseButtons buttons,
                          Qt::MouseButton button, QEvent::Type type);
    void handleWheelEvent(QPoint delta);

private:
    EvdevMouseHandler(const QString &device, int fd, bool abs, bool compression, int jitterLimit);

    void sendMouseEvent();
    void readAvailable();
    void processQueue(qint64 limit);
    int scaled(int value, int min, int range) const;
    bool getHardwareMaximum();

    QString m_device;
    QString m_name;
    QByteArray m_queue; // input_events read but not handled yet
    int m_fd;
    QSocketNotifier *m_notify;
    int m_x, m_y;
    int m_prevx, m_prevy;
    bool m_abs;
    bool m_compression;
    Qt::MouseButtons m_buttons;
    Qt::MouseButton m_button;
    QEvent::Type m_eventType;
    int m_jitterLimitSquared;
    bool m_prevInvalid;
    int m_hardwareMinX;
    int m_hardwareMinY;
    int m_hardwareWidth;
    int m_hardwareHeight;
    qint64 m_frameTime;
};

#endif // QEVDEVMOUSEHANDLER_P_H
