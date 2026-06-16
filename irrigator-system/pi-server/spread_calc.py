"""
Spread polygon calculation for effluent irrigator mapping.

The irrigator sprays in a circle of radius R centred on its GPS position.
As it travels at speed v, a ground point at perpendicular distance d
from the track receives water for:

    t(d) = 2 * sqrt(R^2 - d^2) / v

giving depth:

    depth(d) = Q / (pi * R^2) * t(d)      [metres, convert to mm]

where Q is the flow rate in m^3/s.

For a stationary irrigator (speed ~ 0) depth is uniform:

    depth = Q * duration / (pi * R^2)

Pump-on positions are split into separate runs wherever the gap between
consecutive points exceeds GAP_THRESHOLD_MIN minutes.  Each run produces
ZONE_COUNT concentric ring GeoJSON features colour-coded by depth.
"""

import math
import datetime
from shapely.geometry import LineString, Point, mapping
from shapely.ops import unary_union

GAP_THRESHOLD_MIN  = 2.0    # minutes — gap between pump-on points = new run
ZONE_COUNT         = 5      # concentric depth rings per run
STATIONARY_SPEED   = 0.02   # m/s — below this treat irrigator as stationary


# ── Geometry helpers ──────────────────────────────────────────────────────────

def _m_per_deg(avg_lat_deg):
    lat_r = math.radians(avg_lat_deg)
    m_lat = 111132.92 - 559.82 * math.cos(2*lat_r) + 1.175 * math.cos(4*lat_r)
    m_lon = 111412.84 * math.cos(lat_r) - 93.5 * math.cos(3*lat_r)
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
    """Average travel speed of a run in m/s."""
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

def _depth_moving_mm(d_m, R_m, Q_m3_s, v_ms):
    """Depth in mm at perpendicular distance d from a moving irrigator track."""
    if v_ms <= 0 or d_m >= R_m or R_m <= 0:
        return 0.0
    t = 2.0 * math.sqrt(max(0.0, R_m**2 - d_m**2)) / v_ms
    return (Q_m3_s / (math.pi * R_m**2)) * t * 1000


def _depth_stationary_mm(Q_m3_s, R_m, dur_s):
    """Uniform depth in mm for a stationary irrigator over a duration."""
    area = math.pi * R_m**2
    return (Q_m3_s / area) * dur_s * 1000 if area > 0 else 0.0


# ── Zone geometry ─────────────────────────────────────────────────────────────

def _make_depth_zones(run, half_deg, R_m, Q_m3_s):
    """
    Return ZONE_COUNT GeoJSON Feature dicts, from outermost (lowest depth)
    to innermost (highest depth).
    """
    coords  = [(p[0], p[1]) for p in run]
    speed   = _run_speed_ms(run)
    dur_s   = _run_duration_s(run)
    is_stat = (speed < STATIONARY_SPEED or len(coords) == 1)

    features = []
    for i in range(ZONE_COUNT):
        # outer_frac decreases as i increases (outermost ring first)
        outer_frac = 1.0 - i       / ZONE_COUNT
        inner_frac = 1.0 - (i + 1) / ZONE_COUNT
        mid_r_m    = R_m * (outer_frac + inner_frac) / 2.0

        if is_stat:
            depth = _depth_stationary_mm(Q_m3_s, R_m, dur_s)
        else:
            depth = _depth_moving_mm(mid_r_m, R_m, Q_m3_s, speed)

        outer_r = half_deg * outer_frac
        inner_r = half_deg * inner_frac

        if len(coords) == 1:
            outer_poly = Point(coords[0]).buffer(outer_r)
            inner_poly = Point(coords[0]).buffer(inner_r) if inner_r > 1e-9 else None
        else:
            outer_poly = LineString(coords).buffer(outer_r, cap_style=1, join_style=1)
            inner_poly = LineString(coords).buffer(inner_r, cap_style=1, join_style=1) if inner_r > 1e-9 else None

        ring = outer_poly.difference(inner_poly) if inner_poly else outer_poly
        if ring.is_empty:
            continue

        features.append({
            'type': 'Feature',
            'geometry': mapping(ring),
            'properties': {
                'type':     'depth_zone',
                'zone':     i,         # 0 = outermost/shallowest
                'depth_mm': round(depth, 2),
            }
        })

    return features


# ── Public API ────────────────────────────────────────────────────────────────

def calculate_spread(positions, spread_width_m, flow_rate_lpm):
    """
    positions      : list of dicts — lat, lon, timestamp (ISO), pump_on=1
    spread_width_m : spray diameter in metres (radius = half this)
    flow_rate_lpm  : pump flow rate in litres per minute

    Returns { geojson: FeatureCollection, stats: dict } or None.
    """
    runs = [r for r in _split_runs(positions) if r]
    if not runs:
        return None

    all_pts = [pt for run in runs for pt in run]
    avg_lat = sum(p[1] for p in all_pts) / len(all_pts)
    m_lat, m_lon = _m_per_deg(avg_lat)
    R_m      = spread_width_m / 2.0
    half_deg = R_m / m_lat
    Q_m3_s   = flow_rate_lpm / 60000.0   # L/min → m³/s

    # Overall spread polygon (union of all runs)
    polys = []
    for run in runs:
        coords = [(p[0], p[1]) for p in run]
        if len(coords) == 1:
            polys.append(Point(coords[0]).buffer(half_deg))
        else:
            polys.append(LineString(coords).buffer(half_deg, cap_style=1, join_style=1))
    spread_poly = unary_union(polys)

    area_m2      = spread_poly.area * m_lat * m_lon
    duration_min = _total_duration_minutes(runs)
    volume_l     = flow_rate_lpm * duration_min
    app_rate     = volume_l / area_m2 if area_m2 > 0 else 0.0
    depth_avg_mm = app_rate   # 1 L/m² = 1 mm

    # Depth zones for each run
    zone_features = []
    for run in runs:
        zone_features.extend(_make_depth_zones(run, half_deg, R_m, Q_m3_s))

    max_depth_mm = max(
        (f['properties']['depth_mm'] for f in zone_features if f['properties']['zone'] == ZONE_COUNT - 1),
        default=round(depth_avg_mm, 1)
    )

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
