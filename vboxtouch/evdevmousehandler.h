/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the QtGui module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

// This file was copied from qtbase/qtbase/src/platformsupport/input/evdevmouse
// and stripped of its Q prefix and Qt namespace.
// QEvdevMouseHandler is a private part of Qt, and vboxmouse relies on its
// handleMouseEvent signal which is probably an implementation detail, so
// copying it is better than trying to use it directly. 

#ifndef EVDEVMOUSEHANDLER_H
#define EVDEVMOUSEHANDLER_H

#include <QObject>
#include <QString>

class QSocketNotifier;

class EvdevMouseHandler : public QObject
{
    Q_OBJECT
public:
    static EvdevMouseHandler *create(const QString &device, const QString &specification);
    ~EvdevMouseHandler();

    Qt::MouseButtons buttons() const;

signals:
    void handleMouseEvent(int x, int y, Qt::MouseButtons buttons);
    void handleWheelEvent(int delta, Qt::Orientation orientation);

private slots:
    void readMouseData();

private:
    EvdevMouseHandler(const QString &device, int fd, bool compression, int jitterLimit, float scale);

    void sendMouseEvent();

    QString m_device;
    int m_fd;
    QSocketNotifier *m_notify;
    int m_x, m_y;
    int m_prevx, m_prevy;
    bool m_compression;
    Qt::MouseButtons m_buttons;
    int m_jitterLimitSquared;
    bool m_prevInvalid;
    float m_scale;
};

#endif
