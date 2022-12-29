#!/usr/bin/env python3

import sys
import os
import os.path
import glob
import shutil
import tempfile
import subprocess

if len(sys.argv) != 2:
    print("You need to specify exactly one argument, the directory with test data")

# Check that umockdev is available
try:
    umockdev_version = subprocess.check_output(['umockdev-run', '--version'])
    version = tuple(int(_) for _ in umockdev_version.split(b'.')[:3])
    if version < (0, 13, 2):
        print('umockdev is too old for test to be reliable, expect random failures!')
        print('Please update umockdev to at least 0.13.2.')
    pcap_supported = version >= (0, 16, 3) or os.getenv('CI_PROJECT_NAME') == "libfprint"
    spi_supported = version >= (0, 16) or os.getenv('CI_PROJECT_NAME') == "libfprint"

except FileNotFoundError:
    print('umockdev-run not found, skipping test!')
    print('Please install umockdev.')
    sys.exit(77)

edir = os.path.dirname(sys.argv[0])
ddir = sys.argv[1]

tmpdir = tempfile.mkdtemp(prefix='libfprint-umockdev-test-')

assert os.path.isdir(ddir)

def cmp_pngs(png_a, png_b):
    print("Comparing PNGs %s and %s" % (png_a, png_b))
    import cairo
    img_a = cairo.ImageSurface.create_from_png(png_a)
    img_b = cairo.ImageSurface.create_from_png(png_b)

    assert img_a.get_format() == cairo.FORMAT_RGB24
    assert img_b.get_format() == cairo.FORMAT_RGB24
    assert img_a.get_width() == img_b.get_width()
    assert img_a.get_height() == img_b.get_height()
    assert img_a.get_stride () == img_b.get_stride()

    data_a = img_a.get_data()
    data_b = img_b.get_data()
    stride = img_a.get_stride()

    for x in range(img_a.get_width()):
        for y in range(img_a.get_height()):
            # RGB24 format is endian dependent, using +1 means we test either
            # the G or B component, which works on any endian for the greyscale
            # test.
            assert(data_a[y * stride + x * 4 + 1] == data_b[y * stride + x * 4 + 1])

def get_umockdev_runner(ioctl_basename):
    ioctl = os.path.join(ddir, "{}.ioctl".format(ioctl_basename))
    pcap = os.path.join(ddir, "{}.pcapng".format(ioctl_basename))

    devices = glob.glob(os.path.join(ddir, "device")) + glob.glob(os.path.join(ddir, "device-*[!~]"))
    device_args = []
    for device in devices:
        p = open(device).readline().strip()
        assert p.startswith('P: ')
        device_args.extend(("-d", device))

    if os.path.exists(pcap):
        syspath = '/sys' + p[3:]

        umockdev = ['umockdev-run', *device_args,
                    '-p', "%s=%s" % (syspath, pcap),
                    '--']

        # Skip test if we detect too old umockdev for pcap replay
        if not pcap_supported:
            sys.exit(77)

    else:
        dev = open(ioctl).readline().strip()
        assert dev.startswith('@DEV ')
        dev = dev[5:]
        if dev.endswith(" (SPI)"):
            dev = dev[:dev.rindex(" ")]

            # Skip test if we detect too old umockdev for spi replay
            if not spi_supported:
                sys.exit(77)

        umockdev = ['umockdev-run', *device_args,
                    '-i', "%s=%s" % (dev, ioctl),
                    '--']

    wrapper = os.getenv('LIBFPRINT_TEST_WRAPPER')
    return umockdev + (wrapper.split(' ') if wrapper else []) + [sys.executable]

def capture():
    subprocess.check_call(get_umockdev_runner("capture") +
                          ['%s' % os.path.join(edir, "capture.py"),
                           '%s' % os.path.join(tmpdir, "capture.png")])

    assert os.path.isfile(os.path.join(tmpdir, "capture.png"))
    if os.path.isfile(os.path.join(ddir, "capture.png")):
        # Compare the images, they need to be identical
        cmp_pngs(os.path.join(tmpdir, "capture.png"), os.path.join(ddir, "capture.png"))

def custom():
    subprocess.check_call(get_umockdev_runner("custom") +
                          ['%s' % os.path.join(ddir, "custom.py")])

try:
    if glob.glob(os.path.join(ddir, "capture.*")):
        capture()

    if glob.glob(os.path.join(ddir, "custom.*")):
        custom()

except Exception as e:
    # Store created output files for inspection (in the build directory)
    outdir = os.path.join('errors', os.path.basename(ddir))
    shutil.copytree(tmpdir, outdir, dirs_exist_ok=True)
    raise e

finally:
    shutil.rmtree(tmpdir)

