Generic plug-in that treats the VirtualBox mouse device as a touch screen
input. It relies on the VirtualBox guest modules and /dev/vboxguest.

To use this driver, pass -plugin VBoxTouch on the command line.

The driver responds to two environment variables:

 * `VIRTUALBOX_TOUCH_GUEST_DEVICE` (default `/dev/vboxguest`)
 * `VIRTUALBOX_TOUCH_EVDEV_MOUSE` (default `/dev/input/by-path/platform-i8042-serio-1-event-mouse`)

It gets movement events from the guest device and button events from the evdev device. It's not pretty, but that's the way it is.

The devices can also be specified on the command line as part of the plugin
specification.
For example `-plugin VboxTouch:vboxguest=/dev/vboxguest:evdev=/dev/input/mouse0`

The mouse inputs are converted to touchscreen events by reporting only
left button actions (as touches), and reporting movement only when the left
button is down.
