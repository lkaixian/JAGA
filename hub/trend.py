# Copy of server/trend.py analyze(): the hub image is built from ./hub only, so it can't import ../server.
from statistics import linear_regression

SONAR_ERR = -2147483648
WINDOW_S = 1800  # fit the last 30 min
STEADY_MM_PER_MIN = 1.0


def analyze(rows, warn_mm, danger_mm):
    """rows: [(unix_time_s, water_level_mm)] oldest first. Least-squares trend of the recent window."""
    pts = [(t, v) for t, v in rows if v != SONAR_ERR]
    out = {"samples": 0, "level_mm": None, "slope_mm_per_min": None,
           "direction": "unknown", "eta_warn_s": None, "eta_danger_s": None}
    if not pts:
        return out
    end = pts[-1][0]
    pts = [p for p in pts if end - p[0] <= WINDOW_S]
    out["samples"], out["level_mm"] = len(pts), pts[-1][1]
    if len(pts) < 3 or pts[-1][0] == pts[0][0]:
        return out
    slope, _ = linear_regression([p[0] for p in pts], [p[1] for p in pts])  # mm per second
    out["slope_mm_per_min"] = round(slope * 60, 2)
    out["direction"] = ("steady" if abs(slope * 60) < STEADY_MM_PER_MIN
                        else "rising" if slope > 0 else "falling")
    level = pts[-1][1]
    if slope > 0:
        for key, thr in (("eta_warn_s", warn_mm), ("eta_danger_s", danger_mm)):
            if level < thr:
                out[key] = round((thr - level) / slope)
    return out
