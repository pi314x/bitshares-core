# Post-quantum status: what is protected, what is not

Companion to [PQ-REVIEW-FINDINGS.md](PQ-REVIEW-FINDINGS.md), which covers defects found
while reviewing the PQ_0 work. This document answers a different question: **if this branch
shipped today, what would actually be protected against a quantum adversary?**

The short answer is that the mechanisms exist and are tested, but the chain is not
post-quantum safe, and none of the remaining distance can be closed by writing more code.
Every gap below is a deployment decision — a hardfork date, a committee vote, an operator
flag, or a user running a command.

## Where the code lives

The work spans **two repositories**, which matters for anyone reviewing or building it:

| | |
|---|---|
| `bitshares-core` | consensus gating, evaluators, wallet, operations, tests |
| `libraries/fc` (submodule) | the primitives — `fc/crypto/pqc.*`, `vendor/pqclean/`, and `fc::raw::pq_format` itself |

`fc::raw::pq_format` is the mechanism every hardfork gate is built on, and it lives in the
submodule. The submodule is therefore pinned to a branch on the fork rather than upstream,
because the pinned commit is not reachable from `bitshares/bitshares-fc`. Both the URL and the
pin need reverting to upstream once the fc side is merged, and **the fc side has to merge
first** — core does not compile without it.

## The asymmetry that should drive priorities

Not all of these surfaces fail the same way, and the difference is more important than the
algorithm choices.

**A signature only has to hold until its transaction confirms.** If secp256k1 breaks in
2035, nobody can retroactively forge a 2026 transfer — it is already buried under a decade
of blocks. What breaks is the *future* ability to spend, and an account can re-key at any
time before then. The exposure is real but it is recoverable, and it is bounded by how fast
accounts migrate once the threat is credible.

**A memo is encrypted once and stored forever.** Anyone can archive the chain today and
decrypt it the day secp256k1 falls. Nothing done afterwards — re-keying, migrating,
hardforking — makes a memo written today unreadable again. Harvest-now-decrypt-later is not
hypothetical here; the ciphertext is *already* published to every node and every block
explorer.

So of everything below, the memo work is the only part where delay causes permanent,
unrecoverable loss. The signature work is the part where delay causes recoverable risk.
That is why memos were built even though signatures came first.

## Surface by surface

| Surface | Mechanism | Algorithm | State |
|---|---|---|---|
| Transaction signatures | `authority.pq_key_auths` | ML-DSA-65 | Implemented, **opt-in per account** |
| Block production | `witness_object.pq_signing_key` | ML-DSA-65 | Implemented, **opt-in per witness** |
| Memo encryption | `memo_data.pq_ciphertext` + `account_options.pq_memo_key` | ML-KEM-768 (hybrid with ECDH) | Implemented, **opt-in per recipient** |
| P2P transport | `stcp_socket` handshake | ML-KEM-768 (hybrid with ECDH) | Implemented, **off by default** |
| Address authorities | `authority.address_auths` | secp256k1 only | **No post-quantum equivalent** |
| Hashing (ids, merkle, HTLC preimages) | SHA-256 / RIPEMD-160 | — | No action needed; Grover halves SHA-256 to ~128-bit, which is still adequate |

Everything in the first four rows is gated behind **both** `HARDFORK_PQ_0_PASSED` and the
committee parameter `pq_serialization_active`. Both are currently inactive:

- `HARDFORK_PQ_0_TIME` is `1893456000` = **2030-01-01T00:00:00Z**, an explicit placeholder.
  The real date is a committee decision.
- `pq_serialization_active` is an `optional<bool>` that defaults to unset.

Until both are true, none of the post-quantum fields reach the wire at all, and the
evaluators reject operations that carry them.

## Why "implemented" is not "safe"

Three distinct gaps, in increasing order of how hard they are to close.

### 1. Not activated (a committee decision)

Nothing is on. This is deliberate and correct — the placeholder hardfork date should not be
a real one, and a chain should not silently change its wire format. But it means the honest
answer to "is BitShares post-quantum safe" is *no*, and will stay *no* until a committee
schedules it.

### 2. Opt-in, with no forcing mechanism

This is the substantive gap. Every protection above is per-account or per-operator:

- An account keeps classical-only authorities until someone runs `migrate_wallet_pq_only()`.
- **`migrate_wallet()` is not protection.** It adds a PQ key *alongside* the classical one at
  equal weight, so either signature alone authorizes. Against a quantum adversary that is
  exactly as strong as no PQ key at all — the attacker simply uses the classical path. Only
  `migrate_wallet_pq_only()`, which removes the classical keys, changes the security
  property. The distinction is documented in `wallet_sign.cpp` because it is easy to get
  backwards.
- An account receives only classical memos until someone runs `generate_pq_memo_key()`.
  Sending is automatic once the recipient has published a key, so adoption is limited by
  recipients, not senders.
- A witness signs classically until it configures a PQ signing key.
- A node negotiates a classical P2P channel unless started with `--enable-pq-p2p`.

There is currently no deadline, no incentive, and no mechanism that ever makes an account
post-quantum safe without its owner acting. A chain where 5% of accounts have migrated is
not a post-quantum chain. Closing this is a governance design problem — a migration
deadline, a fee differential, a required re-key — and not something that can be decided in
the code.

### 3. `address_auths` has no post-quantum equivalent

`authority` gained `pq_key_auths` but `address_auths` was left classical. An account holding
an address authority remains quantum-spendable through that path even after migrating its
keys. `migrate_wallet_pq_only()` refuses to operate on such accounts rather than reporting a
false success, which is the right behaviour, but it means those accounts have no migration
path at all on this branch.

## The memo construction, specifically

Both the memo and the P2P handshake are **hybrid**, not post-quantum alone:

```
legacy   sha512( nonce ‖ ecdh_secret )
hybrid   sha512( nonce ‖ ecdh_secret ‖ ml_kem_shared_secret )
```

An attacker must break both secp256k1 ECDH and ML-KEM-768. This is a deliberate hedge in
both directions: no weaker than today's memo if this KEM implementation turns out flawed,
and no weaker than the KEM once secp256k1 falls. Given that ML-KEM is young and that a memo
is unrecoverable once written, spending a few hundred bytes to avoid betting everything on
one primitive is worth it.

Two consequences worth knowing:

- **Encryption stays classical until the chain has activated PQ serialization.** The KEM
  shared secret is half of the AES key, so writing a hybrid memo while the ciphertext field
  is still stripped on the wire would confirm a memo that nobody — sender, recipient, or
  anyone else — could ever read. The wallet checks activation before choosing the hybrid
  path.
- **If an account publishes a memo key and loses the secret, memos sent to it afterwards are
  permanently unreadable.** `generate_pq_memo_key()` persists the secret to the wallet file
  *before* broadcasting the account update for exactly this reason, but a lost wallet file
  is unrecoverable. Back it up with `dump_pq_private_keys()`.

ML-KEM-768 is fixed rather than negotiable: the memo carries no algorithm tag of its own, so
`account_options::validate()` rejects ML-KEM-512 and ML-KEM-1024 keys. Publishing one would
advertise a key no sender could encapsulate to. Widening this requires making the memo
construction algorithm-aware first.

## What is verified, and what is not

**Verified** (620/620 in `chain_test`; the mainnet replay below is a separate opt-in run,
enabled by pointing `BITSHARES_MAINNET_BLOCK_LOG` at a block log, and is skipped in a default
run so CI stays hermetic):

- The wire format is unchanged under `pq_format::legacy`, field by field, for every gated
  structure — including when the in-memory object holds post-quantum data. This is the
  property that a previously-found defect broke, and it is now asserted directly rather than
  inferred.
- Replaying 35,003 real mainnet blocks (27,809 of them carrying transactions) reproduces
  each block's recorded id and merkle root. This is the check that found the
  witness-operation wire-format defect, and the one that synthetic chains cannot substitute
  for: the ids were computed by upstream code years ago and are not ours to choose. It
  matters most for the memo change specifically — `memo_data` lives inside
  `transfer_operation`, which is the most common operation on the chain, so a mistake in its
  gating would show up here in thousands of blocks rather than in some rare corner.
- Hybrid memos round-trip; the classical key alone cannot read one; a foreign KEM key cannot
  either.
- Post-quantum fields are rejected before activation and accepted after, end to end through
  the chain database.
- A PQ-signed transaction verifies and applies with no classical key in the authority.
- **The vendored primitives match FIPS 203/204.** 262 known-answer checks against NIST's own
  ACVP vectors (`libraries/fc/tests/crypto/pqc_kat/`): ML-KEM-768 keyGen, encapsulation and
  decapsulation, and ML-DSA-65 keyGen, all reproducing NIST's expected outputs byte for byte.
  ML-DSA-65 signature verification is checked on 15 cases, 12 of them tampered — modified
  message, modified `z`, modified commitment, modified hint — so a verifier that accepted
  forgeries would fail rather than pass a suite of valid signatures only. The decapsulation
  group covers FIPS 203 §7.3 implicit rejection, where a malformed ciphertext must yield a
  pseudorandom secret rather than an error.

**Not verified:**

- **ML-DSA signature generation has no conformance vectors.** FIPS 204 deterministic signing
  requires `rnd = 0`, and the vendored `crypto_sign_signature_ctx` always draws `rnd` from the
  RNG (the hedged variant), so sigGen vectors cannot be run without a deterministic entry
  point that does not exist yet. Signing is currently covered only indirectly: signatures this
  code produces verify under a verifier that *is* checked against NIST vectors. Key
  generation and verification — the two halves consensus actually depends on — are covered
  directly.
- Only the parameter sets in use (ML-KEM-768, ML-DSA-65) have conformance coverage. The
  vendored tree also builds ML-KEM-512/1024 and ML-DSA-44/87, which nothing consumes.
- No third-party cryptographic audit of the integration.
- No side-channel analysis of the vendored primitives.
- Performance under sustained load with PQ signatures on a real network. An ML-DSA-65
  signature is 3309 bytes against secp256k1's 65; block sizes, bandwidth, and validation
  cost all change materially, and this has only been measured on a devnet.

## What activation would actually require

In order:

1. ~~Add NIST KAT vectors for the vendored ML-DSA and ML-KEM parameter sets.~~ Done for
   ML-KEM-768 and ML-DSA-65; ML-DSA signature generation still needs a deterministic entry
   point before its vectors can be run.
2. Independent review of the cryptographic integration.
3. Measure PQ signature load on a realistic network — `maximum_transaction_size` must leave
   room for key-bearing operations (see the sizing note in `hardfork.d/PQ_0.hf`; the
   historical 2048 default is too small).
4. Set a real `HARDFORK_PQ_0_TIME`.
5. Committee enables `pq_serialization_active`.
6. Witnesses configure PQ signing keys.
7. Design and agree a migration policy for the opt-in gap above — this is the one that
   determines whether the chain is actually post-quantum safe, rather than merely capable
   of being.

Steps 1–6 are engineering and governance mechanics. Step 7 is the one that decides the
answer.
