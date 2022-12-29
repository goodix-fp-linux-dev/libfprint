#!/usr/bin/python3

import os
import gi
gi.require_version('FPrint', '2.0')
from gi.repository import FPrint, GLib

import sys
import traceback
sys.excepthook = lambda *args : (traceback.print_exception(*args), sys.exit(1))


c = FPrint.Context()
c.enumerate()
devices = c.get_devices()

d = devices[0]
del devices

usb_device = d.get_property('fpi-usb-device')
bus_num = usb_device.get_bus()
port = []
while True:
    parent = usb_device.get_parent()
    if parent is None:
        break
    port.append(str(usb_device.get_port_number()))
    usb_device = parent
port = '.'.join(port)

persist = f'/sys/bus/usb/devices/{bus_num}-{port}/power/persist'
wakeup = f'/sys/bus/usb/devices/{bus_num}-{port}/power/wakeup'

# may not have written anything
assert open(persist).read().strip() == "0"
assert open(wakeup).read().strip() == "disabled"

assert d.get_driver() == "synaptics"
assert not d.has_feature(FPrint.DeviceFeature.CAPTURE)
assert d.has_feature(FPrint.DeviceFeature.IDENTIFY)
assert d.has_feature(FPrint.DeviceFeature.VERIFY)
assert not d.has_feature(FPrint.DeviceFeature.DUPLICATES_CHECK)
assert d.has_feature(FPrint.DeviceFeature.STORAGE)
assert d.has_feature(FPrint.DeviceFeature.STORAGE_DELETE)
assert d.has_feature(FPrint.DeviceFeature.STORAGE_CLEAR)

d.open_sync()

d.clear_storage_sync()

template = FPrint.Print.new(d)

def enroll_progress(*args):
    assert d.get_finger_status() & FPrint.FingerStatusFlags.NEEDED
    print('enroll progress: ' + str(args))

# List, enroll, list, verify, delete, list
print("enrolling")
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
p = d.enroll_sync(template, None, enroll_progress, None)
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
print("enroll done")

print("verifying")
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE

# Inject a suspend/resume cycle into the verify
def suspend_resume():
    d.suspend_sync()
    assert open(persist).read().strip() == "0"
    assert open(wakeup).read().strip() == "enabled"

    assert open(persist, 'w').write('0\n')
    d.resume_sync()
    # This tests that libfprint doesn't write if the value is correct
    # (i.e. the trailing \ would be lost inside umockdev if written)
    assert open(persist).read() == "0\n"
    assert open(wakeup).read().strip() == "disabled"

GLib.idle_add(suspend_resume, priority=GLib.PRIORITY_HIGH)
verify_res, verify_print = d.verify_sync(p)
assert d.get_finger_status() == FPrint.FingerStatusFlags.NONE
print("verify done")
assert verify_res == True

print("deleting")
d.delete_print_sync(p)
print("delete done")
d.close_sync()

del d
del c
