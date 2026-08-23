#!/usr/bin/python3

# umockdev-replayed smoke test for the goodix533c driver.
#
# Scope: this driver has no on-chip storage -- matching is done on the host
# (see sigfm/ and libfprint/drivers/goodix533c/) rather than via
# FP_DEVICE_FEATURE_STORAGE, so this does not exercise
# list_prints_sync/delete_print_sync/clear_storage_sync the way
# tests/goodixmoc/custom.py or tests/synaptics/custom.py do.
#
# It is also deliberately scoped to what custom.pcapng can actually
# replay. custom.pcapng is a real, finger-absent capture from a physical
# 27c6:533c device (see README.md for full provenance and the finger-
# absent safety constraint -- no new capture may ever be added here that
# contains a finger-present mcu_get_image reply, for any reason). But as
# documented in detail in README.md's "Verified replay result" section,
# this specific capture file was empirically found (via tshark, with a
# goodixmoc/fpcmoc control confirming the methodology) to carry zero
# captured bulk-IN reply payload bytes on the device's response endpoint,
# anywhere in the file -- so it cannot actually replay the driver's
# open() sequence past its second command (firmware_version) via
# umockdev-run. Calling open_sync() here would therefore always fail
# (timeout, or a fatal "Unknown pack flags" warning under meson test's
# G_DEBUG=fatal-warnings), not because of anything this test or the
# driver gets wrong, but because of a gap in this specific capture file.
# Rather than land a permanently-red suite entry, this test is scoped to
# only what is genuinely, currently verifiable against this fixture:
# device discovery and feature-flag assertions. It deliberately does NOT
# call open_sync()/close_sync()/enroll_sync()/verify_sync()/
# identify_sync() -- see README.md for exactly what a follow-up capture
# needs to provide before those can be added back.

import traceback
import sys
import gi

gi.require_version('FPrint', '2.0')
from gi.repository import FPrint, GLib

# Exit with error on any exception, included those happening in async callbacks
sys.excepthook = lambda *args: (traceback.print_exception(*args), sys.exit(1))

ctx = GLib.main_context_default()

c = FPrint.Context()
c.enumerate()
devices = c.get_devices()

assert len(devices) == 1
d = devices[0]
del devices

assert d.get_driver() == "goodix533c"

# Feature flags as currently wired in goodix533c.c via
# fpi_device_class_auto_initialize_features(): VERIFY/IDENTIFY are derived
# from ->verify/->identify being set; CAPTURE is NOT derived because
# dev_class->capture itself is deliberately left NULL (the open()+one-
# frame-capture path is only exercised via the test-only
# goodix533c-capture-test binary, not the public FPrint API); there is no
# on-chip storage (->list/->delete/->clear_storage all NULL) so none of
# the STORAGE* bits are set either.
#
# NOTE: this is a snapshot verified against a live-moving driver file
# shared with a concurrent SIGFM-matching work stream. If dev_class-
# >capture or any ->list/->delete/->clear_storage vfunc gets wired up
# after this was written, these assertions will start failing and need
# to be re-run/updated.
assert not d.has_feature(FPrint.DeviceFeature.CAPTURE)
assert d.has_feature(FPrint.DeviceFeature.IDENTIFY)
assert d.has_feature(FPrint.DeviceFeature.VERIFY)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE_LIST)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE_DELETE)
assert not d.has_feature(FPrint.DeviceFeature.STORAGE_CLEAR)
assert not d.has_feature(FPrint.DeviceFeature.DUPLICATES_CHECK)

# open_sync()/close_sync() and beyond are intentionally NOT exercised here
# -- see the module docstring and README.md's "Verified replay result"
# for why this fixture cannot currently support that, with the exact
# umockdev-run transcripts that were captured while establishing this.

del d
del c
