#!/usr/bin/env python

from collections import defaultdict
import csv
import sys

import networkx as nx

GRAPH_PATH = sys.argv[1]
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

CSV_FIELD_NAMES = [ "group"
                  , "num_nodes"
                  , "avg_read_from"
                  , "avg_write_to"
                  , "rw_ratio"
                  , "avg_dist_sdf1"
                  , "avg_dist_tier1"
                  , "avg_out_edges"
                  , "avg_in_edges"
                  , "avg_duration"
                  ]

LONG_FIELD_NAMES = { "group" : "Group"
                   , "num_nodes" : "Number of nodes"
                   , "avg_read_from" : "Average read from"
                   , "avg_write_to" : "Average write to"
                   , "rw_ratio" : "Read/Write ratio"
                   , "avg_dist_sdf1" : "Average distance from SDF1"
                   , "avg_dist_tier1" : "Average distance from tier 1"
                   , "avg_out_edges" : "Average out edges"
                   , "avg_in_edges" : "Average in edges"
                   , "avg_duration" : "Average number of seconds connected"
                   }

CSV_OUT_NAME = "out.csv"

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
    total_duration = 0
    seen_edges = set()
    for node in nodes:
        # TODO: Can probably just use undirected graph here and not have to do
        # the whole `chain` thing
        for (src, dest, data) in UNDIRECTED.edges(node, True): #it.chain(GRAPH.in_edges(node, True),
                                 #         GRAPH.out_edges(node, True)):
            if (src, dest) in seen_edges or (dest, src) in seen_edges:
                continue
            seen_edges.add((src, dest))
            num_edges += 1
            total_read += data["bytesRead"]
            total_written += data["bytesWritten"]
            total_duration += data["secondsConnected"]
    return { "avg_read_from" : total_read / num_edges
           , "avg_write_to" : total_written / num_edges
           , "rw_ratio" : total_read / total_written
           , "avg_duration" : total_duration / num_edges
           }

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

def avg_edges(nodes):
    out_edges = 0
    in_edges = 0
    for node in nodes:
        out_edges += len(GRAPH.out_edges(node))
        in_edges += len(GRAPH.in_edges(node))
    return { "avg_out_edges" : out_edges / len(nodes)
           , "avg_in_edges" : in_edges / len(nodes) }

def print_and_write_stats(stats, csv_writer):
    assert len(CSV_FIELD_NAMES) == len(stats)
    print()
    for k in CSV_FIELD_NAMES:
        print(f"{LONG_FIELD_NAMES[k]}: {stats[k]}")
    csv_writer.writerow(stats)


def get_stats(nodes, group_name):
    return { "group" : group_name
           , "num_nodes" : len(nodes)
           , "avg_dist_sdf1" : avg_distance_from([TIER1["SDF 1"]], nodes)
           , "avg_dist_tier1" : avg_distance_from(TIER1.values(), nodes)
           } | avg_edges(nodes) | avg_bandwidth(nodes)

def tier1_connectivity():
    total_distance = 0
    max_distance = 0
    total_pairs = 0
    t1s = list(TIER1.values())
    for i in range(0, len(t1s)):
        for j in range(i+1, len(t1s)):
            total_pairs += 1
            path_len = nx.shortest_path_length(UNDIRECTED, t1s[i], t1s[j])
            total_distance += path_len
            max_distance = max(max_distance, path_len)

    print(f"Average distance between tier 1 nodes: {total_distance / total_pairs}")
    print(f"Max distance between tier 1 nodes: {max_distance}")

if __name__ == "__main__":
    print(f"Total nodes: {len(GRAPH.nodes)}")

    response = [node for node in GRAPH.nodes if node_responded(node)]
    no_response = [node for node in GRAPH.nodes if not node_responded(node)]
    assert(len(response) + len(no_response) == len(GRAPH.nodes))

    responding_non_tier1 = [node for node in response if
                            node not in TIER1.values()]

    num_responded = total_responded()
    print(f"Nodes that responded: {len(response)}")
    print("Percentage of nodes that responded: "
          f"{num_responded / len(GRAPH.nodes) * 100:.2f}%")

    with open(CSV_OUT_NAME, 'w', newline='') as csvfile:
        csv_writer = csv.DictWriter(csvfile, fieldnames=CSV_FIELD_NAMES)
        csv_writer.writeheader()

        print_and_write_stats( get_stats(response, "Responding nodes")
                             , csv_writer )
        print_and_write_stats( get_stats( responding_non_tier1
                                        , "Responding nodes that are not in "
                                          "tier 1" )
                             , csv_writer )
        print_and_write_stats( get_stats(no_response , "Non-responding nodes")
                             , csv_writer )

        non_responding_versions = group_by_version(no_response)
        for version, nodes in non_responding_versions.items():
            stats = get_stats( nodes
                             , f"Non-responding nodes with version {version}")
            print_and_write_stats(stats, csv_writer)

        responding_versions = group_by_version(response)
        for version, nodes in responding_versions.items():
            stats = get_stats( nodes
                             , f"Responding nodes with version {version}")
            print_and_write_stats(stats, csv_writer)

    print()
    tier1_connectivity()