"""
This module simulates the HTTP endpoints of stellar-core's overlay survey
"""

from enum import Enum
import random

import networkx as nx

# The maximum number of peers that a node will include in each response peer
# list. This applies separately to the inbound and outbound peer lists, so the
# total number of reported peers may be up to `2 * MAX_PEERS`
MAX_PEERS = 25

class SimulationError(Exception):
    """An error that occurs during simulation"""

class SimulatedResponse:
    """Simulates a `requests.Response`"""
    def __init__(self, json):
        self._json = json

    def json(self):
        """Simulates the `json` method of a `requests.Response`"""
        return self._json

class PeerListMode(Enum):
    """TODO: Docs. Also clarify that this is up to MAX_PEERS per direction, not
    total."""
    # Return all peers, ignoring the MAX_PEERS limit
    ALL = 1
    # Return up to MAX_PEERS peers, chosen randomly
    RANDOM = 2
    # Return up to MAX_PEERS peers, chosen iteratively
    ITERATIVE = 3


class SurveySimulation:

    """
    Simulates the HTTP endpoints of stellar-core's overlay survey. Raises
    SimulationError if `root_node` is not in the graph represented by
    `graph_path`.
    """
    def __init__(self, graph_path, root_node, peer_list_mode):
        # The graph of the network being simulated
        self._graph = nx.read_graphml(graph_path)
        if root_node not in self._graph.nodes:
            raise SimulationError(f"root node '{root_node}' not in graph")
        # The node the simulation is being performed from
        self._root_node = root_node
        # The set of requests that have not yet been simulated
        self._pending_requests = set()
        # The results of the simulation
        self._results = {"topology" : {}}
        # The total number of requests sent on simulated overlay
        self._total_requests = 0
        # The mode for generating peer lists
        self._peer_list_mode = peer_list_mode
        print(f"simulating from {root_node}")

    @property
    def peer_list_mode(self):
        """The mode for generating peer lists"""
        return self._peer_list_mode

    def _info(self, params):
        """
        Simulate the info endpoint. Only fills in the version info for the
        root node.
        """
        assert not params, f"Unsupported info invocation with params: {params}"
        version = self._graph.nodes[self._root_node]["version"]
        return SimulatedResponse({"info" : {"build" : version}})

    def _peers(self, params):
        """
        Simulate the peers endpoint. Only fills in the "id" field of each
        authenticated peer.
        """
        assert params == {"fullkeys": "true"}, \
               f"Unsupported peers invocation with params: {params}"
        json = {"authenticated_peers": {"inbound": [], "outbound": []}}
        for peer in self._graph.in_edges(self._root_node):
            json["authenticated_peers"]["inbound"].append({"id" : peer[0]})
        for peer in self._graph.out_edges(self._root_node):
            json["authenticated_peers"]["outbound"].append({"id" : peer[1]})
        return SimulatedResponse(json)

    def _scp(self, params):
        """Simulate the scp endpoint. Only fills in the "you" field"""
        assert params == {"fullkeys": "true", "limit": 0}, \
               f"Unsupported scp invocation with params: {params}"
        return SimulatedResponse({"you": self._root_node})

    def _surveytopology(self, params):
        """
        Simulate the surveytopology endpoint. This endpoint currently ignores
        the `duration` parameter
        """
        assert params.keys() == {"node", "duration"}, \
               f"Unsupported surveytopology invocation with params: {params}"
        node = params["node"]
        if node != self._root_node and node not in self._pending_requests:
            self._pending_requests.add(node)
            self._total_requests += 1

    def _addpeer(self, node_id, edge_data, peers):
        """
        Given data on a graph edge in `edge_data`, translate to the expected
        getsurveyresult json and add to `peers` list
        """
        # Start with data on the edge itself
        peer_json = edge_data.copy()
        # Add peer's node id and version
        peer_json["nodeId"] = node_id
        peer_json["version"] = self._graph.nodes[node_id]["version"]
        # Add to inboundPeers
        peers.append(peer_json)

    def _addpeers(self, peers):
        ret = []
        if self._peer_list_mode == PeerListMode.ALL:
            # Add all peers
            for (node_id, data) in peers:
                self._addpeer(node_id, data, ret)
        elif self._peer_list_mode == PeerListMode.RANDOM:
            # Add a random subset of peers, up to MAX_PEERS
            random.shuffle(peers)
            for (node_id, data) in peers[:MAX_PEERS]:
                self._addpeer(node_id, data, ret)
        else:
            assert False, "TODO: Unsupported peer list mode"
        return ret

    def _getsurveyresult(self, params):
        """Simulate the getsurveyresult endpoint"""
        assert not params, \
               f"Unsupported getsurveyresult invocation with params: {params}"

        # For simulation purposes, the survey is in progress so long as there
        # are still pending requests to simulate.
        self._results["surveyInProgress"] = bool(self._pending_requests)

        # Update results
        while self._pending_requests:
            node = self._pending_requests.pop()

            # Start with info on the node itself
            node_json = self._graph.nodes[node].copy()

            # Remove "version" field, which is not part of stellar-core's
            # response
            del node_json["version"]

            # Generate inboundPeers list
            node_json["inboundPeers"] = self._addpeers(
                [ (node_id, data) for
                  (node_id, _, data) in self._graph.in_edges(node, True) ])

            # Generate outboundPeers list
            node_json["outboundPeers"] = self._addpeers(
                [ (node_id, data) for
                  (_, node_id, data) in self._graph.out_edges(node, True) ])

            self._results["topology"][node] = node_json
        return SimulatedResponse(self._results)

    def get(self, url, params):
        """Simulate a GET request"""
        endpoint = url.split("/")[-1]
        if endpoint == "stopsurvey":
            # Do nothing
            return
        if endpoint == "info":
            return self._info(params)
        if endpoint == "peers":
            return self._peers(params)
        if endpoint == "scp":
            return self._scp(params)
        if endpoint == "surveytopology":
            return self._surveytopology(params)
        if endpoint == "getsurveyresult":
            return self._getsurveyresult(params)
        raise SimulationError("Received GET request for unknown endpoint "
                              f"'{endpoint}' with params '{params}'")

    def print_stats(self):
        """Print stats on the simulation"""
        print(f"Total requests sent: {self._total_requests}")

        # stellar-core sends 10 requests every 15 seconds, or 40 per minute
        run_mins = self._total_requests / 40
        print(f"Estimated runtime: {run_mins:.2f} minutes")
