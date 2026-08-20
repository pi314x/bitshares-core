# Oracles: design

Replacing the price feed mechanism with first-class oracles.

This document is the design half of the work. It states what the current mechanism cannot do,
what is being built, and — as importantly — what is deliberately not being built.

## What exists today

Price data enters the chain through `asset_publish_feed_operation`
([asset_ops.hpp:462](libraries/protocol/include/graphene/protocol/asset_ops.hpp#L462)). A
publisher submits a `price_feed` for one market-issued asset; the chain keeps the latest
submission per publisher in `asset_bitasset_data_object::feeds`, and
`update_median_feeds()` ([asset_object.cpp:47](libraries/chain/asset_object.cpp#L47)) takes
the median of those still inside `feed_lifetime_sec`, provided at least `minimum_feeds`
survive.

Authorization is one of three fixed shapes
([asset_evaluator.cpp:1453](libraries/chain/asset_evaluator.cpp#L1453)): active witnesses if
the asset is `witness_fed_asset`, active committee members if `committee_fed_asset`,
otherwise the asset's own producer allow-list.

It works, and it has secured BitShares smartcoins for a decade. The limits below are about
what it *cannot express*, not about it being broken.

## Why that is not enough

**A feed is an attribute of an asset, not a thing in itself.** There is no way to publish
"BTC/USD" as a datum that several consumers reference. Every consumer needs its own asset,
its own producer set, and its own copy of the same numbers. Futures — the next piece of work
— need a mark price that is not the settlement price of any particular smartcoin, and there
is currently nowhere to put one.

**The payload is a fixed struct of smartcoin parameters.** `price_feed` carries
`settlement_price`, `core_exchange_rate`, MCR, MSSR and ICR. A consumer that wants only a
price still inherits collateral-ratio machinery that means nothing to it.

**Producer sets are coarse.** Witness-fed, committee-fed, or a per-asset allow-list, with one
vote each. There is no way to say "these five exchanges, and require four of them", or to
weight a producer that has been reliable for a year above one added yesterday.

**Aggregation is median-of-latest, and nothing else.** With `minimum_feeds` of 3, two
colluding producers set the price outright. There is no outlier handling, no bound on how far
one update may move the result, and no time dimension at all: a value that is true for one
block is as authoritative as one that has held for a day.

**There is no history.** Only the latest submission per publisher is kept, so no
time-averaged measure can be computed even in principle. This is the one limitation that
cannot be fixed retroactively — history has to start being recorded before it can be used.

## The design

A new protocol object, `oracle_object` (`1.23.x`), holding a named price series independent
of any asset, plus four operations (tags 78–81). Existing feeds are untouched; smartcoins opt
in per asset.

### The object

```
oracle_object                     1.23.x
  owner                           account that administers it
  name                            unique, e.g. "BTC.USD"  (validated like an asset symbol)
  description                     free text, bounded
  base_asset / quote_asset        what the price means; every published value must match
  producers                       flat_map<account_id_type, uint16_t weight>
  minimum_producers               quorum: fewer live submissions than this ⇒ no value
  value_lifetime_sec              a submission older than this stops counting
  aggregation                     median_of_latest | median_over_window
  window_sec                      for median_over_window
  max_deviation_ppm               outlier bound, 0 = disabled
  submissions                     flat_map<account_id_type, pair<time_point_sec, price>>
  history                         bounded ring of (time, price), the aggregate over time
  current_value                   optional<price> — absent means "no usable value"
  current_value_time
```

`current_value` being `optional` is deliberate and is the part consumers must respect. Today
a stale or quorum-less feed yields a null `price` that reads as zero in some paths; an
`optional` that is *absent* cannot be silently mistaken for a number. A consumer that cannot
tolerate a gap must say so itself.

### The operations

| Tag | Operation | Who signs | Effect |
|---|---|---|---|
| 78 | `oracle_create_operation` | owner | creates the series, sets producers and policy |
| 79 | `oracle_update_operation` | owner | changes producers, quorum, lifetime, aggregation |
| 80 | `oracle_delete_operation` | owner | removes it; refuses while any asset references it |
| 81 | `oracle_publish_operation` | a producer | submits one `price` |

`oracle_publish_operation` is intentionally the narrowest operation in the set: a producer
supplies an oracle id and a price, nothing else. Producers cannot alter policy, and the owner
cannot publish values unless it is also a producer.

### Aggregation

Both methods are exact. **No floating point and no lossy arithmetic appears anywhere in
consensus aggregation** — `price` is a ratio of two `int64` amounts, and both methods below
only ever *select* an observed value, never compute a new one. That rules out the entire
class of rounding and overflow divergence between nodes.

**`median_of_latest`** — generalizes today's behaviour. Take submissions newer than
`value_lifetime_sec`; if fewer than `minimum_producers` remain, the value is absent; otherwise
the weighted median: the lowest value whose cumulative weight *exceeds* half the total.

With every weight equal to 1 that reduces to the element at index `size/2`, which is precisely
the element the legacy code picks (`effective_feeds.begin() + size/2` in
[asset_object.cpp:103](libraries/chain/asset_object.cpp#L103)) — the upper of the two middle
values in an even-sized set. Matching that convention is not cosmetic: an asset migrating from
legacy feeds to an oracle with the same producers must not see its settlement price jump
because the two disagreed about which element is "the" median.

**`median_over_window`** — the median of the *recorded aggregates* within `window_sec`, drawn
from `history`.

This is a windowed median, **not** a true time-weighted average, and the difference is a
deliberate choice rather than an approximation of one. A real TWAP requires accumulating
price × duration, which for a ratio type means either lossy scaling or 128-bit accumulators
whose overflow behaviour has to be identical on every node forever. A windowed median needs
neither: it returns one of the prices actually observed, so it is exact by construction, and
it still defeats the attack that matters — a single-block price spike cannot move it.

### Outlier handling

`max_deviation_ppm` bounds how far one submission may sit from the current aggregate before
it is excluded.

The subtle part is what happens in a genuine crash, when every honest producer moves at once.
A naive rule that rejects all deviating submissions would freeze the oracle exactly when it
most needs to move — the failure mode that has broken real systems. So the rule is:

> Outliers are excluded **only while enough non-outliers remain to meet quorum.** If
> excluding them would drop the count below `minimum_producers`, no exclusion happens and the
> aggregate moves.

A minority disagreeing with the consensus is filtered; everyone moving together is believed.

### Binding a smartcoin to an oracle

A new optional asset option, `price_oracle_id`. When absent, nothing changes — the legacy
feed path runs exactly as it does today, and this must remain byte-identical for every
existing asset. When set, `update_median_feeds()` sources the settlement price from the
oracle instead of from `feeds`.

Existing smartcoins keep working with no action from anyone. Migration is per asset, opt-in,
and reversible by the asset issuer.

## Consensus safety

The concerns this design has to answer, and how:

- **Determinism.** Aggregation selects among observed values; no division, no floating point,
  no accumulation. Identical inputs give byte-identical output on every node.
- **Wire format.** New object and new operations rather than changes to existing ones. Nothing
  appends a field to a structure already on chain, which is the mistake that broke the witness
  operations during the PQ review and which the mainnet replay test exists to catch.
- **Hardfork gating.** All four operations and the `price_oracle_id` option are rejected
  before the hardfork, so no node can produce a block a pre-fork node cannot validate.
- **State growth.** `history` is a fixed-size ring, and `submissions` is bounded by the
  producer set, which the owner controls. An oracle's storage is bounded at creation, not
  by how long it lives or how often it is published to.
- **Fees.** `oracle_publish_operation` will be published frequently by design. Its fee needs
  to be low enough not to punish honest producers and high enough to bound spam; it is charged
  per publish, and producers are an allow-list, so the spam surface is limited to accounts the
  owner already trusted.

## Out of scope

Stated explicitly, because each is a plausible thing to expect from something called "oracles"
and none of it is being built here:

- **Non-price data.** The value type is `price`. A general-purpose data oracle carrying
  arbitrary bytes or scaled integers invites rounding questions this design deliberately
  avoids, and nothing asked for it.
- **Off-chain sourcing, signing schemes, or a fetch protocol.** How a producer decides what to
  publish stays entirely off-chain, exactly as it is today.
- **Incentives, staking, or slashing.** There is no bond, no reward, and no automatic
  punishment for a bad producer. The owner removes them. Designing a cryptoeconomic incentive
  layer is a separate problem and a much larger one.
- **Reputation or automatic producer scoring.** Weights are set by the owner, not earned.
- **Replacing the legacy feed path.** It stays. Assets migrate to oracles individually, and
  the old code path must keep producing identical results for assets that do not.

## Build order

1. ~~`oracle_object`, its index, and the object type registration.~~
2. ~~`oracle_create` / `oracle_update` / `oracle_delete`, with evaluators and hardfork gating.~~
3. ~~`oracle_publish`, submission storage, and `median_of_latest`.~~
4. ~~History recording and `median_over_window`.~~
5. ~~Outlier handling.~~
6. ~~`price_oracle_id` on assets, and the feed branch.~~
7. ~~Wallet and database API support.~~

All seven are done. Two details settled during implementation that are worth recording:

**Binding lives in `bitasset_options::ext`, not general asset options**, and can only be set
via `asset_update_bitasset` — never at asset creation. The oracle's base asset has to be the
smartcoin itself, which has no id yet while it is being created, so there would be nothing to
validate the orientation against. Binding at creation is refused with a message saying so.

That extension is encoded as a count followed by (field index, value) pairs, with indices from
declaration order, so `price_oracle_id` had to be **appended**. Inserting it would have shifted
every pre-existing field by one and made every historical asset operation decode into the wrong
fields — the same class of defect the PQ review found in the witness operations. A test pins all
seven tag numbers so a later edit that inserts rather than appends fails in CI.

**The oracle supplies only the settlement price.** MCR, MSSR and ICR come from the asset's own
options, which BSIP-75/77 already made owner-settable, and the core exchange rate is left null
so the issuer's `asset_options::core_exchange_rate` keeps applying. An oracle publishes a market
price, not a fee-conversion rate, and deriving one from the settlement price would quietly change
how fees are charged.

Publishing pushes the new value to bound assets in the same block, via a subscriber set held on
the oracle. That set exists so a publish — an operation designed to be sent every few seconds —
never does work proportional to the chain's total asset count, and it makes "refuse to delete an
oracle something depends on" an O(1) check. It is capped at `GRAPHENE_ORACLE_MAX_SUBSCRIBERS`.
