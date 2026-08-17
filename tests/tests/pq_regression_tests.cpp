/*
 * Regression tests for the PQ_0 hardfork serialization defects found during review.
 *
 * Every defect covered here passed the whole existing test suite before it was found, and
 * was caught only by running real nodes against each other across an activation. Each case
 * below fails against the unfixed code, so the suite is a guard against that specific way
 * of getting it wrong rather than a general test of the feature.
 *
 * The defects share one shape: a block's serialization format is decided by its
 * predecessor's timestamp, so any code that captures the format at a moment when local head
 * is not that predecessor gets a different answer than the rest of the network.
 */

#include <boost/test/unit_test.hpp>

#include <graphene/protocol/block.hpp>
#include <graphene/protocol/transaction.hpp>
#include <graphene/protocol/account.hpp>
#include <graphene/chain/block_database.hpp>

#include <fc/io/raw.hpp>
#include <fc/filesystem.hpp>

using namespace graphene::protocol;

BOOST_AUTO_TEST_SUITE( pq_regression_tests )

namespace {

/// A block whose two serialization formats differ only by the empty-optional marker for
/// witness_pq_signature -- i.e. the ordinary case of a block produced by a witness that has
/// no post-quantum key, which is what every block looks like immediately after activation.
signed_block make_block_without_pq_signature()
{
   signed_block b;
   b.previous = block_id_type();
   b.timestamp = fc::time_point_sec( 3000000 );
   b.witness = witness_id_type( 1 );
   b.witness_signature = signature_type();
   // witness_pq_signature deliberately left unset
   return b;
}

} // namespace

/**
 * Root cause of the "chain becomes unjoinable after activation" defect.
 *
 * signed_block_header::id() caches into a mutable member. A block's id depends on the
 * serialization format, because the format decides whether witness_pq_signature is among
 * the hashed bytes, so caching on "computed yet?" alone froze each block's id to whichever
 * format was in effect the first time anything asked for it.
 *
 * On a receiving node that first caller is the log line in handle_block, which runs after
 * the block is decoded but before it is applied -- while chain state still reports the
 * pre-hardfork format. The stale legacy id was then reused by everything downstream, so the
 * producer and the receiver recorded different ids for a byte-for-byte identical block and
 * the receiver rejected the next block as unlinkable.
 */
BOOST_AUTO_TEST_CASE( block_id_is_recomputed_when_the_format_changes )
{
   const signed_block prototype = make_block_without_pq_signature();

   // Reference ids, each taken from a pristine object so no cache can be involved.
   block_id_type legacy_id;
   block_id_type current_id;
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::legacy );
      signed_block fresh = prototype;
      legacy_id = fresh.id();
   }
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::current );
      signed_block fresh = prototype;
      current_id = fresh.id();
   }

   // If these matched, the rest of the test would prove nothing.
   BOOST_CHECK_NE( legacy_id.str(), current_id.str() );

   // The actual regression: one object, asked under both formats, must give both answers.
   // Before the fix the second call returned the first call's cached value.
   signed_block reused = prototype;
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::legacy );
      BOOST_CHECK_EQUAL( reused.id().str(), legacy_id.str() );
   }
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::current );
      BOOST_CHECK_EQUAL( reused.id().str(), current_id.str() );
   }
   // And back, so the cache is not merely being invalidated once.
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::legacy );
      BOOST_CHECK_EQUAL( reused.id().str(), legacy_id.str() );
   }
}

/// Repeated calls under one format must stay stable -- the fix must not have turned the
/// cache into a "recompute every time the member is read" that returns inconsistent values.
BOOST_AUTO_TEST_CASE( block_id_is_stable_within_one_format )
{
   signed_block b = make_block_without_pq_signature();
   fc::raw::scoped_pq_format fmt( fc::raw::pq_format::current );
   const std::string first = b.id().str();
   BOOST_CHECK_EQUAL( b.id().str(), first );
   BOOST_CHECK_EQUAL( b.id().str(), first );
}

/**
 * transaction::id() recomputes from the ambient format on every call rather than caching on
 * "computed yet?", which is why it never developed the defect above. Pin that behaviour so
 * nobody "optimises" it into the same trap.
 */
BOOST_AUTO_TEST_CASE( transaction_id_follows_the_ambient_format )
{
   // The id comes from digest(), which packs the transaction body -- not the signatures --
   // so the format-sensitive part has to be inside an operation. An authority's
   // pq_key_auths is omitted entirely under the legacy format, which is exactly the case
   // that would let two different authorities hash to the same transaction id.
   authority auth;
   auth.weight_threshold = 1;
   auth.pq_key_auths[ pq_public_key_type() ] = 1;

   account_update_operation op;
   op.account = account_id_type( 1 );
   op.owner = auth;

   signed_transaction tx;
   tx.ref_block_num = 2;
   tx.expiration = fc::time_point_sec( 3000001 );
   tx.operations.push_back( operation( op ) );

   std::string legacy_id;
   std::string current_id;
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::legacy );
      legacy_id = tx.id().str();
   }
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::current );
      current_id = tx.id().str();
   }
   BOOST_CHECK_NE( legacy_id, current_id );

   // Same object, back to the first format: must give the first answer again.
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::legacy );
      BOOST_CHECK_EQUAL( tx.id().str(), legacy_id );
   }
}

/**
 * The same cache-once hazard as the block id, in three more places. All were found by
 * re-reading the code after the id fix rather than by testing, because none of them is
 * reachable from a devnet made of empty blocks: the merkle root returns early when a block
 * has no transactions, and the two transaction caches need a transaction to exist at all.
 *
 * Each of these is consensus-relevant. A stale merkle root fails block validation against
 * the rest of the network; a stale packed size feeds fee calculation and the per-block size
 * limit; a stale signee set is a wrong answer about who signed a transaction.
 */
BOOST_AUTO_TEST_CASE( transaction_caches_follow_the_format )
{
   // An authority carrying pq_key_auths is the format-sensitive payload: the legacy format
   // omits it from the packed bytes entirely.
   authority auth;
   auth.weight_threshold = 1;
   auth.pq_key_auths[ pq_public_key_type() ] = 1;

   account_update_operation op;
   op.account = account_id_type( 1 );
   op.owner = auth;

   precomputable_transaction prototype;
   prototype.ref_block_num = 2;
   prototype.expiration = fc::time_point_sec( 3000001 );
   prototype.operations.push_back( operation( op ) );

   // Reference sizes, each measured on a pristine object so the cache cannot be involved.
   // Taking both from one object would conflate the premise with the defect: the stale
   // value is returned for the second format, and the two would then compare equal for the
   // very reason the test exists.
   uint64_t legacy_size = 0;
   uint64_t current_size = 0;
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::legacy );
      precomputable_transaction fresh = prototype;
      legacy_size = fresh.get_packed_size();
   }
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::current );
      precomputable_transaction fresh = prototype;
      current_size = fresh.get_packed_size();
   }
   // The pq_key_auths entry is only counted under the current format.
   BOOST_CHECK_NE( legacy_size, current_size );

   // The regression: one object asked under both formats must give both answers.
   precomputable_transaction reused = prototype;
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::legacy );
      BOOST_CHECK_EQUAL( reused.get_packed_size(), legacy_size );
   }
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::current );
      BOOST_CHECK_EQUAL( reused.get_packed_size(), current_size );
   }
}

/// A block's merkle root is built from format-dependent transaction digests, so it has to
/// be recomputed when the format changes. Needs a non-empty block: calculate_merkle_root()
/// returns a fixed empty checksum before it ever consults the cache, which is why a devnet
/// producing only empty blocks could never have exposed this.
BOOST_AUTO_TEST_CASE( merkle_root_is_recomputed_when_the_format_changes )
{
   authority auth;
   auth.weight_threshold = 1;
   auth.pq_key_auths[ pq_public_key_type() ] = 1;

   account_update_operation op;
   op.account = account_id_type( 1 );
   op.owner = auth;

   signed_transaction tx;
   tx.ref_block_num = 2;
   tx.expiration = fc::time_point_sec( 3000001 );
   tx.operations.push_back( operation( op ) );

   signed_block prototype = make_block_without_pq_signature();
   prototype.transactions.push_back( processed_transaction( tx ) );

   checksum_type legacy_root;
   checksum_type current_root;
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::legacy );
      signed_block fresh = prototype;
      legacy_root = fresh.calculate_merkle_root();
   }
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::current );
      signed_block fresh = prototype;
      current_root = fresh.calculate_merkle_root();
   }
   BOOST_CHECK_NE( legacy_root.str(), current_root.str() );

   signed_block reused = prototype;
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::legacy );
      BOOST_CHECK_EQUAL( reused.calculate_merkle_root().str(), legacy_root.str() );
   }
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::current );
      BOOST_CHECK_EQUAL( reused.calculate_merkle_root().str(), current_root.str() );
   }
}

/**
 * The p2p layer decodes blocks on arrival but applies them later, so at an activation
 * boundary chain state cannot tell it which format a block was sent in. The decoder instead
 * relies on the two formats never both accounting for the buffer exactly, since they differ
 * in length by the empty-optional marker. That premise is what this pins down.
 */
BOOST_AUTO_TEST_CASE( the_two_formats_are_distinguishable_by_exact_consumption )
{
   const signed_block b = make_block_without_pq_signature();

   std::vector<char> current_bytes;
   std::vector<char> legacy_bytes;
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::current );
      current_bytes = fc::raw::pack( b );
   }
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::legacy );
      legacy_bytes = fc::raw::pack( b );
   }
   BOOST_CHECK_NE( current_bytes.size(), legacy_bytes.size() );

   auto consumes_buffer_exactly = []( const std::vector<char>& bytes, fc::raw::pq_format fmt )
   {
      try
      {
         fc::raw::scoped_pq_format scoped( fmt );
         fc::datastream<const char*> ds( bytes.data(), bytes.size() );
         signed_block decoded;
         fc::raw::unpack( ds, decoded );
         return ds.remaining() == 0;
      }
      catch( const fc::exception& ) { return false; }
      catch( const std::exception& ) { return false; }
   };

   // Exactly one format accounts for each encoding, in both directions.
   BOOST_CHECK( consumes_buffer_exactly( current_bytes, fc::raw::pq_format::current ) );
   BOOST_CHECK( !consumes_buffer_exactly( current_bytes, fc::raw::pq_format::legacy ) );
   BOOST_CHECK( consumes_buffer_exactly( legacy_bytes, fc::raw::pq_format::legacy ) );
   BOOST_CHECK( !consumes_buffer_exactly( legacy_bytes, fc::raw::pq_format::current ) );
}

/**
 * block_database::store() packs under the format in effect on the writing thread, which
 * follows chain state when the block was applied. Reads happen on other threads -- API
 * calls, p2p item requests, the startup index check -- under whatever format their own
 * context set. From activation onward every such read used the wrong format, and because
 * every caller in that file swallows the exception, the block simply came back empty.
 *
 * The visible effects were all silent: get_block() returned null for every block after
 * activation, the node could not serve those blocks to peers, and the startup index check
 * mistook them for corruption and truncated the index file.
 */
BOOST_AUTO_TEST_CASE( stored_blocks_are_readable_under_the_other_format )
{
   fc::temp_directory tmp;
   graphene::chain::block_database bdb;
   bdb.open( tmp.path() );
   BOOST_REQUIRE( bdb.is_open() );

   const signed_block b = make_block_without_pq_signature();

   // Written the way a node past the hardfork writes it.
   block_id_type stored_id;
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::current );
      signed_block to_store = b;
      stored_id = to_store.id();
      bdb.store( stored_id, to_store );
   }
   bdb.flush();

   // Read the way an API thread reads it, which is under the default legacy format.
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::legacy );
      auto by_id = bdb.fetch_optional( stored_id );
      BOOST_REQUIRE_MESSAGE( by_id.valid(), "post-hardfork block unreadable under legacy format" );

      auto by_number = bdb.fetch_by_number( block_header::num_from_id( stored_id ) );
      BOOST_REQUIRE_MESSAGE( by_number.valid(), "post-hardfork block unreadable by number" );

      // And it must be the block we stored, not merely something that parsed.
      BOOST_CHECK_EQUAL( by_id->timestamp.sec_since_epoch(), b.timestamp.sec_since_epoch() );
      BOOST_CHECK_EQUAL( by_id->witness.instance.value, b.witness.instance.value );
   }

   // The startup index check must not mistake a format difference for a corrupt index and
   // truncate away everything written since activation.
   {
      fc::raw::scoped_pq_format fmt( fc::raw::pq_format::legacy );
      auto last = bdb.last_id();
      BOOST_REQUIRE( last.valid() );
      BOOST_CHECK_EQUAL( last->str(), stored_id.str() );
   }

   bdb.close();
}

BOOST_AUTO_TEST_SUITE_END()
