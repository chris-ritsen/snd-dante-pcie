#!/usr/bin/env python3
"""dante-live: Dante PCIe TUI dashboard with zero-copy metering via /dev/dante-pcie"""

import ctypes
import curses
import fcntl
import json
import math
import mmap
import os
import subprocess
import sys
import threading
import time

import numpy as np

def find_device():
    import glob

    devs = sorted(glob.glob("/dev/dante-pcie[0-9]*"))
    if devs:
        return devs[0]
    return None


DEVICE_PATH = find_device()
DANTE_IOC_GET_INFO = 0x80404400


class CardInfo(ctypes.Structure):
    _fields_ = [
        ("firmware_version", ctypes.c_uint32),
        ("sample_rate", ctypes.c_uint32),
        ("rx_channels", ctypes.c_uint32),
        ("tx_channels", ctypes.c_uint32),
        ("period_size", ctypes.c_uint32),
        ("chunks_per_channel", ctypes.c_uint32),
        ("channel_step", ctypes.c_uint32),
        ("dma_running", ctypes.c_uint32),
        ("sample_count", ctypes.c_uint64),
        ("rx_buffer_phys", ctypes.c_uint64),
        ("tx_buffer_phys", ctypes.c_uint64),
        ("rx_buffer_size", ctypes.c_uint32),
        ("tx_buffer_size", ctypes.c_uint32),
    ]


def get_channel_names():
    rx_names = {}
    tx_names = {}
    rx_subscriptions = {}
    try:
        result = subprocess.run(
            ["netaudio", "-n", "lx-dante", "-j"],
            capture_output=True, text=True, timeout=15,
        )
        data = json.loads(result.stdout)
        for device in data["dante"].values():
            channels = device.get("channels", {})
            for number_string, channel in channels.get("receivers", {}).items():
                rx_names[int(number_string)] = channel.get("name", number_string)
            for number_string, channel in channels.get("transmitters", {}).items():
                tx_names[int(number_string)] = channel.get("friendly_name") or channel.get("name", number_string)
            for subscription in device.get("subscriptions", []):
                rx_channel = subscription.get("rx_channel", "")
                tx_device = subscription.get("tx_device")
                tx_channel = subscription.get("tx_channel", "")
                for number, name in rx_names.items():
                    if name == rx_channel and tx_device:
                        rx_subscriptions[number] = f"{tx_device}:{tx_channel}"
                        break
    except Exception:
        pass
    return rx_names, tx_names, rx_subscriptions


def peak_to_db(peak):
    if peak <= 0:
        return -140.0
    return 20.0 * math.log10(peak / 0x7FFFFF00)


def compute_peaks(audio_map, channel_count, channel_step, channels=None):
    samples_per_channel = channel_step // 4
    if channels is None:
        buf = np.frombuffer(audio_map, dtype=np.int32,
                            count=channel_count * samples_per_channel).reshape(channel_count, samples_per_channel)
        minimum = np.min(buf, axis=1).astype(np.int64)
        maximum = np.max(buf, axis=1).astype(np.int64)
        raw_peaks = np.maximum(np.abs(minimum), np.abs(maximum))
        return {ch + 1: int(raw_peaks[ch]) for ch in range(channel_count)}
    peaks = {}
    for ch in channels:
        offset = (ch - 1) * samples_per_channel
        chunk = np.frombuffer(audio_map, dtype=np.int32, count=samples_per_channel, offset=offset * 4)
        peaks[ch] = max(abs(int(chunk.min())), abs(int(chunk.max())))
    return peaks


def update_audio_map(audio_map, device_fd, size, page_offset):
    if audio_map is not None and len(audio_map) != size:
        audio_map.close()
        audio_map = None
    if audio_map is None and size:
        audio_map = mmap.mmap(device_fd, size, mmap.MAP_SHARED,
                              mmap.PROT_READ, offset=page_offset * mmap.PAGESIZE)
    return audio_map


class Line:
    __slots__ = ("segments", "meters")

    def __init__(self):
        self.segments = []
        self.meters = []

    def text(self, column, content, color=0, attributes=0):
        self.segments.append((column, content, color, attributes))
        return self

    def set_meter(self, column, width, decibels):
        self.meters.append((column, width, decibels))
        return self

    def draw(self, screen, screen_y, max_width):
        for column, content, color, attributes in self.segments:
            if column >= max_width:
                continue
            content = content[:max_width - column]
            try:
                screen.addstr(screen_y, column, content, curses.color_pair(color) | attributes)
            except curses.error:
                pass
        for column, width, decibels in self.meters:
            fill = max(0, min(width, int((decibels + 60) * width / 60)))
            for position in range(min(width, max_width - column)):
                if position < fill:
                    if position > width * 0.85:
                        meter_color = 2
                    elif position > width * 0.7:
                        meter_color = 3
                    else:
                        meter_color = 4
                    try:
                        screen.addstr(screen_y, column + position, "█", curses.color_pair(meter_color))
                    except curses.error:
                        pass
                else:
                    try:
                        screen.addstr(screen_y, column + position, "░", curses.color_pair(5))
                    except curses.error:
                        pass


def main(screen):
    curses.curs_set(0)
    screen.nodelay(True)

    curses.start_color()
    curses.use_default_colors()
    curses.init_pair(1, curses.COLOR_WHITE, -1)
    curses.init_pair(2, curses.COLOR_RED, -1)
    curses.init_pair(3, curses.COLOR_YELLOW, -1)
    curses.init_pair(4, curses.COLOR_GREEN, -1)
    curses.init_pair(5, 8, -1)
    curses.init_pair(6, curses.COLOR_CYAN, -1)
    curses.init_pair(7, curses.COLOR_MAGENTA, -1)

    device_fd = os.open(DEVICE_PATH, os.O_RDONLY)

    info = CardInfo()
    fcntl.ioctl(device_fd, DANTE_IOC_GET_INFO, info)

    rx_audio = None
    tx_audio = None

    rx_names = {}
    tx_names = {}
    rx_subscriptions = {}
    names_lock = threading.Lock()

    def load_names():
        nonlocal rx_names, tx_names, rx_subscriptions
        r, t, s = get_channel_names()
        with names_lock:
            rx_names, tx_names, rx_subscriptions = r, t, s

    threading.Thread(target=load_names, daemon=True).start()

    show_all = True
    scroll = 0
    force_layout = None
    fps_modes = [120, 60, 30, 10]
    fps_index = 0
    frame_count = 0
    rx_peaks = {}
    tx_peaks = {}

    while True:
        fps = fps_modes[fps_index]
        screen.timeout(1000 // fps)

        fcntl.ioctl(device_fd, DANTE_IOC_GET_INFO, info)
        frame_count += 1

        rx_audio = update_audio_map(rx_audio, device_fd, info.rx_buffer_size, 0)
        tx_audio = update_audio_map(tx_audio, device_fd, info.tx_buffer_size, 1)
        rx_step = info.rx_buffer_size // info.rx_channels if info.rx_channels else 0
        tx_step = info.tx_buffer_size // info.tx_channels if info.tx_channels else 0
        if rx_audio is None:
            rx_peaks = {}
        if tx_audio is None:
            tx_peaks = {}

        key = screen.getch()
        while key != -1:
            if key == ord("q"):
                if rx_audio:
                    rx_audio.close()
                if tx_audio:
                    tx_audio.close()
                os.close(device_fd)
                return
            elif key == ord("a"):
                show_all = not show_all
            elif key == ord("r"):
                threading.Thread(target=load_names, daemon=True).start()
            elif key in (curses.KEY_UP, ord("k")):
                scroll = max(0, scroll - 1)
            elif key in (curses.KEY_DOWN, ord("j")):
                scroll += 1
            elif key == curses.KEY_PPAGE:
                scroll = max(0, scroll - 20)
            elif key == curses.KEY_NPAGE:
                scroll += 20
            elif key in (curses.KEY_HOME, ord("g")):
                scroll = 0
            elif key == ord("G"):
                scroll = 99999
            elif key == ord("f"):
                fps_index = (fps_index + 1) % len(fps_modes)
            elif key == ord("s"):
                if force_layout is None:
                    force_layout = "side"
                elif force_layout == "side":
                    force_layout = "stacked"
                else:
                    force_layout = None
            key = screen.getch()
        terminal_height, terminal_width = screen.getmaxyx()

        with names_lock:
            current_rx_names = dict(rx_names)
            current_tx_names = dict(tx_names)
            current_rx_subscriptions = dict(rx_subscriptions)

        visible_rows = terminal_height - 4

        sample_rate = info.sample_rate

        min_rx_panel_for_subscriptions = 100
        if force_layout == "side":
            side_by_side = True
        elif force_layout == "stacked":
            side_by_side = False
        else:
            side_by_side = terminal_width >= (min_rx_panel_for_subscriptions + 60)
        if side_by_side:
            rx_panel_width = int(terminal_width * 0.6)
            tx_panel_width = terminal_width - rx_panel_width - 1
            panel_width = tx_panel_width
        else:
            rx_panel_width = terminal_width
            tx_panel_width = terminal_width
            panel_width = terminal_width

        def visible_channels(channel_count, names):
            if show_all:
                return list(range(1, channel_count + 1))
            return [ch for ch in range(1, channel_count + 1) if names.get(ch, str(ch)) != str(ch)]

        rx_visible = visible_channels(info.rx_channels, current_rx_names)
        tx_visible = visible_channels(info.tx_channels, current_tx_names)

        if frame_count % 3 == 0:
            if rx_audio and info.rx_channels and rx_step:
                rx_peaks = compute_peaks(rx_audio, info.rx_channels, rx_step, rx_visible)
            if tx_audio and info.tx_channels and tx_step:
                tx_peaks = compute_peaks(tx_audio, info.tx_channels, tx_step, tx_visible)

        def build_channel_lines(visible, peaks, names, subscriptions, x_offset, this_panel_width, has_subscriptions=False):
            longest_name = max((len(names.get(n, str(n))) for n in visible), default=10)
            nw = min(longest_name, this_panel_width - 20)

            longest_subscription = 0
            if has_subscriptions and subscriptions:
                longest_subscription = max((len(v) for v in subscriptions.values()), default=0)

            db_width = 7
            padding = 4 + 1 + 1 + db_width
            subscription_space = (longest_subscription + 5) if longest_subscription > 0 else 0
            mw = max(5, this_panel_width - nw - padding - subscription_space)
            mc = nw + 5
            result = []
            for channel_number in visible:
                peak = peaks.get(channel_number, 0)
                decibels = peak_to_db(peak)
                name = names.get(channel_number, str(channel_number))

                line = Line()
                color = 6 if peak > 0 else 5
                line.text(x_offset, f"{channel_number:3d} {name:<{nw}s}", color)
                line.set_meter(x_offset + mc, mw, decibels)
                if peak == 0:
                    decibels_string = "  -inf"
                elif decibels < -60:
                    decibels_string = "  <-60"
                else:
                    decibels_string = f"{decibels:5.0f}"
                line.text(x_offset + mc + mw + 1, decibels_string, 1 if peak > 0 else 5)
                if subscriptions:
                    subscription = subscriptions.get(channel_number, "")
                    if subscription:
                        subscription_column = x_offset + mc + mw + 8
                        line.text(subscription_column, f"← {subscription}", 5)
                result.append(line)
            return result

        rx_lines = build_channel_lines(rx_visible, rx_peaks, current_rx_names, current_rx_subscriptions, 0, rx_panel_width, has_subscriptions=True)
        tx_x_offset = (rx_panel_width + 1) if side_by_side else 0
        tx_lines = build_channel_lines(tx_visible, tx_peaks, current_tx_names, {}, tx_x_offset, tx_panel_width)

        if side_by_side:
            lines = []
            rx_header = Line().text(0, "RX (from network)", 7, curses.A_BOLD)
            tx_header = Line().text(tx_x_offset, "TX (to network)", 7, curses.A_BOLD)
            combined_header = Line()
            combined_header.segments = rx_header.segments + tx_header.segments
            lines.append(combined_header)

            max_rows = max(len(rx_lines), len(tx_lines))
            for row_index in range(max_rows):
                combined = Line()
                if row_index < len(rx_lines):
                    combined.segments.extend(rx_lines[row_index].segments)
                    combined.meters.extend(rx_lines[row_index].meters)
                if row_index < len(tx_lines):
                    combined.segments.extend(tx_lines[row_index].segments)
                    combined.meters.extend(tx_lines[row_index].meters)
                lines.append(combined)
        else:
            lines = []
            lines.append(Line().text(0, "RX (from network)", 7, curses.A_BOLD))
            lines.extend(rx_lines)
            lines.append(Line().text(0, "─" * (terminal_width - 1), 5))
            lines.append(Line().text(0, "TX (to network)", 7, curses.A_BOLD))
            lines.extend(tx_lines)

        header_rows = 3
        footer_rows = 1
        visible_rows = terminal_height - header_rows - footer_rows
        max_scroll = max(0, len(lines) - visible_rows)
        scroll = max(0, min(scroll, max_scroll))

        screen.erase()

        row = 0
        title = "Dante PCIe — LX-DANTE"
        try:
            screen.addstr(row, 0, title, curses.color_pair(7) | curses.A_BOLD)
            screen.addstr(row, len(title) + 2,
                          f"{sample_rate}Hz  {info.rx_channels}rx/{info.tx_channels}tx  "
                          f"counter={info.sample_count}",
                          curses.color_pair(6))
        except curses.error:
            pass
        row += 1

        dma_state = "RUN" if info.dma_running else "STOP"
        state_color = 4 if info.dma_running else 2
        try:
            screen.addstr(row, 0, "DMA:", curses.color_pair(5))
            screen.addstr(row, 5, dma_state, curses.color_pair(state_color) | curses.A_BOLD)
            screen.addstr(row, 9,
                          f"period={info.period_size}  rx_step={rx_step}  tx_step={tx_step}",
                          curses.color_pair(5))
        except curses.error:
            pass
        row += 1

        try:
            screen.addstr(row, 0, "─" * min(terminal_width - 1, terminal_width), curses.color_pair(5))
        except curses.error:
            pass
        row += 1

        for index in range(visible_rows):
            line_index = scroll + index
            if line_index >= len(lines):
                break
            lines[line_index].draw(screen, row + index, terminal_width)

        if side_by_side:
            for draw_row in range(row, terminal_height - 1):
                try:
                    screen.addstr(draw_row, rx_panel_width, "│", curses.color_pair(5))
                except curses.error:
                    pass

        scroll_percent = int(100 * scroll / max_scroll) if max_scroll > 0 else 100
        layout_label = "auto" if force_layout is None else force_layout
        try:
            screen.addstr(terminal_height - 1, 0,
                          f" q:quit  a:filter  f:{fps}fps  s:layout({layout_label})  r:refresh  j/k:scroll  [{scroll_percent}%] ",
                          curses.color_pair(5) | curses.A_REVERSE)
        except curses.error:
            pass

        screen.refresh()


if __name__ == "__main__":
    if not DEVICE_PATH:
        print("No /dev/dante-pcie* device found")
        sys.exit(1)
    try:
        curses.wrapper(main)
    except KeyboardInterrupt:
        pass
