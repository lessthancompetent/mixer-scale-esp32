"""
Spread polygon calculation for effluent irrigator mapping.

The irrigator sprays in a circle centred on its GPS position. As it
travels with the pump on, the covered area is the union of those circles
(equivalent to buffering the GPS track). When the pump is off the
irrigator is repositioned; those gap periods must NOT be included in the
spread polygon.

Positions are split into continuous pump-on "runs" wherever the time gap
between consecutive points exceeds GAP_THRESHOLD_MIN. Each run is buffered
independently and the results are unioned.
"""

import math
import datetime
import json
from shapely.geometry import LineString, Point, mapping
from shapely.ops import unary_union

GAP_THRESHOLD_MIN = 2.0   # gap > 2 min between pump-on points = new run


def _m_per_deg(avg_lat_deg):
    lat_r = math.radians(avg_lat_deg)
    m_lat = 111132.92 - 559.82 * math.cos(2 * lat_r) + 1.175 * math.cos(4 * lat_r)
    m_lon = 111412.84 * math.cos(lat_r) - 93.5 * math.cos(3 * lat_r)
    return m_lat, m_lon


def _parse_ts(ts_str):
    if not ts_str:
        return None
    try:
        return datetime.datetime.fromisoformat(ts_str.replace('Z', '+00:00'))
    except ValueError:
        return None


def _split_runs(positions):
    """Split pump-on positions into continuous runs by time gap."""
    valid = []
    for p in positions:
        if p.get('lat') is None or p.get('lon') is None:
            continue
        ts = _parse_ts(p.get('timestamp'))
        valid.append((p['lon'], p['lat'], ts))

    if not valid:
        return []

    runs, current = [], [valid[0]]
    for prev, cur in zip(valid, valid[1:]):
        gap_ok = False
        if prev[2] and cur[2]:
            gap_min = (cur[2] - prev[2]).total_seconds() / 60.0
            gap_ok = gap_min <= GAP_THRESHOLD_MIN
        else:
            gap_ok = True  # no timestamps — keep together
        if gap_ok:
            current.append(cur)
        else:
            runs.append(current)
            current = [cur]
    runs.append(current)
    return runs


def calculate_spread(positions, spread_width_m, flow_rate_lpm):
    """
    positions      : list of dicts with keys lat, lon, timestamp (ISO string)
    spread_width_m : spray diameter in metres (radius = half this)
    flow_rate_lpm  : litres per minute

    Returns dict with geojson + stats, or None if not enough data.
    """
    runs = _split_runs(positions)
    runs = [r for r in runs if r]   # drop empty
    if not runs:
        return None

    all_pts = [pt for run in runs for pt in run]
    avg_lat = sum(p[1] for p in all_pts) / len(all_pts)
    m_lat, m_lon = _m_per_deg(avg_lat)
    half_deg = (spread_width_m / 2.0) / m_lat

    polygons = []
    for run in runs:
        coords = [(p[0], p[1]) for p in run]
        if len(coords) == 1:
            # Single stationary point — draw a circle
            poly = Point(coords[0]).buffer(half_deg)
        else:
            poly = LineString(coords).buffer(half_deg, cap_style=1, join_style=1)
        polygons.append(poly)

    spread_poly = unary_union(polygons)

    area_deg2 = spread_poly.area
    area_m2   = area_deg2 * m_lat * m_lon

    # Duration: sum of each run's duration
    duration_min = _total_duration_minutes(runs)
    volume_l     = flow_rate_lpm * duration_min
    app_rate     = volume_l / area_m2 if area_m2 > 0 else 0.0
    depth_mm     = app_rate  # 1 L/m² = 1 mm

    track_coords = [(p[0], p[1]) for p in all_pts]

    geojson = {
        'type': 'FeatureCollection',
        'features': [
            {
                'type': 'Feature',
                'geometry': mapping(spread_poly),
                'properties': {
                    'type':          'spread',
                    'area_m2':       round(area_m2),
                    'area_ha':       round(area_m2 / 10000, 3),
                    'volume_l':      round(volume_l),
                    'app_rate_l_m2': round(app_rate, 3),
                    'depth_mm':      round(depth_mm, 1),
                }
            },
            {
                'type': 'Feature',
                'geometry': {'type': 'LineString', 'coordinates': track_coords},
                'properties': {'type': 'track', 'point_count': len(track_coords)}
            }
        ]
    }

    stats = {
        'area_m2':       round(area_m2),
        'area_ha':       round(area_m2 / 10000, 3),
        'duration_min':  round(duration_min, 1),
        'volume_l':      round(volume_l),
        'app_rate_l_m2': round(app_rate, 3),
        'depth_mm':      round(depth_mm, 1),
        'point_count':   len(all_pts),
    }

    return {'geojson': geojson, 'stats': stats}


def _total_duration_minutes(runs):
    """Sum of each run's duration (first→last timestamp within each run)."""
    total = 0.0
    for run in runs:
        timestamps = [p[2] for p in run if p[2]]
        if len(timestamps) >= 2:
            total += (max(timestamps) - min(timestamps)).total_seconds() / 60.0
        else:
            total += len(run) * 0.5  # fallback: assume 30s intervals
    return total
