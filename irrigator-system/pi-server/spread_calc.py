"""
Spread polygon calculation for effluent irrigator mapping.

Given a GPS track and constant spread width + flow rate, computes:
  - A buffered polygon (GeoJSON) representing the irrigated area
  - Total area (m², ha)
  - Total volume applied (litres)
  - Average application rate (L/m²  and  mm depth)
"""

import math
import json
from shapely.geometry import LineString, mapping
from shapely.ops import unary_union


def _m_per_deg(avg_lat_deg):
    """Metres per degree of latitude and longitude at a given latitude."""
    lat_r = math.radians(avg_lat_deg)
    m_lat = 111132.92 - 559.82 * math.cos(2 * lat_r) + 1.175 * math.cos(4 * lat_r)
    m_lon = 111412.84 * math.cos(lat_r) - 93.5 * math.cos(3 * lat_r)
    return m_lat, m_lon


def calculate_spread(positions, spread_width_m, flow_rate_lpm):
    """
    positions    : list of dicts with keys lat, lon, timestamp (ISO string)
    spread_width_m : total spray width in metres (e.g. 30)
    flow_rate_lpm  : litres per minute delivered by pump

    Returns a dict with:
        geojson  : GeoJSON FeatureCollection (spread polygon + GPS track)
        stats    : area_m2, area_ha, duration_min, volume_l, app_rate_l_m2, depth_mm
    Returns None if fewer than 2 valid positions.
    """
    pts = [(p['lon'], p['lat']) for p in positions
           if p.get('lat') is not None and p.get('lon') is not None]

    if len(pts) < 2:
        return None

    lats = [p[1] for p in pts]
    lons = [p[0] for p in pts]
    avg_lat = sum(lats) / len(lats)

    m_lat, m_lon = _m_per_deg(avg_lat)

    # Half width in degrees (use lat scale; error < 0.1% for typical spread widths)
    half_deg = (spread_width_m / 2.0) / m_lat

    # Buffer the track; cap_style=2 = flat end caps
    track = LineString(pts)
    polygon = track.buffer(half_deg, cap_style=2, join_style=2)

    # Area: convert from deg² to m² using average scale factors
    area_deg2 = polygon.area
    area_m2   = area_deg2 * m_lat * m_lon

    # Duration: difference between first and last timestamp
    duration_min = _duration_minutes(positions)
    volume_l     = flow_rate_lpm * duration_min
    app_rate     = volume_l / area_m2 if area_m2 > 0 else 0.0
    depth_mm     = app_rate * 1.0          # 1 L/m² = 1 mm depth

    geojson = {
        'type': 'FeatureCollection',
        'features': [
            {
                'type': 'Feature',
                'geometry': mapping(polygon),
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
                'geometry': {'type': 'LineString', 'coordinates': pts},
                'properties': {'type': 'track', 'point_count': len(pts)}
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
        'point_count':   len(pts),
    }

    return {'geojson': geojson, 'stats': stats}


def _duration_minutes(positions):
    """Minutes between first and last timestamp in the position list."""
    import datetime
    timestamps = []
    for p in positions:
        ts = p.get('timestamp')
        if not ts:
            continue
        try:
            # Handle both 'Z' suffix and bare ISO
            ts = ts.replace('Z', '+00:00')
            timestamps.append(datetime.datetime.fromisoformat(ts))
        except ValueError:
            pass
    if len(timestamps) < 2:
        # Fall back: assume 30s intervals
        return len(positions) * 0.5
    delta = max(timestamps) - min(timestamps)
    return delta.total_seconds() / 60.0
