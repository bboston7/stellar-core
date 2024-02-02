#!/usr/bin/env python

from collections import defaultdict
import itertools as it

import networkx as nx

GRAPH_PATH = "pubnet.graphml"
GRAPH = nx.read_graphml(GRAPH_PATH)
UNDIRECTED = GRAPH.to_undirected()

TIER1 = {
    "Boötes" : "GCVJ4Z6TI6Z2SOGENSPXDQ2U4RKH3CNQKYUHNSSPYFPNWTLGS6EBH7I2",
    "Lyra by BP Ventures" : "GCIXVKNFPKWVMKJKVK2V4NK7D4TC6W3BUMXSIJ365QUAXWBRPPJXIR2Z",
    "Hercules by OG Technologies" : "GBLJNN3AVZZPG2FYAYTYQKECNWTQYYUUY2KVFN2OUKZKBULXIXBZ4FCT",
    "LOBSTR 3 (North America)" : "GD5QWEVV4GZZTQP46BRXV5CUMMMLP4JTGFD7FWYJJWRL54CELY6JGQ63",
    "LOBSTR 1 (Europe)" : "GCFONE23AB7Y6C5YZOMKUKGETPIAJA4QOYLS5VNS4JHBGKRZCPYHDLW7",
    "LOBSTR 2 (Europe)" : "GCB2VSADESRV2DDTIVTFLBDI562K6KE3KMKILBHUHUWFXCUBHGQDI7VL",
    "LOBSTR 4 (Asia)" : "GA7TEPCBDQKI7JQLQ34ZURRMK44DVYCIGVXQQWNSWAEQR6KB4FMCBT7J",
    "LOBSTR 5 (India)" : "GA5STBMV6QDXFDGD62MEHLLHZTPDI77U3PFOD2SELU5RJDHQWBR5NNK7",
    "FT SCV 2" : "GCMSM2VFZGRPTZKPH5OABHGH4F3AVS6XTNJXDGCZ3MKCOSUBH3FL6DOB",
    "FT SCV 3" : "GA7DV63PBUUWNUFAF4GAZVXU2OZMYRATDLKTC7VTCG7AU4XUPN5VRX4A",
    "FT SCV 1" : "GARYGQ5F2IJEBCZJCBNPWNWVDOFK7IBOHLJKKSG2TMHDQKEEC6P4PE4V",
    "SatoshiPay Frankfurt" : "GC5SXLNAM3C4NMGK2PXK4R34B5GNZ47FYQ24ZIBFDFOCU6D4KBN4POAE",
    "SatoshiPay Singapore" : "GBJQUIXUO4XSNPAUT6ODLZUJRV2NPXYASKUBY4G5MYP3M47PCVI55MNT",
    "SatoshiPay Iowa" : "GAK6Z5UVGUVSEK6PEOCAYJISTT5EJBB34PN3NOLEQG2SUKXRVV2F6HZY",
    "Whalestack (Germany)" : "GD6SZQV3WEJUH352NTVLKEV2JM2RH266VPEM7EH5QLLI7ZZAALMLNUVN",
    "Whalestack (Hong Kong)" : "GAZ437J46SCFPZEDLVGDMKZPLFO77XJ4QVAURSJVRZK2T5S7XUFHXI2Z",
    "Whalestack (Finland)" : "GADLA6BJK6VK33EM2IDQM37L5KGVCY5MSHSHVJA4SCNGNUIEOTCR6J5T",
    "SDF 2" : "GCM6QMP3DLRPTAZW2UZPCPX2LF3SXWXKPMP3GKFZBDSF3QZGV2G5QSTK",
    "SDF 1" : "GCGB2S2KGYARPVIA37HYZXVRM2YZUEXA6S33ZU5BUDC6THSB62LZSTYH",
    "SDF 3" : "GABMKJM6I25XI4K7U6XWMULOUQIQ27BCTMLS6BYYSOWKTBUXVRJSXHYQ",
    "Blockdaemon Validator 3" : "GAYXZ4PZ7P6QOX7EBHPIZXNWY4KCOBYWJCA4WKWRKC7XIUS3UJPT6EZ4",
    "Blockdaemon Validator 2" : "GAVXB7SBJRYHSG6KSQHY74N7JAFRL4PFVZCNWW2ARI6ZEKNBJSMSKW7C",
    "Blockdaemon Validator 1" : "GAAV2GCVFLNN522ORUYFV33E76VPC22E72S75AQ6MBR5V45Z5DWVPWEU"
}

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
        # TODO: Can probably just use undirected graph here and not have to do
        # the whole `chain` thing
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

def avg_distance_from(sources, dests):
    total_dist = 0
    # Horribly inefficient. For each destination, compute the minimum distance
    # from it to any node in the sources list. Then, return the average of these
    # distances
    for dest in dests:
        min_dist = float("inf")
        for source in sources:
            min_dist = min(min_dist,
                           nx.shortest_path_length(UNDIRECTED, source, dest))
        total_dist += min_dist
    return total_dist / len(dests)

def print_stats(nodes):
    print("Number of nodes: ", len(nodes))
    avg_bandwidth(nodes)
    print("Average distance from SDF1: "
          f"{avg_distance_from([TIER1["SDF 1"]], nodes)}")
    print("Average distance from tier 1: "
          f"{avg_distance_from(TIER1.values(), nodes)}")

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
    print("Stats for responding nodes:")
    print_stats(response)

    print()
    print("Stats for non-responding nodes:")
    print_stats(no_response)

    non_responding_versions = group_by_version(no_response)
    for version, nodes in non_responding_versions.items():
        print()
        print(f"Stats for non-responding nodes with version {version}:")
        print_stats(nodes)
