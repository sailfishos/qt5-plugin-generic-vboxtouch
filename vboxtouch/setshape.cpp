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

#include <QByteArray>
#include <QDebug>

#include <stdint.h>
#include <string.h>

#include <sys/ioctl.h>

/*
 * Cursor based on "Elastic" theme, cursor "circle" layers 3+
 * url: http://gnome-look.org/content/show.php/Elastic?content=158064
 * author: Marko Flores
 * license: LGPL
 * With thanks!
 *
 * Changes: convert to gray by copying R value to G
 *          make plus sign 1px larger
 */
#define CURSOR circle
// warning, these files have to contain the same width and height.
// -mask.xbm just has the non-transparent pixels from the xpm.
#include "circle.xpm"  // 32 bpp rgba map
#include "circle-mask.xbm"  // 1 bpp AND map

// preprocessor magic so that CURSOR is evaluated before pasting to token
#define CURSOR_PASTE(token) CURSOR_PASTE_(CURSOR, token)
#define CURSOR_PASTE_(cursor, token) CURSOR_PASTE__(cursor, token)
#define CURSOR_PASTE__(cursor, token) cursor##token

#define CURSOR_WIDTH CURSOR_PASTE(_mask_width)
#define CURSOR_HEIGHT CURSOR_PASTE(_mask_height)
#define HOT_X CURSOR_PASTE(_mask_x_hot)
#define HOT_Y CURSOR_PASTE(_mask_y_hot)
#define MASK_BITS CURSOR_PASTE(_mask_bits)
#define XPM_DATA CURSOR_PASTE(_xpm)

struct vbox_set_pointer_shape_request {
    uint32_t size;
    uint32_t version;
    uint32_t type;
    uint32_t rc;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t flags;
    uint32_t hot_x;
    uint32_t hot_y;
    uint32_t width;
    uint32_t height;
    char data[1]; // variable size
};
const static vbox_set_pointer_shape_request blank_set_pointer_shape_request = {
    0, // size will depend on pixmap size
    0x10001, // request version
    3, // type: set pointer shape
    -1, // rc: pre-emptive error code
    0,
    0,
    7,  // flags: visible (0x1), has alpha (0x2), has shape data (0x4)
    HOT_X,
    HOT_Y,
    CURSOR_WIDTH,
    CURSOR_HEIGHT,
    { 0 }
};

static void decode_xpm(char *dest)
{
    // This is not a full decoder, it just handles what GIMP produces.
    QList<QByteArray> header = QByteArray(XPM_DATA[0]).split(' ');
    QHash<char, uint32_t> colors;
    int ncolors = header.at(2).toInt();

    for (int i = 0; i < ncolors; i++) {
        QByteArray line(XPM_DATA[i + 1]);
        uint32_t color;
        if (line.endsWith(" None"))
            color = 0;
        else {
            // Fill in 0xFF for the alpha.
            color = 0xff000000 | line.right(6).toInt(0, 16);
        }
        colors[line.at(0)] = color;
    }

    for (int y = 0; y < CURSOR_HEIGHT; y++) {
        const char *line = XPM_DATA[y + 1 + ncolors];
        for (int x = 0; x < CURSOR_WIDTH; x++) {
            uint32_t color = colors.value(line[x], 0);
            // Virtualbox wants the colors in ABGR order
            *dest++ = (color >> 24) & 0xFF;
            *dest++ = color & 0xFF;
            *dest++ = (color >> 8) & 0xFF;
            *dest++ = (color >> 16) & 0xFF;
        }
    }
}

bool set_pointer_shape_ioctl(int fd)
{
    int mask_size = ((CURSOR_WIDTH + 7) / 8) * CURSOR_HEIGHT;  // 1bpp mask
    int mask_pad = (mask_size + 3) % 4;
    int pix_size = CURSOR_WIDTH * CURSOR_HEIGHT * 4;           // 32bpp rgba
    int req_size = offsetof(vbox_set_pointer_shape_request, data)
                 + mask_size + mask_pad + pix_size;
    vbox_set_pointer_shape_request *request = (vbox_set_pointer_shape_request *)malloc(req_size);

    *request = blank_set_pointer_shape_request;
    request->size = req_size;

    memcpy(request->data, MASK_BITS, mask_size);
    decode_xpm(request->data + mask_size + mask_pad);

    int err = ioctl(fd, _IOC(_IOC_READ|_IOC_WRITE, 'V', 3, req_size), request);
    if (err < 0)
        qWarning("vboxtouch setpointershape: ioctl error: %s", strerror(errno));
    else if (err > 0)
        qWarning("vboxtouch setpointershape: vboxguest error %d", err);
    else if (request->rc != 0)
        qWarning("vboxtouch setpointershape: vboxguest rc error %d", request->rc);
    return err == 0;
}
