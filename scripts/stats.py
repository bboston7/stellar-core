#!/usr/bin/env python

from collections import defaultdict
import itertools as it

import networkx as nx

GRAPH_PATH = "pubnet.graphml"
GRAPH = nx.read_graphml(GRAPH_PATH)

def node_responded(node_id):
    """Returns true if a given node responded to the survey"""
    # TODO: This works by checking for a field in the node's data that should be
    # present if the node responded to the survey. We should probably sanity
    # check that all expected fields are present, or just "version" is present.
    return "numTotalInboundPeers" in GRAPH.nodes[node_id]

def total_responded():
    """Returns the number of nodes that responded to the survey"""
    return sum(map(node_responded, GRAPH.nodes))

def group_by_version(nodes):
    """Returns a dictionary from version string to a list of nodes with that
    version"""
    ret = defaultdict(list)
    for node in nodes:
        ret[GRAPH.nodes[node]["version"]].append(node)
    return ret

def print_count_dict(d):
    for k, v in d.items():
        print(f"{k}: {len(v)}")

def avg_bandwidth(nodes):
    num_edges = 0
    total_read = 0
    total_written = 0
    seen_edges = set()
    for node in nodes:
        for (src, dest, data) in it.chain(GRAPH.in_edges(node, True),
                                          GRAPH.out_edges(node, True)):
            if (src, dest) in seen_edges or (dest, src) in seen_edges:
                continue
            seen_edges.add((src, dest))
            num_edges += 1
            total_read += data["bytesRead"]
            total_written += data["bytesWritten"]
    print(f"Average read from : {total_read / num_edges}")
    print(f"Average write to  : {total_written / num_edges}")
    print(f"read/write ratio  : {total_read / total_written}")

if __name__ == "__main__":
    print(f"Total nodes: {len(GRAPH.nodes)}")

    response = [node for node in GRAPH.nodes if node_responded(node)]
    no_response = [node for node in GRAPH.nodes if not node_responded(node)]
    assert(len(response) + len(no_response) == len(GRAPH.nodes))

    num_responded = total_responded()
    print(f"Nodes that responded: {len(response)}")
    print("Percentage of nodes that responded: "
          f"{num_responded / len(GRAPH.nodes) * 100:.2f}%")

    print()
    print("Bandwidth stats for responding nodes:")
    avg_bandwidth(response)

    print()
    print("Bandwidth stats for non-responding nodes:")
    avg_bandwidth(no_response)

    non_responding_versions = group_by_version(no_response)
    for version, nodes in non_responding_versions.items():
        print()
        print(f"Stats for non-responding nodes with version {version}:")
        print("Number of nodes: ", len(nodes))
        avg_bandwidth(nodes)
