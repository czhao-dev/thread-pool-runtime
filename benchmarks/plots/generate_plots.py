#!/usr/bin/env python3
"""Renders benchmarks/plots/results.json into the two PNG figures embedded in
the README's Benchmarks section (light + dark variant of each, swapped via a
GitHub <picture> tag). Regenerate after any benchmark run:

    ./build-release/benchmarks/scheduler_bench \
        --benchmark_out=benchmarks/plots/results.json \
        --benchmark_out_format=json
    python3 benchmarks/plots/generate_plots.py
"""

import json
from pathlib import Path

import matplotlib
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker

matplotlib.use("Agg")

HERE = Path(__file__).resolve().parent
RESULTS = HERE / "results.json"

# Fixed strategy order + fixed categorical color slot per strategy, held
# constant across every panel and both figures -- color follows the entity,
# never its position or rank within a given chart.
STRATEGIES = [
    ("single_threaded", "single-threaded"),
    ("thread_per_task", "thread-per-task"),
    ("global_queue", "global-queue"),
    ("work_stealing", "work-stealing"),
    ("thread_per_core", "thread-per-core"),
    ("fair_scheduler", "fair (1 class)"),
]

WORKLOADS = [
    ("many_small_tasks", "many_small_tasks\n2,000 tasks x ~2k ops"),
    ("fewer_large_tasks", "fewer_large_tasks\n64 tasks x ~2M ops"),
    ("uneven_durations", "uneven_durations\n500 tasks, 1-in-10 ~100x heavier"),
]

THEMES = {
    "light": dict(
        surface="#fcfcfb",
        ink="#0b0b0b",
        ink_secondary="#52514e",
        muted="#898781",
        grid="#e1e0d9",
        baseline="#c3c2b7",
        colors=["#2a78d6", "#1baf7a", "#eda100", "#008300", "#4a3aa7", "#e34948"],
    ),
    "dark": dict(
        surface="#1a1a19",
        ink="#ffffff",
        ink_secondary="#c3c2b7",
        muted="#898781",
        grid="#2c2c2a",
        baseline="#383835",
        colors=["#3987e5", "#199e70", "#c98500", "#008300", "#9085e9", "#e66767"],
    ),
}

BAR_HEIGHT = 0.62


def load_results():
    data = json.load(open(RESULTS))
    by_name = {b["name"]: b for b in data["benchmarks"]}
    return by_name


def fmt_ms(value: float) -> str:
    if value >= 100:
        return f"{value:,.0f} ms"
    if value >= 10:
        return f"{value:,.1f} ms"
    return f"{value:,.2f} ms"


def plot_latency(by_name: dict, theme_name: str, out_path: Path) -> None:
    t = THEMES[theme_name]
    fig, axes = plt.subplots(
        1, 3, figsize=(13.5, 4.6), facecolor=t["surface"], constrained_layout=True
    )

    y_pos = range(len(STRATEGIES))
    for ax, (workload_key, workload_label) in zip(axes, WORKLOADS):
        ax.set_facecolor(t["surface"])
        values = [by_name[f"{workload_key}/{key}"]["real_time"] for key, _ in STRATEGIES]
        colors = t["colors"]

        # Reverse so single-threaded reads top-to-bottom in table order.
        bars = ax.barh(
            list(y_pos)[::-1],
            values,
            height=BAR_HEIGHT,
            color=colors,
            zorder=3,
        )

        ax.set_yticks(list(y_pos)[::-1])
        ax.set_yticklabels(
            [label for _, label in STRATEGIES], color=t["ink_secondary"], fontsize=9.5
        )
        ax.set_title(workload_label, color=t["ink"], fontsize=10.5, pad=10, loc="left")

        ax.xaxis.set_major_locator(mticker.MaxNLocator(nbins=4))
        ax.tick_params(axis="x", colors=t["muted"], labelsize=8.5)
        ax.tick_params(axis="y", length=0)
        for spine in ax.spines.values():
            spine.set_visible(False)
        ax.spines["bottom"].set_visible(True)
        ax.spines["bottom"].set_color(t["baseline"])
        ax.spines["bottom"].set_linewidth(1)
        ax.grid(axis="x", color=t["grid"], linewidth=1, zorder=0)
        ax.set_axisbelow(True)

        max_val = max(values)
        ax.set_xlim(0, max_val * 1.22)
        for bar, value in zip(bars, values):
            ax.text(
                bar.get_width() + max_val * 0.02,
                bar.get_y() + bar.get_height() / 2,
                fmt_ms(value),
                va="center",
                ha="left",
                fontsize=8.5,
                color=t["ink"],
            )

    fig.suptitle(
        "Wall-clock time to complete a workload, by scheduler backend (lower is better)",
        color=t["ink"],
        fontsize=11.5,
        x=0.01,
        ha="left",
    )
    fig.savefig(out_path, dpi=200, facecolor=t["surface"])
    plt.close(fig)


def plot_fairness(by_name: dict, theme_name: str, out_path: Path) -> None:
    t = THEMES[theme_name]
    fig, ax = plt.subplots(figsize=(7.2, 4.6), facecolor=t["surface"], constrained_layout=True)
    ax.set_facecolor(t["surface"])

    schedulers = [
        ("fair_scheduler", "fair_scheduler", t["colors"][0]),
        ("global_queue", "global_queue\n(no fairness concept)", t["muted"]),
        ("work_stealing", "work_stealing\n(no fairness concept)", t["muted"]),
    ]
    x_pos = range(len(schedulers))
    ratios = [by_name[f"fairness_under_class_skew/{key}"]["ratio"] for key, _, _ in schedulers]
    colors = [c for _, _, c in schedulers]

    bars = ax.bar(x_pos, ratios, width=0.5, color=colors, zorder=3)

    target = 4.0
    ax.axhline(target, color=t["ink_secondary"], linewidth=1.25, linestyle=(0, (4, 3)), zorder=2)
    ax.text(
        len(schedulers) - 0.42,
        target + 0.12,
        "target 4.0x (weight 4 : weight 1)",
        color=t["ink_secondary"],
        fontsize=9,
        ha="right",
    )

    ax.set_xticks(list(x_pos))
    ax.set_xticklabels([label for _, label, _ in schedulers], color=t["ink_secondary"], fontsize=9.5)
    ax.tick_params(axis="x", length=0)
    ax.tick_params(axis="y", colors=t["muted"], labelsize=8.5)
    ax.set_ylim(0, target * 1.25)
    ax.yaxis.set_major_locator(mticker.MaxNLocator(nbins=5))
    ax.set_ylabel("heavy : light completed ratio", color=t["ink_secondary"], fontsize=9.5)

    for spine in ax.spines.values():
        spine.set_visible(False)
    ax.spines["left"].set_visible(True)
    ax.spines["left"].set_color(t["baseline"])
    ax.grid(axis="y", color=t["grid"], linewidth=1, zorder=0)
    ax.set_axisbelow(True)

    for bar, ratio in zip(bars, ratios):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height() + target * 0.03,
            f"{ratio:.3f}x",
            ha="center",
            va="bottom",
            fontsize=9.5,
            color=t["ink"],
        )

    ax.set_title(
        "Completed-task share while both classes remain backlogged\n"
        "(heavy class: weight 4, light class: weight 1)",
        color=t["ink"],
        fontsize=10.5,
        loc="left",
    )
    fig.savefig(out_path, dpi=200, facecolor=t["surface"])
    plt.close(fig)


def main():
    by_name = load_results()
    for theme_name in THEMES:
        plot_latency(by_name, theme_name, HERE / f"latency_by_workload_{theme_name}.png")
        plot_fairness(by_name, theme_name, HERE / f"fairness_under_class_skew_{theme_name}.png")
    print("Wrote plots to", HERE)


if __name__ == "__main__":
    main()
