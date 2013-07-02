/****************************************************************************
**
** Copyright (C) 2013 Jolla Ltd.
** Contact: Richard Braakman <richard.braakman@jollamobile.com>
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

#include <QtGui/qgenericplugin.h>

#include <QStringList>

#include "vboxtouch.h"

class VirtualboxTouchScreenPlugin : public QGenericPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QGenericPluginFactoryInterface" FILE "vboxtouch.json")

public:
    VirtualboxTouchScreenPlugin() {};

    QStringList keys() const;
    QObject *create(const QString &key, const QString &specification);
};

QStringList VirtualboxTouchScreenPlugin::keys() const
{
    return QStringList() << "VboxTouch";
}

QObject *VirtualboxTouchScreenPlugin::create(const QString &key,
                                             const QString &specification)
{
    if (!key.compare("VboxTouch", Qt::CaseInsensitive))
        return new VirtualboxTouchScreenHandler(specification);

    return 0;
}

#include "main.moc"
