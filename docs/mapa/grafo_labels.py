#!/usr/bin/env python3
"""grafo_labels.py GRAPHIFY_OUT: names graphify's communities without an
LLM, by their two main files and their most connected node, into
GRAPHIFY_OUT/.graphify_labels.json (mapa.py grafo)."""
import collections
import json
import os
import sys

out = sys.argv[1]
g = json.load(open(os.path.join(out, "graph.json")))
by = collections.defaultdict(list)
for n in g["nodes"]:
    by[n.get("community")].append(n)
deg = collections.Counter()
for e in g["links"]:
    deg[e["source"]] += 1
    deg[e["target"]] += 1
labels = {}
for c, ns in by.items():
    files = collections.Counter(os.path.basename(n.get("source_file") or "?") for n in ns)
    hub = max(ns, key=lambda n: deg[n["id"]])["label"]
    labels[str(c)] = " + ".join(f for f, _ in files.most_common(2)) + " · " + hub
json.dump(labels, open(os.path.join(out, ".graphify_labels.json"), "w"), indent=1, ensure_ascii=False)
