# Herder

The [HerderSCPDriver](HerderSCPDriver.h) is a concrete implementation of the [SCP
protocol](../scp), operating in terms of the "transaction sets" and "ledger
numbers" that constitute the Stellar vocabulary. It is implemented as a subclass
of the [SCPDriver class](../scp/SCPDriver.h), and so is most easily understood after
reading that class and understanding where and how a subclass would make the abstract
SCP protocol concrete.

# Key Implementation Details

The Herder considers a ledger number to be a "slot" in the SCP
protocol, and transaction set hashes (along with close-time and base-fee) to be
the sort of "value" that it is attempting to agree on for each "slot".

Herder acts as the glue between SCP and LedgerManager.

## `LedgerManager` Interaction
Herder serializes "slot externalize" events as much as possible so that
LedgerManager only sees strictly monotonic ledger closing events (and deal with
 any potential gaps using catchup).

## SCP Interaction
Herder has two main modes of operation:

### "Tracking" State
Herder knows which slot got externalized last and only processes SCP messages
 for the next slot.

SCP messages received for future slots are stored for later use: receiving
 future messages is not necessarily an indication of a problem.
Messages for the current slot may have been delayed by the network while
 other peers moved on.

#### Timeout
Herder places a timeout to make progress on the expected next slot, if it
 reaches this timeout, it changes its state to "Not tracking".

#### Picking Initial Position
When a ledger is closed and LedgerManager is in sync, herder is responsible
 for picking a starting position to send a PREPARING message.

### "Not Tracking" State
Herder does not know which slot got externalized last, its goal is to go back
 to the tracking state.
In order to do this, it starts processing all SCP messages starting with the
 smallest slot number, maximizing the chances that one of the slots actually
 externalizes.

Note that moving to this state does not necessarily mean that the
 LedgerManager would move out of sync: it could just be that it takes an
 abnormal time (network outage of some sort, partitioning, etc) for nodes to
 reach consensus.

## Leader Election Weight Function

Herder provides an application-specific weight function to ensure leader
election satisfies the following properties:

1. Organizations of the same quality level have equal probabilities of winning leader election.
2. Organizations of higher quality have a higher probability of winning leader election than organizations of lower quality.

The algorithm is "application-specific" because it relies quality information
present in the config file, unlike the default SCP leader election weight
function that does not have access to this information.

The algorithm is as follows:
* Let $O_q$ be the set of all organizations with a given quality level $q$. $O_q$ includes all explicitly defined organizations in the config file at quality level $q$ as well as a single virtual organization containing $O_r$ where $r$ is the next quality level below $q$.
* All organizations of some quality level $q$ have weight $w_q$.
* If an organization of quality level $q$ has $n$ nodes, then the weight of each node is $\frac{w_q}{n}$.
* $w_\top$ is `UINT64_MAX`, where $\top$ is the highest quality value assigned to any organization.
  * $\top$ is likely `HIGH`, but may be `CRITICAL` if `CRITICAL` orgs are present
* $w_\texttt{LOW}$ is $0$.
  *This allows low quality validators to participate in SCP without being trusted enough to be a round leader.
* For all other $q$ values, $w_q = \frac{w_p}{10 \times \left|O_p\right|}$ where $p$ is the next quality value above $q$.
  * The additional constant in the denominator is to handle case in which there is a single node of quality $q$. This node would have a weight equal to an entire org of quality $p$ if the constant didn’t exist.


TODO: Explain that win probability of a given node is `weight(n) / sum(weight(all_nodes))`

