#!/usr/bin/env python3
"""Validate build/hierarchy.json and keep module.json's ui_hierarchy in sync.
usage: check-hierarchy.py build/hierarchy.json module.json [--write]"""
import json, sys
h = json.load(open(sys.argv[1]))
L = h["levels"]
assert h["pad_layout"] == "drums"
assert h["step_buttons_param"] == "step" and h["step_leds_param"] == "step_leds" and h["loop_knob_param"] == "loop_knob"
assert h["step_up_param"] == "step_up" and h["screen_param"] == "screen"
assert h["knob_capture_param"] == "knob_capture" and h["knob_delta_param"] == "knob_delta" and h["undo_param"] == "undo_cmd" and h["pedal_param"] == "pedal"
assert h["display_param"] == "display" and h["pad_leds_param"] == "pad_leds" and h["delete_pad_param"] == "delete_pad"
root = L["root"]
assert root["knobs"] == ["t%d_level" % i for i in range(1, 7)] + ["in_gain", "master"], root["knobs"]
for name in ("track", "setup", "status"):
    assert len(L[name]["knobs"]) <= 8
# every knob key exists as a param on its level
for name, lv in L.items():
    keys = {p["key"] for p in lv.get("params", []) if isinstance(p, dict) and "key" in p}
    for k in lv.get("knobs", []):
        assert k in keys, (name, k)
# pad -> note table (Movy rack order: pad 1 bottom-left, banks of 4x4)
want = {"v01": ("All Start/Stop", 36), "v02": ("Rec/Dub 1", 37), "v05": ("Stop All", 40),
        "v06": ("Play/Stop 1", 41), "v13": ("Shift", 48), "v17": ("Rec/Dub 4", 52),
        "v19": ("Rec/Dub 6", 54), "v20": ("Quantize", 55), "v32": ("Count-in", 67)}
for v, (nm, note) in want.items():
    assert L[v]["name"] == nm and L[v]["note"] == note, (v, L[v])
notes = [L["v%02d" % i]["note"] for i in range(1, 33)]
assert notes == list(range(36, 68))
assert all(p["access"] == "read" for p in L["status"]["params"])
print("hierarchy ok,", len(L), "levels")
m = json.load(open(sys.argv[2]))
cur = m["capabilities"].get("ui_hierarchy")
if cur != h:
    if "--write" in sys.argv:
        m["capabilities"]["ui_hierarchy"] = h
        json.dump(m, open(sys.argv[2], "w"), indent=2); open(sys.argv[2], "a").write("\n")
        print("module.json ui_hierarchy updated")
    else:
        sys.exit("module.json ui_hierarchy is out of date (run make module-json)")
else:
    print("module.json in sync")
