"""
Spread polygon calculation for effluent irrigator mapping.

The irrigator has an annular spray pattern from inner radius R_in to outer
radius R_out (e.g. 6 m to 12 m), produced by two nozzles on a 6 m rotating boom.

Spray intensity is uniform across the annulus:
    I = Q / (pi * (R_out^2 - R_in^2))   [m/s]

For a moving irrigator, a ground point at perpendicular distance d from track:
    d > R_out             -> depth = 0
    R_in <= d <= R_out    -> depth = I * 2*sqrt(R_out^2 - d^2) / v * 1000 mm
    0 <= d < R_in         -> depth = I * 2*(sqrt(R_out^2-d^2) - sqrt(R_in^2-d^2)) / v * 1000 mm

For a stationary irrigator the wetted shape is the true annulus and depth is uniform:
    depth = I * duration_s * 1000 mm

Pump-on positions are split into separate runs wherever the gap between
consecutive points exceeds GAP_THRESHOLD_MIN minutes.
"""

import math
import datetime
from shapely.geometry import LineString, Point, mapping
from shapely.ops import unary_union

GAP_THRESHOLD_MIN = 2.0
ZONE_COUNT        = 7
STATIONARY_SPEED  = 0.02   # m/s


# ── Geometry helpers ──────────────────────────────────────────────────────────

def _m_per_deg(avg_lat_deg):
    lat_r = math.radians(avg_lat_deg)
    m_lat = 111132.92 - 559.82 * math.cos(2*lat_r) + 1.175 * math.cos(4*lat_r)
    m_lon = 111412.84 * math.cos(lat_r) - 93.5  * math.cos(3*lat_r)
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
        valid.append((p['lon'], p['lat'], _parse_ts(p.get('timestamp'))))
    if not valid:
        return []
    runs, current = [], [valid[0]]
    for prev, cur in zip(valid, valid[1:]):
        if prev[2] and cur[2]:
            gap_ok = (cur[2] - prev[2]).total_seconds() / 60.0 <= GAP_THRESHOLD_MIN
        else:
            gap_ok = True
        if gap_ok:
            current.append(cur)
        else:
            runs.append(current)
            current = [cur]
    runs.append(current)
    return runs


def _run_speed_ms(run):
    if len(run) < 2:
        return 0.0
    lat_r = math.radians(sum(p[1] for p in run) / len(run))
    dist = 0.0
    for i in range(1, len(run)):
        dx = math.radians(run[i][0] - run[i-1][0]) * 6371000 * math.cos(lat_r)
        dy = math.radians(run[i][1] - run[i-1][1]) * 6371000
        dist += math.sqrt(dx*dx + dy*dy)
    t1, t2 = run[0][2], run[-1][2]
    dur_s = (t2 - t1).total_seconds() if (t1 and t2 and t2 > t1) else len(run) * 30
    return dist / dur_s if dur_s > 0 else 0.0


def _run_duration_s(run):
    t1, t2 = run[0][2], run[-1][2]
    if t1 and t2 and t2 > t1:
        return (t2 - t1).total_seconds()
    return len(run) * 30


# ── Depth physics ─────────────────────────────────────────────────────────────

def _depth_annular_mm(d_m, R_out_m, R_in_m, intensity_m_s, v_ms):
    """
    Depth in mm at perpendicular distance d from a moving irrigator with
    uniform-intensity annular spray (inner radius R_in, outer radius R_out).
    """
    if v_ms <= 0 or d_m >= R_out_m:
        return 0.0
    if d_m >= R_in_m:
        # Ground point lies within the annular band: exposed once per pass
        t = 2.0 * math.sqrt(max(0.0, R_out_m**2 - d_m**2)) / v_ms
    else:
        # Ground point lies inside the inner hole: exposed on both sides as annulus passes
        chord_out = 2.0 * math.sqrt(max(0.0, R_out_m**2 - d_m**2))
        chord_in  = 2.0 * math.sqrt(max(0.0, R_in_m**2  - d_m**2))
        t = (chord_out - chord_in) / v_ms
    return intensity_m_s * t * 1000.0


# ── Zone geometry ─────────────────────────────────────────────────────────────

def _make_depth_zones(run, deg_per_m, R_out_m, R_in_m, Q_m3_s):
    """
    Return ZONE_COUNT GeoJSON Feature dicts, each band from d=0 to d=R_out.
    Each feature carries depth_mm computed for its mid-radius.
    """
    coords  = [(p[0], p[1]) for p in run]
    speed   = _run_speed_ms(run)
    dur_s   = _run_duration_s(run)
    is_stat = (speed < STATIONARY_SPEED or len(coords) == 1)

    annulus_area = math.pi * (R_out_m**2 - R_in_m**2)
    intensity    = Q_m3_s / annulus_area  # m/s uniform spray rate

    features = []
    for i in range(ZONE_COUNT):
        outer_r_m = R_out_m * (1.0 - i       / ZONE_COUNT)
        inner_r_m = R_out_m * (1.0 - (i + 1) / ZONE_COUNT)
        mid_r_m   = (outer_r_m + inner_r_m) / 2.0

        if is_stat:
            if mid_r_m < R_in_m:
                continue  # inside the inner hole — no water when stationary
            depth = intensity * dur_s * 1000.0
        else:
            depth = _depth_annular_mm(mid_r_m, R_out_m, R_in_m, intensity, speed)

        outer_r_deg = outer_r_m * deg_per_m
        inner_r_deg = inner_r_m * deg_per_m

        if len(coords) == 1:
            outer_poly = Point(coords[0]).buffer(outer_r_deg)
            inner_poly = Point(coords[0]).buffer(inner_r_deg) if inner_r_deg > 1e-9 else None
        else:
            outer_poly = LineString(coords).buffer(outer_r_deg, cap_style=1, join_style=1)
            inner_poly = (LineString(coords).buffer(inner_r_deg, cap_style=1, join_style=1)
                          if inner_r_deg > 1e-9 else None)

        ring = outer_poly.difference(inner_poly) if inner_poly else outer_poly
        if ring.is_empty:
            continue

        features.append({
            'type': 'Feature',
            'geometry': mapping(ring),
            'properties': {
                'type':     'depth_zone',
                'zone':     i,
                'depth_mm': round(depth, 2),
            }
        })

    return features


# ── Public API ────────────────────────────────────────────────────────────────

def calculate_spread(positions, spread_width_m, flow_rate_lpm, spread_inner_m=6.0):
    """
    positions       : list of dicts — lat, lon, timestamp (ISO), pump_on=1
    spread_width_m  : outer spray diameter in metres (outer radius = half this)
    flow_rate_lpm   : pump flow rate in litres per minute
    spread_inner_m  : inner radius of annular spray in metres (default 6.0)

    Returns { geojson: FeatureCollection, stats: dict } or None.
    """
    runs = [r for r in _split_runs(positions) if r]
    if not runs:
        return None

    all_pts = [pt for run in runs for pt in run]
    avg_lat  = sum(p[1] for p in all_pts) / len(all_pts)
    m_lat, m_lon = _m_per_deg(avg_lat)

    R_out_m   = spread_width_m / 2.0
    R_in_m    = float(spread_inner_m)
    deg_per_m = 1.0 / m_lat
    half_deg_out = R_out_m * deg_per_m

    Q_m3_s = flow_rate_lpm / 60000.0   # L/min → m³/s

    # Overall wetted footprint: outer buffer of each run (moving case covers center too)
    polys = []
    for run in runs:
        coords = [(p[0], p[1]) for p in run]
        if len(coords) == 1:
            polys.append(Point(coords[0]).buffer(half_deg_out))
        else:
            polys.append(LineString(coords).buffer(half_deg_out, cap_style=1, join_style=1))
    spread_poly = unary_union(polys)

    area_m2      = spread_poly.area * m_lat * m_lon
    duration_min = _total_duration_minutes(runs)
    volume_l     = flow_rate_lpm * duration_min
    app_rate     = volume_l / area_m2 if area_m2 > 0 else 0.0
    depth_avg_mm = app_rate  # 1 L/m² = 1 mm

    # Depth zones for each run
    zone_features = []
    for run in runs:
        zone_features.extend(_make_depth_zones(run, deg_per_m, R_out_m, R_in_m, Q_m3_s))

    max_depth_mm = (max(f['properties']['depth_mm'] for f in zone_features)
                    if zone_features else round(depth_avg_mm, 1))

    track_coords = [(p[0], p[1]) for p in all_pts]

    geojson = {
        'type': 'FeatureCollection',
        'features': zone_features + [
            {
                'type': 'Feature',
                'geometry': mapping(spread_poly),
                'properties': {
                    'type':          'spread',
                    'area_m2':       round(area_m2),
                    'area_ha':       round(area_m2 / 10000, 3),
                    'volume_l':      round(volume_l),
                    'app_rate_l_m2': round(app_rate, 3),
                    'depth_mm':      round(depth_avg_mm, 1),
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
        'depth_mm':      round(depth_avg_mm, 1),
        'max_depth_mm':  round(max_depth_mm, 1),
        'point_count':   len(all_pts),
    }

    return {'geojson': geojson, 'stats': stats}


def _total_duration_minutes(runs):
    total = 0.0
    for run in runs:
        tss = [p[2] for p in run if p[2]]
        if len(tss) >= 2:
            total += (max(tss) - min(tss)).total_seconds() / 60.0
        else:
            total += len(run) * 0.5
    return total
