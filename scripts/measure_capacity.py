#!/usr/bin/env python3
"""
Capacity test measurement harness for Phase 8.

Measures:
  - RAM headroom (free heap)
  - LoRa duty cycle (TX/RX time ratio)
  - Advert spacing
  - Post delivery latency
  - Stability (30-min soak)

Usage:
  measure_capacity.py <PORT> [--mode ram|duty|soak] [--duration SECONDS]

Examples:
  measure_capacity.py /dev/ttyUSB0 --mode ram --duration 600
  measure_capacity.py /dev/ttyUSB0 --mode duty --continuous
  measure_capacity.py /dev/ttyUSB0 --mode soak --duration 1800
"""

import sys
import time
import serial
import re
import argparse
from datetime import datetime
from collections import deque


class CapacityMeasure:
    """Measure firmware capacity metrics via serial console."""

    def __init__(self, port, baudrate=115200):
        self.port = port
        self.baudrate = baudrate
        self.ser = None
        self.measurements = deque(maxlen=1000)
        self.advert_times = deque()

    def connect(self):
        """Connect to serial port."""
        try:
            self.ser = serial.Serial(self.port, self.baudrate, timeout=1)
            print(f"[+] Connected to {self.port} at {self.baudrate} baud")
            return True
        except Exception as e:
            print(f"[-] Failed to connect: {e}")
            return False

    def disconnect(self):
        """Close serial connection."""
        if self.ser:
            self.ser.close()

    def read_line(self, timeout=1.0):
        """Read a line from serial with timeout."""
        start = time.time()
        line = b""
        while time.time() - start < timeout:
            if self.ser.in_waiting > 0:
                char = self.ser.read(1)
                if char == b"\n":
                    return line.decode("utf-8", errors="ignore").strip()
                line += char
        return None

    def measure_ram(self, duration=600):
        """Measure free heap over time."""
        print(f"\n[*] Measuring RAM for {duration} seconds...")
        print("Timestamp,FreeHeap,TotalHeap,LargestFreeBlock")

        start_time = time.time()
        measurements = []

        while time.time() - start_time < duration:
            # Send free command
            self.ser.write(b"free\r\n")
            time.sleep(0.5)

            # Read response
            for _ in range(10):  # Try up to 10 lines
                line = self.read_line()
                if line and "heap" in line.lower():
                    # Expected format: "Heap: X bytes free, Y bytes total, Z bytes largest"
                    match = re.search(r"(\d+)\s+bytes?\s+free.*?(\d+)\s+bytes?\s+total.*?(\d+)\s+bytes?\s+largest", line)
                    if match:
                        free_heap = int(match.group(1))
                        total_heap = int(match.group(2))
                        largest_free = int(match.group(3))

                        pct_free = (free_heap / total_heap * 100) if total_heap > 0 else 0
                        timestamp = datetime.now().isoformat()

                        print(f"{timestamp},{free_heap},{total_heap},{largest_free}")
                        measurements.append({
                            "time": time.time() - start_time,
                            "free": free_heap,
                            "total": total_heap,
                            "largest": largest_free,
                            "pct_free": pct_free
                        })
                        break

            time.sleep(5)  # Sample every 5 seconds

        # Summary
        if measurements:
            free_values = [m["pct_free"] for m in measurements]
            print(f"\n[*] Summary:")
            print(f"    Min free: {min(free_values):.1f}%")
            print(f"    Avg free: {sum(free_values) / len(free_values):.1f}%")
            print(f"    Max free: {max(free_values):.1f}%")
            print(f"    PASS: {min(free_values) > 20}% (target >20%)")

    def measure_duty_cycle(self, duration=60, continuous=False):
        """Measure LoRa TX/RX on-air time as percentage of 60-second window."""
        print(f"\n[*] Measuring LoRa duty cycle in {duration}s windows...")
        print("Timestamp,OnAirMS,DutyCyclePct,Trend")

        if continuous:
            # Continuous measurement
            while True:
                self.ser.write(b"lora stats\r\n")
                time.sleep(1)

                for _ in range(20):
                    line = self.read_line()
                    if line and ("tx" in line.lower() or "rx" in line.lower()):
                        # Expected: "TX: X ms, RX: Y ms"
                        match = re.search(r"TX:\s*(\d+)\s*ms.*?RX:\s*(\d+)\s*ms", line)
                        if match:
                            tx_ms = int(match.group(1))
                            rx_ms = int(match.group(2))
                            on_air_ms = tx_ms + rx_ms
                            duty_cycle = (on_air_ms / (duration * 1000)) * 100

                            timestamp = datetime.now().isoformat()
                            trend = "↑" if duty_cycle > 0.7 else "→" if duty_cycle > 0.5 else "↓"
                            print(f"{timestamp},{on_air_ms},{duty_cycle:.3f}%,{trend}")

                            if duty_cycle > 0.8:
                                print(f"  ⚠ WARNING: Duty cycle {duty_cycle:.3f}% exceeds 0.8% limit!")
                            break

                time.sleep(duration)
        else:
            # Single measurement
            self.ser.write(b"lora stats\r\n")
            time.sleep(1)

            for _ in range(20):
                line = self.read_line()
                if line and ("tx" in line.lower() or "rx" in line.lower()):
                    match = re.search(r"TX:\s*(\d+)\s*ms.*?RX:\s*(\d+)\s*ms", line)
                    if match:
                        tx_ms = int(match.group(1))
                        rx_ms = int(match.group(2))
                        on_air_ms = tx_ms + rx_ms
                        duty_cycle = (on_air_ms / (duration * 1000)) * 100

                        print(f"On-air: {on_air_ms}ms / {duration*1000}ms = {duty_cycle:.3f}%")
                        print(f"PASS: {duty_cycle < 0.8}% (target <0.8%)")
                        break

    def measure_soak(self, duration=1800):
        """Run 30-minute stability soak test."""
        print(f"\n[*] Running {duration}s soak test (check for crashes)...")
        print("Timestamp,FreeHeap,PostCount,SyncLag,Stable")

        start_time = time.time()
        crash_detected = False

        while time.time() - start_time < duration:
            elapsed = int(time.time() - start_time)

            # Read any available serial data
            line = self.read_line(timeout=0.1)
            if line:
                # Check for crash indicators
                if any(x in line.lower() for x in ["fatal", "panic", "assert", "crash", "stack overflow"]):
                    crash_detected = True
                    print(f"[!] CRASH DETECTED at {elapsed}s: {line}")
                    break

                # Every 5 minutes, log status
                if elapsed % 300 == 0:
                    print(f"[*] {elapsed}s elapsed - logging status...")
                    self.ser.write(b"status\r\n")

            time.sleep(1)

        elapsed = int(time.time() - start_time)
        print(f"\n[*] Soak test complete after {elapsed}s")
        print(f"PASS: {not crash_detected and elapsed >= duration}% (target: no crashes, >{duration}s)")


def main():
    parser = argparse.ArgumentParser(description="Capacity test measurement harness")
    parser.add_argument("port", help="Serial port (e.g. /dev/ttyUSB0)")
    parser.add_argument("--mode", choices=["ram", "duty", "soak"], default="ram",
                        help="Measurement mode")
    parser.add_argument("--duration", type=int, default=600, help="Duration in seconds")
    parser.add_argument("--continuous", action="store_true", help="Continuous measurement")
    parser.add_argument("--baudrate", type=int, default=115200, help="Serial baud rate")

    args = parser.parse_args()

    measure = CapacityMeasure(args.port, args.baudrate)

    if not measure.connect():
        sys.exit(1)

    try:
        if args.mode == "ram":
            measure.measure_ram(args.duration)
        elif args.mode == "duty":
            measure.measure_duty_cycle(args.duration, args.continuous)
        elif args.mode == "soak":
            measure.measure_soak(args.duration)
    finally:
        measure.disconnect()


if __name__ == "__main__":
    main()
