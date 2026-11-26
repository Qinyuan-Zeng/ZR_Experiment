"""Draw actual DAC updates and signed current feedback from a profiles CSV.

Usage: python PlotCurrentProfiles.py <current_profiles_*.log.csv> [output.png]
Requires Pillow. Each panel uses the first successful command as t=0.
"""
import argparse
import csv
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONT = Path("C:/Windows/Fonts/msyh.ttc")
BLUE = "#195889"
ORANGE = "#BF5719"
INK = "#1D3042"
GRID = "#DFE8EF"
WIDTH, HEIGHT = 1920, 1080
PROFILES = (
    ("三角波", (0, 20, 40, 60, 80, 100, 80, 60, 40, 20, 0)),
    ("正弦形", (0, 10, 35, 65, 90, 100, 90, 65, 35, 10, 0)),
    ("S 型", (0, 6, 32, 68, 94, 100, 94, 68, 32, 6, 0)),
)


def label(draw, xy, message, size=24, fill=INK, anchor=None):
    draw.text(xy, message, font=ImageFont.truetype(str(FONT), size),
              fill=fill, anchor=anchor)


def axes(draw, box, xmin, xmax, yticks, caption, show_x):
    left, top, right, bottom = box
    ymin, ymax = yticks[0], yticks[-1]

    def point(x, y):
        return (left + (x - xmin) * (right - left) / (xmax - xmin),
                bottom - (y - ymin) * (bottom - top) / (ymax - ymin))

    label(draw, (left, top - 34), caption, 21)
    for value in yticks:
        _, y = point(0, value)
        draw.line((left, y, right, y), fill=GRID, width=2)
        label(draw, (left - 12, y), str(value), 20, anchor="rm")
    for value in range(0, int(xmax) + 1, 4):
        x, _ = point(value, 0)
        draw.line((x, top, x, bottom), fill=GRID, width=2)
        if show_x:
            label(draw, (x, bottom + 12), str(value), 20, anchor="mt")
    draw.line((left, top, left, bottom, right, bottom), fill="#8EA3B5", width=2)
    if show_x:
        label(draw, ((left + right) / 2, bottom + 48), "相对首次发送时间 / ms",
              21, anchor="mt")
    return point


def steps(points, end=None):
    line = [points[0]]
    for previous, current in zip(points, points[1:]):
        line.extend(((current[0], previous[1]), current))
    if end is not None and end > points[-1][0]:
        line.append((end, points[-1][1]))
    return line


def plot(source, output):
    with source.open(newline="", encoding="utf-8-sig") as f:
        rows = list(csv.DictReader(f))
    image = Image.new("RGB", (WIDTH, HEIGHT), "white")
    draw = ImageDraw.Draw(image)
    label(draw, (50, 42), "开环 DAC 指令与电流反馈", 45)
    label(draw, (50, 105), "蓝色阶梯：DAC 指令     橙色采样点：有符号电流反馈", 25)
    successful_currents = [int(r["current_raw"]) for r in rows
                           if r["current_rc"] == "0" and int(r["stage"]) in (1, 2, 3, 4)]
    high = max(16, 4 * math.ceil(max(successful_currents, default=0) / 4))
    low = min(0, 4 * math.floor(min(successful_currents, default=0) / 4))

    for group, (name, planned) in enumerate(PROFILES, 1):
        wave = [r for r in rows if int(r["stage"]) == group]
        sent = [r for r in wave if r["set_rc"] == "0"]
        observed = [r for r in rows if int(r["stage"]) == 4 and int(r["group"]) == group]
        origin = float(sent[0]["set_begin_ms"]) if sent else 0
        targets = [(float(r["set_begin_ms"]) - origin, float(r["dac"])) for r in sent]
        xmax = max(24, 4 * math.ceil(max((t for t, _ in targets), default=20) / 4))
        left = 100 + (group - 1) * 625
        right = left + 505
        status = f"实测 {len(sent)}/{len(planned)} 点" if sent else "本次未发送"
        label(draw, (left, 176), f"{name}   {status}", 28)
        command_point = axes(draw, (left, 270, right, 540), 0, xmax,
                             [0, 25, 50, 75, 100], "目标 DAC / bit", False)
        current_point = axes(draw, (left, 650, right, 906), 0, xmax,
                             list(range(low, high + 1, 4)), "电流反馈 / raw", True)
        if targets:
            feedback = [(float(r["telemetry_end_ms"]) - origin, int(r["current_raw"]))
                        for r in wave + observed if r["current_rc"] == "0"]
            feedback = [(t, y) for t, y in feedback if 0 <= t <= xmax]
            hold_until = None
            if targets[-1][1] == 0 and feedback:
                hold_until = feedback[-1][0]
            draw.line([command_point(t, y) for t, y in steps(targets, hold_until)],
                      fill=BLUE, width=4)
            if len(feedback) > 1:
                draw.line([current_point(t, y) for t, y in feedback],
                          fill=ORANGE, width=3)
            for t, y in feedback:
                x, z = current_point(t, y)
                draw.ellipse((x-4, z-4, x+4, z+4), fill=ORANGE)
        else:
            plan = steps([(2 * i, y) for i, y in enumerate(planned)])
            draw.line([command_point(t, y) for t, y in plan], fill="#9CAAB7", width=4)
            label(draw, (left + 22, 290), "灰线为计划指令", 22, "#596F81")
            label(draw, ((left + right) / 2, 760), "无实测电流", 27, "#596F81", "mm")

    label(draw, (50, 1025), source.name + "  |  电流点以主机遥测结束时间绘制", 21, "#4A6173")
    image.save(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_file", type=Path)
    parser.add_argument("output", type=Path, nargs="?")
    args = parser.parse_args()
    plot(args.csv_file, args.output or args.csv_file.with_name(args.csv_file.stem + "_waveforms.png"))
