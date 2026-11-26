import sys
import xml.etree.ElementTree as ET

PHASES = (
    ("part of a manage or render sequence", 3),
    ("part of a manage sequence", 1),
    ("part of a render sequence", 2),
)

rows = []
for path in sys.argv[1:]:
    root = ET.parse(path).getroot()
    for iface in root.findall("interface"):
        for op, req in enumerate(iface.findall("request")):
            d = req.find("description")
            text = " ".join((d.text or "").split()) if d is not None else ""
            ph = next((p for s, p in PHASES if s in text), 0)
            if ph:
                rows.append('\t{ "%s", %d, %d, "%s" },' % (iface.get("name"), op, ph, req.get("name")))
if len(rows) < 40:
    sys.exit("gen_phase.py: only %d phase rules found, protocol wording changed?" % len(rows))
print("\n".join(rows))
