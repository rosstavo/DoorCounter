#!/usr/bin/env python3
"""Serial monitor for the door counter.

Used instead of `arduino-cli monitor`, which doesn't reliably hold the port on
this setup. Auto-detects the USB-serial port, can pulse the ESP32 reset line to
capture the boot banner, and streams output until Ctrl-C (or --seconds).

Requires pyserial:  pip install pyserial   (use a venv on PEP 668 systems)

Examples:
  python3 scripts/monitor.py                 # stream until Ctrl-C
  python3 scripts/monitor.py --reset         # reboot the board first, then stream
  python3 scripts/monitor.py --seconds 30    # capture 30s then exit
  python3 scripts/monitor.py --port /dev/cu.usbserial-1110
"""
import argparse
import glob
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial not installed.  pip install pyserial  (use a venv if needed)")


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbserial-*"))
    return ports[0] if ports else None


def main():
    ap = argparse.ArgumentParser(description="Door-counter serial monitor")
    ap.add_argument("--port", default=None, help="default: first /dev/cu.usbserial-*")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=0, help="0 = run until Ctrl-C")
    ap.add_argument("--reset", action="store_true",
                    help="pulse EN/RTS to reboot the board before reading")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        sys.exit("No /dev/cu.usbserial-* port found. Is the board plugged in?")

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = args.baud
    ser.timeout = 0.2
    # Don't assert the reset lines just by opening the port.
    ser.dtr = False
    ser.rts = False
    ser.open()

    if args.reset:
        ser.setDTR(False)
        ser.setRTS(True)   # hold EN low = reset
        time.sleep(0.1)
        ser.setRTS(False)  # release = boot
        time.sleep(0.05)

    sys.stderr.write(f"# {port} @ {args.baud} (Ctrl-C to stop)\n")
    sys.stderr.flush()

    end = time.time() + args.seconds if args.seconds else None
    buf = b""
    try:
        while end is None or time.time() < end:
            data = ser.read(256)
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                print(line.decode("utf-8", "replace").rstrip("\r"), flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()


if __name__ == "__main__":
    main()
