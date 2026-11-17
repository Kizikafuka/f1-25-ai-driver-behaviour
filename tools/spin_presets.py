"""The "AI spins" preset (option ai_spins, 0.2.0): one choice that sets the five slide options together. make_ops.py calls
expand() on its ops; run alone it updates db/racecraft.json in place (once: it skips a file that is already expanded).

Every op gated on one of the slide options (or using {{throttle_cut}} / {{give_up}}) becomes
  - the same op gated on {ai_spins: custom, <option>: <level>}   (the Custom spins settings), and
  - one op per preset that picks that level, gated on {ai_spins: <preset>}.
The slide reaction part of a preset (stock off, some quick, more normal, lots slow) is read by the DLL, not here.
"""
import json, os

# preset -> level of each slide option ("stock" = no op, the game's value)
PRESETS = {
    "stock": {"exit_mistakes": "stock", "steer_correction": "stock", "traction_control": "stock", "throttle_cut": "0.5", "give_up": "0.4"},
    "some":  {"exit_mistakes": "more",  "steer_correction": "75",    "traction_control": "half",  "throttle_cut": "0.75", "give_up": "0.4"},
    "more":  {"exit_mistakes": "more",  "steer_correction": "75",    "traction_control": "half",  "throttle_cut": "1.0", "give_up": "0.3"},
    "lots":  {"exit_mistakes": "lots",  "steer_correction": "50",    "traction_control": "off",   "throttle_cut": "1.0", "give_up": "0.2"},
}
GATED = ("exit_mistakes", "steer_correction", "traction_control")
TEMPLATED = {"throttle_cut": ("0.5", "0.75", "1.0"), "give_up": ("0.4", "0.3", "0.2")}


def expand(ops):
    out = []
    for op in ops:
        when = op.get("when") or {}
        key = next((k for k in GATED if k in when), None)
        tmpl = next((k for k in TEMPLATED if op.get("value") == "{{%s}}" % k), None)
        if key:
            level = when[key]
            out.append({**op, "when": {**when, "ai_spins": "custom"}})
            out += [{**op, "when": {**{k: v for k, v in when.items() if k != key}, "ai_spins": p}}
                    for p, lv in PRESETS.items() if lv[key] == level]
        elif tmpl:
            out += [{**op, "value": v, "when": {**when, "ai_spins": "custom", tmpl: v}} for v in TEMPLATED[tmpl]]
            out += [{**op, "value": lv[tmpl], "when": {**when, "ai_spins": p}} for p, lv in PRESETS.items()]
        else:
            out.append(op)
    return out


if __name__ == "__main__":
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "db", "racecraft.json")
    doc = json.load(open(path, encoding="utf-8"))
    if any("ai_spins" in (op.get("when") or {}) for op in doc["ops"]):
        print("already expanded")
    else:
        n = len(doc["ops"])
        doc["ops"] = expand(doc["ops"])
        json.dump(doc, open(path, "w", encoding="utf-8", newline="\n"), indent=1)
        print(f"{n} -> {len(doc['ops'])} ops")
