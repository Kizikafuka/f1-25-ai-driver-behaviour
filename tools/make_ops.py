"""Write db/racecraft.json from the game database: the racecraft and car-control edits, one `set` per AI car / driver and
field, rows picked by label. usage: python make_ops.py <gamedatabases.2025.bin> [swofty-ref tools dir]
Only rows that have the field get an op (a set on a missing field fails the build).
"""
import json, os, sys
sys.path.insert(0, sys.argv[2] if len(sys.argv) > 2 else r"D:\02 Games\Mods\F1\Script\swofty-ref\tools")
import gamedb

recs = {r.key: r for r in gamedb.read(open(sys.argv[1], "rb").read())}


def val(p):
    r = recs.get(gamedb.fnv1a64(p.encode()))
    return None if r is None else r.value.decode("utf-8", "replace")


def curve(xmin, xmax, xs, opt):
    pts = ",".join(f"{x}:{{{{{opt}}}}}" for x in xs)
    return f"@v2@minx:{xmin},maxx:{xmax},miny:0,maxy:1,{pts}"


# (table, row word, field, value written)
FIELDS = [
    # racecraft: space an AI car keeps from other cars (stock 0.35 at every speed; x = speed 50..500)
    ("entrants.vehicles", "vehicle", "ai_vehicle_config.buffer_space_settings.vehicle_buffer",
     curve(50, 500, ["0", "0", "0.4736842", "1", "1"], "spacing")),
    # car control (stock 0.5 / 0.4 rad). The AI's reaction after a hit is not here: AI Racing Tweaks does that in memory.
    ("entrants.vehicles", "vehicle", "ai_vehicle_config.pedal_controller.lateral_traction_slip_angle_cut_in_proportion", "{{throttle_cut}}"),
    ("entrants.vehicles", "vehicle", "ai_vehicle_config.lost_control.angle", "{{give_up}}"),
]
ops = []
for table, row, field, value in FIELDS:
    for i in range(int(val(f"{table}._{row}_instancecount_") or 0)):
        label = val(f"{table}.{row}_{i}._metadata_")
        if label and val(f"{table}.{row}_{i}.{field}") is not None:
            ops.append({"op": "set", "select": f"{table}.{row}[_metadata_={label}]", "field": field, "value": value})


def num(x):
    return f"{x:.4f}".rstrip("0").rstrip(".") if x != int(x) else str(int(x))


def scale_curve(curve, f):
    """'@v2@minx:..,maxx:..,miny:..,maxy:..,x:y,...' with every point's y times f."""
    head, body = curve[:4], curve[4:].split(",")
    pts = [p for p in body if not p.split(":")[0] in ("minx", "maxx", "miny", "maxy")]
    keep = [p for p in body if p.split(":")[0] in ("minx", "maxx", "miny", "maxy")]
    return head + ",".join(keep + [f"{p.split(':')[0]}:{num(float(p.split(':')[1]) * f)}" for p in pts])


# Per-driver style ("variety"): every driver row of the game database (ai_driver_config) gets its own number a in -1..1, fixed by the
# driver's row label (the same every race; a triangular spread, so few drivers sit at the extremes). a > 0 is a more aggressive driver:
#   alongside.min_presence_within_buffer   later to make room             stock 0.33, set by the "alongside" option
#   overtake.close_range_car_lengths_to_start_overtake   attacks from further back   stock 1.5
#   overtake.braking_zone_abort_overtake_time            commits longer to a pass    stock 0.667 (lower = commits longer)
# With variety off every driver gets the option's value as it is. Rows without a label (or without the field) get no op.
ALONG = {"0.33": 0.33, "0.5": 0.5, "0.7": 0.7, "0.85": 0.85, "0.95": 0.95}
VARIETY = {"small": 0.5, "medium": 1.0, "big": 1.5}
AL = "ai_driver_config.alongside.min_presence_within_buffer"
OT1 = "ai_driver_config.overtake.close_range_car_lengths_to_start_overtake"
OT2 = "ai_driver_config.overtake.braking_zone_abort_overtake_time"


def style(label):
    u = lambda salt: (gamedb.fnv1a64(f"{label}#{salt}".encode()) % 100000) / 99999.0
    return (u(1) + u(2)) - 1.0


for i in range(int(val("entrants.drivers._driver_instancecount_") or 0)):
    label = val(f"entrants.drivers.driver_{i}._metadata_")
    if not label:
        continue
    sel = f"entrants.drivers.driver[_metadata_={label}]"
    a = 0.0 if label == "default_driver" else style(label)
    if val(f"entrants.drivers.driver_{i}.{AL}") is not None:
        ops.append({"op": "set", "select": sel, "field": "ai_driver_config.alongside.min_presence_within_buffer",
                    "value": curve(0, 1, ["0", "0", "1", "1"], "alongside"), "when": {"variety": "off"}})
        for vlevel, vs in VARIETY.items():
            for lvl, base in ALONG.items():
                v = min(0.98, max(0.05, base + a * 0.12 * vs))
                ops.append({"op": "set", "select": sel, "field": "ai_driver_config.alongside.min_presence_within_buffer",
                            "value": "@v2@minx:0,maxx:1,miny:0,maxy:1," + ",".join(f"{x}:{num(v)}" for x in ("0", "0", "1", "1")),
                            "when": {"variety": vlevel, "alongside": lvl}})
    for field, mul in ((OT1, lambda a, vs: 1 + a * 0.3 * vs), (OT2, lambda a, vs: 1 - a * 0.4 * vs)):
        stock = val(f"entrants.drivers.driver_{i}.{field}")
        if stock is None or stock.startswith("@") or label == "default_driver":
            continue
        for vlevel, vs in VARIETY.items():
            ops.append({"op": "set", "select": sel, "field": field, "value": num(max(0.2, float(stock) * mul(a, vs))),
                        "when": {"variety": vlevel}})


# Per-level options: one op per car and level, computed from that car's own stock value and gated with "when".
# Steering correction: the AI's steering PID (stock e.g. P -2 / I -1 / D -0.5, output smoothing 0.3 s) straightens
# the car at once after a push. Lower gains + slower output = it catches slides late.
STEER = {"75": 0.75, "50": 0.5, "25": 0.25}
PID = "ai_vehicle_config.steering_pid_settings."
# AI traction control: traction_control.grip_atc_strength curve (stock ~0.53 on F1 cars); lower = more wheelspin.
ATC = {"half": 0.5, "off": 0.0}
for i in range(int(val("entrants.vehicles._vehicle_instancecount_") or 0)):
    label = val(f"entrants.vehicles.vehicle_{i}._metadata_")
    sel = f"entrants.vehicles.vehicle[_metadata_={label}]"
    if not label:
        continue
    for level, f in STEER.items():
        for k in ("proportional_gain", "integral_gain", "derivative_gain"):
            stock = val(f"entrants.vehicles.vehicle_{i}.{PID}{k}")
            if stock is not None:
                ops.append({"op": "set", "select": sel, "field": PID + k, "value": num(float(stock) * f),
                            "when": {"steer_correction": level}})
        stock = val(f"entrants.vehicles.vehicle_{i}.{PID}output_smoothing_half_life")
        if stock is not None:
            ops.append({"op": "set", "select": sel, "field": PID + "output_smoothing_half_life",
                        "value": num(float(stock) / f), "when": {"steer_correction": level}})
    # Damaged car driving: how much a damaged AI car slows down (damage_settings: aero 0.12 / 0.04 / 0.1, burst
    # 0.325 / 0.3 / 0.25 on F1 cars = corner speed cut / wider line / earlier braking) and its puncture speed cap.
    for level, (f, cap) in {"pushing": (0.5, 1.5), "flat_out": (0.25, 2.0)}.items():
        for k in ("aero_speed_reduction_factor", "aero_radius_modifier", "aero_brake_distance_increase_factor",
                  "burst_speed_reduction_factor", "burst_radius_modifier", "burst_brake_distance_increase_factor"):
            stock = val(f"entrants.vehicles.vehicle_{i}.ai_vehicle_config.damage_settings.{k}")
            if stock is not None:
                ops.append({"op": "set", "select": sel, "field": f"ai_vehicle_config.damage_settings.{k}",
                            "value": num(float(stock) * f), "when": {"damaged_driving": level}})
        stock = val(f"entrants.vehicles.vehicle_{i}.ai_vehicle_config.speed_limit_settings.puncture_speed_limit")
        if stock is not None:
            ops.append({"op": "set", "select": sel, "field": "ai_vehicle_config.speed_limit_settings.puncture_speed_limit",
                        "value": num(float(stock) * cap), "when": {"damaged_driving": level}})
    curve = val(f"entrants.vehicles.vehicle_{i}.ai_vehicle_config.traction_control.grip_atc_strength")
    if curve and curve.startswith("@v2@"):
        for level, f in ATC.items():
            ops.append({"op": "set", "select": sel, "field": "ai_vehicle_config.traction_control.grip_atc_strength",
                        "value": scale_curve(curve, f), "when": {"traction_control": level}})
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "db", "racecraft.json")
json.dump({"schema_version": 1, "ops": ops}, open(out, "w", encoding="utf-8", newline="\n"), indent=1)
print(f"{len(ops)} ops -> {os.path.normpath(out)}")
